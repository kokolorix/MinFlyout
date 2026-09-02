/**
 * \file Painting.cpp
 * \ingroup ui
 * \brief Implementation of the shared drawing helpers.
 */
#include "Painting.h"

#include <algorithm>

namespace mfly {
namespace {

/**
 * \brief Fills a triangle in one flat color.
 * \param dc    Target device context.
 * \param a     First corner.
 * \param b     Second corner.
 * \param c     Third corner.
 * \param color Fill color.
 */
void FillTriangle(HDC dc, POINT a, POINT b, POINT c, COLORREF color) {
    const POINT points[3] = {a, b, c};
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, points, 3);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

/**
 * \brief Draws the arrow that spans the gap between a window edge and the screen.
 *
 * The triangle always fills the whole gap; only its orientation differs. Growing
 * puts the tip on the screen edge - "this edge is on its way out there" -
 * shrinking puts the tip on the window edge and the base outside, so the arrow
 * points at what is about to give way.
 *
 * \param dc      Target device context.
 * \param frame   The screen rectangle of the glyph.
 * \param win     The window rectangle inside it.
 * \param side    \c HTLEFT, \c HTRIGHT, \c HTTOP or \c HTBOTTOM.
 * \param outward \c true for growing.
 * \param half    Half the width of the triangle base.
 * \param color   Fill color.
 */
void DrawGapArrow(HDC dc, const RECT& frame, const RECT& win, int side,
                  bool outward, int half, COLORREF color) {
    const LONG cx = (win.left + win.right) / 2;
    const LONG cy = (win.top + win.bottom) / 2;

    LONG tip = 0, base = 0;
    switch (side) {
    case HTLEFT:   tip = outward ? frame.left   : win.left;   base = outward ? win.left   : frame.left;   break;
    case HTRIGHT:  tip = outward ? frame.right  : win.right;  base = outward ? win.right  : frame.right;  break;
    case HTTOP:    tip = outward ? frame.top    : win.top;    base = outward ? win.top    : frame.top;    break;
    default:       tip = outward ? frame.bottom : win.bottom; base = outward ? win.bottom : frame.bottom; break;
    }

    if (side == HTLEFT || side == HTRIGHT) {
        FillTriangle(dc, POINT{tip, cy}, POINT{base, cy - half}, POINT{base, cy + half}, color);
    } else {
        FillTriangle(dc, POINT{cx, tip}, POINT{cx - half, base}, POINT{cx + half, base}, color);
    }
}

}  // namespace

COLORREF AccentColor() {
    using PFN_DwmGetColorizationColor = HRESULT(WINAPI*)(DWORD*, BOOL*);
    static PFN_DwmGetColorizationColor fn = [] {
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<PFN_DwmGetColorizationColor>(
                         reinterpret_cast<void*>(
                             ::GetProcAddress(dwm, "DwmGetColorizationColor")))
                   : nullptr;
    }();

    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (fn && SUCCEEDED(fn(&argb, &opaque))) {
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    }
    return RGB(59, 130, 246);  // #3B82F6
}

Palette MakePalette(bool dark) {
    Palette p{};
    p.accent = AccentColor();
    if (dark) {
        p.background = RGB(43, 43, 43);
        p.border     = RGB(69, 69, 69);
        p.text       = RGB(255, 255, 255);
        p.textDim    = RGB(150, 150, 150);
        p.hover      = RGB(64, 64, 64);
        p.separator  = RGB(69, 69, 69);
        p.miniBack   = RGB(32, 32, 32);
        p.zone       = RGB(90, 90, 90);
    } else {
        p.background = RGB(249, 249, 249);
        p.border     = RGB(214, 214, 214);
        p.text       = RGB(26, 26, 26);
        p.textDim    = RGB(120, 120, 120);
        p.hover      = RGB(234, 234, 234);
        p.separator  = RGB(222, 222, 222);
        p.miniBack   = RGB(255, 255, 255);
        p.zone       = RGB(180, 180, 180);
    }
    return p;
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    // RoundRect leaves the last row/column out, hence the +1.
    ::RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void FrameRounded(HDC dc, const RECT& r, COLORREF color, int radius, int width) {
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

HFONT CreateUiFont(UINT dpi, bool bold, int delta) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    LOGFONTW lf = ncm.lfMessageFont;
    const int height = std::max<long>(9, std::abs(lf.lfHeight) + delta);
    lf.lfHeight = -MulDiv(height, static_cast<int>(dpi), 96);
    lf.lfWeight = bold ? FW_SEMIBOLD : lf.lfWeight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    HFONT f = ::CreateFontIndirectW(&lf);
    return f ? f : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

void DrawResizeGlyph(HDC dc, const RECT& box, ResizeCommand command,
                     COLORREF ink, COLORREF fill) {
    const int side = std::min<int>(box.right - box.left, box.bottom - box.top);
    if (side < 10) return;  // below this nothing of it would be readable anyway

    // Square, centred in whatever box the caller reserved for it.
    const LONG originX = box.left + (box.right - box.left - side) / 2;
    const LONG originY = box.top + (box.bottom - box.top - side) / 2;
    const RECT screen{originX, originY, originX + side, originY + side};

    // How large the window sits inside the screen, and which of its edges are
    // about to move. Two things say the same thing twice, which is what keeps
    // these readable at eighteen pixels: a small window with outward arrows
    // means "larger", a large one with inward arrows means "smaller". The
    // fractions are chosen so that the gap the arrows live in stays at least
    // four pixels wide in both cases.
    double fx = 0.60;
    double fy = 0.60;
    bool horizontal = false;
    bool vertical = false;
    bool outward = true;

    switch (command) {
    case ResizeCommand::Grow:
        fx = fy = 0.34; horizontal = vertical = true; outward = true;  break;
    case ResizeCommand::Shrink:
        fx = fy = 0.54; horizontal = vertical = true; outward = false; break;
    case ResizeCommand::GrowWidth:
        fx = 0.34; horizontal = true; outward = true;  break;
    case ResizeCommand::ShrinkWidth:
        fx = 0.54; horizontal = true; outward = false; break;
    case ResizeCommand::GrowHeight:
        fy = 0.34; vertical = true; outward = true;  break;
    case ResizeCommand::ShrinkHeight:
        fy = 0.54; vertical = true; outward = false; break;
    case ResizeCommand::MaximizeHorizontal:
        // Flush against both edges - which is what the command does, and needs
        // no arrow to say so.
        fx = 1.0; fy = 0.44; break;
    }

    const int w = std::max(2, static_cast<int>(side * fx + 0.5));
    const int h = std::max(2, static_cast<int>(side * fy + 0.5));
    const RECT win{screen.left + (side - w) / 2, screen.top + (side - h) / 2,
                   screen.left + (side - w) / 2 + w, screen.top + (side - h) / 2 + h};

    // The screen: a thin outline, so the window inside it has something to be
    // measured against.
    HPEN pen = ::CreatePen(PS_SOLID, 1, ink);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, screen.left, screen.top, screen.right, screen.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    HBRUSH body = ::CreateSolidBrush(fill);
    ::FillRect(dc, &win, body);
    ::DeleteObject(body);

    const int half = std::max(2, side / 6);
    const int gapX = win.left - screen.left;
    const int gapY = win.top - screen.top;

    if (horizontal && gapX >= 2) {
        DrawGapArrow(dc, screen, win, HTLEFT, outward, half, ink);
        DrawGapArrow(dc, screen, win, HTRIGHT, outward, half, ink);
    }
    if (vertical && gapY >= 2) {
        DrawGapArrow(dc, screen, win, HTTOP, outward, half, ink);
        DrawGapArrow(dc, screen, win, HTBOTTOM, outward, half, ink);
    }
}

}  // namespace mfly
