/**
 * \file BuiltinProviders.cpp
 * \ingroup config
 * \brief The items of the flyout.
 *
 * Two providers: \c core (minimize, notification area, always on top) and
 * \c groups (app windows, other windows, show desktop). The window positions
 * themselves are not items - they are drawn as monitor miniatures by
 * \ref mfly::FlyoutWindow.
 *
 * All providers read the configuration afresh every time the flyout opens, so
 * that "Reload configuration" takes effect without signing out.
 */
#include "CaptionProbe.h"
#include "Config.h"
#include "ItemRegistry.h"
#include "TrayStash.h"

namespace mfly {
namespace {

/**
 * \brief Checks whether a window would appear in the Alt-Tab list.
 * \param hwnd Window to check.
 * \return \c true for visible, standalone application windows.
 */
bool IsAltTabWindow(HWND hwnd) {
    if (!::IsWindowVisible(hwnd)) return false;
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (IsIgnoredWindow(hwnd)) return false;

    wchar_t title[8] = {};
    if (::GetWindowTextLengthW(hwnd) == 0 &&
        ::GetWindowTextW(hwnd, title, ARRAYSIZE(title)) == 0) {
        return false;
    }
    return true;
}

/**
 * \brief Checks whether a window can be minimized.
 * \param hwnd Window to check.
 * \return \c true if it has a minimize button and is not already minimized.
 */
bool CanMinimize(HWND hwnd) {
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    return (style & WS_MINIMIZEBOX) != 0 && !::IsIconic(hwnd);
}

/// Search parameters for \ref CollectWindows.
struct EnumCtx {
    DWORD pid = 0;             ///< Process ID to look for.
    HWND  skip = nullptr;      ///< Skip this window.
    bool  samePidOnly = false; ///< Only collect windows with \ref pid.
    std::vector<HWND> found;   ///< Result.
};

/// Callback for \c EnumWindows; collects matching windows in \ref EnumCtx.
BOOL CALLBACK CollectWindows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (hwnd == ctx->skip || !IsAltTabWindow(hwnd) || !CanMinimize(hwnd)) return TRUE;

    if (ctx->samePidOnly) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
    }
    ctx->found.push_back(hwnd);
    return TRUE;
}

/**
 * \brief Collects windows by process or by exclusion.
 * \param pid         Process ID to look for (only with \p samePidOnly).
 * \param skip        Window to leave out.
 * \param samePidOnly \c true restricts the result to \p pid.
 * \return The windows that were found.
 */
std::vector<HWND> FindWindows(DWORD pid, HWND skip, bool samePidOnly) {
    EnumCtx ctx;
    ctx.pid = pid;
    ctx.skip = skip;
    ctx.samePidOnly = samePidOnly;
    ::EnumWindows(&CollectWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

/// Minimizes all given windows without waiting for them.
void MinimizeAll(const std::vector<HWND>& windows) {
    for (HWND h : windows) {
        ::ShowWindowAsync(h, SW_MINIMIZE);
    }
}

/// Provider \c core: the actions that map directly to the minimize button.
void ProvideCore(const Context& ctx, ItemList& out) {
    if (!ConfigStore::Instance().current().showBuiltinItems) return;

    out.AddAction(L"Minimize",
                  [](const Context& c) { ::ShowWindowAsync(c.targetWindow, SW_MINIMIZE); },
                  kItemDefault);

    out.AddAction(L"Minimize to tray",
                  [](const Context& c) { TrayStash::Instance().Stash(c.targetWindow); });

    const bool topmost =
        (::GetWindowLongPtrW(ctx.targetWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    out.AddAction(L"Always on top",
                  [topmost](const Context& c) {
                      ::SetWindowPos(c.targetWindow, topmost ? HWND_NOTOPMOST : HWND_TOPMOST,
                                     0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                  },
                  topmost ? kItemChecked : kItemNone);
}

/// Provider \c groups: actions that affect several windows.
void ProvideGroups(const Context& ctx, ItemList& out) {
    if (!ConfigStore::Instance().current().showBuiltinItems) return;

    const bool hasSiblings = FindWindows(ctx.targetProcessId, nullptr, true).size() > 1;

    out.AddAction(L"Minimize all windows of this app",
                  [](const Context& c) {
                      MinimizeAll(FindWindows(c.targetProcessId, nullptr, true));
                  },
                  hasSiblings ? kItemNone : kItemDisabled);

    out.AddAction(L"Minimize other windows",
                  [](const Context& c) {
                      MinimizeAll(FindWindows(0, c.targetWindow, false));
                  });

    out.AddAction(L"Show desktop", [](const Context&) {
        // 419 = MIN_ALL (documented shell command of the taskbar)
        if (HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr)) {
            ::PostMessageW(tray, WM_COMMAND, 419, 0);
        }
    });
}

}  // namespace

void RegisterBuiltinProviders() {
    Registry::Instance().Register(L"core", 0, &ProvideCore);
    Registry::Instance().Register(L"groups", 20, &ProvideGroups);
}

}  // namespace mfly
