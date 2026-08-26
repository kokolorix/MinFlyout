/**
 * \file WindowSizer.h
 * \ingroup config
 * \brief Places a window into a zone of a monitor.
 */
#pragma once

#include "Common.h"
#include "Config.h"

namespace mfly {

/**
 * \brief Moves a window into a zone of the given monitor.
 *
 * The percentages of the zone refer to the work area of \p monitor (or to its
 * full area, see Config::useWorkArea). Because the target monitor is passed in
 * explicitly, a click in the flyout can move a window to another screen in one
 * step. A maximized or minimized window is restored first.
 *
 * The visible edges are placed flush: since Windows Vista the window rectangle
 * is wider than the visible border because the drop shadow counts towards it.
 * The difference comes from \c DWMWA_EXTENDED_FRAME_BOUNDS and is factored out,
 * so two windows side by side end up without a gap.
 *
 * \param window      Target window.
 * \param zone        Zone in percent.
 * \param monitor     Monitor to place the window on.
 * \param useWorkArea \c true bases the percentages on the work area.
 * \return \c true if \c SetWindowPos was accepted.
 *
 * \note Uses \c SWP_ASYNCWINDOWPOS: a hung target window must not block our own
 *       UI thread.
 */
bool ApplyZone(HWND window, const Zone& zone, HMONITOR monitor, bool useWorkArea);

/**
 * \brief Checks whether a window can be resized freely at all.
 * \param window Target window.
 * \return \c true if the window has a sizing border.
 */
bool IsResizable(HWND window);

}  // namespace mfly
