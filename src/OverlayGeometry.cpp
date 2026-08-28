/**
 * \file OverlayGeometry.cpp
 * \ingroup ui
 * \brief Implementation of the overlay arithmetic.
 */
#include "OverlayGeometry.h"

namespace mfly {
namespace {

/**
 * \brief Rounds to the nearest integer, away from zero on a half.
 *
 * Percentages are never negative here, but the helper stays symmetric so a
 * reference area with a negative origin - a monitor left of the primary one -
 * cannot introduce a bias.
 *
 * \param value Value to round.
 * \return The rounded value.
 */
int Round(double value) {
    return value < 0.0 ? -static_cast<int>(-value + 0.5)
                       : static_cast<int>(value + 0.5);
}

}  // namespace

RectI PercentToPixels(const RectI& area, const PercentRect& part) {
    const double w = static_cast<double>(area.width());
    const double h = static_cast<double>(area.height());
    if (w <= 0.0 || h <= 0.0) return RectI{};

    // Both edges are computed from area.left / area.top, never as "origin plus
    // rounded width" - that is what makes adjacent tiles share a pixel column
    // instead of leaving a one-pixel seam at some widths.
    RectI r;
    r.left   = area.left + Round(w * part.left / 100.0);
    r.top    = area.top  + Round(h * part.top / 100.0);
    r.right  = area.left + Round(w * (part.left + part.width) / 100.0);
    r.bottom = area.top  + Round(h * (part.top + part.height) / 100.0);
    return r;
}

RectI ShrunkBy(const RectI& r, int gap) {
    RectI out = r;
    if (gap <= 0) return out;
    if (out.width()  > 2 * gap) { out.left += gap; out.right  -= gap; }
    if (out.height() > 2 * gap) { out.top  += gap; out.bottom -= gap; }
    return out;
}

int SmallestHit(const std::vector<RectI>& rects, int x, int y) {
    int best = -1;
    long long bestArea = 0;

    for (size_t i = 0; i < rects.size(); ++i) {
        const RectI& r = rects[i];
        if (r.empty() || !r.contains(x, y)) continue;

        const long long area = static_cast<long long>(r.width()) *
                               static_cast<long long>(r.height());
        if (best < 0 || area < bestArea) {
            best = static_cast<int>(i);
            bestArea = area;
        }
    }
    return best;
}

}  // namespace mfly
