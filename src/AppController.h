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
#include "FlyoutWindow.h"
#include "HookThread.h"

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

    /// IDs of the timers in use.
    enum : UINT_PTR {
        kTimerHover = 1,  ///< Hover delay \ref kHoverDelayMs.
        kTimerGrace = 2,  ///< Grace period \ref kCloseGraceMs before closing.
        kTimerPoll  = 3,  ///< Periodic cleanup check.
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
