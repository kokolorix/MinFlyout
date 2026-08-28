/**
 * \file OverlayGeometry.h
 * \ingroup ui
 * \brief Where the drop targets of the touch overlay sit - pure arithmetic.
 *
 * The touch overlay knows only two kinds of rectangle: the trigger field that
 * unfolds the zones, and the zone tiles themselves. Both are percentages of a
 * reference area, and both end up as pixels on one monitor. That conversion,
 * the gap that keeps neighbouring tiles apart and the rule for overlapping
 * zones live here.
 *
 * Like Json.h, MonitorSelector.h and CaptionGeometry.h this file is
 * deliberately free of Windows dependencies, so the rules can be tested without
 * a desktop. \ref mfly::RectI is borrowed from CaptionGeometry.h rather than
 * declared a second time.
 */
#pragma once

#include <vector>

#include "CaptionGeometry.h"  // RectI

namespace mfly {

/**
 * \brief A rectangle given in percent of some reference area.
 *
 * The same four numbers a \c "zones" entry of the configuration file carries,
 * without the Windows types Config.h drags in. Convert a mfly::Zone with
 * \ref PercentOf.
 */
struct PercentRect {
    double left = 0.0;    ///< Left edge in percent.
    double top = 0.0;     ///< Top edge in percent.
    double width = 0.0;   ///< Width in percent.
    double height = 0.0;  ///< Height in percent.

    /// \return \c true if the rectangle has a usable size.
    bool valid() const { return width > 0.0 && height > 0.0; }
};

/**
 * \brief Maps a percentage rectangle onto an area in pixels.
 *
 * Both edges are rounded from the same origin, so two zones that meet at 50 %
 * produce tiles that meet at the same pixel - no gap, no overlap, whatever the
 * width of the area happens to be. This is the same arithmetic
 * \ref mfly::ApplyZone uses to position a window, which is what makes the
 * highlighted tile and the window that lands there cover the same rectangle.
 *
 * \param area Reference area in pixels (screen coordinates for the overlay).
 * \param part Rectangle in percent.
 * \return The rectangle in pixels; empty for an empty area.
 */
RectI PercentToPixels(const RectI& area, const PercentRect& part);

/**
 * \brief Shrinks a tile on every side so neighbouring tiles stay apart.
 *
 * A tile smaller than twice the gap is left alone - a sliver zone should stay
 * visible rather than collapse to nothing.
 *
 * \param r   Tile rectangle.
 * \param gap Gap per side in pixels; values \c <= 0 change nothing.
 * \return The shrunk copy.
 */
RectI ShrunkBy(const RectI& r, int gap);

/**
 * \brief Finds the drop target under a point.
 *
 * Zones are meant to tile the area without overlapping, but nothing enforces
 * that - so where they do overlap the smallest one wins, exactly as in the
 * flyout. A small tile drawn on top of a large one therefore stays reachable.
 *
 * \param rects Candidate rectangles, in drawing order.
 * \param x     Horizontal coordinate.
 * \param y     Vertical coordinate.
 * \return Index into \p rects, or \c -1 if the point hits nothing.
 */
int SmallestHit(const std::vector<RectI>& rects, int x, int y);

}  // namespace mfly
