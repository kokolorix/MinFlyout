/**
 * \file OverlayGeometry.cpp
 * \ingroup ui
 * \brief Implementation of the overlay arithmetic.
 */
#include "OverlayGeometry.h"

#include <algorithm>

namespace mfly {
namespace {

/**
 * \brief Rounds to the nearest integer, away from zero on a half.
 *
 * Symmetric on purpose: a zone may start at a negative percentage (part of
 * the window on the screen to the left), and a reference area may have a
 * negative origin (a monitor left of the primary one). Neither may shift an
 * edge by a pixel compared with its positive counterpart.
 *
 * \param value Value to round.
 * \return The rounded value.
 */
int Round(double value) {
    return value < 0.0 ? -static_cast<int>(-value + 0.5)
                       : static_cast<int>(value + 0.5);
}

/**
 * \brief std::clamp that maps NaN to the lower bound.
 * \param value Value to clamp.
 * \param lo    Lower bound.
 * \param hi    Upper bound.
 * \return The clamped value.
 */
double ClampOrLow(double value, double lo, double hi) {
    if (!(value == value)) return lo;  // NaN
    return std::clamp(value, lo, hi);
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

RectI ClippedTo(const RectI& r, const RectI& bounds) {
    RectI out{std::max(r.left, bounds.left), std::max(r.top, bounds.top),
              std::min(r.right, bounds.right), std::min(r.bottom, bounds.bottom)};
    return out.empty() ? RectI{} : out;
}

bool LimitZone(PercentRect& zone) {
    zone.left   = ClampOrLow(zone.left, kZoneMinOrigin, kZoneMaxOrigin);
    zone.top    = ClampOrLow(zone.top, kZoneMinOrigin, kZoneMaxOrigin);
    zone.width  = ClampOrLow(zone.width, kZoneMinSize, kZoneMaxSize);
    zone.height = ClampOrLow(zone.height, kZoneMinSize, kZoneMaxSize);

    // The upper bound of left/top already keeps kZoneMinOnScreen percent on the
    // screen towards the right and the bottom; this is the same rule towards
    // the left and the top.
    return zone.left + zone.width >= kZoneMinOnScreen &&
           zone.top + zone.height >= kZoneMinOnScreen;
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
