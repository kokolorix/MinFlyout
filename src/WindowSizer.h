/**
 * \file WindowSizer.h
 * \ingroup config
 * \brief Places a window into a zone of a monitor, and resizes it in steps.
 */
#pragma once

#include "Common.h"
#include "Config.h"

namespace mfly {

/**
 * \brief Which edges of a window a step resize moves.
 *
 * A bit mask, because the interesting cases are combinations: pulling the left
 * and the right edge apart keeps the window centred, pulling only the right one
 * keeps its left edge where it is. \ref SizeEdge::Horizontal and
 * \ref SizeEdge::Vertical are the two symmetric pairs, \ref SizeEdge::All grows
 * the window in every direction at once.
 */
enum class SizeEdge : unsigned {
    None       = 0x0,
    Left       = 0x1,   ///< Left edge moves outward when growing.
    Right      = 0x2,   ///< Right edge moves outward when growing.
    Top        = 0x4,   ///< Upper edge moves outward when growing.
    Bottom     = 0x8,   ///< Lower edge moves outward when growing.
    Horizontal = Left | Right,   ///< Both vertical edges - width changes, centre stays.
    Vertical   = Top | Bottom,   ///< Both horizontal edges - height changes, centre stays.
    All        = Horizontal | Vertical,  ///< Every edge.
};

/// Bitwise or of two edge masks.
inline SizeEdge operator|(SizeEdge a, SizeEdge b) {
    return static_cast<SizeEdge>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

/// \return \c true if \p mask contains every bit of \p bit.
inline bool HasEdge(SizeEdge mask, SizeEdge bit) {
    return (static_cast<unsigned>(mask) & static_cast<unsigned>(bit)) != 0;
}

/**
 * \brief The commands the resize toolbar offers.
 *
 * The same set the thumbnail toolbar of wiNpoS carried, with its last button -
 * "show the positioning window" - dropped: in MinFlyout that window *is* the
 * flyout the toolbar is drawn in. \ref MaximizeHorizontal took its place.
 */
enum class ResizeCommand {
    Grow,          ///< Larger in every direction.
    Shrink,        ///< Smaller in every direction.
    GrowWidth,     ///< Wider, centre stays.
    ShrinkWidth,   ///< Narrower, centre stays.
    GrowHeight,    ///< Taller, centre stays.
    ShrinkHeight,  ///< Shorter, centre stays.
    /**
     * \brief Full width of the screen, height untouched - and back again.
     *
     * The horizontal counterpart of what Windows already does vertically when
     * the upper or lower border is double clicked. A second invocation puts the
     * window back where it was, see \ref MaximizeHorizontally.
     */
    MaximizeHorizontal,
};

/// Number of entries in \ref ResizeCommand.
constexpr size_t kResizeCommandCount = 7;

/// The commands in toolbar order.
constexpr ResizeCommand kResizeCommands[kResizeCommandCount] = {
    ResizeCommand::Grow,        ResizeCommand::Shrink,
    ResizeCommand::GrowWidth,   ResizeCommand::ShrinkWidth,
    ResizeCommand::GrowHeight,  ResizeCommand::ShrinkHeight,
    ResizeCommand::MaximizeHorizontal,
};

/**
 * \brief Label of a command, shown below the toolbar while it is hovered.
 * \param command The command.
 * \return A static string, never \c nullptr.
 */
const wchar_t* ResizeCommandName(ResizeCommand command);

/**
 * \brief Moves the edges of a window by a fixed number of pixels.
 *
 * Positive \p step grows, negative shrinks; every edge in \p edges moves by
 * that much, so \ref SizeEdge::Horizontal with a step of 10 makes the window
 * twenty pixels wider and leaves its centre alone.
 *
 * What is moved is the *visible* frame - the same rectangle \ref ApplyZone
 * positions - so the invisible shadow border does not creep into the numbers
 * and a window grown against a screen edge really touches it.
 *
 * Three things it refuses or reinterprets:
 *
 * * A maximized window cannot grow, and is restored first when it shrinks.
 * * The result is clamped to the monitor's reference area, so growing walks the
 *   window up to the edge instead of off the screen.
 * * A window that would fill the whole area afterwards is maximized properly
 *   rather than left as a normal window happening to cover everything.
 *
 * \param[in]  window      Target window.
 * \param[in]  step        Pixels per edge; positive grows, negative shrinks.
 * \param[in]  edges       Which edges to move.
 * \param[in]  useWorkArea \c true keeps the window out of the taskbar.
 * \param[out] edgeShift   How far each edge really travelled, signed along its
 *        axis: negative left/top means "further left / further up". Written on
 *        every return, so a caller that follows an edge with the pointer never
 *        reads a stale value. It is the *actual* movement and not the step that
 *        was asked for - an edge stopped by the screen may have moved three
 *        pixels of the ten requested, and all four are zero when the call ended
 *        in a maximize, a restore or a refusal. May be \c nullptr.
 * \return \c true if the window was changed.
 *
 * \note Uses \c SWP_ASYNCWINDOWPOS: a hung target window must not block our own
 *       UI thread.
 */
bool ResizeWindow(HWND window, int step, SizeEdge edges, bool useWorkArea,
                  RECT* edgeShift = nullptr);

/**
 * \brief Stretches a window across the full width of its screen, and back.
 *
 * Windows maximizes a window vertically when its upper or lower border is
 * double clicked, and offers nothing for the other axis. This is that other
 * axis: the first call remembers the frame and pulls the window out to both
 * edges of the reference area, the next one puts it back.
 *
 * The remembered frame is only used while the window still is at full width -
 * a window that was moved or resized in between starts over, so the toggle can
 * never restore a rectangle that has stopped meaning anything.
 *
 * \param window      Target window.
 * \param useWorkArea \c true uses the work area rather than the whole screen.
 * \return \c true if the window was changed.
 */
bool MaximizeHorizontally(HWND window, bool useWorkArea);

/**
 * \brief Runs one toolbar command against a window.
 * \param window      Target window.
 * \param command     What to do.
 * \param step        Step width in pixels, Config::resize.stepPx.
 * \param useWorkArea \c true keeps the window out of the taskbar.
 * \return \c true if the window was changed.
 */
bool ApplyResizeCommand(HWND window, ResizeCommand command, int step, bool useWorkArea);

/**
 * \brief How far the pointer has to travel to stay on the edge it just moved.
 *
 * Which edge the pointer was on is what the window answered when it was asked -
 * \c HTLEFT, \c HTBOTTOMRIGHT and so on - and \p edgeShift says how far each
 * edge actually went. A corner moves the pointer on both axes.
 *
 * \param hitTest   The border code the pointer was over.
 * \param edgeShift Output of \ref ResizeWindow.
 * \return The offset to add to the pointer position; \c {0,0} for anything that
 *         is not a border code.
 */
POINT CursorShiftForHitTest(LRESULT hitTest, const RECT& edgeShift);

/**
 * \brief Forgets the frames \ref MaximizeHorizontally remembers.
 *
 * Called from the periodic cleanup, so entries for windows that have been
 * closed do not pile up over a session.
 */
void ForgetDeadResizeState();

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
