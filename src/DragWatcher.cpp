/**
 * \file DragWatcher.cpp
 * \ingroup detect
 * \brief Implementation of the move/size watcher.
 */
#include "DragWatcher.h"

#include <atomic>

#include "Log.h"

namespace mfly {
namespace {

/// Recipient of the two messages; read from the callback.
std::atomic<HWND> g_owner{nullptr};

}  // namespace

bool DragWatcher::Start(HWND owner) {
    if (hook_) return true;
    g_owner.store(owner, std::memory_order_relaxed);

    // WINEVENT_OUTOFCONTEXT: nothing is loaded into the foreign process, the
    // callback is delivered to this thread's message loop instead.
    // WINEVENT_SKIPOWNPROCESS: our own windows are never dragged by the user.
    hook_ = ::SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
                              nullptr, &DragWatcher::EventProc, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        WRITE_ERROR_LOG(L"WinEvent hook for move/size could not be installed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }
    WRITE_INFO_LOG(L"Move/size watcher running");
    return true;
}

void DragWatcher::Stop() {
    if (hook_) {
        ::UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    g_owner.store(nullptr, std::memory_order_relaxed);
}

void CALLBACK DragWatcher::EventProc(HWINEVENTHOOK, DWORD event, HWND window,
                                     LONG object, LONG child, DWORD, DWORD) {
    // Only the window itself, not one of its accessible children.
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !window) return;

    HWND owner = g_owner.load(std::memory_order_relaxed);
    if (!owner) return;

    const UINT message = (event == EVENT_SYSTEM_MOVESIZESTART) ? WM_MFLY_DRAGSTART
                                                               : WM_MFLY_DRAGEND;
    ::PostMessageW(owner, message, 0, reinterpret_cast<LPARAM>(window));
}

}  // namespace mfly
