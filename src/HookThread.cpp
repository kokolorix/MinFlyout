/**
 * \file HookThread.cpp
 * \ingroup detect
 * \brief Implementation of the hook thread; the callbacks only post messages.
 */
#include "HookThread.h"

#include <atomic>

#include "Log.h"

namespace mfly {
namespace {

enum : UINT {
    WM_HOOK_SETKEY    = WM_APP + 100,  // thread message: turn the keyboard hook on/off
    WM_HOOK_REINSTALL = WM_APP + 101,  // thread message: put the mouse hook back
};

std::atomic<HWND> g_owner{nullptr};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_movePending{false};
std::atomic<bool> g_keyHookActive{false};

/// Mouse movements the callback saw - the proof that the hook still fires.
std::atomic<unsigned long long> g_movesSeen{0};
/// Movements actually posted to the controller.
std::atomic<unsigned long long> g_movesPosted{0};

HHOOK g_mouseHook = nullptr;   // used on the hook thread only
HHOOK g_keyHook = nullptr;

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && !g_paused.load(std::memory_order_relaxed)) {
        HWND owner = g_owner.load(std::memory_order_relaxed);
        if (owner) {
            switch (wParam) {
            case WM_MOUSEMOVE:
                g_movesSeen.fetch_add(1, std::memory_order_relaxed);
                // Keep at most one move message in the queue at a time. If the
                // post fails the flag has to go back - otherwise a single lost
                // message would silence the detection for good.
                if (!g_movePending.exchange(true, std::memory_order_acq_rel)) {
                    if (::PostMessageW(owner, WM_MFLY_MOUSEMOVE, 0, 0)) {
                        g_movesPosted.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_movePending.store(false, std::memory_order_release);
                    }
                }
                break;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                ::PostMessageW(owner, WM_MFLY_MOUSEDOWN, 0, 0);
                break;
            default:
                break;
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        HWND owner = g_owner.load(std::memory_order_relaxed);
        if (kb && kb->vkCode == VK_ESCAPE && owner &&
            g_keyHookActive.load(std::memory_order_relaxed)) {
            ::PostMessageW(owner, WM_MFLY_CANCEL, 0, 0);
            return 1;  // ESC only closes the flyout and is not passed on
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

void HookThread::AckMouseMove() {
    g_movePending.store(false, std::memory_order_release);
}

unsigned long long HookThread::MovesSeen() {
    return g_movesSeen.load(std::memory_order_relaxed);
}

unsigned long long HookThread::MovesPosted() {
    return g_movesPosted.load(std::memory_order_relaxed);
}

bool HookThread::MovePending() {
    return g_movePending.load(std::memory_order_acquire);
}

bool HookThread::Installed() {
    return g_mouseHook != nullptr;
}

void HookThread::SetPaused(bool paused) {
    g_paused.store(paused, std::memory_order_relaxed);
}

bool HookThread::Start(HWND owner) {
    g_owner.store(owner, std::memory_order_relaxed);
    g_movePending.store(false, std::memory_order_release);
    ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) return false;

    thread_ = ::CreateThread(nullptr, 0, &HookThread::ThreadMain, this, 0, &threadId_);
    if (!thread_) return false;

    // Wait until the hooks have been installed.
    HANDLE handles[2] = {ready_, thread_};
    const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, 5000);
    return wait == WAIT_OBJECT_0 && g_mouseHook != nullptr;
}

void HookThread::Stop() {
    if (thread_) {
        ::PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        ::WaitForSingleObject(thread_, 3000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (ready_) {
        ::CloseHandle(ready_);
        ready_ = nullptr;
    }
    g_owner.store(nullptr, std::memory_order_relaxed);
}

void HookThread::Revive() {
    // The stuck flag is the cheap half and is cleared right here; putting the
    // hook back has to happen on the thread that owns it.
    g_movePending.store(false, std::memory_order_release);
    if (threadId_) ::PostThreadMessageW(threadId_, WM_HOOK_REINSTALL, 0, 0);
}

void HookThread::SetKeyboardHookEnabled(bool enabled) {
    if (!threadId_) return;
    g_keyHookActive.store(enabled, std::memory_order_relaxed);
    ::PostThreadMessageW(threadId_, WM_HOOK_SETKEY, enabled ? 1u : 0u, 0);
}

DWORD WINAPI HookThread::ThreadMain(LPVOID param) {
    static_cast<HookThread*>(param)->Run();
    return 0;
}

void HookThread::Run() {
    // Force creation of the message queue before anyone calls PostThreadMessage.
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseProc, ::GetModuleHandleW(nullptr), 0);
    ::SetEvent(ready_);
    if (!g_mouseHook) {
        WRITE_ERROR_LOG(L"WH_MOUSE_LL could not be installed",
                        log::dformat(L"error {}", ::GetLastError()));
        return;
    }
    WRITE_INFO_LOG(L"Hook thread running",
                   log::dformat(L"thread {}", ::GetCurrentThreadId()));

    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOOK_REINSTALL) {
            if (g_mouseHook) {
                ::UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = nullptr;
            }
            g_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseProc,
                                              ::GetModuleHandleW(nullptr), 0);
            if (g_mouseHook) {
                WRITE_WARNING_LOG(L"Mouse hook reinstalled");
            } else {
                WRITE_ERROR_LOG(L"Mouse hook could not be reinstalled",
                                log::dformat(L"error {}", ::GetLastError()));
            }
            continue;
        }
        if (msg.message == WM_HOOK_SETKEY) {
            const bool enable = msg.wParam != 0;
            if (enable && !g_keyHook) {
                g_keyHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardProc,
                                                ::GetModuleHandleW(nullptr), 0);
            } else if (!enable && g_keyHook) {
                ::UnhookWindowsHookEx(g_keyHook);
                g_keyHook = nullptr;
            }
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g_keyHook) {
        ::UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }
    if (g_mouseHook) {
        ::UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

}  // namespace mfly
