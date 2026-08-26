/**
 * \file Common.h
 * \ingroup app
 * \brief Shared types, constants and small helpers.
 */
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>

#include <functional>
#include <string>
#include <vector>

/// Namespace of all MinFlyout components.
namespace mfly {

/**
 * \brief Application-internal window messages of the controller.
 *
 * Everything coming from the low-level hooks or the flyout reaches the state
 * machine exclusively through these posted messages.
 */
enum : UINT {
    WM_MFLY_MOUSEMOVE = WM_APP + 1,  ///< Mouse moved (posted by the HookThread).
    WM_MFLY_MOUSEDOWN = WM_APP + 2,  ///< Mouse button pressed (posted by the HookThread).
    WM_MFLY_CANCEL    = WM_APP + 3,  ///< ESC pressed, close the flyout.
    WM_MFLY_INVOKE    = WM_APP + 4,  ///< Flyout reports the chosen item (\c wParam = index).
    WM_MFLY_CLOSED    = WM_APP + 5,  ///< Flyout asks to be closed (e.g. theme change).
    WM_MFLY_TRAY      = WM_APP + 6,  ///< Notification from a tray icon.
    WM_MFLY_ZONE      = WM_APP + 7,  ///< Flyout reports the chosen zone (\c wParam = hotspot index).
};

/** \name Default values for timings (milliseconds)
 *  Can be changed through the configuration file.
 *  @{ */
constexpr UINT kHoverDelayMs   = 350;  ///< Hover delay on the button before the flyout opens.
constexpr UINT kCloseGraceMs   = 260;  ///< Grace period before the flyout closes again.
/** @} */

/** \name Fixed timing constants (milliseconds)
 *  @{ */
constexpr UINT kProbeThrottle  = 40;   ///< Minimum interval between two hit-test probes.
constexpr UINT kSendTimeoutMs  = 60;   ///< Timeout for \c SendMessageTimeout to foreign windows.
/** @} */

/**
 * \brief Describes over which window the flyout is currently opening.
 *
 * Created once per open, handed to every provider and later passed on
 * unchanged to the chosen action.
 */
struct Context {
    HWND  targetWindow = nullptr;  ///< Top-level window whose minimize button is hovered.
    DWORD targetProcessId = 0;     ///< Process ID of the target window.
    DWORD targetThreadId = 0;      ///< UI thread of the target window.
    RECT  buttonRect{};            ///< Minimize button in screen coordinates.
    RECT  windowRect{};            ///< Target window in screen coordinates.
    UINT  dpi = 96;                ///< DPI of the monitor the flyout appears on.
};

/** \name Item flags
 *  Bit mask for Item::flags.
 *  @{ */
constexpr UINT32 kItemNone      = 0x0000u;  ///< Ordinary, clickable item.
constexpr UINT32 kItemDisabled  = 0x0001u;  ///< Drawn greyed out, not clickable.
constexpr UINT32 kItemSeparator = 0x0002u;  ///< Separator; text and action are ignored.
constexpr UINT32 kItemChecked   = 0x0004u;  ///< Check mark left of the text (state indicator).
constexpr UINT32 kItemDefault   = 0x0008u;  ///< Emphasized (semibold) as the default action.
/** @} */

/**
 * \brief An item in the flyout.
 */
struct Item {
    std::wstring text;                     ///< Label.
    UINT32       flags = kItemNone;        ///< Combination of the \c kItem* flags.
    std::function<void(const Context&)> action;  ///< Action on click (may be empty).

    /// \return \c true if the item is a separator.
    bool separator() const { return (flags & kItemSeparator) != 0; }
    /// \return \c true if the item is not clickable.
    bool disabled()  const { return (flags & kItemDisabled) != 0; }
    /// \return \c true if a check mark should be drawn.
    bool checked()   const { return (flags & kItemChecked) != 0; }
    /// \return \c true if the item is drawn emphasized.
    bool bold()      const { return (flags & kItemDefault) != 0; }
};

/**
 * \brief \c PtInRect with value semantics.
 * \param r Rectangle.
 * \param p Point.
 * \return \c true if \p p lies inside \p r.
 */
inline bool PtInRectPt(const RECT& r, POINT p) { return ::PtInRect(&r, p) != FALSE; }

/**
 * \brief Returns an inflated rectangle without modifying the original.
 * \param r  Source rectangle.
 * \param dx Horizontal growth per side.
 * \param dy Vertical growth per side.
 * \return The inflated rectangle.
 */
inline RECT InflateCopy(RECT r, int dx, int dy) {
    ::InflateRect(&r, dx, dy);
    return r;
}

/**
 * \brief Converts a value expressed at 96 dpi to the target resolution.
 *
 * All layout constants of the flyout are given in 96-dpi pixels and go through
 * this function.
 *
 * \param v   Value in 96-dpi pixels.
 * \param dpi Target DPI.
 * \return The scaled value.
 */
inline int Scale(int v, UINT dpi) {
    return MulDiv(v, static_cast<int>(dpi), 96);
}

/**
 * \brief Determines the DPI of a window.
 *
 * Uses \c GetDpiForWindow, falls back to \c GetDpiForMonitor and finally to the
 * screen DC, so that the application also starts on older systems.
 *
 * \param hwnd Window (may be \c nullptr).
 * \return DPI value, at least 96.
 */
UINT DpiForWindowCompat(HWND hwnd);

/**
 * \brief Determines the DPI of the monitor under a screen point.
 * \param pt Point in screen coordinates.
 * \return DPI value, at least 96.
 */
UINT DpiForPoint(POINT pt);

/** \name DWM window attributes
 *  The IDs from \c dwmapi.h; the function itself is resolved at run time.
 *  @{ */
constexpr DWORD kDwmCaptionButtonBounds = 5;  ///< \c DWMWA_CAPTION_BUTTON_BOUNDS.
constexpr DWORD kDwmExtendedFrameBounds = 9;  ///< \c DWMWA_EXTENDED_FRAME_BOUNDS.
constexpr DWORD kDwmCloaked             = 14; ///< \c DWMWA_CLOAKED.
/** @} */

/**
 * \brief Reads a DWM window attribute.
 *
 * \c DwmGetWindowAttribute is resolved once through \c GetProcAddress, so the
 * application also starts where \c dwmapi.dll is missing.
 *
 * \param[in]  window    Window to query.
 * \param[in]  attribute One of the \c kDwm* constants.
 * \param[out] value     Buffer for the result.
 * \param[in]  size      Size of the buffer in bytes.
 * \return \c false if the function is unavailable or the call failed.
 */
bool DwmWindowAttribute(HWND window, DWORD attribute, void* value, DWORD size);

/**
 * \brief Reads the visible frame of a window.
 *
 * For composited windows the window rectangle carries an invisible shadow
 * border; \c DWMWA_EXTENDED_FRAME_BOUNDS reports what is actually drawn. Falls
 * back to \c GetWindowRect where DWM has no answer.
 *
 * \param[in]  window Window to measure.
 * \param[out] frame  The visible frame in screen coordinates.
 * \return \c false if not even \c GetWindowRect worked.
 */
bool VisibleFrame(HWND window, RECT& frame);

/**
 * \brief Reads a system metric for a given DPI.
 *
 * Uses \c GetSystemMetricsForDpi where available and otherwise scales the
 * system-DPI value, so the result stays usable on a mixed-DPI desktop.
 *
 * \param index One of the \c SM_* constants.
 * \param dpi   Target DPI.
 * \return The metric in pixels of that DPI.
 */
int SystemMetricForDpi(int index, UINT dpi);

/**
 * \brief Height of the title bar including the upper frame edge.
 *
 * This is the strip DWM draws the caption buttons into: caption height plus
 * padded border, but never less than the 32 pixels at 100 % that Windows 11
 * documents for a standard title bar. The floor matters for
 * \ref EstimateCaptionBlock - too small a height would push the minimize slot
 * onto the maximize button.
 *
 * \param dpi Target DPI.
 * \return The height in pixels.
 */
int TitleBarHeight(UINT dpi);

/**
 * \brief Checks whether Windows apps currently run in dark mode.
 *
 * Reads \c AppsUseLightTheme under
 * \c HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize.
 *
 * \return \c true for dark mode.
 */
bool SystemUsesDarkTheme();

}  // namespace mfly
