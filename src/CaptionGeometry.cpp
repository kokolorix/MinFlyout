/**
 * \file CaptionGeometry.cpp
 * \ingroup detect
 * \brief Implementation of the caption button arithmetic.
 */
#include "CaptionGeometry.h"

namespace mfly {
namespace {

/// Width of a caption button relative to the title bar height on Windows 11.
constexpr int kButtonWidthNum = 46;
/// Denominator of \ref kButtonWidthNum: the 32 pixel title bar at 100 %.
constexpr int kButtonWidthDen = 32;

}  // namespace

int CaptionLayout::slots() const {
    // The boxes are what counts, not WS_SYSMENU. An app that draws its own
    // title bar commonly drops WS_SYSMENU while keeping WS_MINIMIZEBOX and
    // WS_MAXIMIZEBOX - Visual Studio Code reports 0x14C70000, which has both
    // boxes and no system menu, and still draws all three buttons. Requiring
    // WS_SYSMENU here is what kept those windows out.
    if (minimizeBox || maximizeBox) return 3;  // minimize, maximize, close
    if (!sysMenu) return 0;
    if (contextHelp) return 2;                 // help, close
    return 1;                                  // close
}

bool CaptionLayout::slotOf(CaptionButton which, int& index) const {
    const int count = slots();
    if (count == 0) return false;

    switch (which) {
    case CaptionButton::Minimize:
        if (count != 3) return false;
        index = 0;
        return true;
    case CaptionButton::Maximize:
        if (count != 3) return false;
        index = 1;
        return true;
    case CaptionButton::Help:
        if (count != 2) return false;
        index = 0;
        return true;
    case CaptionButton::Close:
        index = count - 1;
        return true;
    }
    return false;
}

bool CaptionButtonRect(const RectI& block, const CaptionLayout& layout,
                       CaptionButton which, RectI& out) {
    if (block.empty()) return false;

    const int count = layout.slots();
    int index = 0;
    if (!layout.slotOf(which, index)) return false;

    // RTL mirrors the order inside the block, not the block itself.
    if (layout.rightToLeft) index = count - 1 - index;

    // Boundaries from the same division, so neighbouring slots share an edge
    // instead of leaving a rounding gap between them.
    const int width = block.width();
    out.left = block.left + (width * index) / count;
    out.right = block.left + (width * (index + 1)) / count;
    out.top = block.top;
    out.bottom = block.bottom;
    return !out.empty();
}

int CaptionButtonWidth(int titleBarHeight) {
    if (titleBarHeight <= 0) return 1;
    const int width = (titleBarHeight * kButtonWidthNum + kButtonWidthDen / 2) / kButtonWidthDen;
    return width > 0 ? width : 1;
}

RectI EstimateCaptionBlock(const RectI& frame, int titleBarHeight,
                           const CaptionLayout& layout) {
    const int count = layout.slots();
    if (count == 0 || titleBarHeight <= 0 || frame.empty()) return RectI{};

    const int blockWidth = CaptionButtonWidth(titleBarHeight) * count;

    RectI block;
    block.top = frame.top;
    block.bottom = frame.top + titleBarHeight;
    if (layout.rightToLeft) {
        block.left = frame.left;
        block.right = frame.left + blockWidth;
    } else {
        block.right = frame.right;
        block.left = frame.right - blockWidth;
    }
    return block;
}

RectI CaptionButtonRegion(const RectI& frame, int titleBarHeight,
                          const CaptionLayout& layout) {
    const RectI block = EstimateCaptionBlock(frame, titleBarHeight, layout);
    if (block.empty()) return block;

    // Deliberately generous: an app that draws its buttons a little wider or a
    // little lower than the system must still get through the pre-filter,
    // because the sources behind it can cope with that. Two button widths to
    // the sides, half a title bar up and a whole one down.
    const int wide = 2 * CaptionButtonWidth(titleBarHeight);
    return RectI{block.left - wide, block.top - titleBarHeight / 2,
                 block.right + wide, block.bottom + titleBarHeight};
}

}  // namespace mfly
