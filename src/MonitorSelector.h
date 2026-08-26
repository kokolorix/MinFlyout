/**
 * \file MonitorSelector.h
 * \ingroup config
 * \brief Matching the \c "monitors" entries of a layout against one screen.
 *
 * Like Json.h this file is deliberately free of Windows dependencies, so the
 * rules below can be tested without a desktop attached. The Win32 side of it -
 * turning an \c HMONITOR into \ref mfly::MonitorFacts - lives in Monitors.cpp.
 */
#pragma once

#include <string>

namespace mfly {

/**
 * \brief Everything a selector is matched against.
 *
 * The plain-data view of a monitor: no handles, no rectangles, nothing that
 * would need a running desktop.
 */
struct MonitorFacts {
    int          index = 0;      ///< 1-based number in flyout order, primary first.
    bool         primary = false;///< \c true for the primary monitor.
    std::wstring device;         ///< Device name, e.g. <code>\\.\DISPLAY2</code>.
    long         width = 0;      ///< Width of the full monitor area in pixels.
    long         height = 0;     ///< Height of the full monitor area in pixels.
};

/**
 * \brief Tests one selector from Layout::monitors.
 *
 * Selectors are compared case-insensitively (ASCII) and surrounding blanks are
 * ignored. These forms are understood:
 *
 * | Selector | Matches |
 * |---|---|
 * | \c "*" or \c "all"   | every monitor |
 * | \c "1", \c "2", ...  | MonitorFacts::index - the number the flyout caption shows |
 * | \c "primary"         | the primary monitor |
 * | \c "secondary"       | every monitor that is not the primary one |
 * | \c "DISPLAY2"        | MonitorFacts::device, with or without the <code>\\.\</code> prefix |
 * | \c "3840x2160"       | that resolution, in physical pixels of the full monitor area |
 *
 * The resolution accepts \c x, \c X and the multiplication sign as the
 * separator, so a caption can be copied straight out of the flyout. Anything
 * that is neither a keyword, a number nor a resolution is treated as a device
 * name; an unknown one simply matches nothing.
 *
 * \param monitor  The screen to test.
 * \param selector One entry from Layout::monitors.
 * \return \c true if the selector names this monitor.
 */
bool SelectorMatches(const MonitorFacts& monitor, const std::wstring& selector);

}  // namespace mfly
