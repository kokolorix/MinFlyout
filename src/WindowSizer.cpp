/**
 * \file WindowSizer.cpp
 * \ingroup config
 * \brief Conversion of percentage values into screen coordinates.
 */
#include "WindowSizer.h"

#include <algorithm>

#include "Log.h"

namespace mfly {
namespace {

/**
 * \brief Determines the invisible border widths of a window.
 *
 * For composited windows the window rectangle is larger than the visibly
 * drawn frame. Without this correction a visible gap would remain between two
 * windows placed side by side.
 *
 * \param window Target window.
 * \return Overhang per side in pixels (0 if it cannot be determined).
 */
RECT InvisibleBorders(HWND window) {
    RECT padding{0, 0, 0, 0};

    RECT windowRect{};
    RECT frame{};
    if (!::GetWindowRect(window, &windowRect)) return padding;
    if (!DwmWindowAttribute(window, kDwmExtendedFrameBounds, &frame, sizeof(frame))) {
        return padding;
    }

    padding.left   = frame.left - windowRect.left;
    padding.top    = frame.top - windowRect.top;
    padding.right  = windowRect.right - frame.right;
    padding.bottom = windowRect.bottom - frame.bottom;

    // Discard implausible values (for example for windows without composition).
    if (padding.left < 0 || padding.top < 0 || padding.right < 0 || padding.bottom < 0 ||
        padding.left > 32 || padding.top > 32 || padding.right > 32 || padding.bottom > 32) {
        return RECT{0, 0, 0, 0};
    }
    return padding;
}

}  // namespace

bool IsResizable(HWND window) {
    if (!window || !::IsWindow(window)) return false;
    return (::GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME) != 0;
}

bool ApplyZone(HWND window, const Zone& zone, HMONITOR monitor, bool useWorkArea) {
    if (!window || !::IsWindow(window)) {
        WRITE_WARNING_LOG(L"Zone applied to an invalid window");
        return false;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor) monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!::GetMonitorInfoW(monitor, &info)) {
        WRITE_ERROR_LOG(L"GetMonitorInfo failed", log::Describe(window));
        return false;
    }
    const RECT area = useWorkArea ? info.rcWork : info.rcMonitor;
    const int areaWidth = area.right - area.left;
    const int areaHeight = area.bottom - area.top;

    // A window cannot be positioned sensibly while it is maximized or
    // minimized.
    if (::IsZoomed(window) || ::IsIconic(window)) {
        ::ShowWindow(window, SW_RESTORE);
    }

    const int width = std::max(1, static_cast<int>(areaWidth * zone.width / 100.0 + 0.5));
    const int height = std::max(1, static_cast<int>(areaHeight * zone.height / 100.0 + 0.5));
    const int originX = area.left + static_cast<int>(areaWidth * zone.left / 100.0 + 0.5);
    const int originY = area.top + static_cast<int>(areaHeight * zone.top / 100.0 + 0.5);

    // Align the visible edges flush: compensate for the invisible border.
    const RECT pad = InvisibleBorders(window);
    const int x = originX - pad.left;
    const int y = originY - pad.top;
    const int w = width + pad.left + pad.right;
    const int h = height + pad.top + pad.bottom;

    const bool ok = ::SetWindowPos(window, nullptr, x, y, w, h,
                                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != FALSE;
    if (ok) {
        WRITE_DEBUG_LOG(log::dformat(L"Zone applied: {},{} {}x{} (border {},{},{},{})",
                                     x, y, w, h,
                                     pad.left, pad.top, pad.right, pad.bottom),
                        log::Describe(window));
    } else {
        WRITE_ERROR_LOG(log::dformat(L"SetWindowPos failed, error {}", ::GetLastError()),
                        log::Describe(window));
    }
    return ok;
}

}  // namespace mfly
