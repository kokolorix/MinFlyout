/**
 * \file HookThread.h
 * \ingroup detect
 * \brief Low-level mouse and keyboard hook on a dedicated thread.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Owns the low-level hooks and forwards their events.
 *
 * \c WH_MOUSE_LL callbacks are processed synchronously inside the system's
 * input pipeline. If the thread owning the hook stalls - for instance because
 * it queries a foreign window through \c SendMessageTimeout - the mouse stutters
 * system-wide and Windows disables the hook after \c LowLevelHooksTimeout.
 *
 * The hooks therefore run on a dedicated thread with its own message loop, and
 * the callbacks do nothing but \c PostMessage to the controller window. All
 * evaluation happens on the UI thread of the \ref AppController.
 *
 * \see AppController
 */
class HookThread {
public:
    /**
     * \brief Starts the hook thread and installs the mouse hook.
     *
     * Blocks until the hook is up (at most five seconds).
     *
     * \param owner Window that receives \ref WM_MFLY_MOUSEMOVE, \ref WM_MFLY_MOUSEDOWN,
     *              \ref WM_MFLY_MOUSEUP and \ref WM_MFLY_CANCEL.
     * \return \c true if the hook could be installed.
     */
    bool Start(HWND owner);

    /**
     * \brief Stops the hook thread and removes all hooks.
     *
     * Safe to call more than once; a no-op without a preceding \ref Start.
     */
    void Stop();

    /**
     * \brief Turns the keyboard hook on or off.
     *
     * It is only active while a flyout is open; then it swallows ESC and reports
     * \ref WM_MFLY_CANCEL instead. Outside of that no keyboard hook is
     * installed.
     *
     * \param enabled \c true installs the hook.
     */
    void SetKeyboardHookEnabled(bool enabled);

    /**
     * \brief Suspends the evaluation of mouse events.
     *
     * The hook stays installed but no longer posts anything.
     *
     * \param paused \c true goes silent.
     */
    void SetPaused(bool paused);

    /**
     * \brief Acknowledges the processing of a posted mouse movement.
     *
     * Throttling: at most one movement message is in the queue at a time. The
     * hook only posts again after this acknowledgement.
     */
    static void AckMouseMove();

    /**
     * \brief Puts the mouse hook back and clears the throttle flag.
     *
     * Two things can silence the detection without any error being visible.
     * Windows removes a low-level hook whose owner did not answer within
     * \c LowLevelHooksTimeout - which happens to every process that sits at a
     * debugger breakpoint. And the one-message-at-a-time throttle would stay
     * armed forever if its message were ever lost. The watchdog in
     * \ref AppController calls this when the cursor moves but nothing arrives.
     */
    void Revive();

    /// \return Mouse movements the callback has seen since the start.
    static unsigned long long MovesSeen();

    /// \return Movements that were actually posted to the controller.
    static unsigned long long MovesPosted();

    /// \return \c true while a posted movement has not been acknowledged yet.
    static bool MovePending();

    /// \return \c true if the mouse hook is currently installed.
    static bool Installed();

    /**
     * \brief Reports how the last button press reached the system.
     *
     * The callback keeps the extra information of the event instead of
     * evaluating it, so the decision "was that a finger?" can be taken on the
     * UI thread where it belongs. Read it while handling
     * \ref WM_MFLY_MOUSEDOWN, or when \ref WM_MFLY_DRAGSTART arrives - the
     * press that started a drag is always the last one seen.
     *
     * \return \c MSLLHOOKSTRUCT::dwExtraInfo of the last button press, or 0.
     * \see FromPenOrTouch, FromTouch
     */
    static ULONG_PTR LastDownExtraInfo();

    /**
     * \brief Position of the last button release.
     *
     * \ref WM_MFLY_DRAGEND can arrive a moment after the finger has left the
     * glass, and \c GetCursorPos would then report where the pointer ended up
     * rather than where the window was dropped. The hook has the right point,
     * so it keeps it.
     *
     * \return The release point in screen coordinates.
     */
    static POINT LastUpPoint();

private:
    /**
     * \brief Thread entry point, calls \ref Run.
     * \param param Pointer to the \ref HookThread instance.
     * \return Always 0.
     */
    static DWORD WINAPI ThreadMain(LPVOID param);

    /// Installs the hooks and runs the message loop of the thread.
    void Run();

    HANDLE thread_ = nullptr;    ///< Handle of the hook thread.
    DWORD  threadId_ = 0;        ///< Thread ID for \c PostThreadMessage.
    HANDLE ready_ = nullptr;     ///< Signaled as soon as the mouse hook is up.
};

}  // namespace mfly
