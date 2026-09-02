/**
 * \file AppController.cpp
 * \ingroup app
 * \brief Implementation of the state machine \ref mfly::AppController.
 */
#include "AppController.h"

#include <algorithm>
#include <cstdlib>

#include <shellapi.h>

#include "Config.h"
#include "Diagnostics.h"
#include "ItemRegistry.h"
#include "Log.h"
#include "Monitors.h"
#include "SettingsBackup.h"
#include "TrayStash.h"
#include "WindowSizer.h"
#include "resource.h"

namespace mfly {
namespace {

constexpr UINT kAppTrayId = 0;       ///< ID of the application tray icon (windows start at 1).
constexpr UINT kMenuPause = 100;     ///< Menu command: pause detection.
constexpr UINT kMenuRestoreAll = 101;///< Menu command: restore stashed windows.
constexpr UINT kMenuOpenConfig = 102;///< Menu command: open configuration.
constexpr UINT kMenuReloadConfig = 103;  ///< Menu command: reload configuration.
constexpr UINT kMenuOpenDiagnosis = 105; ///< Menu command: open the diagnosis file.
constexpr UINT kMenuCopyZone = 106;  ///< Menu command: copy the zone of the active window.
constexpr UINT kMenuBackupConfig = 107;  ///< Menu command: copy the configuration to the share.
constexpr UINT kMenuCompareConfig = 108; ///< Menu command: diff the share against this machine.
constexpr UINT kMenuExit = 104;      ///< Menu command: exit.

/// ID of the diagnosis hotkey (Ctrl+Alt+F12).
constexpr int kHotkeyDiagnose = 1;

/// ID of the zone capture hotkey (Ctrl+Alt+F11).
constexpr int kHotkeyCaptureZone = 2;

/** \name Resize hotkeys
 *
 *  Consecutive IDs, because \ref AppController::resizeHotkeys_ indexes them by
 *  <code>id - kHotkeyWiden</code>. The arrow keys are the obvious home for
 *  "wider" and "taller"; Ctrl+Alt keeps them out of the way of the Windows snap
 *  shortcuts, which live on the Windows key.
 *  @{ */
constexpr int kHotkeyWiden     = 3;  ///< Ctrl+Alt+Right.
constexpr int kHotkeyNarrow    = 4;  ///< Ctrl+Alt+Left.
constexpr int kHotkeyTaller    = 5;  ///< Ctrl+Alt+Down.
constexpr int kHotkeyShorter   = 6;  ///< Ctrl+Alt+Up.
constexpr int kHotkeyFullWidth = 7;  ///< Ctrl+Alt+Shift+Right.
/** @} */

/// The resize hotkeys in the order \ref AppController::resizeHotkeys_ stores them.
struct HotkeySpec {
    int id;             ///< One of the \c kHotkey* IDs above.
    UINT modifiers;     ///< \c MOD_* combination.
    UINT key;           ///< Virtual key code.
    const wchar_t* name;///< How it is written in the log and the README.
};

/// Definition of every resize hotkey, in ID order.
constexpr HotkeySpec kResizeHotkeys[] = {
    {kHotkeyWiden,     MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,             VK_RIGHT, L"Ctrl+Alt+Right"},
    {kHotkeyNarrow,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,             VK_LEFT,  L"Ctrl+Alt+Left"},
    {kHotkeyTaller,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,             VK_DOWN,  L"Ctrl+Alt+Down"},
    {kHotkeyShorter,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,             VK_UP,    L"Ctrl+Alt+Up"},
    {kHotkeyFullWidth, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_RIGHT, L"Ctrl+Alt+Shift+Right"},
};

/**
 * \brief How long a step resize waits for the target's sizing loop to end.
 *
 * A press on a window border puts that window into a modal sizing loop, and a
 * position set while it runs is discarded when it ends. Long enough for the
 * loop to unwind after the button came up, short enough that the step still
 * feels like part of the click.
 */
constexpr UINT kResizeDelayMs = 80;

/// How long a wheel hit test stays valid for the same point.
constexpr ULONGLONG kWheelCacheMs = 400;

/**
 * \brief How far the pointer may have drifted and still be taken along.
 *
 * The question this answers is "is the user still on this edge, or have they
 * moved on?", and \c SM_CXDRAG - four pixels on a standard system - answers a
 * different one. That threshold exists to tell a click from a drag within a few
 * milliseconds; here a whole sizing loop and a short wait lie in between, and a
 * hand resting on a mouse moves further than that without meaning anything by
 * it. Deliberately leaving an edge is a movement of tens of pixels.
 */
constexpr LONG kFollowSlackPx = 16;

/// Poll ticks with cursor movement but no hook report before reviving the hook.
constexpr int kWatchdogStrikes = 3;

/**
 * \brief How long to wait for \c EVENT_SYSTEM_MOVESIZEEND after the finger lifted.
 *
 * The window is still finishing its move loop when the button goes up, and a
 * position set during that loop would be overwritten by it. Long enough for the
 * loop to unwind, short enough that a window whose application never reports
 * the end still lands where it was dropped.
 */
constexpr UINT kDropGraceMs = 250;

/**
 * \brief Reports whether a window is worth measuring for a zone.
 *
 * \ref IsIgnoredWindow already covers our own windows, the shell and everything
 * cloaked or invisible; minimized is added here, because the rectangle of a
 * minimized window says nothing about where the user put it.
 *
 * \param window Candidate window.
 * \return \c true if the window can be measured.
 */
bool IsMeasurable(HWND window) {
    return window && ::IsWindow(window) && !::IsIconic(window) && !IsIgnoredWindow(window);
}

/**
 * \brief Turns a border hit-test code into the edges a step should move.
 *
 * A click on the left or right border changes the width and leaves the centre
 * where it is, which is what "a bit wider" usually means. \p singleEdge - the
 * Alt variant, and what the wheel always does - moves only the edge that was
 * actually pointed at, so the opposite side of the window stays put. A corner
 * moves both of its edges either way.
 *
 * \param code       Answer of \c WM_NCHITTEST.
 * \param singleEdge \c true moves only the edge under the pointer.
 * \return The edges, or \ref SizeEdge::None if the code is not a border.
 */
SizeEdge EdgesForHitTest(LRESULT code, bool singleEdge) {
    switch (code) {
    case HTLEFT:        return singleEdge ? SizeEdge::Left   : SizeEdge::Horizontal;
    case HTRIGHT:       return singleEdge ? SizeEdge::Right  : SizeEdge::Horizontal;
    case HTTOP:         return singleEdge ? SizeEdge::Top    : SizeEdge::Vertical;
    case HTBOTTOM:      return singleEdge ? SizeEdge::Bottom : SizeEdge::Vertical;
    case HTTOPLEFT:     return SizeEdge::Top | SizeEdge::Left;
    case HTTOPRIGHT:    return SizeEdge::Top | SizeEdge::Right;
    case HTBOTTOMLEFT:  return SizeEdge::Bottom | SizeEdge::Left;
    case HTBOTTOMRIGHT: return SizeEdge::Bottom | SizeEdge::Right;
    case HTSIZE:        return SizeEdge::Bottom | SizeEdge::Right;  // the size box
    default:            return SizeEdge::None;
    }
}

}  // namespace

AppController& AppController::Instance() {
    static AppController instance;
    return instance;
}

bool AppController::Init(HINSTANCE instance) {
    instance_ = instance;

    // Pick the matching size from the icon resource: large for Alt-Tab and the
    // taskbar, small for the notification area. LoadImage selects the image
    // variant that belongs to the requested size from the .ico.
    appIcon_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    trayIcon_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppController::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hIcon = appIcon_;
    wc.hIconSm = trayIcon_;
    if (!::RegisterClassExW(&wc)) {
        WRITE_ERROR_LOG(L"RegisterClassEx failed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"MinFlyout",
                              WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, instance, this);
    if (!hwnd_) {
        WRITE_ERROR_LOG(L"CreateWindowEx failed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    if (!flyout_.Create(instance, hwnd_)) {
        WRITE_ERROR_LOG(L"Flyout window could not be created");
        return false;
    }

    // The overlay is optional: without it the flyout still works, so a failure
    // here costs the touch drag and nothing else.
    if (!overlay_.Create(instance)) {
        WRITE_WARNING_LOG(L"Overlay window could not be created, "
                          L"dragging a window into a zone is unavailable",
                          log::dformat(L"error {}", ::GetLastError()));
    }

    TrayStash::Instance().Init(hwnd_);

    ConfigStore::Instance().Reload();  // creates the template on first run
    const Config& config = ConfigStore::Instance().current();
    log::SetFileLogging(config.logToFile);
    ParseProbeMode(config.buttonDetection, probeMode_);
    traceDetection_ = config.traceDetection;
    ApplyWatchSetting();
    if (config.hasError()) {
        WRITE_WARNING_LOG(L"Configuration not usable, running on defaults", config.error);
    } else {
        WRITE_INFO_LOG(log::dformat(L"Configuration loaded, {} layouts", config.layouts.size()),
                       config.path);
    }

    RegisterBuiltinProviders();
    AddAppTrayIcon();

    if (!hooks_.Start(hwnd_)) {
        WRITE_ERROR_LOG(L"Mouse hook could not be installed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    // Same deal as the overlay: nice to have, never a reason not to start.
    if (!dragger_.Start(hwnd_)) {
        WRITE_WARNING_LOG(L"Move/size watcher unavailable, "
                          L"dragging a window into a zone is unavailable");
    }

    ::SetTimer(hwnd_, kTimerPoll, 400, nullptr);

    // Diagnosis hotkey. If something else already owns the combination the
    // application still runs - only the hotkey is then unavailable.
    hotkeyOk_ = ::RegisterHotKey(hwnd_, kHotkeyDiagnose,
                                 MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12) != FALSE;
    if (!hotkeyOk_) {
        WRITE_WARNING_LOG(L"Ctrl+Alt+F12 is taken, the diagnosis hotkey is unavailable",
                          log::dformat(L"error {}", ::GetLastError()));
    }

    // Zone capture, same deal: nice to have, never a reason not to start.
    hotkeyZoneOk_ = ::RegisterHotKey(hwnd_, kHotkeyCaptureZone,
                                     MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != FALSE;
    if (!hotkeyZoneOk_) {
        WRITE_WARNING_LOG(L"Ctrl+Alt+F11 is taken, the zone capture hotkey is unavailable; "
                          L"the tray menu entry keeps working",
                          log::dformat(L"error {}", ::GetLastError()));
    }

    ApplyHotkeySetting();

    // Asked now so the first tray menu already knows the answer, and asked off
    // the UI thread so a share that has gone away costs nothing at startup.
    RefreshBackupStatus();

    WRITE_INFO_LOG(log::dformat(L"MinFlyout started, build {}", BuildStamp()),
                   log::dformat(L"pid {}", ::GetCurrentProcessId()));
    return true;
}

void AppController::ApplyHotkeySetting() {
    const ResizeConfig& resize = ConfigStore::Instance().current().resize;
    const bool wanted = resize.enabled && resize.hotkeys;

    if (!wanted) {
        UnregisterResizeHotkeys();
        return;
    }

    for (const HotkeySpec& spec : kResizeHotkeys) {
        const size_t slot = static_cast<size_t>(spec.id - kHotkeyWiden);
        if (resizeHotkeys_[slot]) continue;  // already ours

        resizeHotkeys_[slot] =
            ::RegisterHotKey(hwnd_, spec.id, spec.modifiers, spec.key) != FALSE;
        if (!resizeHotkeys_[slot]) {
            WRITE_WARNING_LOG(
                log::dformat(L"{} is taken, that resize hotkey is unavailable", spec.name),
                log::dformat(L"error {}", ::GetLastError()));
        }
    }
}

void AppController::UnregisterResizeHotkeys() {
    for (const HotkeySpec& spec : kResizeHotkeys) {
        const size_t slot = static_cast<size_t>(spec.id - kHotkeyWiden);
        if (!resizeHotkeys_[slot]) continue;
        ::UnregisterHotKey(hwnd_, spec.id);
        resizeHotkeys_[slot] = false;
    }
}

void AppController::Shutdown() {
    WRITE_INFO_LOG(L"Shutting down");
    if (hotkeyOk_) {
        ::UnregisterHotKey(hwnd_, kHotkeyDiagnose);
        hotkeyOk_ = false;
    }
    if (hotkeyZoneOk_) {
        ::UnregisterHotKey(hwnd_, kHotkeyCaptureZone);
        hotkeyZoneOk_ = false;
    }
    UnregisterResizeHotkeys();
    EndDrag(/*applyZone=*/false);
    CloseFlyout(false);
    dragger_.Stop();
    hooks_.Stop();
    watcher_.Stop();

    // Hidden foreign windows MUST come back.
    TrayStash::Instance().RestoreAll();
    RemoveAppTrayIcon();
    overlay_.Destroy();
    flyout_.Destroy();

    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }

    // Icons loaded with LoadImage belong to us (no LR_SHARED).
    if (trayIcon_) { ::DestroyIcon(trayIcon_); trayIcon_ = nullptr; }
    if (appIcon_)  { ::DestroyIcon(appIcon_);  appIcon_ = nullptr; }
}

int AppController::RunMessageLoop() {
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// --- State machine ---------------------------------------------------------

void AppController::HandleMouseMove() {
    HookThread::AckMouseMove();  // the next move may be posted again
    if (paused_) return;

    POINT pt{};
    if (!::GetCursorPos(&pt)) return;

    // A window in mid-drag owns the pointer. The button detection would only
    // fight it for the caption travelling underneath, so it stands aside.
    if (dragState_ != DragState::None) {
        OnDragMove(pt);
        return;
    }

    const RECT hotZone = InflateCopy(hit_.buttonRect, 2, 2);

    if (state_ == State::Open) {
        const RECT flyRect = flyout_.ScreenRect();
        // Corridor between button and flyout, so the mouse may travel there.
        RECT bridge{std::min(flyRect.left, hotZone.left),
                    std::min(flyRect.top, hotZone.top),
                    std::max(flyRect.right, hotZone.right),
                    std::max(flyRect.bottom, hotZone.bottom)};

        if (PtInRectPt(flyRect, pt) || PtInRectPt(hotZone, pt) || PtInRectPt(bridge, pt)) {
            if (graceRunning_) {
                ::KillTimer(hwnd_, kTimerGrace);
                graceRunning_ = false;
            }
        } else if (!graceRunning_) {
            ::SetTimer(hwnd_, kTimerGrace,
                       ConfigStore::Instance().current().closeGraceMs, nullptr);
            graceRunning_ = true;
        }
        return;
    }

    if (state_ == State::Armed) {
        if (PtInRectPt(hotZone, pt)) return;  // still on the button: keep waiting
        ::KillTimer(hwnd_, kTimerHover);
        state_ = State::Idle;
    }

    if (suppressUntilLeave_) {
        if (PtInRectPt(hotZone, pt)) return;
        suppressUntilLeave_ = false;
    }

    // Throttling: hit-tests go to foreign windows as a message.
    const ULONGLONG now = ::GetTickCount64();
    if (now - lastProbeTick_ < kProbeThrottle) return;
    if (pt.x == lastProbePt_.x && pt.y == lastProbePt_.y) return;
    lastProbeTick_ = now;
    lastProbePt_ = pt;

    // The hit test comes first and is gated by nothing of ours. It is one
    // message, it is what the detection was originally built on, and it works
    // for every window that answers - including the ones with their own title
    // bar. Putting the computed geometry in front of it made the reliable path
    // depend on the speculative one; it does not any more.
    HWND under = ::WindowFromPoint(pt);
    HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;

    if (!root) {
        TraceGate(nullptr, pt, L"WindowFromPoint found nothing");
        return;
    }
    if (IsIgnoredWindow(root)) {
        TraceGate(root, pt, L"window is on the ignore list");
        return;
    }

    const log::Stopwatch probeTime;
    HitInfo info{};
    if (!ProbeMinimizeButton(pt, probeMode_, info)) {
        // Only complain where a button could plausibly be - otherwise every
        // sweep across the desktop would write a line.
        if (MayBeCaptionButton(root, pt)) {
            LogDetectionMiss(root, pt);
        } else {
            TraceGate(root, pt, L"cursor outside the caption button region");
        }
        return;
    }

    lastMissWindow_ = nullptr;  // the window works, forget the complaint
    hit_ = info;
    state_ = State::Armed;
    WRITE_DEBUG_LOG(log::dformat(L"Minimize button detected {}x{} via {}",
                                 info.buttonRect.right - info.buttonRect.left,
                                 info.buttonRect.bottom - info.buttonRect.top,
                                 ProbeSourceName(info.source)),
                    log::Describe(info.window), probeTime.ElapsedMs());
    ::SetTimer(hwnd_, kTimerHover,
               ConfigStore::Instance().current().hoverDelayMs, nullptr);
}

void AppController::LogDetectionMiss(HWND window, POINT pt) {
    const LRESULT code = HitTestCode(window, pt);
    if (window == lastMissWindow_ && code == lastMissHit_) return;  // already said

    lastMissWindow_ = window;
    lastMissHit_ = code;

    wchar_t cls[64] = {};
    ::GetClassNameW(window, cls, ARRAYSIZE(cls));

    const CaptionLayout layout = CaptionLayoutOf(window);
    RECT frame{};
    VisibleFrame(window, frame);

    RECT dwmBlock{};
    const bool haveBlock =
        DwmWindowAttribute(window, kDwmCaptionButtonBounds, &dwmBlock, sizeof(dwmBlock));

    WRITE_DEBUG_LOG(
        log::dformat(L"In the caption button region, but no button: {} answered NCHITTEST {}",
                     cls, code),
        log::dformat(L"cursor {},{}  frame {},{},{},{}  style {:#x} exstyle {:#x}  slots {}  "
                     L"DWM block {}",
                     pt.x, pt.y, frame.left, frame.top, frame.right, frame.bottom,
                     static_cast<unsigned long long>(::GetWindowLongPtrW(window, GWL_STYLE)),
                     static_cast<unsigned long long>(::GetWindowLongPtrW(window, GWL_EXSTYLE)),
                     layout.slots(),
                     haveBlock ? log::dformat(L"{},{},{},{}", dwmBlock.left, dwmBlock.top,
                                              dwmBlock.right, dwmBlock.bottom)
                               : std::wstring(L"none")));
}

void AppController::HandleMouseDown() {
    // First, because the window pressed has not entered its move loop yet.
    RememberPress();
    HandleBorderPress();

    if (state_ == State::Idle) return;

    POINT pt{};
    ::GetCursorPos(&pt);
    if (state_ == State::Open && PtInRectPt(flyout_.ScreenRect(), pt)) {
        return;  // click on an item - the flyout reports it itself
    }
    // Click on the button itself or anywhere else: cancel.
    if (state_ == State::Armed) ::KillTimer(hwnd_, kTimerHover);
    CloseFlyout(/*suppressUntilLeave=*/true);
}

// --- Touch drag ------------------------------------------------------------

void AppController::RememberPress() {
    pressHitTest_ = HTNOWHERE;
    pressWindow_ = nullptr;
    pressMods_ = HookThread::LastDownModifiers();
    pressWasContact_ = FromPenOrTouch(HookThread::LastDownExtraInfo());
    if (paused_) return;

    // Two features want to know what was pressed, and both want it now: the
    // touch drag, and the border gestures of the step resize. Whether the one
    // message is worth sending is decided by whether anybody is listening -
    // with both switched off, a press costs nothing at all.
    const Config& config = ConfigStore::Instance().current();
    const bool touchWants =
        config.touch.enabled && (pressWasContact_ || config.touch.alsoMouse);
    const bool resizeWants =
        config.resize.enabled &&
        (config.resize.borderModifiers || config.resize.doubleClickMaximizes);
    if (!touchWants && !resizeWants) return;

    // Where the hook saw the press, not where the pointer happens to be now.
    POINT pt = HookThread::LastDownPoint();
    if (pt.x == 0 && pt.y == 0 && !::GetCursorPos(&pt)) return;

    HWND under = ::WindowFromPoint(pt);
    HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    if (!root || IsIgnoredWindow(root)) return;

    // Only the raw answer is kept. What it has to mean is decided in
    // OnDragStart, where it is clear that a move or size loop actually started,
    // and in HandleBorderPress / HandleBorderRelease - here it is just the one
    // moment at which the window still answers.
    pressWindow_ = root;
    pressHitTest_ = HitTestCode(root, pt);
}

// --- Step resizing ---------------------------------------------------------

void AppController::HandleBorderPress() {
    const bool wasDouble = borderDoubleClick_;
    borderDoubleClick_ = false;

    const ResizeConfig& resize = ConfigStore::Instance().current().resize;
    if (paused_ || !resize.enabled || !resize.doubleClickMaximizes) return;

    const POINT pt = HookThread::LastDownPoint();
    const ULONGLONG now = ::GetTickCount64();

    const bool inTime = !wasDouble &&                     // a third click starts over
                        lastClickTick_ != 0 &&
                        now - lastClickTick_ <= ::GetDoubleClickTime();
    const bool inPlace =
        std::abs(pt.x - lastClickPt_.x) <= ::GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        std::abs(pt.y - lastClickPt_.y) <= ::GetSystemMetrics(SM_CYDOUBLECLK) / 2;

    lastClickPt_ = pt;
    lastClickTick_ = now;
    if (!inTime || !inPlace) return;

    // Modifiers mean the other border gesture; a double click is the plain one.
    if (pressMods_ != kModNone) return;
    if (pressHitTest_ != HTLEFT && pressHitTest_ != HTRIGHT) return;
    if (!pressWindow_) return;

    borderDoubleClick_ = true;
    QueueMaximizeHorizontal(pressWindow_);
}

void AppController::HandleBorderRelease() {
    // The double click already claimed this press; its release means nothing.
    if (borderDoubleClick_) return;

    const Config& config = ConfigStore::Instance().current();
    const ResizeConfig& resize = config.resize;
    if (paused_ || !resize.enabled || !resize.borderModifiers) return;
    if (!pressWindow_ || !::IsWindow(pressWindow_)) return;

    // Exactly one of the two: Ctrl grows, Shift shrinks, neither or both means
    // this was not meant for us.
    const bool grow = (pressMods_ & kModCtrl) != 0;
    const bool shrink = (pressMods_ & kModShift) != 0;
    if (grow == shrink) return;

    const SizeEdge edges =
        EdgesForHitTest(pressHitTest_, (pressMods_ & kModAlt) != 0);
    if (edges == SizeEdge::None) return;

    // A press that travelled was a real resize drag. The window is already
    // where the user dragged it to, and adding a step on top of that would
    // undo the aiming they just did.
    const POINT down = HookThread::LastDownPoint();
    const POINT up = HookThread::LastUpPoint();
    if (std::abs(up.x - down.x) > ::GetSystemMetrics(SM_CXDRAG) ||
        std::abs(up.y - down.y) > ::GetSystemMetrics(SM_CYDRAG)) {
        return;
    }

    QueueResize(pressWindow_, grow ? resize.stepPx : -resize.stepPx, edges,
                pressHitTest_, down);
}

bool AppController::FollowEdge(POINT from, LRESULT hitTest, const RECT& edgeShift,
                               POINT& moved) {
    if (!ConfigStore::Instance().current().resize.followEdge) return false;

    const POINT shift = CursorShiftForHitTest(hitTest, edgeShift);
    if (shift.x == 0 && shift.y == 0) return false;

    // Only if the pointer is still about where the gesture started. Between the
    // click and this moment lie the sizing loop and a short wait, and a user who
    // has meanwhile moved on must not have the pointer yanked back.
    POINT now{};
    if (!::GetCursorPos(&now)) return false;
    if (std::abs(now.x - from.x) > kFollowSlackPx ||
        std::abs(now.y - from.y) > kFollowSlackPx) {
        return false;
    }

    // Measured from where the pointer *is*, not from where the gesture began:
    // a hand that drifted two pixels keeps those two pixels instead of being
    // snapped back to a position it has already left.
    moved = POINT{now.x + shift.x, now.y + shift.y};
    if (!::SetCursorPos(moved.x, moved.y)) return false;

    WRITE_DEBUG_LOG(log::dformat(L"Pointer followed the edge by {},{}", shift.x, shift.y));
    return true;
}

void AppController::HandleWheel(int direction) {
    const Config& config = ConfigStore::Instance().current();
    const ResizeConfig& resize = config.resize;
    if (paused_ || !resize.enabled || !resize.wheel) return;
    if (dragState_ != DragState::None) return;

    const POINT pt = HookThread::LastWheelPoint();
    const ULONGLONG now = ::GetTickCount64();

    // The edge that is being moved travels out from under the pointer, so
    // asking again a notch later would answer "client area" - or worse, answer
    // for whatever window has just been uncovered - and the gesture would end
    // after a single notch or continue on the wrong window. So both the answer
    // and the window it came from are kept for as long as the pointer stays
    // put: scrolling on keeps moving the edge that was first pointed at.
    const bool reuse = wheelWindow_ && ::IsWindow(wheelWindow_) &&
                       wheelHit_ != HTNOWHERE &&
                       pt.x == wheelPt_.x && pt.y == wheelPt_.y &&
                       now - wheelTick_ <= kWheelCacheMs;

    if (!reuse) {
        wheelWindow_ = nullptr;
        wheelHit_ = HTNOWHERE;
        wheelPt_ = pt;

        HWND under = ::WindowFromPoint(pt);
        HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
        if (!root || IsIgnoredWindow(root)) return;

        wheelHit_ = HitTestCode(root, pt);
        wheelWindow_ = root;
    }
    wheelTick_ = now;

    const SizeEdge edges = EdgesForHitTest(wheelHit_, /*singleEdge=*/true);
    if (edges == SizeEdge::None) return;

    // No sizing loop is running for a wheel notch, so this needs no delay.
    RECT edgeShift{};
    if (!ResizeWindow(wheelWindow_, direction > 0 ? resize.stepPx : -resize.stepPx,
                      edges, config.useWorkArea, &edgeShift)) {
        return;
    }

    // The pointer rides the edge. Moving it invalidates the cache above, which
    // is keyed on the point - so the new position is written back and the next
    // notch still finds the answer instead of asking a window that may not have
    // finished moving yet.
    POINT moved{};
    if (FollowEdge(pt, wheelHit_, edgeShift, moved)) wheelPt_ = moved;
}

void AppController::QueueResize(HWND window, int step, SizeEdge edges,
                                LRESULT hitTest, POINT origin) {
    // A second click on the same edge before the first has landed adds to it
    // instead of replacing it. Clicking three times quickly has to move the edge
    // three steps - and one write of the window position for the lot of them
    // also means the pointer follows the whole distance in one go, rather than
    // chasing an edge that is still moving.
    if (resizePending_ && !pendingResize_.maximizeH &&
        pendingResize_.window == window && pendingResize_.edges == edges &&
        pendingResize_.hitTest == hitTest &&
        (pendingResize_.step > 0) == (step > 0)) {
        pendingResize_.step += step;
        return;
    }

    // Anything else - another window, the other direction, a different edge -
    // is a separate gesture and must not swallow the one already waiting.
    if (resizePending_) RunPendingResize();

    pendingResize_ = PendingResize{window, step, edges, false, hitTest, origin};
    ::SetTimer(hwnd_, kTimerResize, kResizeDelayMs, nullptr);
    resizePending_ = true;
}

void AppController::QueueMaximizeHorizontal(HWND window) {
    if (resizePending_) RunPendingResize();

    pendingResize_ = PendingResize{window, 0, SizeEdge::None, true, HTNOWHERE, POINT{}};
    ::SetTimer(hwnd_, kTimerResize, kResizeDelayMs, nullptr);
    resizePending_ = true;
}

void AppController::RunPendingResize() {
    if (resizePending_) {
        ::KillTimer(hwnd_, kTimerResize);
        resizePending_ = false;
    }

    const PendingResize job = pendingResize_;
    pendingResize_ = PendingResize{};
    if (!job.window || !::IsWindow(job.window)) return;

    const bool useWorkArea = ConfigStore::Instance().current().useWorkArea;
    const log::Stopwatch actionTime;
    RECT edgeShift{};
    const bool changed =
        job.maximizeH ? MaximizeHorizontally(job.window, useWorkArea)
                      : ResizeWindow(job.window, job.step, job.edges, useWorkArea, &edgeShift);

    // The pointer follows the edge, so the next click needs no aiming. Not for
    // the full-width toggle: that one puts the border at the edge of the screen,
    // and carrying the pointer all the way there would surprise more than the
    // re-aiming it saves.
    if (changed && !job.maximizeH) {
        POINT moved{};
        FollowEdge(job.origin, job.hitTest, edgeShift, moved);
    }

    if (changed) {
        WRITE_INFO_LOG(job.maximizeH
                           ? std::wstring(L"Border gesture: full width")
                           : log::dformat(L"Border gesture: {:+} px on edges {:#x}",
                                          job.step, static_cast<unsigned>(job.edges)),
                       log::Describe(job.window), actionTime.ElapsedMs());
    }
}

void AppController::InvokeTool(size_t index) {
    if (state_ != State::Open) return;
    const std::vector<ResizeCommand>& tools = flyout_.tools();
    if (index >= tools.size()) return;

    const ResizeCommand command = tools[index];
    const Config& config = ConfigStore::Instance().current();

    // The flyout deliberately stays open. Every other click in it is a
    // destination - one zone, one item, done - but a step is a step: nudging a
    // window two hundred pixels wider means pressing the same button twenty
    // times, and reopening the flyout for each of them is not an interface.
    const log::Stopwatch actionTime;
    const bool changed = ApplyResizeCommand(ctx_.targetWindow, command,
                                            config.resize.stepPx, config.useWorkArea);
    WRITE_INFO_LOG(log::dformat(L"Resize button: {}{}", ResizeCommandName(command),
                                changed ? L"" : L" (no change)"),
                   log::Describe(ctx_.targetWindow), actionTime.ElapsedMs());
}

void AppController::OnDragStart(HWND window) {
    if (dragState_ != DragState::None) EndDrag(/*applyZone=*/false);

    const Config& config = ConfigStore::Instance().current();
    if (paused_ || !config.touch.enabled) return;
    if (!pressWasContact_ && !config.touch.alsoMouse) return;

    // MOVESIZESTART brackets sizing just as much as moving. What separates the
    // two is where the press landed, and that was settled before the window
    // disappeared into its modal loop.
    //
    // Asked as a blacklist, not as a whitelist: requiring HTCAPTION would be
    // the obvious form and would exclude every application that draws its own
    // title bar, because such a window answers HTCLIENT there and starts the
    // move loop itself. Explorer is one of them. The sizing codes are the
    // closed list, so everything else is a move.
    if (IsSizingHitTest(pressHitTest_)) return;
    if (!IsMeasurable(window) || !IsResizable(window)) return;

    POINT pt{};
    if (!::GetCursorPos(&pt)) return;

    // The two ways of picking a zone do not share the screen.
    CloseFlyout(/*suppressUntilLeave=*/true);

    dragWindow_ = window;
    dragState_ = DragState::Watching;
    hooks_.SetKeyboardHookEnabled(true);  // ESC withdraws the overlay

    // A screen no layout applies to simply shows no field; the drag stays an
    // ordinary drag there, and moving to a screen that has one still works.
    ShowTriggerFor(pt);
    WRITE_INFO_LOG(L"Touch drag started", log::Describe(window));
}

bool AppController::ShowTriggerFor(POINT pt) {
    const Config& config = ConfigStore::Instance().current();

    const HMONITOR handle = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    const std::vector<MonitorEntry> monitors = EnumerateMonitors();

    // Remembered even when nothing can be shown here, so a screen without a
    // touch layout is not re-examined on every single movement.
    dragMonitor_ = handle;

    for (const MonitorEntry& entry : monitors) {
        if (entry.handle != handle) continue;

        // Touch has its own layouts; an empty section means "the ones the
        // flyout offers", so a configuration that never heard of touch still
        // gets a drop target.
        const std::vector<Layout>& source =
            config.touch.layouts.empty() ? config.layouts : config.touch.layouts;

        const std::vector<Layout> offered = LayoutsForMonitor(source, entry);
        if (!offered.empty() &&
            overlay_.ShowTrigger(entry, offered.front(), config.touch.trigger,
                                 config.useWorkArea)) {
            return true;
        }
        break;
    }

    overlay_.Hide();
    return false;
}

void AppController::OnDragMove(POINT pt) {
    if (dragState_ == DragState::Zones) {
        overlay_.Track(pt);
        return;
    }

    // Following the finger from screen to screen is also how the overlay picks
    // up the other monitor's DPI and the layout configured for it.
    if (::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST) != dragMonitor_) {
        StopDwell();
        ShowTriggerFor(pt);
        return;
    }

    const RectI field = overlay_.triggerRect();
    const bool inside = !field.empty() &&
                        field.contains(static_cast<int>(pt.x), static_cast<int>(pt.y));

    if (inside && !dwellRunning_) {
        // Merely carrying a window across the middle of the screen must not
        // unfold the zones - resting there has to mean something.
        ::SetTimer(hwnd_, kTimerDwell,
                   ConfigStore::Instance().current().touch.dwellMs, nullptr);
        dwellRunning_ = true;
    } else if (!inside && dwellRunning_) {
        StopDwell();
    }
}

void AppController::StopDwell() {
    if (!dwellRunning_) return;
    ::KillTimer(hwnd_, kTimerDwell);
    dwellRunning_ = false;
}

void AppController::StopDropGrace() {
    if (!dropGraceRunning_) return;
    ::KillTimer(hwnd_, kTimerDrop);
    dropGraceRunning_ = false;
}

void AppController::EndDrag(bool applyZone) {
    if (dragState_ == DragState::None) return;

    // Read everything out before the overlay forgets it.
    const int hot = overlay_.hot();
    const HWND window = dragWindow_;
    const HMONITOR monitor = overlay_.monitor();
    const std::wstring layoutName = overlay_.layoutName();
    const Zone zone = hot >= 0 ? overlay_.zoneAt(static_cast<size_t>(hot)) : Zone{};
    const bool useWorkArea = ConfigStore::Instance().current().useWorkArea;

    StopDwell();
    StopDropGrace();
    overlay_.Hide();
    dragState_ = DragState::None;
    dragWindow_ = nullptr;
    dragMonitor_ = nullptr;

    // The flyout may have opened again in the meantime; leave its hook alone.
    hooks_.SetKeyboardHookEnabled(state_ == State::Open);

    if (!applyZone || hot < 0 || !window || !::IsWindow(window)) {
        WRITE_DEBUG_LOG(L"Touch drag ended without a zone");
        return;
    }

    const log::Stopwatch actionTime;
    ApplyZone(window, zone, monitor, useWorkArea);
    WRITE_INFO_LOG(log::dformat(L"Window dropped into zone {} of '{}'", hot, layoutName),
                   log::Describe(window), actionTime.ElapsedMs());
}

void AppController::WriteDiagnosis() {
    const std::wstring report = DiagnoseWindowUnderCursor();

    std::wstring path;
    const bool wrote = AppendDiagnosisFile(report, path);
    const bool copied = CopyToClipboard(report);

    // The balloon has to work in a plain release build too, so it says what
    // happened instead of relying on the log.
    std::wstring text;
    text += copied ? L"Copied to the clipboard." : L"Clipboard was not available.";
    if (wrote) {
        text += L"\nAppended to ";
        text += path;
    } else if (!path.empty()) {
        text += L"\nCould not write ";
        text += path;
    }
    ShowTrayBalloon(L"MinFlyout diagnosis", text, !copied && !wrote);

    WRITE_INFO_LOG(L"Diagnosis written", report);
}

void AppController::CaptureZone(HWND window) {
    // The foreground window is the one the user just arranged. If it is not
    // measurable - our own flyout has the focus, the shell does, the window is
    // minimized - fall back to whatever the cursor points at, which is what the
    // diagnosis hotkey uses and needs no click beforehand.
    if (!IsMeasurable(window)) {
        POINT pt{};
        ::GetCursorPos(&pt);
        HWND under = ::WindowFromPoint(pt);
        window = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    }

    Zone zone{};
    const bool measured =
        IsMeasurable(window) &&
        ZoneFromWindow(window, ConfigStore::Instance().current().useWorkArea, zone);
    if (!measured) {
        ShowTrayBalloon(L"MinFlyout",
                        L"No window to measure. Bring the window you want to capture "
                        L"to the front - a minimized one cannot be measured.", true);
        WRITE_WARNING_LOG(L"Zone capture found no measurable window");
        return;
    }

    const std::wstring line = FormatZoneEntry(zone);

    // The balloon shows the line without its indentation and line break, so the
    // result is visible without opening the configuration first.
    std::wstring shown = line;
    while (!shown.empty() && (shown.back() == L'\r' || shown.back() == L'\n')) shown.pop_back();
    const size_t first = shown.find_first_not_of(L' ');
    if (first != std::wstring::npos) shown.erase(0, first);

    if (CopyToClipboard(line)) {
        ShowTrayBalloon(L"Zone copied", shown + L"\nPaste it into a \"zones\" array.", false);
    } else {
        // Without the clipboard the numbers would be lost, so the balloon is
        // the only place left to put them.
        ShowTrayBalloon(L"Zone (clipboard unavailable)", shown, true);
    }

    WRITE_INFO_LOG(log::dformat(L"Zone captured: {}", shown), log::Describe(window));
}

void AppController::OpenDiagnosisFile() {
    // Writing an entry first guarantees the file exists and shows the current
    // state even if the hotkey has never been pressed.
    std::wstring path;
    if (!AppendDiagnosisFile(DiagnoseWindowUnderCursor(), path) || path.empty()) {
        ShowTrayBalloon(L"MinFlyout", L"The diagnosis file could not be written.", true);
        return;
    }
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppController::TraceGate(HWND window, POINT pt, const wchar_t* gate) {
    // Without the trace flag every window still gets one line the first time it
    // is rejected. That is bounded by the number of windows on the desktop and
    // is what answers "why does this app never even get probed?" without
    // anybody having to switch anything on first.
    if (!traceDetection_) {
        for (const auto& seen : tracedWindows_) {
            if (seen.first == window && seen.second == gate) return;
        }
        tracedWindows_[tracedNext_] = {window, gate};
        tracedNext_ = (tracedNext_ + 1) % tracedWindows_.size();
    } else {
        // One line per change, not per mouse movement - otherwise a single
        // sweep across the desktop buries everything else.
        if (window == lastTraceWindow_ && gate == lastTraceGate_) return;
        lastTraceWindow_ = window;
        lastTraceGate_ = gate;
    }

    wchar_t cls[64] = L"-";
    if (window) ::GetClassNameW(window, cls, ARRAYSIZE(cls));

    const CaptionLayout layout = CaptionLayoutOf(window);
    RECT frame{};
    VisibleFrame(window, frame);

    WRITE_DEBUG_LOG(log::dformat(L"Trace: {}  ({})", gate, cls),
                    log::dformat(L"cursor {},{}  frame {},{},{},{}  slots {}",
                                 pt.x, pt.y, frame.left, frame.top,
                                 frame.right, frame.bottom, layout.slots()));
}

void AppController::CheckHookAlive() {
    POINT pt{};
    if (!::GetCursorPos(&pt)) return;

    const bool cursorMoved = (pt.x != watchdogPt_.x || pt.y != watchdogPt_.y);
    watchdogPt_ = pt;

    const unsigned long long seen = HookThread::MovesSeen();
    const bool hookFired = (seen != watchdogMoves_);
    watchdogMoves_ = seen;

    // The cursor moved but the hook did not report it: either Windows removed
    // the hook after a LowLevelHooksTimeout - which is what a debugger
    // breakpoint does to any process - or the throttle flag is stuck because a
    // posted message went missing. Both are silent and both are permanent
    // without this.
    if (!cursorMoved || hookFired || paused_) {
        watchdogStrikes_ = 0;
        return;
    }

    if (++watchdogStrikes_ < kWatchdogStrikes) return;
    watchdogStrikes_ = 0;

    WRITE_WARNING_LOG(
        L"Cursor moves but the hook reports nothing - reviving",
        log::dformat(L"moves seen {}, posted {}, pending {}, installed {}",
                     HookThread::MovesSeen(), HookThread::MovesPosted(),
                     HookThread::MovePending() ? 1 : 0,
                     HookThread::Installed() ? 1 : 0));
    hooks_.Revive();
}

void AppController::OpenFlyout() {
    if (!::IsWindow(hit_.window) || ::IsIconic(hit_.window)) {
        state_ = State::Idle;
        return;
    }

    POINT pt{};
    ::GetCursorPos(&pt);
    if (!PtInRectPt(InflateCopy(hit_.buttonRect, 2, 2), pt)) {
        state_ = State::Idle;
        return;
    }

    ctx_ = Context{};
    ctx_.targetWindow = hit_.window;
    ctx_.targetThreadId = ::GetWindowThreadProcessId(hit_.window, &ctx_.targetProcessId);
    ctx_.buttonRect = hit_.buttonRect;
    ctx_.windowRect = hit_.windowRect;
    // The screen the flyout will appear on, not the DPI the target window
    // thinks in - an application that is not per-monitor aware reports 96 on a
    // 175 % display, and the flyout would come out a third of its proper size
    // beside it. The anchor decides, because that is what FlyoutWindow::Show
    // positions against on a mixed-DPI desktop.
    const POINT anchorCentre{(hit_.buttonRect.left + hit_.buttonRect.right) / 2,
                             (hit_.buttonRect.top + hit_.buttonRect.bottom) / 2};
    ctx_.dpi = DpiForPoint(anchorCentre);

    const Config& config = ConfigStore::Instance().current();
    const log::Stopwatch collectTime;

    FlyoutContent content;
    content.useWorkArea = config.useWorkArea;

    // A window without a sizing border cannot be moved into a zone - it gets
    // the text items only, so the monitor rows are not built at all.
    const std::vector<MonitorEntry> monitors = EnumerateMonitors();
    const size_t current = IndexOfMonitorFor(monitors, hit_.window);
    size_t rowOfCurrent = monitors.size();  // "not among the rows"

    // Same condition, same reason: without a sizing border there is nothing for
    // the resize buttons to do either.
    if (config.resize.enabled && config.resize.toolbar && IsResizable(hit_.window)) {
        content.tools.assign(std::begin(kResizeCommands), std::end(kResizeCommands));
    }

    if (IsResizable(hit_.window)) {
        for (size_t m = 0; m < monitors.size(); ++m) {
            // Which screens to offer: all of them, or only the one the window
            // is on. And of those, only the ones a layout applies to.
            if (!config.showAllMonitors && m != current) continue;

            MonitorRow row;
            row.monitor = monitors[m];
            row.layouts = LayoutsForMonitor(config.layouts, monitors[m]);
            if (row.layouts.empty()) continue;

            if (m == current) rowOfCurrent = content.rows.size();
            content.rows.push_back(std::move(row));
        }
    }
    content.currentRow = std::min(rowOfCurrent, content.rows.size());

    ItemList list = Registry::Instance().Collect(ctx_);
    content.items = std::move(list.items());

    if (content.items.empty() && content.rows.empty() && content.tools.empty()) {
        WRITE_WARNING_LOG(L"Nothing to show", log::Describe(hit_.window));
        state_ = State::Idle;
        return;
    }

    const size_t itemCount = content.items.size();
    const size_t rowCount = content.rows.size();
    // ctx_.dpi stays the real DPI - the providers and the actions mean the
    // screen by it. Only the layout is enlarged.
    flyout_.Show(std::move(content), hit_.buttonRect, ctx_.dpi, config.uiScale);
    state_ = State::Open;
    hooks_.SetKeyboardHookEnabled(true);
    WRITE_INFO_LOG(log::dformat(L"Flyout opened: {} zones on {} of {} monitors, {} items, {} dpi",
                                flyout_.hotspots().size(), rowCount, monitors.size(),
                                itemCount, ctx_.dpi),
                   log::Describe(ctx_.targetWindow), collectTime.ElapsedMs());
}

void AppController::CloseFlyout(bool suppressUntilLeave) {
    if (graceRunning_) {
        ::KillTimer(hwnd_, kTimerGrace);
        graceRunning_ = false;
    }
    if (state_ == State::Armed) ::KillTimer(hwnd_, kTimerHover);

    flyout_.Hide();
    hooks_.SetKeyboardHookEnabled(false);
    state_ = State::Idle;
    suppressUntilLeave_ = suppressUntilLeave;
}

void AppController::InvokeItem(size_t index) {
    if (state_ != State::Open) return;
    const std::vector<Item>& items = flyout_.items();
    if (index >= items.size()) return;

    // Copy: the flyout is closed immediately afterwards.
    std::function<void(const Context&)> action = items[index].action;
    const std::wstring label = items[index].text;
    const Context ctx = ctx_;

    CloseFlyout(/*suppressUntilLeave=*/true);
    if (!action) return;

    const log::Stopwatch actionTime;
    action(ctx);
    WRITE_INFO_LOG(log::dformat(L"Item invoked: {}", label),
                   log::Describe(ctx.targetWindow), actionTime.ElapsedMs());
}

void AppController::InvokeZone(size_t index) {
    if (state_ != State::Open) return;
    const std::vector<ZoneHotspot>& spots = flyout_.hotspots();
    if (index >= spots.size()) return;

    const ZoneHotspot spot = spots[index];
    const Context ctx = ctx_;
    const bool useWorkArea = ConfigStore::Instance().current().useWorkArea;

    CloseFlyout(/*suppressUntilLeave=*/true);

    const log::Stopwatch actionTime;
    ApplyZone(ctx.targetWindow, spot.zone, spot.monitor, useWorkArea);
    WRITE_INFO_LOG(log::dformat(L"Zone invoked: {:.0f},{:.0f} {:.0f}x{:.0f} %",
                                spot.zone.left, spot.zone.top,
                                spot.zone.width, spot.zone.height),
                   log::Describe(ctx.targetWindow), actionTime.ElapsedMs());
}

// --- Tray icon of the application ------------------------------------------

void AppController::AddAppTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_MFLY_TRAY;
    nid.hIcon = trayIcon_ ? trayIcon_ : ::LoadIconW(nullptr, IDI_APPLICATION);
    // The tooltip is the only place the double click and the hotkey can be
    // discovered - and the build stamp answers "am I running what I just built?"
    // without opening a single file.
    wchar_t tip[160] = {};
    ::swprintf(tip, ARRAYSIZE(tip),
               L"MinFlyout  %s\nDouble click reloads the configuration"
               L"\nCtrl+Alt+F12 diagnose  Ctrl+Alt+F11 copy zone",
               BuildStamp());
    ::lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    ::Shell_NotifyIconW(NIM_ADD, &nid);
}

void AppController::RemoveAppTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AppController::SetPaused(bool paused) {
    paused_ = paused;
    hooks_.SetPaused(paused);
    if (paused_) {
        EndDrag(/*applyZone=*/false);
        CloseFlyout(false);
    }
    WRITE_INFO_LOG(paused ? L"Detection paused" : L"Detection resumed");
}

void AppController::ReloadConfig() {
    const bool ok = ConfigStore::Instance().Reload();
    const Config& config = ConfigStore::Instance().current();
    log::SetFileLogging(config.logToFile);
    probeMode_ = ProbeMode::Auto;
    ParseProbeMode(config.buttonDetection, probeMode_);
    traceDetection_ = config.traceDetection;
    ApplyWatchSetting();
    ApplyHotkeySetting();

    if (ok) {
        wchar_t text[128] = {};
        ::swprintf(text, ARRAYSIZE(text), L"Loaded %zu layouts.",
                   config.layouts.size());
        ShowTrayBalloon(L"MinFlyout", text, false);
        WRITE_INFO_LOG(log::dformat(L"Configuration reloaded, {} layouts",
                                    config.layouts.size()), config.path);
    } else {
        ShowTrayBalloon(L"Configuration error", config.error, true);
        WRITE_WARNING_LOG(L"Configuration reload failed", config.error);
    }
}

void AppController::ApplyWatchSetting() {
    const Config& config = ConfigStore::Instance().current();

    if (!config.watchConfig) {
        watcher_.Stop();
        return;
    }
    if (watcher_.watching()) return;

    // config.path is empty only when %APPDATA% could not be resolved - then
    // there is nothing to watch and the error is already in Config::error.
    if (!config.path.empty() && !watcher_.Start(hwnd_, config.path)) {
        WRITE_WARNING_LOG(L"Configuration file is not being watched, "
                          L"use the tray menu to reload", config.path);
    }
}

void AppController::ShowTrayBalloon(const wchar_t* title, const std::wstring& text,
                                    bool error) const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    ::lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    ::lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppController::ShowTrayMenu() {
    // Before anything else: SetForegroundWindow below makes our own window the
    // foreground one, and "Copy zone of the active window" would then measure
    // an invisible 0x0 window instead of the one the user means.
    menuTarget_ = ::GetForegroundWindow();

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kMenuPause, L"Paused");
    ::AppendMenuW(menu, MF_STRING | (TrayStash::Instance().empty() ? MF_GRAYED : 0),
                  kMenuRestoreAll, L"Restore all stashed windows");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Open configuration");
    ::AppendMenuW(menu, MF_STRING, kMenuReloadConfig, L"Reload configuration");
    ::AppendMenuW(menu, MF_STRING, kMenuCopyZone,
                  L"Copy zone of the active window\t Ctrl+Alt+F11");

    // Only where they can work. The answer comes from the last probe and is
    // never asked for here - the share is a network drive, and a menu that
    // waits for one to answer is a menu that hangs. See SettingsBackup.h.
    const BackupAvailability backup = BackupStatus();
    if (backup.folder) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        const std::wstring file = BackupFileName();
        std::wstring label = L"Back up configuration";
        if (!file.empty()) label += L" as " + file;
        ::AppendMenuW(menu, MF_STRING, kMenuBackupConfig, label.c_str());

        if (backup.comparer) {
            ::AppendMenuW(menu, MF_STRING, kMenuCompareConfig,
                          L"Compare with the backup in Beyond Compare");
        }
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuOpenDiagnosis,
                  L"Open diagnosis file\t Ctrl+Alt+F12 writes it");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    // Drawn in bold, and it is what a double click on the icon triggers.
    ::SetMenuDefaultItem(menu, kMenuOpenConfig, FALSE);

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(hwnd_);  // so that the menu closes correctly
    ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    // For the next time the menu is opened. A share mounted while the
    // application runs therefore shows up on the second open rather than
    // needing a restart, and nobody waits for the drive to answer.
    RefreshBackupStatus();
}

void AppController::BackupConfiguration() {
    std::wstring target;
    std::wstring error;
    if (SaveConfigurationBackup(target, error)) {
        ShowTrayBalloon(L"Configuration backed up", target, false);
    } else {
        ShowTrayBalloon(L"Backup failed", error, true);
    }
}

void AppController::CompareConfiguration() {
    std::wstring error;
    if (!CompareConfigurationFolders(error)) {
        ShowTrayBalloon(L"Comparison failed", error, true);
    }
}

// --- Window procedure ------------------------------------------------------

LRESULT CALLBACK AppController::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<AppController*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_NCCREATE) self->hwnd_ = hwnd;
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT AppController::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MFLY_MOUSEMOVE:
        HandleMouseMove();
        return 0;

    case WM_MFLY_MOUSEDOWN:
        HandleMouseDown();
        return 0;

    case WM_MFLY_MOUSEUP:
        // The finger has left the glass, but the window is still finishing its
        // move loop and would overwrite anything positioned now. Wait for
        // MOVESIZEEND, and give up on it after a short grace - an app that
        // never reports it should not cost the user the drop.
        if (dragState_ != DragState::None && !dropGraceRunning_) {
            ::SetTimer(hwnd_, kTimerDrop, kDropGraceMs, nullptr);
            dropGraceRunning_ = true;
        }
        HandleBorderRelease();
        return 0;

    case WM_MFLY_WHEEL:
        HandleWheel(static_cast<int>(static_cast<INT_PTR>(wParam)));
        return 0;

    case WM_MFLY_DRAGSTART:
        OnDragStart(reinterpret_cast<HWND>(lParam));
        return 0;

    case WM_MFLY_DRAGEND:
        if (dragState_ != DragState::None &&
            reinterpret_cast<HWND>(lParam) == dragWindow_) {
            EndDrag(/*applyZone=*/true);
        }
        return 0;

    case WM_MFLY_CANCEL:
        if (dragState_ != DragState::None) {
            EndDrag(/*applyZone=*/false);
        } else {
            CloseFlyout(/*suppressUntilLeave=*/true);
        }
        return 0;

    case WM_MFLY_INVOKE:
        InvokeItem(static_cast<size_t>(wParam));
        return 0;

    case WM_MFLY_ZONE:
        InvokeZone(static_cast<size_t>(wParam));
        return 0;

    case WM_MFLY_TOOL:
        InvokeTool(static_cast<size_t>(wParam));
        return 0;

    case WM_MFLY_CLOSED:
        CloseFlyout(false);
        return 0;

    case WM_MFLY_CONFIG:
        // The watcher has debounced the save already; this is one message per
        // save, not one per write.
        ReloadConfig();
        return 0;

    case WM_MFLY_TRAY:
        if (static_cast<UINT>(wParam) == kAppTrayId) {
            const UINT event = static_cast<UINT>(LOWORD(lParam));
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                ShowTrayMenu();
            } else if (event == WM_LBUTTONDBLCLK) {
                // Double click is the default action of the icon - the same
                // command the menu marks as default.
                ConfigStore::Instance().OpenInEditor();
            }
        } else {
            TrayStash::Instance().OnTrayMessage(wParam, lParam);
        }
        return 0;

    case WM_HOTKEY: {
        // The resize hotkeys act on the foreground window: it is the one the
        // user is working in, and unlike the flyout they are not aimed at
        // anything with the pointer.
        const Config& config = ConfigStore::Instance().current();
        const int step = config.resize.stepPx;
        HWND front = ::GetForegroundWindow();

        switch (static_cast<int>(wParam)) {
        case kHotkeyDiagnose:    WriteDiagnosis(); return 0;
        case kHotkeyCaptureZone: CaptureZone(front); return 0;
        case kHotkeyWiden:
            ResizeWindow(front, step, SizeEdge::Horizontal, config.useWorkArea);
            return 0;
        case kHotkeyNarrow:
            ResizeWindow(front, -step, SizeEdge::Horizontal, config.useWorkArea);
            return 0;
        case kHotkeyTaller:
            ResizeWindow(front, step, SizeEdge::Vertical, config.useWorkArea);
            return 0;
        case kHotkeyShorter:
            ResizeWindow(front, -step, SizeEdge::Vertical, config.useWorkArea);
            return 0;
        case kHotkeyFullWidth:
            MaximizeHorizontally(front, config.useWorkArea);
            return 0;
        default: break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuPause:        SetPaused(!paused_); return 0;
        case kMenuRestoreAll:   TrayStash::Instance().RestoreAll(); return 0;
        case kMenuOpenConfig:   ConfigStore::Instance().OpenInEditor(); return 0;
        case kMenuReloadConfig: ReloadConfig(); return 0;
        case kMenuOpenDiagnosis: OpenDiagnosisFile(); return 0;
        case kMenuCopyZone:     CaptureZone(menuTarget_); return 0;
        case kMenuBackupConfig: BackupConfiguration(); return 0;
        case kMenuCompareConfig: CompareConfiguration(); return 0;
        case kMenuExit:         ::PostQuitMessage(0); return 0;
        default: break;
        }
        break;

    case WM_TIMER:
        switch (wParam) {
        case kTimerHover:
            ::KillTimer(hwnd_, kTimerHover);
            if (state_ == State::Armed) OpenFlyout();
            return 0;
        case kTimerGrace:
            ::KillTimer(hwnd_, kTimerGrace);
            graceRunning_ = false;
            CloseFlyout(false);
            return 0;
        case kTimerDwell:
            StopDwell();
            if (dragState_ == DragState::Watching) {
                overlay_.SwitchToZones();
                if (overlay_.phase() == ZoneOverlay::Phase::Zones) {
                    dragState_ = DragState::Zones;
                    POINT pt{};
                    if (::GetCursorPos(&pt)) overlay_.Track(pt);
                }
            }
            return 0;

        case kTimerResize:
            // The sizing loop the press started has had its moment to unwind.
            RunPendingResize();
            return 0;

        case kTimerDrop:
            // MOVESIZEEND did not come. The button is up, so the move loop is
            // over one way or another - apply what the finger was over.
            StopDropGrace();
            WRITE_WARNING_LOG(L"Drag ended without MOVESIZEEND",
                              log::Describe(dragWindow_));
            EndDrag(/*applyZone=*/true);
            return 0;

        case kTimerPoll:
            TrayStash::Instance().DropDeadWindows();
            ForgetDeadResizeState();
            CheckHookAlive();
            if (state_ == State::Open &&
                (!::IsWindow(hit_.window) || ::IsIconic(hit_.window))) {
                CloseFlyout(false);
            }
            // Nothing may leave a full-screen overlay behind: if no button is
            // down and no drop is pending, there is no drag left to draw for.
            if (dragState_ != DragState::None && !dropGraceRunning_ &&
                (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                WRITE_WARNING_LOG(L"Touch drag lost, overlay withdrawn",
                                  log::Describe(dragWindow_));
                EndDrag(/*applyZone=*/false);
            }
            return 0;
        default:
            break;
        }
        break;

    case WM_QUERYENDSESSION:
        TrayStash::Instance().RestoreAll();
        return TRUE;

    case WM_ENDSESSION:
        TrayStash::Instance().RestoreAll();
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace mfly
