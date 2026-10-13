/**
 * \file AboutDialog.cpp
 * \ingroup app
 * \brief Implementation of the About dialog.
 */
#include "AboutDialog.h"

#include <commctrl.h>

#include <algorithm>

#include "Diagnostics.h"  // CopyToClipboard, BuildStamp
#include "Log.h"
#include "Version.h"
#include "resource.h"

namespace mfly {
namespace {

/// Colour of the warning about uncommitted changes.
constexpr COLORREF kDirtyTextColor = RGB(176, 0, 0);

/// Timer that restores the caption of the copy button.
constexpr UINT_PTR kCopyFeedbackTimer = 1;

/// How long the copy confirmation stays on the button, in milliseconds.
constexpr UINT kCopyFeedbackDelay = 1200;

/// Default caption of the copy button.
constexpr const wchar_t* kCopyCaption = L"&Copy";

/// The open dialog, so a second request brings it forward instead.
HWND g_about = nullptr;

/**
 * \brief Attaches a tooltip to one control of a dialog.
 *
 * \param dialog    The dialog.
 * \param controlId Identifier of the control.
 * \param text      Tooltip text; the tooltip keeps the pointer, so it must stay
 *                  valid for the lifetime of the dialog.
 * \return The tooltip window, or \c nullptr on failure.
 */
HWND CreateControlTooltip(HWND dialog, int controlId, const wchar_t* text) {
    HWND control = ::GetDlgItem(dialog, controlId);
    if (!control) return nullptr;

    HWND tooltip = ::CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     CW_USEDEFAULT, dialog, nullptr, nullptr, nullptr);
    if (!tooltip) return nullptr;

    TTTOOLINFOW info{};
#ifdef TTTOOLINFOW_V2_SIZE
    // The V2 size is understood by both the v5 and the v6 common controls.
    info.cbSize = TTTOOLINFOW_V2_SIZE;
#else
    info.cbSize = sizeof(info);
#endif
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = dialog;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<LPWSTR>(text);

    if (::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)) == FALSE) {
        ::DestroyWindow(tooltip);
        return nullptr;
    }
    return tooltip;
}

/**
 * \brief Fills the shortcut list: two columns, one group per heading.
 * \param list The list view from the dialog template.
 * \param rows What to show.
 */
void FillShortcutList(HWND list, const std::vector<ShortcutRow>& rows) {
    if (!list) return;

    // The list styles are set here, so the resource script needs no commctrl.h.
    ::SetWindowLongPtrW(list, GWL_STYLE,
                        ::GetWindowLongPtrW(list, GWL_STYLE) | LVS_REPORT | LVS_SINGLESEL |
                            LVS_NOSORTHEADER | LVS_SHOWSELALWAYS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 100;
    column.pszText = const_cast<LPWSTR>(L"Keys");
    ListView_InsertColumn(list, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Command");
    ListView_InsertColumn(list, 1, &column);
    ListView_EnableGroupView(list, TRUE);

    std::vector<std::wstring> groups;
    for (const ShortcutRow& row : rows) {
        auto found = std::find(groups.begin(), groups.end(), row.group);
        int groupId = static_cast<int>(found - groups.begin()) + 1;
        if (found == groups.end()) {
            groups.push_back(row.group);
            groupId = static_cast<int>(groups.size());

            LVGROUP group{};
            group.cbSize = sizeof(group);
            group.mask = LVGF_HEADER | LVGF_GROUPID;
            group.pszHeader = const_cast<LPWSTR>(groups.back().c_str());
            group.iGroupId = groupId;
            ListView_InsertGroup(list, -1, &group);
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_GROUPID;
        item.iItem = ListView_GetItemCount(list);
        item.iGroupId = groupId;
        item.pszText = const_cast<LPWSTR>(row.keys.c_str());
        const int index = ListView_InsertItem(list, &item);
        if (index >= 0) {
            ListView_SetItemText(list, index, 1, const_cast<LPWSTR>(row.command.c_str()));
        }
    }

    // Keys as wide as the widest entry, the command column takes the rest.
    ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
    RECT client{};
    ::GetClientRect(list, &client);
    const int keysWidth = ListView_GetColumnWidth(list, 0);
    const int rest = (client.right - client.left) - keysWidth - ::GetSystemMetrics(SM_CXVSCROLL);
    ListView_SetColumnWidth(list, 1, std::max(rest, 80));
}

/**
 * \brief Puts the dialog in the middle of the screen the pointer is on.
 *
 * The owner is our invisible message window in the corner of the primary
 * screen, and centring on it would put the dialog wherever that happens to be
 * rather than where the user just clicked the tray icon.
 *
 * \param dialog The dialog.
 */
void CentreOnPointerScreen(HWND dialog) {
    POINT pt{};
    ::GetCursorPos(&pt);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return;

    RECT r{};
    ::GetWindowRect(dialog, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    const RECT& work = mi.rcWork;
    ::SetWindowPos(dialog, nullptr,
                   work.left + std::max(0L, ((work.right - work.left) - w) / 2),
                   work.top + std::max(0L, ((work.bottom - work.top) - h) / 2), 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/**
 * \brief Dialog procedure of the About dialog.
 * \param dialog  The dialog.
 * \param message Message.
 * \param wParam  First parameter.
 * \param lParam  Second parameter; the shortcut rows with \c WM_INITDIALOG.
 * \return \c TRUE if the message was handled.
 */
INT_PTR CALLBACK AboutProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        g_about = dialog;
        const auto* rows = reinterpret_cast<const std::vector<ShortcutRow>*>(lParam);

        ::SetDlgItemTextW(dialog, IDC_ABOUT_VERSION,
                          (std::wstring(L"MinFlyout  ") + VersionString()).c_str());
        ::SetDlgItemTextW(dialog, IDC_ABOUT_COMMIT, VER_WIDEN(VER_COMMIT_SHORT));
        ::SetDlgItemTextW(dialog, IDC_ABOUT_BUILT,
                          (std::wstring(L"branch ") + VER_WIDEN(VER_BRANCH) +
                           L", built " + BuildStamp()).c_str());

        // The full hash would dominate the dialog, so it lives in a tooltip
        // and on the clipboard.
        CreateControlTooltip(dialog, IDC_ABOUT_COMMIT, VER_WIDEN(VER_COMMIT_FULL));

#if VER_DIRTY
        ::SetDlgItemTextW(dialog, IDC_ABOUT_DIRTY,
                          L"Uncommitted changes - binary differs from " VER_WIDEN(
                              VER_COMMIT_SHORT) L".");
#endif

        if (rows) FillShortcutList(::GetDlgItem(dialog, IDC_ABOUT_SHORTCUTS), *rows);

        CentreOnPointerScreen(dialog);
        ::SetForegroundWindow(dialog);

        // Focus the OK button; the dialog manager would otherwise pick the
        // copy button.
        ::SetFocus(::GetDlgItem(dialog, IDOK));
        return FALSE;
    }

    case WM_CTLCOLORSTATIC:
        if (::GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_ABOUT_DIRTY) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(dc, kDirtyTextColor);
            ::SetBkColor(dc, ::GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        break;

    case WM_TIMER:
        if (wParam == kCopyFeedbackTimer) {
            ::KillTimer(dialog, kCopyFeedbackTimer);
            ::SetDlgItemTextW(dialog, IDC_ABOUT_COPY, kCopyCaption);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_ABOUT_COMMIT:
            // SS_NOTIFY: clicking the hash copies it, too.
            if (HIWORD(wParam) != STN_CLICKED) break;
            [[fallthrough]];

        case IDC_ABOUT_COPY:
            if (CopyToClipboard(VER_WIDEN(VER_COMMIT_FULL))) {
                ::SetDlgItemTextW(dialog, IDC_ABOUT_COPY, L"Copied");
                ::SetTimer(dialog, kCopyFeedbackTimer, kCopyFeedbackDelay, nullptr);
            }
            return TRUE;

        case IDOK:
        case IDCANCEL:
            ::EndDialog(dialog, LOWORD(wParam));
            return TRUE;

        default:
            break;
        }
        break;

    case WM_DESTROY:
        g_about = nullptr;
        break;

    default:
        break;
    }
    return FALSE;
}

}  // namespace

const wchar_t* VersionString() {
    return VER_WIDEN(VER_VERSION_STR);
}

const wchar_t* CommitDescription() {
    return VER_WIDEN(VER_BRANCH) L": " VER_WIDEN(VER_COMMIT_SHORT) VER_WIDEN(VER_DIRTY_SUFFIX);
}

void ShowAboutDialog(HINSTANCE instance, HWND owner, const std::vector<ShortcutRow>& rows) {
    if (g_about) {
        ::SetForegroundWindow(g_about);
        return;
    }

    // List view with groups and the tooltip; the manifest selects version 6.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&controls);

    const INT_PTR result = ::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_ABOUT), owner,
                                             &AboutProc, reinterpret_cast<LPARAM>(&rows));
    if (result == -1) {
        WRITE_ERROR_LOG(L"About dialog could not be shown",
                        log::dformat(L"error {}", ::GetLastError()));
    }
}

}  // namespace mfly
