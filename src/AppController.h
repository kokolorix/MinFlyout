/**
 * \file AppController.h
 * \ingroup app
 * \brief Central state machine of the application.
 */
#pragma once

#include <array>
#include <utility>

#include "CaptionProbe.h"
#include "Common.h"
#include "ConfigWatcher.h"
#include "DragWatcher.h"
#include "FlyoutWindow.h"
#include "HookThread.h"
#include "ZoneOverlay.h"

namespace mfly {

/**
 * \brief Ties together hooks, detection, registry and flyout.
 *
 * The controller owns an invisible window that receives every event as a
 * posted message, and drives the following states from it:
 *
 * ```
 *                 button detected             hover delay elapsed
 *   [Idle] ──────────────────────▶ [Armed] ─────────────────────────▶ [Open]
 *      ▲                              │                                  │
 *      │      mouse leaves button     │                                  │
 *      ├──────────────────────────────┘                                  │
 *      │  click / ESC / grace period elapsed / target window gone        │
 *      └─────────────────────────────────────────────────────────────────┘
 * ```
 *
 * The actual work happens on the UI thread: the hit tests performed by
 * \ref ProbeMinimizeButton must not run on the hook thread (see
 * \ref HookThread).
 *
 * Touch runs a second, independent state machine beside it, because a finger
 * has no hover and the minimize button is far too small for one. There the
 * window itself is the pointer:
 *
 * ```
 *              window picked up by its caption      finger rests in the field
 *   [None] ──────────────────────────────────▶ [Watching] ──────────────────▶ [Zones]
 *      ▲       (touch, resizable, not paused)        │                            │
 *      │                                             │                            │
 *      └─────────────────────────────────────────────┴────────────────────────────┘
 *          window let go, ESC, or the move loop ended - a zone under the finger
 *          at that moment is applied
 * ```
 *
 * The two never run at once: a drag closes the flyout and suppresses the
 * detection until it is over.
 */
class AppController {
public:
    /// Window class name of the invisible controller window.
    static constexpr const wchar_t* kClassName = L"MinFlyout.Controller";

    /**
     * \brief Sets up window, flyout, configuration, providers, tray icon and hooks.
     * \param instance Module instance.
     * \return \c true if everything is up; otherwise \ref Shutdown should be called.
     */
    bool Init(HINSTANCE instance);

    /**
     * \brief Tears everything down again.
     *
     * In particular it restores every foreign window hidden through
     * \ref TrayStash. Safe to call more than once.
     */
    void Shutdown();

    /**
     * \brief Runs the message loop until \c WM_QUIT.
     * \return The exit code from \c WM_QUIT.
     */
    int RunMessageLoop();

    /// \return The process-wide instance.
    static AppController& Instance();

private:
    /// States of the detection.
    enum class State {
        Idle,   ///< Nothing in sight.
        Armed,  ///< Button detected, hover delay running.
        Open,   ///< Flyout is visible.
    };

    /// States of the touch drag.
    enum class DragState {
        None,      ///< No window is being dragged by finger.
        Watching,  ///< Dragging; the trigger field is on screen.
        Zones,     ///< The zones are unfolded and one of them can be dropped on.
    };

    /// IDs of the timers in use.
    enum : UINT_PTR {
        kTimerHover = 1,  ///< Hover delay \ref kHoverDelayMs.
        kTimerGrace = 2,  ///< Grace period \ref kCloseGraceMs before closing.
        kTimerPoll  = 3,  ///< Periodic cleanup check.
        kTimerDwell = 4,  ///< Rest in the touch trigger field before the zones unfold.
        kTimerDrop  = 5,  ///< Grace after the finger lifted, waiting for the move loop to end.
    };

    /**
     * \brief Static window procedure; forwards to \ref HandleMessage.
     * \param hwnd   Window.
     * \param msg    Message.
     * \param wParam First parameter.
     * \param lParam Second parameter.
     * \return Result of the message handling.
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * \brief Per-instance message handling.
     * \param msg    Message.
     * \param wParam First parameter.
     * \param lParam Second parameter.
     * \return Result of the message handling.
     */
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * \brief Evaluates a mouse movement and drives the state machine.
     *
     * Contains the throttling (\ref kProbeThrottle), the cheap pre-test
     * \ref IsOverMinimizeButton and only afterwards the expensive rectangle
     * lookup.
     */
    void HandleMouseMove();

    /// Handles a mouse click: a click outside the flyout cancels it.
    void HandleMouseDown();

    /**
     * \brief Notes how and where the last button press arrived.
     *
     * Run from \ref HandleMouseDown, before anything else: at that moment the
     * pressed window has not entered its move loop yet, so \c WM_NCHITTEST
     * still answers. When \ref WM_MFLY_DRAGSTART follows a moment later it is
     * too late to ask - and \c EVENT_SYSTEM_MOVESIZESTART alone cannot tell a
     * move from a resize. What the answer has to mean is decided in
     * \ref OnDragStart, see \ref IsSizingHitTest.
     *
     * Costs one message per press, and only for a press that could start a
     * touch drag at all: with the feature off, or a mouse press without
     * TouchConfig::alsoMouse, nothing is sent.
     */
    void RememberPress();

    /**
     * \brief A foreign window entered its move/size loop.
     *
     * Decides whether this is a drag worth offering zones for, and puts the
     * trigger field on the screen the pointer is on.
     *
     * \param window The window being dragged.
     */
    void OnDragStart(HWND window);

    /**
     * \brief Follows the finger while a drag is running.
     * \param pt Pointer position in screen coordinates.
     */
    void OnDragMove(POINT pt);

    /**
     * \brief Shows the trigger field on the monitor under a point.
     *
     * Also called again when the finger crosses to another screen, which is how
     * the overlay picks up that monitor's DPI and its own layout.
     *
     * \param pt Pointer position in screen coordinates.
     * \return \c true if a field is on screen afterwards.
     */
    bool ShowTriggerFor(POINT pt);

    /**
     * \brief Ends the touch drag and cleans up.
     * \param applyZone \c true applies the zone under the finger, if there is
     *        one; \c false withdraws the overlay and leaves the window alone.
     */
    void EndDrag(bool applyZone);

    /// Stops the dwell timer if it is running.
    void StopDwell();

    /// Stops the drop grace timer if it is running.
    void StopDropGrace();

    /// Builds the context, collects the items and shows the flyout.
    /**
     * \brief Notes in the log why a window in the caption region was not detected.
     *
     * Written at most once per window and hit-test result, so hovering a title
     * bar does not fill the log. It is what turns "it does not work with this
     * app" into a line that says which class answered what.
     *
     * \param window Top-level window under the cursor.
     * \param pt     Cursor position in screen coordinates.
     */
    void LogDetectionMiss(HWND window, POINT pt);

    /**
     * \brief Diagnoses the window under the cursor and puts the report away.
     *
     * Bound to Ctrl+Alt+F12: point at the button that stays undetected, press
     * the hotkey, and the whole decision chain lands on the clipboard and in
     * \c minflyout-diagnosis.txt next to the configuration. Works in a plain
     * release build too, where the log macros are compiled out.
     */
    void WriteDiagnosis();

    /**
     * \brief Measures a window and puts its zone on the clipboard.
     *
     * Bound to Ctrl+Alt+F11 and to the matching tray menu entry: drag a window
     * to where it belongs, press the hotkey, and the line
     * <code>{ "left": ..., "top": ..., "width": ..., "height": ... },</code>
     * is ready to be pasted into a \c "zones" array. It is the counterpart of
     * \ref mfly::ApplyZone, so what gets pasted reproduces what was measured.
     *
     * \param window Window to measure - the foreground window for the hotkey,
     *        the window that was in front when the tray menu opened for the menu
     *        entry. When that one cannot be measured (our own window, the shell,
     *        minimized) the window under the cursor is tried instead.
     */
    void CaptureZone(HWND window);

    /**
     * \brief Watchdog for the mouse hook, run by the poll timer.
     *
     * Compares the cursor position with the number of movements the hook has
     * reported. Moves without reports mean the hook is gone or the throttle
     * flag is stuck - see \ref HookThread::Revive.
     */
    void CheckHookAlive();

    /**
     * \brief Notes which gate of the detection a mouse movement died at.
     *
     * Only active with \c "traceDetection": \c true, and only one line per
     * change of window or gate - the pre-test runs on every throttled movement.
     *
     * \param window Window under the cursor (may be \c nullptr).
     * \param pt     Cursor position in screen coordinates.
     * \param gate   Static text naming the gate; compared by pointer.
     */
    void TraceGate(HWND window, POINT pt, const wchar_t* gate);

    /// Writes a fresh diagnosis and opens the file in the default editor.
    void OpenDiagnosisFile();

    void OpenFlyout();

    /**
     * \brief Closes the flyout and cleans up timers and keyboard hook.
     * \param suppressUntilLeave \c true suppresses reopening until the mouse
     *        has left the button once.
     */
    void CloseFlyout(bool suppressUntilLeave);

    /**
     * \brief Invokes the action of an item.
     *
     * The flyout is closed first; action and context are cached for that
     * purpose.
     *
     * \param index Index into the items of the flyout.
     */
    void InvokeItem(size_t index);

    /**
     * \brief Moves the target window into the chosen zone.
     *
     * Monitor and zone both come from the hotspot, so one click can move a
     * window to another screen.
     *
     * \param index Index into the hotspots of the flyout.
     */
    void InvokeZone(size_t index);

    /// Shows the context menu of the application tray icon.
    void ShowTrayMenu();

    /**
     * \brief Re-reads the configuration file and reports the result.
     *
     * Reachable through the tray menu, and through \ref WM_MFLY_CONFIG whenever
     * \ref mfly::ConfigWatcher has seen a save. The providers read the
     * configuration afresh on every open, so re-registering them is not
     * necessary.
     */
    void ReloadConfig();

    /**
     * \brief Brings \ref watcher_ in line with Config::watchConfig.
     *
     * Called after every load, so the switch takes effect within the reload it
     * appears in. Starting it twice is a no-op, so the common case - the
     * setting did not change - costs nothing.
     */
    void ApplyWatchSetting();

    /**
     * \brief Shows a balloon tip on the application tray icon.
     * \param title Heading.
     * \param text  Message text.
     * \param error \c true shows the error icon.
     */
    void ShowTrayBalloon(const wchar_t* title, const std::wstring& text, bool error) const;

    /**
     * \brief Turns the detection off or back on.
     * \param paused \c true suspends the detection.
     */
    void SetPaused(bool paused);

    /// Creates the application tray icon (ID 0).
    void AddAppTrayIcon();

    /// Removes the application tray icon.
    void RemoveAppTrayIcon();

    HINSTANCE instance_ = nullptr;  ///< Module instance.
    HWND      hwnd_ = nullptr;      ///< Invisible controller window.
    HICON     appIcon_ = nullptr;   ///< Program icon in window size (Alt-Tab, taskbar).
    HICON     trayIcon_ = nullptr;  ///< Program icon in notification area size.

    HookThread    hooks_;   ///< Low-level hooks on their own thread.
    FlyoutWindow  flyout_;  ///< The popup.
    ConfigWatcher watcher_; ///< Watches the configuration file, if enabled.
    DragWatcher   dragger_; ///< Reports the move/size loop of foreign windows.
    ZoneOverlay   overlay_; ///< The touch drop target.

    DragState dragState_ = DragState::None;  ///< State of the touch drag.
    HWND      dragWindow_ = nullptr;         ///< Window currently being dragged.
    HMONITOR  dragMonitor_ = nullptr;        ///< Monitor the overlay is on.
    bool      dwellRunning_ = false;         ///< \ref kTimerDwell is running.
    bool      dropGraceRunning_ = false;     ///< \ref kTimerDrop is running.
    bool      pressWasContact_ = false;      ///< Last press came from pen or touch.
    LRESULT   pressHitTest_ = HTNOWHERE;     ///< What the window answered at the last press.

    State state_ = State::Idle;       ///< Current state.
    bool  paused_ = false;            ///< Detection paused.
    bool  suppressUntilLeave_ = false;///< No reopening until the mouse leaves the button.
    bool  graceRunning_ = false;      ///< \ref kTimerGrace is running.

    HitInfo      hit_{};              ///< Last detected button along with its window.
    Context      ctx_{};              ///< Context of the open flyout.
    ULONGLONG    lastProbeTick_ = 0;  ///< Time of the last probe (throttling).
    POINT        lastProbePt_{-32000, -32000};  ///< Position of the last probe.

    ProbeMode probeMode_ = ProbeMode::Auto;  ///< Resolved Config::buttonDetection.
    bool hotkeyOk_ = false;             ///< Ctrl+Alt+F12 could be registered.
    bool hotkeyZoneOk_ = false;         ///< Ctrl+Alt+F11 could be registered.

    /**
     * \brief Foreground window at the moment the tray menu was opened.
     *
     * \ref ShowTrayMenu has to call \c SetForegroundWindow on our own window so
     * the menu closes properly, which destroys the answer \ref CaptureZone
     * needs. So it is remembered one line before that happens.
     */
    HWND menuTarget_ = nullptr;

    POINT     watchdogPt_{-32000, -32000};  ///< Cursor position at the last watchdog tick.
    unsigned long long watchdogMoves_ = 0;  ///< HookThread::MovesSeen at that tick.
    int       watchdogStrikes_ = 0;         ///< Consecutive ticks without a report.

    bool     traceDetection_ = false;       ///< Resolved Config::traceDetection.
    HWND     lastTraceWindow_ = nullptr;    ///< Window of the last trace line.
    const wchar_t* lastTraceGate_ = nullptr;///< Gate of the last trace line.

    /// Window/gate pairs already reported once, so the log stays bounded.
    std::array<std::pair<HWND, const wchar_t*>, 16> tracedWindows_{};
    size_t   tracedNext_ = 0;               ///< Next slot in \ref tracedWindows_.
    HWND    lastMissWindow_ = nullptr;  ///< Window of the last logged failed detection.
    LRESULT lastMissHit_ = 0;           ///< Its hit-test code, so the log is written once.
};

}  // namespace mfly
