/**
 * \file TrayStash.cpp
 * \ingroup ui
 * \brief Implementation of "Minimize to notification area".
 */
#include "TrayStash.h"

#include <shellapi.h>

#include "Log.h"

namespace mfly {
namespace {

HICON GetWindowIconSmall(HWND hwnd, bool& ownsIcon) {
    ownsIcon = false;
    DWORD_PTR result = 0;
    if (::SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0,
                              SMTO_ABORTIFHUNG, kSendTimeoutMs, &result) && result) {
        return reinterpret_cast<HICON>(result);
    }
    if (::SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0,
                              SMTO_ABORTIFHUNG, kSendTimeoutMs, &result) && result) {
        return reinterpret_cast<HICON>(result);
    }
    if (HICON cls = reinterpret_cast<HICON>(::GetClassLongPtrW(hwnd, GCLP_HICONSM))) {
        return cls;
    }
    if (HICON cls = reinterpret_cast<HICON>(::GetClassLongPtrW(hwnd, GCLP_HICON))) {
        return cls;
    }
    return ::LoadIconW(nullptr, IDI_APPLICATION);  // shared system icon
}

}  // namespace

TrayStash& TrayStash::Instance() {
    static TrayStash instance;
    return instance;
}

void TrayStash::Init(HWND owner) { owner_ = owner; }

bool TrayStash::Stash(HWND target) {
    if (!owner_ || !target || !::IsWindow(target)) return false;
    for (const Entry& e : entries_) {
        if (e.window == target) return false;  // already stashed
    }

    Entry entry;
    entry.window = target;
    entry.id = nextId_++;
    entry.icon = GetWindowIconSmall(target, entry.ownsIcon);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = entry.id;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_MFLY_TRAY;
    nid.hIcon = entry.icon;
    ::GetWindowTextW(target, nid.szTip, ARRAYSIZE(nid.szTip) - 1);
    if (nid.szTip[0] == L'\0') {
        ::lstrcpynW(nid.szTip, L"(untitled)", ARRAYSIZE(nid.szTip));
    }

    if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
        if (entry.ownsIcon && entry.icon) ::DestroyIcon(entry.icon);
        return false;
    }

    ::ShowWindow(target, SW_HIDE);
    entries_.push_back(entry);
    WRITE_INFO_LOG(log::dformat(L"Window stashed, id {}", entry.id), log::Describe(target));
    return true;
}

void TrayStash::RemoveEntry(size_t index, bool restoreWindow) {
    if (index >= entries_.size()) return;
    Entry entry = entries_[index];
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = entry.id;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);

    if (restoreWindow && ::IsWindow(entry.window)) {
        ::ShowWindow(entry.window, SW_SHOW);
        ::SetForegroundWindow(entry.window);
    }
    if (entry.ownsIcon && entry.icon) ::DestroyIcon(entry.icon);
}

void TrayStash::RestoreAll() {
    if (!entries_.empty()) {
        WRITE_INFO_LOG(log::dformat(L"Restoring {} stashed windows", entries_.size()));
    }
    while (!entries_.empty()) {
        RemoveEntry(entries_.size() - 1, /*restoreWindow=*/true);
    }
}

void TrayStash::OnTrayMessage(WPARAM wParam, LPARAM lParam) {
    const UINT id = static_cast<UINT>(wParam);
    const UINT msg = static_cast<UINT>(LOWORD(lParam));
    if (msg != WM_LBUTTONUP && msg != WM_RBUTTONUP && msg != WM_LBUTTONDBLCLK) return;

    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            RemoveEntry(i, /*restoreWindow=*/true);
            return;
        }
    }
}

void TrayStash::DropDeadWindows() {
    for (size_t i = entries_.size(); i-- > 0;) {
        if (!::IsWindow(entries_[i].window)) {
            RemoveEntry(i, /*restoreWindow=*/false);
        }
    }
}

}  // namespace mfly
