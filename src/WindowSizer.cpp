/**
 * \file WindowSizer.cpp
 * \ingroup config
 * \brief Conversion of percentage values into screen coordinates, and back.
 */
#include "WindowSizer.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>

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

/**
 * \brief Formats one percentage the way the configuration file spells it.
 *
 * Two decimals, then trailing zeros and a bare decimal point are dropped: a
 * window on an exact half yields \c "50" and not \c "50.00". \c std::format
 * without \c {:L} is locale-independent, so the separator stays a point even
 * where the user's locale spells it as a comma - which JSON would not accept.
 *
 * \param value Percentage.
 * \return The number as text.
 */
std::wstring Percent(double value) {
    std::wstring text = std::format(L"{:.2f}", value);
    if (text.find(L'.') == std::wstring::npos) return text;

    while (!text.empty() && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

/**
 * \brief How far a frame may miss an edge and still count as sitting on it.
 *
 * A window placed flush by \ref ApplyZone lands exactly on the edge, but one
 * the user dragged there by hand is a pixel or two off, and the horizontal
 * maximize toggle has to recognise both as "already at full width".
 */
constexpr LONG kEdgeTolerance = 2;

/// Largest number of windows \ref WideStore keeps a frame for.
constexpr size_t kWideStoreLimit = 64;

/**
 * \brief Reference area of the screen a window sits on.
 * \param[in]  window      The window.
 * \param[in]  useWorkArea \c true excludes the taskbar.
 * \param[out] out         The area in screen coordinates.
 * \return \c false if the monitor could not be determined.
 */
bool ReferenceArea(HWND window, bool useWorkArea, RECT& out) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
        return false;
    }
    out = useWorkArea ? info.rcWork : info.rcMonitor;
    return true;
}

/**
 * \brief Positions the *visible* frame of a window.
 *
 * The counterpart of \ref VisibleFrame, and the same correction \ref ApplyZone
 * makes: what the caller hands in is what ends up drawn on the screen, with the
 * invisible shadow border added back on the way to \c SetWindowPos.
 *
 * \param window Target window.
 * \param frame  Desired visible frame in screen coordinates.
 * \return \c true if \c SetWindowPos was accepted.
 */
bool PlaceVisibleFrame(HWND window, const RECT& frame) {
    const RECT pad = InvisibleBorders(window);
    return ::SetWindowPos(window, nullptr,
                          frame.left - pad.left, frame.top - pad.top,
                          (frame.right - frame.left) + pad.left + pad.right,
                          (frame.bottom - frame.top) + pad.top + pad.bottom,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != FALSE;
}

/**
 * \brief The frames \ref MaximizeHorizontally has to be able to restore.
 *
 * A window property on the target would be the tidier place, but setting one on
 * a foreign window means leaving something of ours behind in a process we do
 * not control - and this application makes a point of not doing that. So the
 * frames stay here, keyed by window, and \ref ForgetDeadResizeState clears out
 * what has been closed.
 *
 * \return The process-wide store.
 */
std::map<HWND, RECT>& WideStore() {
    static std::map<HWND, RECT> store;
    return store;
}

/// Drops every entry of \ref WideStore whose window no longer exists.
void PurgeWideStore() {
    std::map<HWND, RECT>& store = WideStore();
    for (auto it = store.begin(); it != store.end();) {
        it = ::IsWindow(it->first) ? std::next(it) : store.erase(it);
    }
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

bool ZoneFromWindow(HWND window, bool useWorkArea, Zone& zone, HMONITOR* monitor) {
    if (!window || !::IsWindow(window)) return false;

    // A minimized window keeps the rectangle it had before, but the frame DWM
    // reports for it is meaningless - measuring it would hand out a zone that
    // has nothing to do with what the user sees.
    if (::IsIconic(window)) return false;

    HMONITOR mon = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(mon, &info)) {
        WRITE_WARNING_LOG(L"GetMonitorInfo failed while capturing a zone",
                          log::Describe(window));
        return false;
    }

    // The counterpart of the invisible-border correction in ApplyZone: that one
    // positions the visible frame, so this one measures the visible frame.
    RECT frame{};
    if (!VisibleFrame(window, frame)) return false;

    const RECT area = useWorkArea ? info.rcWork : info.rcMonitor;
    const double areaWidth = static_cast<double>(area.right - area.left);
    const double areaHeight = static_cast<double>(area.bottom - area.top);
    if (areaWidth <= 0.0 || areaHeight <= 0.0) return false;

    // Same ranges the parser clamps a loaded zone to, so a captured line comes
    // back out of the file exactly as it went in.
    zone.left   = std::clamp((frame.left - area.left) * 100.0 / areaWidth, 0.0, 99.0);
    zone.top    = std::clamp((frame.top - area.top) * 100.0 / areaHeight, 0.0, 99.0);
    zone.width  = std::clamp((frame.right - frame.left) * 100.0 / areaWidth, 1.0, 100.0);
    zone.height = std::clamp((frame.bottom - frame.top) * 100.0 / areaHeight, 1.0, 100.0);

    if (monitor) *monitor = mon;

    WRITE_DEBUG_LOG(log::dformat(L"Zone captured: {:.2f},{:.2f} {:.2f}x{:.2f} %",
                                 zone.left, zone.top, zone.width, zone.height),
                    log::dformat(L"frame {},{},{},{}  area {},{},{},{}",
                                 frame.left, frame.top, frame.right, frame.bottom,
                                 area.left, area.top, area.right, area.bottom));
    return true;
}

std::wstring FormatZoneEntry(const Zone& zone) {
    // Eight blanks of indentation and the trailing comma are what the "zones"
    // arrays of the template look like - the line is meant to be pasted, not
    // reformatted afterwards.
    return std::format(L"        {{ \"left\": {}, \"top\": {}, "
                       L"\"width\": {}, \"height\": {} }},\r\n",
                       Percent(zone.left), Percent(zone.top),
                       Percent(zone.width), Percent(zone.height));
}

// --- Step resizing ---------------------------------------------------------

const wchar_t* ResizeCommandName(ResizeCommand command) {
    switch (command) {
    case ResizeCommand::Grow:               return L"Larger";
    case ResizeCommand::Shrink:             return L"Smaller";
    case ResizeCommand::GrowWidth:          return L"Wider";
    case ResizeCommand::ShrinkWidth:        return L"Narrower";
    case ResizeCommand::GrowHeight:         return L"Taller";
    case ResizeCommand::ShrinkHeight:       return L"Shorter";
    case ResizeCommand::MaximizeHorizontal: return L"Full width";
    }
    return L"";
}

bool ResizeWindow(HWND window, int step, SizeEdge edges, bool useWorkArea,
                  RECT* edgeShift) {
    // Cleared first and written once, at the end: every early return below then
    // reports "nothing moved" without having to remember to say so.
    if (edgeShift) *edgeShift = RECT{0, 0, 0, 0};

    if (!window || !::IsWindow(window) || step == 0 || edges == SizeEdge::None) return false;
    if (!IsResizable(window)) {
        WRITE_DEBUG_LOG(L"Step resize refused: the window has no sizing border",
                        log::Describe(window));
        return false;
    }

    // A minimized window has no frame worth changing, and its rectangle says
    // nothing about where the user last put it.
    if (::IsIconic(window)) return false;

    // A maximized window is already as large as it gets, so growing is a no-op.
    // Shrinking means "leave that state" - and it is left the ordinary way, by
    // restoring, rather than by inventing a rectangle out of the maximized one.
    // The next step then works on the restored frame.
    if (::IsZoomed(window)) {
        if (step > 0) return false;
        ::ShowWindowAsync(window, SW_RESTORE);
        WRITE_DEBUG_LOG(L"Step resize restored a maximized window", log::Describe(window));
        return true;
    }

    RECT frame{};
    RECT area{};
    if (!VisibleFrame(window, frame) || !ReferenceArea(window, useWorkArea, area)) {
        return false;
    }

    RECT wanted = frame;
    if (HasEdge(edges, SizeEdge::Left))   wanted.left   -= step;
    if (HasEdge(edges, SizeEdge::Right))  wanted.right  += step;
    if (HasEdge(edges, SizeEdge::Top))    wanted.top    -= step;
    if (HasEdge(edges, SizeEdge::Bottom)) wanted.bottom += step;

    // Growing walks the window up to the edge of its screen and stops there,
    // rather than pushing part of it out of sight. Only the edges that were
    // asked to move are clamped, so shrinking away from an edge still works
    // when the window hangs over one.
    if (step > 0) {
        if (HasEdge(edges, SizeEdge::Left))   wanted.left   = std::max(wanted.left, area.left);
        if (HasEdge(edges, SizeEdge::Right))  wanted.right  = std::min(wanted.right, area.right);
        if (HasEdge(edges, SizeEdge::Top))    wanted.top    = std::max(wanted.top, area.top);
        if (HasEdge(edges, SizeEdge::Bottom)) wanted.bottom = std::min(wanted.bottom, area.bottom);
    }

    // The window manager enforces its minimum tracking size anyway; doing it
    // here keeps a shrink from producing a rectangle that is silently corrected
    // into something else - or, with a large step on a small window, from
    // inverting altogether.
    //
    // The comparison is against the current size and not against the minimum
    // alone, and that is the whole point of it: a little tool window without a
    // caption may already be narrower than SM_CXMIN, and the plain test would
    // then refuse to make it taller as well. What is refused is making an
    // undersized dimension smaller, never leaving it as it is.
    const LONG minWidth = ::GetSystemMetrics(SM_CXMIN);
    const LONG minHeight = ::GetSystemMetrics(SM_CYMIN);
    const LONG wantedWidth = wanted.right - wanted.left;
    const LONG wantedHeight = wanted.bottom - wanted.top;
    if ((wantedWidth < minWidth && wantedWidth < frame.right - frame.left) ||
        (wantedHeight < minHeight && wantedHeight < frame.bottom - frame.top)) {
        WRITE_DEBUG_LOG(L"Step resize refused: the window would fall below its minimum size",
                        log::Describe(window));
        return false;
    }

    if (::EqualRect(&wanted, &frame)) return false;  // already against the edges

    // A window covering the whole area is maximized properly rather than left
    // as a normal window that happens to look maximized - otherwise the caption
    // buttons keep saying "maximize" for a window that already fills the screen.
    if (step > 0 &&
        wanted.left <= area.left + kEdgeTolerance &&
        wanted.top <= area.top + kEdgeTolerance &&
        wanted.right >= area.right - kEdgeTolerance &&
        wanted.bottom >= area.bottom - kEdgeTolerance &&
        (::GetWindowLongPtrW(window, GWL_STYLE) & WS_MAXIMIZEBOX) != 0) {
        ::ShowWindowAsync(window, SW_MAXIMIZE);
        WRITE_DEBUG_LOG(L"Step resize filled the screen and maximized instead",
                        log::Describe(window));
        return true;
    }

    const bool ok = PlaceVisibleFrame(window, wanted);
    if (ok) {
        // What the edges did, not what they were asked to do: one stopped by
        // the screen has moved less than a full step, and a pointer following
        // it has to stop with it.
        if (edgeShift) {
            *edgeShift = RECT{wanted.left - frame.left, wanted.top - frame.top,
                              wanted.right - frame.right, wanted.bottom - frame.bottom};
        }
        WRITE_DEBUG_LOG(log::dformat(L"Step resize {:+} px: {},{} {}x{}", step,
                                     wanted.left, wanted.top,
                                     wanted.right - wanted.left,
                                     wanted.bottom - wanted.top),
                        log::Describe(window));
    } else {
        WRITE_ERROR_LOG(log::dformat(L"SetWindowPos failed, error {}", ::GetLastError()),
                        log::Describe(window));
    }
    return ok;
}

bool MaximizeHorizontally(HWND window, bool useWorkArea) {
    if (!window || !::IsWindow(window) || ::IsIconic(window)) return false;
    if (!IsResizable(window)) return false;

    std::map<HWND, RECT>& store = WideStore();

    // A maximized window is at full width already, and the honest answer to
    // "toggle that" is the same one Windows gives for its own vertical maximize:
    // put the window back.
    if (::IsZoomed(window)) {
        store.erase(window);
        ::ShowWindowAsync(window, SW_RESTORE);
        return true;
    }

    RECT frame{};
    RECT area{};
    if (!VisibleFrame(window, frame) || !ReferenceArea(window, useWorkArea, area)) {
        return false;
    }

    const bool alreadyWide = frame.left <= area.left + kEdgeTolerance &&
                             frame.right >= area.right - kEdgeTolerance;

    if (alreadyWide) {
        const auto it = store.find(window);
        if (it != store.end()) {
            const RECT back = it->second;
            store.erase(it);
            WRITE_DEBUG_LOG(L"Full width undone", log::Describe(window));
            return PlaceVisibleFrame(window, back);
        }
        // At full width, but not by our doing - there is nothing to go back to,
        // so the command has nothing left to change.
        return false;
    }

    if (store.size() >= kWideStoreLimit) PurgeWideStore();
    store[window] = frame;

    const RECT wide{area.left, frame.top, area.right, frame.bottom};
    WRITE_DEBUG_LOG(log::dformat(L"Full width: {} px", wide.right - wide.left),
                    log::Describe(window));
    return PlaceVisibleFrame(window, wide);
}

bool ApplyResizeCommand(HWND window, ResizeCommand command, int step, bool useWorkArea) {
    switch (command) {
    case ResizeCommand::Grow:
        return ResizeWindow(window, step, SizeEdge::All, useWorkArea);
    case ResizeCommand::Shrink:
        return ResizeWindow(window, -step, SizeEdge::All, useWorkArea);
    case ResizeCommand::GrowWidth:
        return ResizeWindow(window, step, SizeEdge::Horizontal, useWorkArea);
    case ResizeCommand::ShrinkWidth:
        return ResizeWindow(window, -step, SizeEdge::Horizontal, useWorkArea);
    case ResizeCommand::GrowHeight:
        return ResizeWindow(window, step, SizeEdge::Vertical, useWorkArea);
    case ResizeCommand::ShrinkHeight:
        return ResizeWindow(window, -step, SizeEdge::Vertical, useWorkArea);
    case ResizeCommand::MaximizeHorizontal:
        return MaximizeHorizontally(window, useWorkArea);
    }
    return false;
}

POINT CursorShiftForHitTest(LRESULT hitTest, const RECT& edgeShift) {
    POINT shift{0, 0};
    switch (hitTest) {
    case HTLEFT:        shift.x = edgeShift.left;  break;
    case HTRIGHT:       shift.x = edgeShift.right; break;
    case HTTOP:         shift.y = edgeShift.top;    break;
    case HTBOTTOM:      shift.y = edgeShift.bottom; break;
    case HTTOPLEFT:     shift.x = edgeShift.left;  shift.y = edgeShift.top;    break;
    case HTTOPRIGHT:    shift.x = edgeShift.right; shift.y = edgeShift.top;    break;
    case HTBOTTOMLEFT:  shift.x = edgeShift.left;  shift.y = edgeShift.bottom; break;
    case HTBOTTOMRIGHT:
    case HTSIZE:        shift.x = edgeShift.right; shift.y = edgeShift.bottom; break;
    default: break;
    }
    return shift;
}

void ForgetDeadResizeState() {
    PurgeWideStore();
}

}  // namespace mfly
