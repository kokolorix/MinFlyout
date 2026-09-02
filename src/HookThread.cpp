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

/// Extra information of the last button press - pen, touch or mouse.
std::atomic<ULONG_PTR> g_lastDownExtra{0};

/// Modifier keys held at the last button press, see \ref mfly::kModCtrl.
std::atomic<UINT32> g_lastDownMods{kModNone};

/// Screen position of the last button press, packed by \ref PackPoint.
std::atomic<unsigned long long> g_lastDownPoint{0};

/// Screen position of the last button release, x in the low half, y in the high.
std::atomic<unsigned long long> g_lastUpPoint{0};

/// Screen position of the last wheel notch, packed by \ref PackPoint.
std::atomic<unsigned long long> g_lastWheelPoint{0};

/**
 * \brief Packs a point into one atomically readable value.
 *
 * Two separate atomics could be read between two events and yield a point that
 * never existed; one 64-bit word cannot.
 *
 * \param pt Point in screen coordinates.
 * \return The packed value.
 */
unsigned long long PackPoint(POINT pt) {
    return (static_cast<unsigned long long>(static_cast<unsigned int>(pt.x))) |
           (static_cast<unsigned long long>(static_cast<unsigned int>(pt.y)) << 32);
}

/**
 * \brief Unpacks what \ref PackPoint produced.
 * \param value Packed value.
 * \return The point.
 */
POINT UnpackPoint(unsigned long long value) {
    return POINT{static_cast<LONG>(static_cast<int>(value & 0xFFFFFFFFull)),
                 static_cast<LONG>(static_cast<int>((value >> 32) & 0xFFFFFFFFull))};
}

/**
 * \brief Reads the modifier keys currently held down.
 *
 * \c GetAsyncKeyState reads state the window manager already keeps; it sends
 * nothing and waits for nobody, so it is one of the few calls that may appear
 * in a low-level hook callback.
 *
 * \return A combination of \ref mfly::kModCtrl, \ref mfly::kModShift and
 *         \ref mfly::kModAlt.
 */
UINT32 CurrentModifiers() {
    UINT32 mods = kModNone;
    if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
    if (::GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= kModShift;
    if (::GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= kModAlt;
    return mods;
}

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
                // Keep the marker, do not interpret it: whether this was a
                // finger decides nothing here, and the callback sits in the
                // input path.
                if (auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)) {
                    g_lastDownExtra.store(ms->dwExtraInfo, std::memory_order_relaxed);
                    g_lastDownPoint.store(PackPoint(ms->pt), std::memory_order_relaxed);
                }
                // Recorded here rather than asked for later: by the time the
                // message is handled the key may be up again.
                g_lastDownMods.store(CurrentModifiers(), std::memory_order_relaxed);
                ::PostMessageW(owner, WM_MFLY_MOUSEDOWN, 0, 0);
                break;
            case WM_LBUTTONUP:
                if (auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)) {
                    g_lastUpPoint.store(PackPoint(ms->pt), std::memory_order_relaxed);
                }
                ::PostMessageW(owner, WM_MFLY_MOUSEUP, 0, 0);
                break;
            case WM_MOUSEWHEEL:
                // The notch is not swallowed. Deciding that it should be would
                // mean asking the window under the cursor whether that is a
                // border, and asking is exactly what this callback must not do.
                // Over a border there is nothing to scroll anyway.
                if (auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)) {
                    const short delta = static_cast<short>(HIWORD(ms->mouseData));
                    if (delta != 0) {
                        g_lastWheelPoint.store(PackPoint(ms->pt), std::memory_order_relaxed);
                        ::PostMessageW(owner, WM_MFLY_WHEEL,
                                       static_cast<WPARAM>(delta > 0 ? 1 : -1), 0);
                    }
                }
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

ULONG_PTR HookThread::LastDownExtraInfo() {
    return g_lastDownExtra.load(std::memory_order_relaxed);
}

UINT32 HookThread::LastDownModifiers() {
    return g_lastDownMods.load(std::memory_order_relaxed);
}

POINT HookThread::LastDownPoint() {
    return UnpackPoint(g_lastDownPoint.load(std::memory_order_relaxed));
}

POINT HookThread::LastUpPoint() {
    return UnpackPoint(g_lastUpPoint.load(std::memory_order_relaxed));
}

POINT HookThread::LastWheelPoint() {
    return UnpackPoint(g_lastWheelPoint.load(std::memory_order_relaxed));
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
