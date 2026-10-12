/**
 * \file OverlayGeometryTest.cpp
 * \brief Checks for the touch overlay arithmetic, without a test framework.
 *
 * Exit code 0 means every check passed; a failure prints file, line and the
 * expression and makes the build fail.
 *
 * \note This file carries its own \c main and is built as the separate target
 *       \c overlay_tests, because \c tests/JsonTest.cpp, \c tests/SelectorTest.cpp
 *       and \c tests/GeometryTest.cpp are not present in the working tree - the
 *       \c json_tests target they belong to cannot be configured at the moment.
 *       Once they are back, rename \c main here to
 *       <code>int RunOverlayGeometryTests()</code> and fold it into that target.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "OverlayGeometry.h"

namespace {

int g_failures = 0;  ///< Number of failed checks.
int g_checks = 0;    ///< Number of checks run.

/**
 * \brief Records the result of one check.
 * \param ok    Result of the expression.
 * \param text  The expression as written.
 * \param file  Source file.
 * \param line  Source line.
 */
void Check(bool ok, const char* text, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED  %s(%d): %s\n", file, line, text);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/// \return \c true if the two rectangles are identical.
bool Same(const mfly::RectI& a, const mfly::RectI& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

/// The percentage conversion on a plain 1920x1080 area at the origin.
void TestPercentToPixels() {
    const mfly::RectI area{0, 0, 1920, 1080};

    const mfly::RectI full = mfly::PercentToPixels(area, {0, 0, 100, 100});
    CHECK(Same(full, area));

    const mfly::RectI left = mfly::PercentToPixels(area, {0, 0, 50, 100});
    CHECK(left.left == 0 && left.right == 960);
    CHECK(left.top == 0 && left.bottom == 1080);

    const mfly::RectI right = mfly::PercentToPixels(area, {50, 0, 50, 100});
    CHECK(right.left == 960 && right.right == 1920);

    // The halves meet exactly - no seam, no overlap.
    CHECK(left.right == right.left);

    // An empty area cannot produce a tile.
    CHECK(mfly::PercentToPixels(mfly::RectI{0, 0, 0, 0}, {0, 0, 50, 50}).empty());
}

/// A monitor that does not start at the origin, and thirds that do not divide.
void TestOffsetAndThirds() {
    // A screen to the left of the primary one has negative coordinates; the
    // area also starts below zero when the taskbar sits at the top.
    const mfly::RectI area{-1920, -200, 0, 880};

    const mfly::RectI whole = mfly::PercentToPixels(area, {0, 0, 100, 100});
    CHECK(Same(whole, area));

    // Thirds as the template spells them: 0 / 33.33 / 66.66, each 33.34 wide.
    const mfly::RectI a = mfly::PercentToPixels(area, {0.0,   0, 33.34, 100});
    const mfly::RectI b = mfly::PercentToPixels(area, {33.33, 0, 33.34, 100});
    const mfly::RectI c = mfly::PercentToPixels(area, {66.66, 0, 33.34, 100});

    CHECK(a.left == area.left);
    CHECK(c.right == area.right);
    // Neighbours differ by at most one pixel, and never leave a hole.
    CHECK(b.left - a.right <= 1 && b.left - a.right >= -1);
    CHECK(c.left - b.right <= 1 && c.left - b.right >= -1);
}

/// The gap between two tiles.
void TestShrunkBy() {
    const mfly::RectI tile{100, 100, 300, 200};

    const mfly::RectI gapped = mfly::ShrunkBy(tile, 4);
    CHECK(gapped.left == 104 && gapped.right == 296);
    CHECK(gapped.top == 104 && gapped.bottom == 196);

    // A gap of zero or less changes nothing.
    CHECK(Same(mfly::ShrunkBy(tile, 0), tile));
    CHECK(Same(mfly::ShrunkBy(tile, -3), tile));

    // A sliver stays visible instead of collapsing.
    const mfly::RectI sliver{0, 0, 6, 6};
    CHECK(Same(mfly::ShrunkBy(sliver, 4), sliver));

    // Exactly twice the gap is still left alone (the > in the implementation).
    const mfly::RectI exact{0, 0, 8, 8};
    CHECK(Same(mfly::ShrunkBy(exact, 4), exact));
}

/// The drop target under a point, including the overlap rule.
void TestSmallestHit() {
    std::vector<mfly::RectI> tiles{
        mfly::RectI{0, 0, 960, 1080},     // left half
        mfly::RectI{960, 0, 1920, 1080},  // right half
    };

    CHECK(mfly::SmallestHit(tiles, 10, 10) == 0);
    CHECK(mfly::SmallestHit(tiles, 1000, 10) == 1);
    CHECK(mfly::SmallestHit(tiles, 5000, 10) == -1);

    // The right edge is exclusive, so the seam belongs to the right tile.
    CHECK(mfly::SmallestHit(tiles, 959, 10) == 0);
    CHECK(mfly::SmallestHit(tiles, 960, 10) == 1);

    // A small tile drawn on top of a large one stays reachable.
    tiles.push_back(mfly::RectI{400, 400, 560, 560});
    CHECK(mfly::SmallestHit(tiles, 480, 480) == 2);
    CHECK(mfly::SmallestHit(tiles, 300, 480) == 0);

    // Empty rectangles are never hit, and never shadow a real tile.
    tiles.push_back(mfly::RectI{480, 480, 480, 480});
    CHECK(mfly::SmallestHit(tiles, 480, 480) == 2);

    CHECK(mfly::SmallestHit({}, 0, 0) == -1);
}

/// A trigger field in the centre of the screen, the way the template spells it.
void TestTriggerField() {
    const mfly::RectI work{0, 0, 2880, 1920};
    const mfly::RectI trigger = mfly::PercentToPixels(work, {40, 40, 20, 20});

    CHECK(trigger.left == 1152 && trigger.right == 1728);
    CHECK(trigger.top == 768 && trigger.bottom == 1152);

    // It is centred, and comfortably larger than a finger.
    CHECK(trigger.left - work.left == work.right - trigger.right);
    CHECK(trigger.top - work.top == work.bottom - trigger.bottom);
    CHECK(trigger.width() > 100 && trigger.height() > 100);
}

/// Zones reaching beyond their monitor, in pixels.
void TestOverhang() {
    const mfly::RectI area{0, 0, 1920, 1080};

    // A tenth of the screen to the left: rounded symmetrically, so -10 % is the
    // mirror image of +10 %.
    const mfly::RectI left = mfly::PercentToPixels(area, {-10, 0, 60, 100});
    CHECK(left.left == -192 && left.right == 960);

    const mfly::RectI right = mfly::PercentToPixels(area, {50, 0, 60, 100});
    CHECK(right.left == 960 && right.right == 2112);

    // A half-pixel on the negative side rounds away from zero, like on the
    // positive side (truncating "+ 0.5" would give 0 here).
    const mfly::RectI half = mfly::PercentToPixels(mfly::RectI{0, 0, 200, 100},
                                                   {-0.5, 0, 50, 100});
    CHECK(half.left == -1);

    // Two full screens wide, on a monitor left of the primary one.
    const mfly::RectI wide = mfly::PercentToPixels(mfly::RectI{-1920, 0, 0, 1080},
                                                   {0, 0, 200, 100});
    CHECK(wide.left == -1920 && wide.right == 1920);
}

/// Clipping a tile to the area it is drawn in.
void TestClippedTo() {
    const mfly::RectI mini{10, 10, 110, 70};

    CHECK(Same(mfly::ClippedTo(mfly::RectI{20, 20, 50, 50}, mini),
               (mfly::RectI{20, 20, 50, 50})));
    CHECK(Same(mfly::ClippedTo(mfly::RectI{-40, 20, 50, 90}, mini),
               (mfly::RectI{10, 20, 50, 70})));
    CHECK(Same(mfly::ClippedTo(mfly::RectI{0, 0, 500, 500}, mini), mini));

    // Not overlapping, or only touching: nothing is left.
    CHECK(mfly::ClippedTo(mfly::RectI{200, 20, 300, 50}, mini).empty());
    CHECK(mfly::ClippedTo(mfly::RectI{110, 20, 150, 50}, mini).empty());

    // A tile running over the left edge: after gap and clip it touches the
    // frame on that side and keeps its gap on the others.
    const mfly::RectI tile = mfly::ClippedTo(
        mfly::ShrunkBy(mfly::PercentToPixels(mini, {-10, 0, 60, 100}), 2), mini);
    CHECK(tile.left == mini.left);
    CHECK(tile.right == 10 + 50 - 2);
    CHECK(tile.top == mini.top + 2 && tile.bottom == mini.bottom - 2);
}

/// The limits a zone has to stay within.
void TestLimitZone() {
    // An ordinary zone passes untouched.
    mfly::PercentRect z{50, 0, 50, 100};
    CHECK(mfly::LimitZone(z));
    CHECK(z.left == 50 && z.top == 0 && z.width == 50 && z.height == 100);

    // Overhang to the left and to the right is kept as it is.
    z = {-10, -5, 60, 110};
    CHECK(mfly::LimitZone(z));
    CHECK(z.left == -10 && z.top == -5 && z.width == 60 && z.height == 110);

    z = {80, 0, 150, 100};
    CHECK(mfly::LimitZone(z));
    CHECK(z.left == 80 && z.width == 150);

    // Out-of-range values are clamped.
    z = {-1000, 0, 2000, 0};
    CHECK(mfly::LimitZone(z));
    CHECK(z.left == mfly::kZoneMinOrigin && z.width == mfly::kZoneMaxSize);
    CHECK(z.height == mfly::kZoneMinSize);

    z = {150, 0, 50, 100};
    CHECK(mfly::LimitZone(z));
    CHECK(z.left == mfly::kZoneMaxOrigin);

    // Entirely beside the monitor - refused, not corrected.
    z = {-60, 0, 50, 100};
    CHECK(!mfly::LimitZone(z));
    z = {0, -100, 50, 100};
    CHECK(!mfly::LimitZone(z));

    // Exactly the minimum on screen is still accepted.
    z = {-49, 0, 50, 100};
    CHECK(mfly::LimitZone(z));

    // NaN counts as the lower bound.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    z = {0, 0, nan, 100};
    CHECK(mfly::LimitZone(z));
    CHECK(z.width == mfly::kZoneMinSize && !std::isnan(z.width));
}

}  // namespace

int main() {
    TestPercentToPixels();
    TestOffsetAndThirds();
    TestShrunkBy();
    TestSmallestHit();
    TestTriggerField();
    TestOverhang();
    TestClippedTo();
    TestLimitZone();

    std::printf("OverlayGeometry: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
