/**
 * \file CaptionProbe.h
 * \ingroup detect
 * \brief Detection of the minimize button of foreign windows.
 *
 * Two ways lead to the button, and the detection walks them in order.
 *
 * **Asking.** \c WM_NCHITTEST goes to the window and \c HTMINBUTTON comes back
 * - exactly the mechanism Windows itself uses to evaluate its caption buttons.
 * Exact, but it needs a window that answers. Standard Win32 windows do, and so
 * do most Chromium-based apps, because Windows only offers them the snap
 * layouts if they do. Some do not: a window at a higher integrity level cannot
 * be reached across UIPI, a hung window does not answer within
 * \ref kSendTimeoutMs, and a window with a custom title bar may report the
 * button area as plain client space.
 *
 * **Computing.** The position of the caption buttons is not a matter of taste:
 * Windows puts them in one block flush against the upper right corner of the
 * visible frame, in a fixed order, each slot equally wide. So the button can be
 * derived from the window frame, its styles and the title bar height, without
 * asking anybody. Three sources feed that computation, from precise to
 * approximate - see \ref ProbeSource.
 *
 * The arithmetic itself is in CaptionGeometry.h, free of Windows dependencies
 * and covered by \c tests/GeometryTest.cpp.
 */
#pragma once

#include "CaptionGeometry.h"
#include "Common.h"

namespace mfly {

/**
 * \brief Where a button rectangle came from.
 *
 * Written into \ref HitInfo::source and into the log, so a window that behaves
 * oddly can be told apart from one that simply is not asked.
 */
enum class ProbeSource {
    /// \c WM_NCHITTEST plus edge search - measured on the window itself.
    HitTest,
    /// \c DWMWA_CAPTION_BUTTON_BOUNDS - the block as DWM knows it, split into slots.
    DwmBounds,
    /// \c GetTitleBarInfo - the exact title bar strip, split into slots.
    TitleBarInfo,
    /// Computed from the visible frame and the system caption height - the last resort.
    Estimate,
};

/**
 * \brief Which of the ways in \ref CaptionProbe.h may be walked.
 */
enum class ProbeMode {
    Auto,        ///< Ask first, compute when that yields nothing. The default.
    HitTestOnly, ///< Only ask - the behaviour before computing was added.
    Computed,    ///< Only compute, never ask. Useful to test the computation.
};

/**
 * \brief Reads a probe mode from its configuration name.
 * \param[in]  text One of \c "auto", \c "hittest", \c "computed" (any case).
 * \param[out] out  Written only on \c true.
 * \return \c false for an unknown name.
 */
bool ParseProbeMode(const std::wstring& text, ProbeMode& out);

/**
 * \brief Name of a probe source for the log.
 * \param source The source.
 * \return A static string, never \c nullptr.
 */
const wchar_t* ProbeSourceName(ProbeSource source);

/**
 * \brief Result of a successful button detection.
 */
struct HitInfo {
    HWND window = nullptr;  ///< Top-level window that was hit.
    RECT buttonRect{};      ///< Minimize button in screen coordinates.
    RECT windowRect{};      ///< Window rectangle in screen coordinates.
    ProbeSource source = ProbeSource::HitTest;  ///< How the rectangle was found.
};

/**
 * \brief Tests a screen point and determines the button rectangle.
 *
 * \param[in]  pt   Point in screen coordinates (usually the cursor position).
 * \param[in]  mode Which ways may be walked.
 * \param[out] out  Written only when \c true is returned.
 * \return \c true if the minimize button of a foreign window lies under \p pt.
 *
 * The hit test runs unconditionally: it is a single message and it is what the
 * detection was originally built on, so no guess of ours is allowed in front of
 * it. \ref MayBeCaptionButton only guards the computed path behind it.
 *
 * \warning Sends messages to a foreign window and therefore does not belong in
 *          a low-level hook callback.
 */
bool ProbeMinimizeButton(POINT pt, ProbeMode mode, HitInfo& out);

/**
 * \brief Cheap pre-test without a single message.
 *
 * Answers the question "can a caption button be here at all?" from the window
 * styles and the frame alone: a generous region around the computed button
 * block, see \ref CaptionButtonRegion.
 *
 * It guards the computed path only, never the hit test - the region rests on
 * the same assumptions the computation does, and a window that answers
 * \c HTMINBUTTON must be found even where those assumptions are wrong.
 *
 * \param topLevel Top-level window under the cursor.
 * \param pt       Point in screen coordinates.
 * \return \c true if the point lies in the caption button region.
 */
bool MayBeCaptionButton(HWND topLevel, POINT pt);

/**
 * \brief Plain hit test without determining the rectangle.
 * \param topLevel Top-level window to query.
 * \param pt       Point in screen coordinates.
 * \return \c true if the window reports \c HTMINBUTTON.
 */
bool IsOverMinimizeButton(HWND topLevel, POINT pt);

/**
 * \brief Sends \c WM_NCHITTEST and returns the raw answer.
 *
 * Only interesting for the diagnosis: a window that reports \c HTCLIENT over
 * its own minimize button is the reason the computed path exists.
 *
 * \param topLevel Top-level window to query.
 * \param pt       Point in screen coordinates.
 * \return The \c HT* code, or \c HTNOWHERE if the window did not answer.
 */
LRESULT HitTestCode(HWND topLevel, POINT pt);

/**
 * \brief Reports windows that are skipped on principle.
 *
 * These are our own windows as well as shell windows (taskbar, desktop) and
 * anything that is invisible, cloaked or already destroyed.
 *
 * \param topLevel Window to check.
 * \return \c true if the window should be ignored.
 */
bool IsIgnoredWindow(HWND topLevel);

/**
 * \brief Reads the caption layout of a window from its styles.
 * \param topLevel Window to inspect.
 * \return Which caption buttons this window shows.
 */
CaptionLayout CaptionLayoutOf(HWND topLevel);

/**
 * \brief Asks DWM where it puts the caption buttons.
 *
 * \c DWMWA_CAPTION_BUTTON_BOUNDS reports the whole block in window-relative
 * coordinates. The documentation warns that the value is undefined for a window
 * that is not visible, so the result is validated against the window rectangle
 * before it is handed out.
 *
 * \param[in]  window Target window.
 * \param[out] out    The block in screen coordinates.
 * \return \c false if DWM has no answer or the answer is not plausible.
 */
bool CaptionBlockFromDwm(HWND window, RECT& out);

/**
 * \brief Reads the title bar strip of a window.
 *
 * \c GetTitleBarInfo is a plain API, not a message: it reads what the window
 * manager itself keeps and therefore also works across process boundaries.
 *
 * \param[in]  window Target window.
 * \param[out] out    The title bar in screen coordinates.
 * \return \c false if the window manager knows no title bar for this window.
 */
bool TitleBarStrip(HWND window, RECT& out);

/**
 * \brief Computes the minimize button without asking the window.
 *
 * Walks the three computed sources in order of precision, see
 * \ref ProbeSource.
 *
 * \param[in]  window Target window.
 * \param[in]  layout Which buttons the window shows.
 * \param[out] out    Minimize button in screen coordinates.
 * \param[out] source Which of the sources answered.
 * \return \c false if none of them produced a rectangle.
 */
bool ComputeMinimizeButton(HWND window, const CaptionLayout& layout,
                           RECT& out, ProbeSource& source);

}  // namespace mfly
