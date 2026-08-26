/**
 * \file CaptionProbe.cpp
 * \ingroup detect
 * \brief Hit test, edge search and the computed fallbacks.
 */
#include "CaptionProbe.h"

#include <algorithm>

#include "Log.h"

namespace mfly {
namespace {

/// Converts a \c RECT into the Windows-free \ref RectI.
RectI ToRectI(const RECT& r) {
    return RectI{r.left, r.top, r.right, r.bottom};
}

/// Converts a \ref RectI back into a \c RECT.
RECT ToRect(const RectI& r) {
    return RECT{r.left, r.top, r.right, r.bottom};
}

/**
 * \brief Sends \c WM_NCHITTEST to a foreign window.
 * \param hwnd Target window.
 * \param pt   Point in screen coordinates.
 * \return The \c HT* code, or \c HTNOWHERE if the window did not answer.
 */
LRESULT NcHitTest(HWND hwnd, POINT pt) {
    DWORD_PTR result = 0;
    LPARAM lp = MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y));
    LRESULT ok = ::SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, lp,
                                       SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                                       kSendTimeoutMs, &result);
    return ok ? static_cast<LRESULT>(result) : HTNOWHERE;
}

/**
 * \brief Searches for the last coordinate that still returns \c HTMINBUTTON.
 *
 * Coarse linear scan in 4-pixel steps, then binary refinement between the last
 * hit and the first miss.
 *
 * \param hwnd   Target window.
 * \param inside A point known to lie inside the button.
 * \param dx     Horizontal direction (-1, 0 or +1).
 * \param dy     Vertical direction (-1, 0 or +1).
 * \param limit  Hard bound, normally the window edge.
 * \return The last coordinate belonging to the button.
 */
int FindEdge(HWND hwnd, POINT inside, int dx, int dy, int limit) {
    const bool horizontal = (dx != 0);
    const int step = 4 * (horizontal ? dx : dy);
    const int start = horizontal ? inside.x : inside.y;

    int lastHit = start;
    int firstMiss = limit;

    for (int v = start + step;
         (step > 0) ? (v <= limit) : (v >= limit);
         v += step) {
        POINT p = inside;
        (horizontal ? p.x : p.y) = v;
        if (NcHitTest(hwnd, p) == HTMINBUTTON) {
            lastHit = v;
        } else {
            firstMiss = v;
            break;
        }
    }

    while (std::abs(firstMiss - lastHit) > 1) {
        int mid = lastHit + (firstMiss - lastHit) / 2;
        POINT p = inside;
        (horizontal ? p.x : p.y) = mid;
        if (NcHitTest(hwnd, p) == HTMINBUTTON) {
            lastHit = mid;
        } else {
            firstMiss = mid;
        }
    }
    return lastHit;
}

/**
 * \brief Checks whether a window is hidden by DWM.
 * \param window Window to check.
 * \return \c true for a cloaked window - a UWP app suspended in the background.
 */
bool IsCloaked(HWND window) {
    DWORD cloaked = 0;
    if (!DwmWindowAttribute(window, kDwmCloaked, &cloaked, sizeof(cloaked))) return false;
    return cloaked != 0;
}

}  // namespace

bool TitleBarStrip(HWND window, RECT& out) {
    TITLEBARINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetTitleBarInfo(window, &info)) return false;
    if ((info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0) return false;

    const RECT& bar = info.rcTitleBar;
    if (bar.right <= bar.left || bar.bottom <= bar.top) return false;

    out = bar;
    return true;
}

bool CaptionBlockFromDwm(HWND window, RECT& out) {
    RECT relative{};
    if (!DwmWindowAttribute(window, kDwmCaptionButtonBounds, &relative, sizeof(relative))) {
        return false;
    }
    if (relative.right <= relative.left || relative.bottom <= relative.top) return false;

    RECT windowRect{};
    if (!::GetWindowRect(window, &windowRect)) return false;

    RECT screen{windowRect.left + relative.left, windowRect.top + relative.top,
                windowRect.left + relative.right, windowRect.top + relative.bottom};

    // Plausibility: the block belongs into the upper part of the window and
    // must not be wider than the window itself.
    if (screen.left < windowRect.left || screen.right > windowRect.right) return false;
    if (screen.top < windowRect.top) return false;
    if (screen.bottom > windowRect.top + (windowRect.bottom - windowRect.top) / 2) return false;

    out = screen;
    return true;
}

bool ComputeMinimizeButton(HWND window, const CaptionLayout& layout,
                           RECT& out, ProbeSource& source) {
    int slot = 0;
    if (!layout.slotOf(CaptionButton::Minimize, slot)) return false;

    RectI button;

    // DWM knows the block itself, including its width.
    RECT block{};
    if (CaptionBlockFromDwm(window, block) &&
        CaptionButtonRect(ToRectI(block), layout, CaptionButton::Minimize, button)) {
        out = ToRect(button);
        source = ProbeSource::DwmBounds;
        return true;
    }

    // The title bar strip gives the exact edges; only the button width is
    // derived from its height. But a window with its own title bar keeps a
    // vestigial system caption that is far too flat - Visual Studio Code
    // reports 23 pixels where the drawn bar is 56 - and believing it would put
    // the buttons in the wrong place. Anything clearly below the system title
    // bar height is therefore not trusted.
    const int expected = TitleBarHeight(DpiForWindowCompat(window));
    RECT strip{};
    if (TitleBarStrip(window, strip) && (strip.bottom - strip.top) * 4 >= expected * 3) {
        const RectI bar = ToRectI(strip);
        const RectI fromStrip = EstimateCaptionBlock(bar, bar.height(), layout);
        if (CaptionButtonRect(fromStrip, layout, CaptionButton::Minimize, button)) {
            out = ToRect(button);
            source = ProbeSource::TitleBarInfo;
            return true;
        }
    }

    // Last resort: the visible frame and the system's caption height.
    RECT frame{};
    if (!VisibleFrame(window, frame)) return false;

    const RectI estimate = EstimateCaptionBlock(ToRectI(frame), expected, layout);
    if (!CaptionButtonRect(estimate, layout, CaptionButton::Minimize, button)) return false;

    out = ToRect(button);
    source = ProbeSource::Estimate;
    return true;
}

const wchar_t* ProbeSourceName(ProbeSource source) {
    switch (source) {
    case ProbeSource::HitTest:      return L"hit test";
    case ProbeSource::DwmBounds:    return L"DWM button bounds";
    case ProbeSource::TitleBarInfo: return L"title bar strip";
    case ProbeSource::Estimate:     return L"estimate";
    }
    return L"unknown";
}

bool ParseProbeMode(const std::wstring& text, ProbeMode& out) {
    if (::lstrcmpiW(text.c_str(), L"auto") == 0)     { out = ProbeMode::Auto; return true; }
    if (::lstrcmpiW(text.c_str(), L"hittest") == 0)  { out = ProbeMode::HitTestOnly; return true; }
    if (::lstrcmpiW(text.c_str(), L"computed") == 0) { out = ProbeMode::Computed; return true; }
    return false;
}

bool IsIgnoredWindow(HWND topLevel) {
    if (!topLevel || !::IsWindow(topLevel) || !::IsWindowVisible(topLevel)) return true;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(topLevel, &pid);
    if (pid == ::GetCurrentProcessId()) return true;

    wchar_t cls[64] = {};
    ::GetClassNameW(topLevel, cls, ARRAYSIZE(cls));
    static const wchar_t* kIgnored[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"Windows.UI.Core.CoreWindow", L"MinFlyout.Flyout", L"MinFlyout.Controller",
    };
    for (const wchar_t* name : kIgnored) {
        if (::lstrcmpiW(cls, name) == 0) return true;
    }
    return IsCloaked(topLevel);
}

CaptionLayout CaptionLayoutOf(HWND topLevel) {
    CaptionLayout layout;
    if (!topLevel || !::IsWindow(topLevel)) {
        layout.sysMenu = false;
        return layout;
    }

    const LONG_PTR style = ::GetWindowLongPtrW(topLevel, GWL_STYLE);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(topLevel, GWL_EXSTYLE);

    // Deliberately gated on neither WS_CAPTION nor WS_SYSMENU. An app that draws
    // its own title bar usually drops both and keeps the boxes - and those are
    // precisely the windows the computed path exists for. A child window on the
    // other hand never has caption buttons of its own.
    const bool child = (style & WS_CHILD) != 0;
    layout.sysMenu = !child && (style & WS_SYSMENU) != 0;
    layout.minimizeBox = !child && (style & WS_MINIMIZEBOX) != 0;
    layout.maximizeBox = !child && (style & WS_MAXIMIZEBOX) != 0;
    layout.contextHelp = (exStyle & WS_EX_CONTEXTHELP) != 0;
    layout.rightToLeft = (exStyle & WS_EX_LAYOUTRTL) != 0;
    return layout;
}

LRESULT HitTestCode(HWND topLevel, POINT pt) {
    if (!topLevel || !::IsWindow(topLevel)) return HTNOWHERE;
    return NcHitTest(topLevel, pt);
}

bool IsOverMinimizeButton(HWND topLevel, POINT pt) {
    if (!topLevel || IsIgnoredWindow(topLevel)) return false;
    return NcHitTest(topLevel, pt) == HTMINBUTTON;
}

bool MayBeCaptionButton(HWND topLevel, POINT pt) {
    if (!topLevel || !::IsWindow(topLevel)) return false;

    const CaptionLayout layout = CaptionLayoutOf(topLevel);
    int slot = 0;
    if (!layout.slotOf(CaptionButton::Minimize, slot)) return false;

    RECT frame{};
    if (!VisibleFrame(topLevel, frame)) return false;

    const int titleBar = TitleBarHeight(DpiForWindowCompat(topLevel));
    const RectI region = CaptionButtonRegion(ToRectI(frame), titleBar, layout);
    return region.contains(pt.x, pt.y);
}

bool ProbeMinimizeButton(POINT pt, ProbeMode mode, HitInfo& out) {
    HWND under = ::WindowFromPoint(pt);
    if (!under) return false;

    HWND root = ::GetAncestor(under, GA_ROOT);
    if (!root || IsIgnoredWindow(root)) return false;
    if (::IsIconic(root)) return false;

    RECT windowRect{};
    if (!::GetWindowRect(root, &windowRect)) return false;

    // Asking: exact, but only for windows that answer.
    if (mode != ProbeMode::Computed && NcHitTest(root, pt) == HTMINBUTTON) {
        const int left   = FindEdge(root, pt, -1, 0, windowRect.left);
        const int right  = FindEdge(root, pt, +1, 0, windowRect.right);
        const int top    = FindEdge(root, pt, 0, -1, windowRect.top);
        const int bottom = FindEdge(root, pt, 0, +1, windowRect.bottom);

        const UINT dpi = DpiForPoint(pt);
        const bool plausible = right > left && bottom > top &&
                               (right - left) <= Scale(160, dpi) &&
                               (bottom - top) <= Scale(90, dpi);
        if (plausible) {
            out.window = root;
            out.buttonRect = RECT{left, top, right + 1, bottom + 1};
            out.windowRect = windowRect;
            out.source = ProbeSource::HitTest;
            return true;
        }
    }
    if (mode == ProbeMode::HitTestOnly) return false;

    // Computing: works even when the window says nothing. Only from here on does
    // the cheap region test apply - it must never stand in front of the hit
    // test above, or a wrong guess about the geometry would hide a window that
    // answers perfectly well.
    if (!MayBeCaptionButton(root, pt)) return false;

    // The point has to fall into the computed rectangle - otherwise the cursor
    // is somewhere else in the caption region, on a tab or in the search box of
    // a modern title bar.
    const CaptionLayout layout = CaptionLayoutOf(root);
    RECT button{};
    ProbeSource source = ProbeSource::Estimate;
    if (!ComputeMinimizeButton(root, layout, button, source)) return false;

    // A little slack downwards for the decision only, not for the rectangle
    // that is handed on: an app with its own title bar often draws its buttons
    // a few pixels taller than the system does. Sideways there is no slack -
    // that space belongs to the maximize button.
    RECT generous = button;
    generous.bottom += TitleBarHeight(DpiForWindowCompat(root)) / 3;
    if (!PtInRectPt(generous, pt)) return false;

    out.window = root;
    out.buttonRect = button;
    out.windowRect = windowRect;
    out.source = source;
    return true;
}

}  // namespace mfly
