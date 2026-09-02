/**
 * \file FlyoutWindow.cpp
 * \ingroup ui
 * \brief GDI layout and rendering of the flyout.
 */
#include "FlyoutWindow.h"

#include <algorithm>

#include "Log.h"
#include "OverlayGeometry.h"
#include "Painting.h"

namespace mfly {
namespace {

/** \name Metrics in 96-dpi pixels
 *  @{ */
constexpr int kPanelPad     = 12;  ///< Padding around the miniature area.
constexpr int kMiniWidth    = 104; ///< Width of one monitor miniature.
constexpr int kMiniGap      = 10;  ///< Gap between two miniatures.
constexpr int kZoneGap      = 2;   ///< Gap between two zone tiles.
constexpr int kCaptionH     = 15;  ///< Height of the caption below a miniature.
constexpr int kMonitorHeadH = 17;  ///< Height of the monitor caption.
constexpr int kRowGap       = 12;  ///< Gap between two monitor rows.
constexpr int kItemHeight   = 30;  ///< Height of one text item.
constexpr int kSepHeight    = 9;   ///< Height of a separator row.
constexpr int kItemPadX     = 12;  ///< Left/right padding of the item text.
constexpr int kCheckWidth   = 22;  ///< Width of the check mark column.
constexpr int kCornerRad    = 8;   ///< Corner radius of the window.
constexpr int kMiniRad      = 4;   ///< Corner radius of a miniature.
constexpr int kAnchorGap    = 4;   ///< Gap between button and flyout.
constexpr int kMinMiniH     = 40;  ///< Lower bound for very wide monitors.
constexpr int kToolSize     = 30;  ///< Edge length of one resize button.
constexpr int kToolGap      = 3;   ///< Gap between two resize buttons.
constexpr int kToolGlyphPad = 4;   ///< Padding between button edge and pictogram.
constexpr int kToolCaptionH = 16;  ///< Line below the toolbar naming the hovered button.
constexpr int kToolRad      = 4;   ///< Corner radius of a resize button.
/** @} */

/**
 * \brief Margin the flyout keeps to the edges of the work area, in real pixels.
 *
 * Not scaled: it exists so the window does not sit flush against a screen edge,
 * and that is a property of the screen rather than of the content.
 */
constexpr int kEdgeGap = 4;

/**
 * \brief Maps a zone onto a miniature.
 *
 * The arithmetic itself lives in OverlayGeometry.h, where it is free of Windows
 * types and covered by tests - and where the touch overlay reads it from as
 * well, so the tile drawn here and the same zone drawn across a whole screen
 * really are the same rectangle.
 *
 * \param mini Miniature rectangle in client coordinates.
 * \param zone Zone in percent.
 * \param gap  Gap that keeps the tiles apart.
 * \return The tile rectangle.
 */
RECT ZoneRect(const RECT& mini, const Zone& zone, int gap) {
    // RECT holds LONG, RectI holds int - the same width on Win32, so the casts
    // only make the crossing between the two visible.
    const RectI area{static_cast<int>(mini.left), static_cast<int>(mini.top),
                     static_cast<int>(mini.right), static_cast<int>(mini.bottom)};
    const PercentRect part{zone.left, zone.top, zone.width, zone.height};
    const RectI tile = ShrunkBy(PercentToPixels(area, part), gap);
    return RECT{tile.left, tile.top, tile.right, tile.bottom};
}

}  // namespace

bool FlyoutWindow::Create(HINSTANCE instance, HWND notifyTarget) {
    notify_ = notifyTarget;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FlyoutWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void FlyoutWindow::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    for (HFONT* f : {&font_, &fontBold_, &fontSmall_}) {
        if (*f) { ::DeleteObject(*f); *f = nullptr; }
    }
}

RECT FlyoutWindow::ScreenRect() const {
    RECT r{};
    if (hwnd_) ::GetWindowRect(hwnd_, &r);
    return r;
}

void FlyoutWindow::Measure(UINT dpi, SIZE& size) {
    hotspots_.clear();
    monitorRects_.clear();
    miniRects_.clear();
    miniFirst_.clear();
    zoneFirst_.clear();
    toolRects_.clear();
    toolCaption_ = RECT{};
    itemRects_.assign(content_.items.size(), RECT{});

    const int pad = Scale(kPanelPad, dpi);
    const int miniW = Scale(kMiniWidth, dpi);
    const int miniGap = Scale(kMiniGap, dpi);
    const int zoneGap = Scale(kZoneGap, dpi);
    const int captionH = Scale(kCaptionH, dpi);
    const int headH = Scale(kMonitorHeadH, dpi);
    const int rowGap = Scale(kRowGap, dpi);

    const bool showHead = content_.rows.size() > 1;

    // Width is driven by the widest miniature row; the item rows follow it.
    size_t widestRow = 0;
    for (const MonitorRow& row : content_.rows) {
        widestRow = std::max(widestRow, row.layouts.size());
    }
    int panelWidth = 0;
    if (widestRow > 0) {
        panelWidth = static_cast<int>(widestRow) * (miniW + miniGap) - miniGap;
    }

    // Measure the item texts so long labels are not cut off.
    int textWidth = 0;
    if (!content_.items.empty()) {
        HDC screen = ::GetDC(nullptr);
        for (const Item& it : content_.items) {
            if (it.separator() || it.text.empty()) continue;
            ::SelectObject(screen, it.bold() ? fontBold_ : font_);
            SIZE ext{};
            if (::GetTextExtentPoint32W(screen, it.text.c_str(),
                                        static_cast<int>(it.text.size()), &ext)) {
                textWidth = std::max<int>(textWidth, ext.cx);
            }
        }
        ::ReleaseDC(nullptr, screen);
        textWidth += Scale(kCheckWidth + kItemPadX * 2, dpi);
    }

    const int toolSize = Scale(kToolSize, dpi);
    const int toolGap = Scale(kToolGap, dpi);
    const int toolCount = static_cast<int>(content_.tools.size());
    const int toolbarWidth =
        toolCount > 0 ? toolCount * (toolSize + toolGap) - toolGap : 0;

    const int width = std::max({panelWidth, textWidth, toolbarWidth}) + 2 * pad;
    int y = pad;

    // The toolbar sits above everything else and is centred, so it reads as one
    // strip rather than as a left-aligned first row of a list.
    if (toolCount > 0) {
        int x = (width - toolbarWidth) / 2;
        for (int t = 0; t < toolCount; ++t) {
            toolRects_.push_back(RECT{x, y, x + toolSize, y + toolSize});
            x += toolSize + toolGap;
        }
        y += toolSize;

        // Reserved whether or not a button is hovered, so the flyout does not
        // change height under the pointer.
        toolCaption_ = RECT{pad, y, width - pad, y + Scale(kToolCaptionH, dpi)};
        y = toolCaption_.bottom;

        if (!content_.rows.empty() || !content_.items.empty()) {
            y += Scale(kSepHeight, dpi);
        }
    }

    for (size_t m = 0; m < content_.rows.size(); ++m) {
        const MonitorRow& row = content_.rows[m];
        const MonitorEntry& mon = row.monitor;
        const int rowTop = y;

        // Kept parallel to content_.rows even for an empty row, so painting and
        // hit testing can index both with the same number.
        miniFirst_.push_back(miniRects_.size());
        if (row.layouts.empty()) {
            monitorRects_.push_back(RECT{pad, rowTop, width - pad, y});
            continue;
        }

        if (showHead) y += headH;

        // Miniature height follows the aspect ratio of this monitor.
        const RECT& area = mon.area(content_.useWorkArea);
        const int aw = std::max(1L, area.right - area.left);
        const int ah = std::max(1L, area.bottom - area.top);
        const int miniH = std::max(Scale(kMinMiniH, dpi),
                                   static_cast<int>(static_cast<double>(miniW) * ah / aw + 0.5));

        int x = pad;
        for (const Layout& layout : row.layouts) {
            miniRects_.push_back(RECT{x, y, x + miniW, y + miniH});
            zoneFirst_.push_back(hotspots_.size());

            for (const Zone& zone : layout.zones) {
                ZoneHotspot spot;
                spot.rect = ZoneRect(miniRects_.back(), zone, zoneGap);
                spot.monitor = mon.handle;
                spot.zone = zone;
                hotspots_.push_back(spot);
            }
            x += miniW + miniGap;
        }

        y += miniH + captionH;
        monitorRects_.push_back(RECT{pad, rowTop, width - pad, y});
        if (m + 1 < content_.rows.size()) y += rowGap;
    }

    panelBottom_ = y;

    if (!content_.items.empty()) {
        if (!miniRects_.empty()) y += Scale(kSepHeight, dpi);

        for (size_t i = 0; i < content_.items.size(); ++i) {
            const int h = content_.items[i].separator() ? Scale(kSepHeight, dpi)
                                                        : Scale(kItemHeight, dpi);
            itemRects_[i] = RECT{0, y, width, y + h};
            y += h;
        }
    }

    size.cx = width;
    size.cy = y + pad;
}

void FlyoutWindow::CreateFonts() {
    for (HFONT* f : {&font_, &fontBold_, &fontSmall_}) {
        if (*f) { ::DeleteObject(*f); *f = nullptr; }
    }
    font_ = CreateUiFont(dpi_, false, 0);
    fontBold_ = CreateUiFont(dpi_, true, 0);
    fontSmall_ = CreateUiFont(dpi_, false, -2);
}

void FlyoutWindow::Show(FlyoutContent content, const RECT& anchor, UINT dpi, double scale) {
    if (!hwnd_) return;

    content_ = std::move(content);
    if (content_.items.empty() && !content_.hasLayouts() && content_.tools.empty()) return;

    const UINT baseDpi = dpi ? dpi : 96;
    dpi_ = ScaledDpi(baseDpi, scale);
    dark_ = SystemUsesDarkTheme();
    hotItem_ = -1;
    hotZone_ = -1;
    hotTool_ = -1;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const bool haveMonitor =
        ::GetMonitorInfoW(::MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;

    CreateFonts();

    SIZE size{};
    Measure(dpi_, size);

    // A size factor is a wish, not an instruction: on the small screen it is
    // meant for, it can ask for more room than the screen has. Rather than let
    // the flyout hang off an edge, the factor is walked back until it fits -
    // never below the plain DPI, so enlarging can only ever enlarge.
    if (haveMonitor && dpi_ > baseDpi) {
        const int roomX = (mi.rcWork.right - mi.rcWork.left) - 2 * kEdgeGap;
        const int roomY = (mi.rcWork.bottom - mi.rcWork.top) - 2 * kEdgeGap;

        if ((size.cx > roomX && size.cx > 0) || (size.cy > roomY && size.cy > 0)) {
            const int byWidth =
                size.cx > 0 ? ::MulDiv(static_cast<int>(dpi_), roomX, static_cast<int>(size.cx))
                            : static_cast<int>(dpi_);
            const int byHeight =
                size.cy > 0 ? ::MulDiv(static_cast<int>(dpi_), roomY, static_cast<int>(size.cy))
                            : static_cast<int>(dpi_);
            const UINT fitted = static_cast<UINT>(
                std::max<int>(static_cast<int>(baseDpi), std::min(byWidth, byHeight)));

            if (fitted < dpi_) {
                WRITE_DEBUG_LOG(
                    log::dformat(L"Size factor reduced to fit: {} dpi instead of {}",
                                 fitted, dpi_),
                    log::dformat(L"wanted {}x{}, room {}x{}", size.cx, size.cy, roomX, roomY));
                dpi_ = fitted;
                CreateFonts();
                Measure(dpi_, size);
            }
        }
    }

    int x = (anchor.left + anchor.right) / 2 - size.cx / 2;
    int y = anchor.bottom + Scale(kAnchorGap, dpi_);

    if (haveMonitor) {
        x = std::clamp<int>(x, mi.rcWork.left + kEdgeGap,
                            std::max<int>(mi.rcWork.left + kEdgeGap,
                                          mi.rcWork.right - size.cx - kEdgeGap));
        if (y + size.cy > mi.rcWork.bottom) {
            y = anchor.top - Scale(kAnchorGap, dpi_) - size.cy;  // flip above
        }
        y = std::max<int>(y, mi.rcWork.top + kEdgeGap);
    }

    HRGN rgn = ::CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1,
                                    Scale(kCornerRad, dpi_) * 2, Scale(kCornerRad, dpi_) * 2);
    ::SetWindowRgn(hwnd_, rgn, FALSE);  // the window takes ownership

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, size.cx, size.cy,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ::UpdateWindow(hwnd_);
}

void FlyoutWindow::Hide() {
    if (hwnd_ && ::IsWindowVisible(hwnd_)) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    hotItem_ = -1;
    hotZone_ = -1;
    hotTool_ = -1;
    tracking_ = false;
}

int FlyoutWindow::HitTestTool(POINT pt) const {
    for (size_t i = 0; i < toolRects_.size(); ++i) {
        if (PtInRectPt(toolRects_[i], pt)) return static_cast<int>(i);
    }
    return -1;
}

void FlyoutWindow::SetHotTool(int index) {
    if (index == hotTool_) return;
    const int previous = hotTool_;
    hotTool_ = index;
    for (int i : {previous, hotTool_}) {
        if (i >= 0 && i < static_cast<int>(toolRects_.size())) {
            ::InvalidateRect(hwnd_, &toolRects_[i], FALSE);
        }
    }
    // The caption line spells out what the hovered button does; without it the
    // pictograms would have to carry the whole explanation on their own.
    if (toolCaption_.bottom > toolCaption_.top) {
        ::InvalidateRect(hwnd_, &toolCaption_, TRUE);
    }
}

int FlyoutWindow::HitTestItem(POINT pt) const {
    for (size_t i = 0; i < itemRects_.size(); ++i) {
        if (content_.items[i].separator() || content_.items[i].disabled()) continue;
        if (PtInRectPt(itemRects_[i], pt)) return static_cast<int>(i);
    }
    return -1;
}

int FlyoutWindow::HitTestZone(POINT pt) const {
    // Smallest match wins, so a small zone drawn on top of a larger one stays
    // reachable even if a configuration lets them overlap.
    int best = -1;
    LONG bestArea = 0;
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const RECT& r = hotspots_[i].rect;
        if (!PtInRectPt(r, pt)) continue;
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (best < 0 || area < bestArea) {
            best = static_cast<int>(i);
            bestArea = area;
        }
    }
    return best;
}

void FlyoutWindow::SetHotItem(int index) {
    if (index == hotItem_) return;
    const int previous = hotItem_;
    hotItem_ = index;
    for (int i : {previous, hotItem_}) {
        if (i >= 0 && i < static_cast<int>(itemRects_.size())) {
            ::InvalidateRect(hwnd_, &itemRects_[i], FALSE);
        }
    }
}

void FlyoutWindow::SetHotZone(int index) {
    if (index == hotZone_) return;
    const int previous = hotZone_;
    hotZone_ = index;
    for (int i : {previous, hotZone_}) {
        if (i >= 0 && i < static_cast<int>(hotspots_.size())) {
            RECT r = InflateCopy(hotspots_[i].rect, 2, 2);
            ::InvalidateRect(hwnd_, &r, FALSE);
        }
    }
}

void FlyoutWindow::PaintMonitorRow(HDC dc, size_t rowIndex) {
    if (rowIndex >= content_.rows.size() || rowIndex >= monitorRects_.size()) return;

    const Palette pal = MakePalette(dark_);
    const MonitorRow& monitorRow = content_.rows[rowIndex];
    const MonitorEntry& mon = monitorRow.monitor;
    const RECT& row = monitorRects_[rowIndex];
    if (monitorRow.layouts.empty()) return;

    ::SetBkMode(dc, TRANSPARENT);

    // Caption of the monitor row - only useful with more than one screen. It
    // spells out exactly the selectors the configuration accepts, so the file
    // can be written from what the flyout shows.
    if (content_.rows.size() > 1) {
        const RECT& area = mon.area(content_.useWorkArea);
        std::wstring device = mon.shortDevice();
        if (!device.empty()) device += L"   ";

        wchar_t caption[128] = {};
        ::swprintf(caption, ARRAYSIZE(caption), L"Monitor %d   %s%ld × %ld%s",
                   mon.index, device.c_str(),
                   area.right - area.left, area.bottom - area.top,
                   rowIndex == content_.currentRow ? L"   (current)" : L"");

        RECT head{row.left, row.top, row.right, row.top + Scale(kMonitorHeadH, dpi_)};
        ::SelectObject(dc, fontSmall_);
        ::SetTextColor(dc, pal.textDim);
        ::DrawTextW(dc, caption, -1, &head,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    for (size_t l = 0; l < monitorRow.layouts.size(); ++l) {
        const size_t miniIndex = miniFirst_[rowIndex] + l;
        if (miniIndex >= miniRects_.size()) break;

        const Layout& layout = monitorRow.layouts[l];
        const RECT& mini = miniRects_[miniIndex];

        FillRounded(dc, mini, pal.miniBack, Scale(kMiniRad, dpi_));
        FrameRounded(dc, mini, pal.border, Scale(kMiniRad, dpi_));

        // The hotspots of this miniature follow its zones one by one, so the
        // hovered tile is known by index instead of by comparing geometry.
        size_t spot = zoneFirst_[miniIndex];
        for (const Zone& zone : layout.zones) {
            const RECT tile = ZoneRect(mini, zone, Scale(kZoneGap, dpi_));
            const bool hot = static_cast<int>(spot) == hotZone_;
            if(!hot)
				{
					FillRounded(dc, tile, pal.zone, Scale(2, dpi_));
					FrameRounded(dc, tile, pal.border, Scale(2, dpi_));
				}
            ++spot;
        }

        // Layout name below the miniature.
        if (!layout.name.empty()) {
            RECT caption{mini.left, mini.bottom, mini.right,
                         mini.bottom + Scale(kCaptionH, dpi_)};
            ::SelectObject(dc, fontSmall_);
            ::SetTextColor(dc, pal.textDim);
            ::DrawTextW(dc, layout.name.c_str(), -1, &caption,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    for (size_t l = 0; l < monitorRow.layouts.size(); ++l) {
        const size_t miniIndex = miniFirst_[rowIndex] + l;
        if (miniIndex >= miniRects_.size()) break;

        const Layout& layout = monitorRow.layouts[l];
        const RECT& mini = miniRects_[miniIndex];


        // The hotspots of this miniature follow its zones one by one, so the
        // hovered tile is known by index instead of by comparing geometry.
        size_t spot = zoneFirst_[miniIndex];
		  for (const Zone& zone : layout.zones) {
			  const RECT tile = ZoneRect(mini, zone, Scale(kZoneGap, dpi_));
			  const bool hot = static_cast<int>(spot) == hotZone_;
			  if (hot)
			  {
				  FillRounded(dc, tile, pal.accent, Scale(2, dpi_));
				  FrameRounded(dc, tile, pal.border, Scale(2, dpi_), Scale(2, dpi_));
			  }
			  ++spot;
        }
    }
}

void FlyoutWindow::PaintToolbar(HDC dc) {
    if (toolRects_.empty()) return;

    const Palette pal = MakePalette(dark_);
    const int glyphPad = Scale(kToolGlyphPad, dpi_);

    for (size_t i = 0; i < toolRects_.size() && i < content_.tools.size(); ++i) {
        const RECT& button = toolRects_[i];
        const bool hot = static_cast<int>(i) == hotTool_;

        if (hot) FillRounded(dc, button, pal.hover, Scale(kToolRad, dpi_));
        FrameRounded(dc, button, pal.border, Scale(kToolRad, dpi_));

        const RECT glyph = InflateCopy(button, -glyphPad, -glyphPad);
        DrawResizeGlyph(dc, glyph, content_.tools[i],
                        hot ? pal.text : pal.textDim,
                        hot ? pal.accent : pal.zone);
    }

    ::SelectObject(dc, fontSmall_);
    ::SetTextColor(dc, pal.textDim);
    const wchar_t* caption =
        (hotTool_ >= 0 && hotTool_ < static_cast<int>(content_.tools.size()))
            ? ResizeCommandName(content_.tools[hotTool_])
            : L"";
    RECT line = toolCaption_;
    ::DrawTextW(dc, caption, -1, &line,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void FlyoutWindow::Paint(HDC target) {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const Palette pal = MakePalette(dark_);

    // Double buffering against flicker while the highlight moves.
    HDC mem = ::CreateCompatibleDC(target);
    HBITMAP bmp = ::CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    HBRUSH bg = ::CreateSolidBrush(pal.background);
    ::FillRect(mem, &client, bg);
    ::DeleteObject(bg);
    FrameRounded(mem, client, pal.border, Scale(kCornerRad, dpi_));

    ::SetBkMode(mem, TRANSPARENT);

    PaintToolbar(mem);

    // Separator below the toolbar; it has one whenever something follows it.
    if (!toolRects_.empty() && (!content_.rows.empty() || !content_.items.empty())) {
        const int y = toolCaption_.bottom + Scale(kSepHeight, dpi_) / 2;
        RECT line{Scale(kItemPadX, dpi_), y, client.right - Scale(kItemPadX, dpi_), y + 1};
        HBRUSH sep = ::CreateSolidBrush(pal.separator);
        ::FillRect(mem, &line, sep);
        ::DeleteObject(sep);
    }

    for (size_t m = 0; m < content_.rows.size(); ++m) {
        PaintMonitorRow(mem, m);
    }

    // Separator between the miniatures and the text items.
    if (!content_.items.empty() && !miniRects_.empty()) {
        const int y = panelBottom_ + Scale(kSepHeight, dpi_) / 2;
        RECT line{Scale(kItemPadX, dpi_), y, client.right - Scale(kItemPadX, dpi_), y + 1};
        HBRUSH sep = ::CreateSolidBrush(pal.separator);
        ::FillRect(mem, &line, sep);
        ::DeleteObject(sep);
    }

    for (size_t i = 0; i < content_.items.size(); ++i) {
        const Item& item = content_.items[i];
        const RECT rc = itemRects_[i];

        if (item.separator()) {
            RECT line{rc.left + Scale(kItemPadX, dpi_), (rc.top + rc.bottom) / 2,
                      rc.right - Scale(kItemPadX, dpi_), (rc.top + rc.bottom) / 2 + 1};
            HBRUSH sep = ::CreateSolidBrush(pal.separator);
            ::FillRect(mem, &line, sep);
            ::DeleteObject(sep);
            continue;
        }

        if (static_cast<int>(i) == hotItem_) {
            RECT hover = InflateCopy(rc, -Scale(4, dpi_), 0);
            HBRUSH hot = ::CreateSolidBrush(pal.hover);
            ::FillRect(mem, &hover, hot);
            ::DeleteObject(hot);
        }

        ::SelectObject(mem, item.bold() ? fontBold_ : font_);
        ::SetTextColor(mem, item.disabled() ? pal.textDim : pal.text);

        if (item.checked()) {
            RECT check{rc.left + Scale(kItemPadX, dpi_), rc.top,
                       rc.left + Scale(kItemPadX + kCheckWidth, dpi_), rc.bottom};
            ::DrawTextW(mem, L"✓", 1, &check,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }

        RECT text{rc.left + Scale(kItemPadX + kCheckWidth, dpi_), rc.top,
                  rc.right - Scale(kItemPadX, dpi_), rc.bottom};
        ::DrawTextW(mem, item.text.c_str(), static_cast<int>(item.text.size()), &text,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    ::BitBlt(target, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
}

LRESULT CALLBACK FlyoutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FlyoutWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<FlyoutWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<FlyoutWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT FlyoutWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // focus stays with the target window

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = ::BeginPaint(hwnd_, &ps);
        Paint(hdc);
        ::EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetHotTool(HitTestTool(pt));
        SetHotZone(HitTestZone(pt));
        SetHotItem(HitTestItem(pt));
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = ::TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking_ = false;
        SetHotTool(-1);
        SetHotZone(-1);
        SetHotItem(-1);
        return 0;

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        // The toolbar comes first, and it is the one click that leaves the
        // flyout open: a window is nudged wider a step at a time.
        const int tool = HitTestTool(pt);
        if (tool >= 0 && notify_) {
            ::PostMessageW(notify_, WM_MFLY_TOOL, static_cast<WPARAM>(tool), 0);
            return 0;
        }
        const int zone = HitTestZone(pt);
        if (zone >= 0 && notify_) {
            ::PostMessageW(notify_, WM_MFLY_ZONE, static_cast<WPARAM>(zone), 0);
            return 0;
        }
        const int index = HitTestItem(pt);
        if (index >= 0 && notify_) {
            ::PostMessageW(notify_, WM_MFLY_INVOKE, static_cast<WPARAM>(index), 0);
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (notify_) ::PostMessageW(notify_, WM_MFLY_CLOSED, 0, 0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace mfly
