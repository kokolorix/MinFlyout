/**
 * \file Painting.cpp
 * \ingroup ui
 * \brief Implementation of the shared drawing helpers.
 */
#include "Painting.h"

#include <algorithm>

namespace mfly {

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
        p.zone       = RGB(214, 214, 214);
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

}  // namespace mfly
