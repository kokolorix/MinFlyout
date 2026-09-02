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
    WM_MFLY_CONFIG    = WM_APP + 8,  ///< Configuration file was saved (posted by the ConfigWatcher).
    WM_MFLY_MOUSEUP   = WM_APP + 9,  ///< Mouse button released (posted by the HookThread).
    WM_MFLY_DRAGSTART = WM_APP + 10, ///< A foreign window entered its move/size loop (\c lParam = HWND).
    WM_MFLY_DRAGEND   = WM_APP + 11, ///< That loop ended (\c lParam = HWND).
    WM_MFLY_WHEEL     = WM_APP + 12, ///< Wheel turned (\c wParam = +1 up, -1 down).
    WM_MFLY_TOOL      = WM_APP + 13, ///< Flyout reports the chosen resize button (\c wParam = index).
};

/** \name Modifier keys of a mouse event
 *
 *  The low-level hook records which modifiers were held when a button went
 *  down, rather than leaving the question to be asked later: by the time the
 *  message is handled the user may well have let go of the key, and a Ctrl+click
 *  would then arrive as a plain one.
 *  @{ */
constexpr UINT32 kModNone  = 0x0;  ///< No modifier was held.
constexpr UINT32 kModCtrl  = 0x1;  ///< Ctrl was held.
constexpr UINT32 kModShift = 0x2;  ///< Shift was held.
constexpr UINT32 kModAlt   = 0x4;  ///< Alt was held.
/** @} */

/** \name Default values for timings (milliseconds)
 *  Can be changed through the configuration file.
 *  @{ */
constexpr UINT kHoverDelayMs   = 350;  ///< Hover delay on the button before the flyout opens.
constexpr UINT kCloseGraceMs   = 260;  ///< Grace period before the flyout closes again.
constexpr UINT kTouchDwellMs   = 250;  ///< Rest in the touch trigger field before the zones unfold.
/** @} */

/** \name Touch and pen input
 *
 *  Windows promotes pen and touch contacts to ordinary mouse messages and marks
 *  them in the extra information of the message - \c MSLLHOOKSTRUCT::dwExtraInfo
 *  in a low-level hook, \c GetMessageExtraInfo in a window procedure. The
 *  signature and the mask are the ones documented under "Distinguishing Pen
 *  Input from Mouse and Touch"; bit 7 of the low byte separates touch from pen.
 *
 *  \note The signature is the only way to tell a finger from a mouse in the
 *        hook, and it is worth verifying on the target machine rather than
 *        trusting. \c "traceDetection" in a debug build logs what a press
 *        carried; \c "alsoMouse" makes the touch route work regardless, which
 *        is the fallback if the marker ever turns out not to arrive.
 *  @{ */
constexpr ULONG_PTR kPenTouchSignature = 0xFF515700;  ///< Marker Windows stamps on promoted input.
constexpr ULONG_PTR kPenTouchMask      = 0xFFFFFF00;  ///< Mask for \ref kPenTouchSignature.
constexpr ULONG_PTR kTouchBit          = 0x80;        ///< Set for touch, clear for the pen.

/**
 * \brief Reports whether a mouse event was promoted from pen or touch.
 * \param extraInfo \c MSLLHOOKSTRUCT::dwExtraInfo of the event.
 * \return \c true for a contact, \c false for a real mouse.
 */
inline bool FromPenOrTouch(ULONG_PTR extraInfo) {
    return (extraInfo & kPenTouchMask) == kPenTouchSignature;
}

/**
 * \brief Reports whether a mouse event came from a finger rather than the pen.
 * \param extraInfo \c MSLLHOOKSTRUCT::dwExtraInfo of the event.
 * \return \c true only for touch.
 */
inline bool FromTouch(ULONG_PTR extraInfo) {
    return FromPenOrTouch(extraInfo) && (extraInfo & kTouchBit) != 0;
}
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

/** \name Size factor of the flyout
 *  @{ */
constexpr double kMinUiScale = 0.75;  ///< Smallest accepted Config::uiScale.
constexpr double kMaxUiScale = 3.00;  ///< Largest accepted Config::uiScale.
/** @} */

/**
 * \brief Folds the user's size factor into a DPI value.
 *
 * Every metric of the flyout already goes through \ref Scale, so enlarging it
 * is a matter of enlarging the DPI it is laid out for - fonts, paddings, corner
 * radii and the miniatures all follow, and no metric constant has to know about
 * the factor. That is also why the factor cannot pull anything out of
 * proportion: it is the same multiplication a higher-resolution screen applies.
 *
 * \param dpi    The real DPI of the target monitor.
 * \param factor Config::uiScale; clamped to \ref kMinUiScale ... \ref kMaxUiScale,
 *               and a value that is not a number leaves the DPI alone.
 * \return The DPI to lay out for; never 0, so \ref Scale cannot collapse.
 */
UINT ScaledDpi(UINT dpi, double factor);

/**
 * \brief Determines the DPI of a window.
 *
 * Uses \c GetDpiForWindow, falls back to \c GetDpiForMonitor and finally to the
 * screen DC, so that the application also starts on older systems.
 *
 * \warning This is the DPI the window *thinks* in - its DPI awareness context.
 *          For our own windows that is the screen's DPI; for a foreign one it
 *          need not be, and for an application that is not per-monitor aware it
 *          is 96 however large DWM draws it. Anything measured against physical
 *          pixels wants \ref DpiForWindowMonitor instead.
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

/**
 * \brief Determines the DPI of the screen a window is drawn on.
 *
 * Not the same thing as \ref DpiForWindowCompat, and the difference matters for
 * every foreign window: \c GetDpiForWindow answers with the DPI that window
 * *thinks* in, which is its DPI awareness context, not its screen. An
 * application that is not per-monitor aware - an older MFC program, say - is
 * told 96 on a 175 % display and drawn by DWM at 175 % anyway. Asking it
 * therefore yields a number that describes nobody's pixels.
 *
 * MinFlyout is \c PerMonitorV2 and reads physical coordinates, so wherever it
 * reasons about what is actually on the glass - how tall that title bar is, how
 * wide those caption buttons are, how large to draw its own flyout beside them -
 * the screen's DPI is the right one. \ref DpiForWindowCompat stays correct for
 * our own windows.
 *
 * \param window Window to locate (may be \c nullptr - then the primary screen).
 * \return DPI value, at least 96.
 */
UINT DpiForWindowMonitor(HWND window);

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
