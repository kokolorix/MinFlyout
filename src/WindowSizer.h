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

/**
 * \brief Reads the zone a window currently occupies - the inverse of \ref ApplyZone.
 *
 * Measures the *visible* frame, which is exactly what \ref ApplyZone positions,
 * so the invisible shadow border cancels out on both ways: applying a captured
 * zone puts the window back where it was measured.
 *
 * The result is clamped to the ranges the parser in Config.cpp enforces
 * (left/top 0-99, width/height 1-100). A window hanging over an edge of its
 * monitor would otherwise produce a line that the next load silently corrects.
 *
 * \param[in]  window      Window to measure.
 * \param[in]  useWorkArea \c true bases the percentages on the work area.
 * \param[out] zone        Written only when \c true is returned.
 * \param[out] monitor     Monitor the window was measured against; may be \c nullptr.
 * \return \c false for an invalid or minimized window, or when the monitor
 *         cannot be determined.
 */
bool ZoneFromWindow(HWND window, bool useWorkArea, Zone& zone,
                    HMONITOR* monitor = nullptr);

/**
 * \brief Formats a zone as one ready-to-paste line of the configuration file.
 *
 * Indentation, key order and the trailing comma match the \c "zones" arrays of
 * the template, so the line can be dropped between two existing zones without
 * touching anything else:
 *
 * ```
 *         { "left": 2.03, "top": 65.74, "width": 96.15, "height": 28.7 },
 * ```
 *
 * Percentages are rounded to two decimals, and trailing zeros are dropped - a
 * window sitting exactly on a half yields \c 50 rather than \c 50.00. The
 * formatting is locale-independent (\c std::format without \c {:L}), so the
 * decimal separator stays a point on a German system too.
 *
 * \param zone Zone to format.
 * \return The line including its indentation and a trailing \c CRLF.
 */
std::wstring FormatZoneEntry(const Zone& zone);

}  // namespace mfly
