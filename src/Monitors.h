/**
 * \file Monitors.h
 * \ingroup ui
 * \brief Enumerating the monitors shown in the flyout, and matching layouts to them.
 */
#pragma once

#include "Common.h"
#include "Config.h"
#include "MonitorSelector.h"

namespace mfly {

/**
 * \brief One monitor of the current desktop.
 */
struct MonitorEntry {
    HMONITOR     handle = nullptr;  ///< Handle, used as the target of a zone.
    RECT         rect{};            ///< Full monitor area in screen coordinates.
    RECT         work{};            ///< Work area (without the taskbar).
    bool         primary = false;   ///< \c true for the primary monitor.
    int          index = 0;         ///< 1-based number in flyout order, primary first.
    std::wstring device;            ///< Device name, e.g. <code>\\.\DISPLAY2</code>.

    /**
     * \brief Area the percentages refer to.
     * \param useWorkArea \c true selects the work area.
     * \return The reference rectangle.
     */
    const RECT& area(bool useWorkArea) const { return useWorkArea ? work : rect; }

    /**
     * \brief Short form of \ref device for captions and selectors.
     *
     * Everything up to and including the last backslash is dropped, so
     * <code>\\.\DISPLAY2</code> becomes \c DISPLAY2.
     *
     * \return The last component of the device name, or an empty string.
     */
    std::wstring shortDevice() const;
};

/**
 * \brief Enumerates all monitors, primary one first.
 *
 * The order is stable within a session, so the flyout does not reshuffle its
 * rows between openings. MonitorEntry::index is assigned here and is what the
 * numeric selector in \ref MonitorMatchesSelector refers to.
 *
 * \return One entry per monitor; never empty on a working desktop.
 */
std::vector<MonitorEntry> EnumerateMonitors();

/**
 * \brief Finds the monitor a window is on.
 * \param monitors Result of \ref EnumerateMonitors.
 * \param window   Window to locate.
 * \return Index into \p monitors, or 0 if it cannot be determined.
 */
size_t IndexOfMonitorFor(const std::vector<MonitorEntry>& monitors, HWND window);

/**
 * \brief Tests one monitor selector from Layout::monitors.
 *
 * Reduces the monitor to \ref MonitorFacts and hands it to
 * \ref SelectorMatches, where the accepted forms are documented.
 *
 * \param monitor  Monitor to test.
 * \param selector One entry from Layout::monitors.
 * \return \c true if the selector names this monitor.
 */
bool MonitorMatchesSelector(const MonitorEntry& monitor, const std::wstring& selector);

/**
 * \brief Tests whether a layout is offered on a monitor.
 *
 * A layout without selectors applies everywhere; otherwise one matching
 * selector is enough.
 *
 * \param layout  Layout to test.
 * \param monitor Monitor to test it against.
 * \return \c true if the layout belongs on this monitor.
 */
bool LayoutAppliesTo(const Layout& layout, const MonitorEntry& monitor);

/**
 * \brief Picks the layouts that belong on one monitor.
 * \param layouts All configured layouts, in file order.
 * \param monitor Monitor to filter for.
 * \return The matching layouts, order preserved; may be empty.
 */
std::vector<Layout> LayoutsForMonitor(const std::vector<Layout>& layouts,
                                      const MonitorEntry& monitor);

}  // namespace mfly
