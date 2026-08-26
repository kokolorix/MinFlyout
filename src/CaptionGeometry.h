/**
 * \file CaptionGeometry.h
 * \ingroup detect
 * \brief Where the caption buttons of a window have to be.
 *
 * Windows places the caption buttons by a fixed rule: one block flush against
 * the upper right corner of the visible frame (upper left with RTL layout),
 * inside it one equally wide slot per button in a fixed order. That rule is
 * arithmetic, not a question to the window - which is the point of this file:
 * it lets \ref mfly::ProbeMinimizeButton find the button even when the window
 * refuses to answer \c WM_NCHITTEST.
 *
 * Like Json.h and MonitorSelector.h this file is deliberately free of Windows
 * dependencies, so the rules can be tested without a desktop. The Win32 side -
 * reading window styles, asking DWM for the real block - lives in
 * CaptionProbe.cpp.
 */
#pragma once

namespace mfly {

/**
 * \brief A rectangle without Windows types.
 *
 * Same convention as \c RECT: \c right and \c bottom are exclusive.
 */
struct RectI {
    int left = 0;    ///< Left edge (inclusive).
    int top = 0;     ///< Top edge (inclusive).
    int right = 0;   ///< Right edge (exclusive).
    int bottom = 0;  ///< Bottom edge (exclusive).

    /// \return Width in pixels, negative for an inverted rectangle.
    int width() const { return right - left; }
    /// \return Height in pixels, negative for an inverted rectangle.
    int height() const { return bottom - top; }
    /// \return \c true if the rectangle encloses no pixel.
    bool empty() const { return width() <= 0 || height() <= 0; }

    /**
     * \brief Point test.
     * \param x Horizontal coordinate.
     * \param y Vertical coordinate.
     * \return \c true if the point lies inside.
     */
    bool contains(int x, int y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }

    /**
     * \brief Grows the rectangle on every side.
     * \param dx Horizontal growth per side.
     * \param dy Vertical growth per side.
     * \return The inflated copy.
     */
    RectI inflated(int dx, int dy) const {
        return RectI{left - dx, top - dy, right + dx, bottom + dy};
    }
};

/// The caption buttons, in the order Windows draws them from left to right.
enum class CaptionButton {
    Minimize,  ///< Minimize.
    Maximize,  ///< Maximize or restore.
    Help,      ///< Question mark of a dialog (\c WS_EX_CONTEXTHELP).
    Close,     ///< Close.
};

/**
 * \brief Which buttons a window shows, taken from its styles.
 *
 * Windows draws the minimize and the maximize button together: a window with
 * only one of the two boxes still shows both, the missing one greyed out. The
 * help button on the other hand replaces them and only appears when neither box
 * is set - which is why \ref slots yields three, two or one, never something
 * in between.
 *
 * \ref sysMenu is deliberately not a precondition for the boxes. An app that
 * draws its own title bar often drops \c WS_SYSMENU and keeps the boxes; it
 * still shows three buttons.
 */
struct CaptionLayout {
    bool sysMenu = true;       ///< \c WS_SYSMENU - decides the close-only and help cases.
    bool minimizeBox = false;  ///< \c WS_MINIMIZEBOX.
    bool maximizeBox = false;  ///< \c WS_MAXIMIZEBOX.
    bool contextHelp = false;  ///< \c WS_EX_CONTEXTHELP.
    bool rightToLeft = false;  ///< \c WS_EX_LAYOUTRTL: block on the left, order mirrored.

    /// \return Number of button slots in the caption: 3, 2, 1 or 0.
    int slots() const;

    /**
     * \brief Position of a button within the block.
     * \param[in]  which Button to look for.
     * \param[out] index Slot index, counted from the left in reading order.
     * \return \c false if this window does not show the button at all.
     */
    bool slotOf(CaptionButton which, int& index) const;
};

/**
 * \brief Cuts one button out of the caption button block.
 *
 * The block is divided into \ref CaptionLayout::slots equal parts; with
 * \ref CaptionLayout::rightToLeft the order is mirrored. Rounding is done so
 * that the slots tile the block exactly, without a gap or an overlap.
 *
 * \param[in]  block  The whole button block.
 * \param[in]  layout Which buttons the window shows.
 * \param[in]  which  Button that is wanted.
 * \param[out] out    The button rectangle, written only on \c true.
 * \return \c false for an empty block or a button this window does not show.
 */
bool CaptionButtonRect(const RectI& block, const CaptionLayout& layout,
                       CaptionButton which, RectI& out);

/**
 * \brief Width of one caption button, derived from the title bar height.
 *
 * Windows 11 draws the buttons 46 x 32 at 100 %, so the width follows the
 * height at a ratio of 46:32. Tying it to a measured height rather than to a
 * constant keeps the estimate usable at every scaling factor.
 *
 * \param titleBarHeight Height of the title bar in pixels.
 * \return Width of one button in pixels, at least 1.
 */
int CaptionButtonWidth(int titleBarHeight);

/**
 * \brief Estimates the caption button block from the visible frame.
 *
 * The last resort: used when neither the window nor DWM reports where the
 * buttons are. The block sits flush in the upper right corner of \p frame
 * (upper left with RTL layout).
 *
 * \param frame          Visible window frame - the extended frame bounds, not
 *                       the window rectangle with its invisible border.
 * \param titleBarHeight Height of the title bar in pixels.
 * \param layout         Which buttons the window shows.
 * \return The estimated block; empty if the window has no buttons.
 */
RectI EstimateCaptionBlock(const RectI& frame, int titleBarHeight,
                           const CaptionLayout& layout);

/**
 * \brief Generous region around the caption buttons.
 *
 * The cheap pre-filter of the detection: everything outside is certainly not a
 * caption button, and only inside is it worth asking the window or DWM. It
 * reaches two button widths beyond \ref EstimateCaptionBlock to either side,
 * half a title bar above it and a whole one below, so an app that draws its
 * buttons a little differently is still caught.
 *
 * \param frame          Visible window frame.
 * \param titleBarHeight Height of the title bar in pixels.
 * \param layout         Which buttons the window shows.
 * \return The region; empty if the window has no buttons.
 */
RectI CaptionButtonRegion(const RectI& frame, int titleBarHeight,
                          const CaptionLayout& layout);

}  // namespace mfly
