/**
 * \file DragWatcher.h
 * \ingroup detect
 * \brief Notices when a foreign window is being moved or resized.
 *
 * Dragging a window with the finger is the touch counterpart of hovering the
 * minimize button: it is the moment at which the user has already said "this
 * window is going somewhere". Catching it needs neither a hook in the input
 * path nor anything loaded into the foreign process - Windows announces it.
 *
 * \c EVENT_SYSTEM_MOVESIZESTART and \c EVENT_SYSTEM_MOVESIZEEND bracket the
 * modal loop \c DefWindowProc runs while a window is dragged by its caption or
 * sized by its border. Registered \c WINEVENT_OUTOFCONTEXT, the callback runs
 * on our own thread while it dispatches messages - no DLL is mapped into the
 * dragged application, which is the same promise \ref mfly::CaptionProbe makes.
 *
 * The watcher only reports. Whether the drag came from a finger, whether the
 * window may be tiled at all and what to draw is decided by
 * \ref mfly::AppController, on the same thread, out of the input path.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief WinEvent hook for the move/size loop of foreign windows.
 *
 * \ref Start must be called on the thread that runs the message loop, because
 * an out-of-context WinEvent hook delivers its callbacks there.
 */
class DragWatcher {
public:
    /**
     * \brief Installs the hook.
     * \param owner Window that receives \ref WM_MFLY_DRAGSTART and
     *        \ref WM_MFLY_DRAGEND; the dragged window travels in \c lParam.
     * \return \c true if the hook is in place.
     */
    bool Start(HWND owner);

    /// Removes the hook; safe to call more than once.
    void Stop();

    /// \return \c true if the hook is installed.
    bool Installed() const { return hook_ != nullptr; }

private:
    /**
     * \brief Callback of the WinEvent hook; posts and returns.
     * \param hook     The hook handle.
     * \param event    \c EVENT_SYSTEM_MOVESIZESTART or \c ..._MOVESIZEEND.
     * \param window   Window the event is about.
     * \param object   Object ID; only \c OBJID_WINDOW is of interest.
     * \param child    Child ID; only \c CHILDID_SELF is of interest.
     * \param thread   Thread that generated the event (unused).
     * \param time     Event time (unused).
     */
    static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                   LONG object, LONG child, DWORD thread, DWORD time);

    HWINEVENTHOOK hook_ = nullptr;  ///< The installed hook.
};

}  // namespace mfly
