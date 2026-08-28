/**
 * \file ZoneOverlay.cpp
 * \ingroup ui
 * \brief Layout and rendering of the touch drop target.
 */
#include "ZoneOverlay.h"

#include <algorithm>

#include "Log.h"

namespace mfly {
namespace {

/** \name Metrics in 96-dpi pixels
 *  Finger sized, not pointer sized - these are deliberately larger than the
 *  flyout's.
 *  @{ */
constexpr int kTileGap     = 6;   ///< Gap between two zone tiles.
constexpr int kTileRad     = 10;  ///< Corner radius of a zone tile.
constexpr int kTileBorder  = 2;   ///< Outline width of a zone tile.
constexpr int kHotBorder   = 4;   ///< Outline width of the highlighted tile.
constexpr int kFieldRad    = 16;  ///< Corner radius of the trigger field.
constexpr int kFieldPad    = 14;  ///< Padding inside the trigger field.
constexpr int kFieldLabelH = 22;  ///< Height of the caption below the miniature.
constexpr int kMiniGap     = 3;   ///< Gap between the tiles of the miniature.
/** @} */

/**
 * \brief The color that becomes fully transparent.
 *
 * Only ever used to erase the background; nothing the overlay draws may end up
 * exactly this color, which is why it is a shade nobody picks on purpose.
 */
constexpr COLORREF kColorKey = RGB(255, 0, 254);

/// Opacity of everything that is drawn, 0-255.
constexpr BYTE kOverlayAlpha = 224;

/**
 * \brief Converts a \ref Zone into the Windows-free \ref PercentRect.
 * \param zone Zone from the configuration.
 * \return The same four numbers.
 */
PercentRect AsPercent(const Zone& zone) {
    return PercentRect{zone.left, zone.top, zone.width, zone.height};
}

/**
 * \brief Converts a \c RECT into the Windows-free \ref RectI.
 *
 * \c RECT holds \c LONG and \ref RectI holds \c int. On Win32 the two are the
 * same width, so the conversion is silent and the casts are only there to say
 * out loud that a coordinate is crossing from the Windows types into the ones
 * the tested arithmetic uses.
 *
 * \param r Source rectangle.
 * \return The same edges.
 */
RectI AsRectI(const RECT& r) {
    return RectI{static_cast<int>(r.left), static_cast<int>(r.top),
                 static_cast<int>(r.right), static_cast<int>(r.bottom)};
}

/**
 * \brief Converts a \ref RectI into a \c RECT.
 * \param r Source rectangle.
 * \return The same edges.
 */
RECT AsRect(const RectI& r) {
    return RECT{r.left, r.top, r.right, r.bottom};
}

}  // namespace

bool ZoneOverlay::Create(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ZoneOverlay::WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_EX_TRANSPARENT is the important one: the overlay must not swallow a
    // single touch point, or it would break the very drag it is drawn for.
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED |
            WS_EX_TRANSPARENT,
        kClassName, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    ::SetLayeredWindowAttributes(hwnd_, kColorKey, kOverlayAlpha,
                                 LWA_COLORKEY | LWA_ALPHA);
    return true;
}

void ZoneOverlay::Destroy() {
    ReleaseBuffer();
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

bool ZoneOverlay::ShowTrigger(const MonitorEntry& monitor, const Layout& layout,
                              const Zone& trigger, bool useWorkArea) {
    if (!hwnd_ || layout.zones.empty()) return false;

    screen_ = AsRectI(monitor.rect);
    area_ = AsRectI(monitor.area(useWorkArea));
    if (screen_.empty() || area_.empty()) return false;

    trigger_ = PercentToPixels(area_, AsPercent(trigger));
    if (trigger_.empty()) return false;

    monitor_ = monitor.handle;
    layout_ = layout;
    hot_ = -1;
    tiles_.clear();

    const POINT centre{(screen_.left + screen_.right) / 2,
                       (screen_.top + screen_.bottom) / 2};
    const UINT dpi = DpiForPoint(centre);
    const bool dark = SystemUsesDarkTheme();
    if (dpi != dpi_ || !font_) {
        if (font_) ::DeleteObject(font_);
        font_ = CreateUiFont(dpi, true, 1);
    }
    dpi_ = dpi;
    dark_ = dark;

    phase_ = Phase::Trigger;
    PlaceOnMonitor();

    WRITE_DEBUG_LOG(log::dformat(L"Touch trigger shown: {},{} {}x{} at {} dpi",
                                 trigger_.left, trigger_.top,
                                 trigger_.width(), trigger_.height(), dpi_),
                    log::dformat(L"layout '{}', {} zones", layout_.name,
                                 layout_.zones.size()));
    return true;
}

void ZoneOverlay::SwitchToZones() {
    if (phase_ != Phase::Trigger) return;

    MeasureZones();
    if (tiles_.empty()) return;

    phase_ = Phase::Zones;
    hot_ = -1;
    Render(nullptr);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    ::UpdateWindow(hwnd_);

    WRITE_INFO_LOG(log::dformat(L"Touch zones unfolded: {} tiles", tiles_.size()),
                   layout_.name);
}

void ZoneOverlay::Hide() {
    if (phase_ == Phase::None) return;
    phase_ = Phase::None;
    hot_ = -1;
    tiles_.clear();
    monitor_ = nullptr;
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    // A screen-sized bitmap is not worth keeping between two drags.
    ReleaseBuffer();
}

RectI ZoneOverlay::triggerRect() const {
    return phase_ == Phase::Trigger ? trigger_ : RectI{};
}

const Zone& ZoneOverlay::zoneAt(size_t index) const {
    static const Zone kNone{};
    return index < layout_.zones.size() ? layout_.zones[index] : kNone;
}

bool ZoneOverlay::Track(POINT screenPt) {
    if (phase_ != Phase::Zones) return false;

    const int hit = SmallestHit(tiles_, static_cast<int>(screenPt.x),
                                static_cast<int>(screenPt.y));
    if (hit == hot_) return false;

    // Repaint only the two tiles involved - the overlay covers a whole screen,
    // and a full repaint on every movement would be felt during the drag.
    const int previous = hot_;
    hot_ = hit;
    const int slack = Scale(kHotBorder, dpi_) + 1;
    for (int index : {previous, hit}) {
        if (index < 0 || static_cast<size_t>(index) >= tiles_.size()) continue;
        const RECT dirty = InflateCopy(ToClient(tiles_[static_cast<size_t>(index)]),
                                       slack, slack);
        Render(&dirty);
        ::InvalidateRect(hwnd_, &dirty, FALSE);
    }
    ::UpdateWindow(hwnd_);
    return true;
}

void ZoneOverlay::MeasureZones() {
    tiles_.clear();
    tiles_.reserve(layout_.zones.size());

    const int gap = Scale(kTileGap, dpi_);
    for (const Zone& zone : layout_.zones) {
        const RectI tile = ShrunkBy(PercentToPixels(area_, AsPercent(zone)), gap);
        tiles_.push_back(tile);
    }
}

void ZoneOverlay::PlaceOnMonitor() {
    ::SetWindowPos(hwnd_, HWND_TOPMOST, screen_.left, screen_.top,
                   screen_.width(), screen_.height(),
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Render(nullptr);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    ::UpdateWindow(hwnd_);
}

RECT ZoneOverlay::ToClient(const RectI& r) const {
    return RECT{r.left - screen_.left, r.top - screen_.top,
                r.right - screen_.left, r.bottom - screen_.top};
}

bool ZoneOverlay::EnsureBuffer() {
    if (!hwnd_) return false;

    const int width = screen_.width();
    const int height = screen_.height();
    if (width <= 0 || height <= 0) return false;

    if (memDc_ && memSize_.cx == width && memSize_.cy == height) return true;
    ReleaseBuffer();

    HDC window = ::GetDC(hwnd_);
    if (!window) return false;

    memDc_ = ::CreateCompatibleDC(window);
    if (memDc_) {
        memBmp_ = ::CreateCompatibleBitmap(window, width, height);
        if (memBmp_) {
            memOld_ = ::SelectObject(memDc_, memBmp_);
            memSize_ = SIZE{width, height};
        } else {
            ::DeleteDC(memDc_);
            memDc_ = nullptr;
        }
    }
    ::ReleaseDC(hwnd_, window);
    return memDc_ != nullptr;
}

void ZoneOverlay::ReleaseBuffer() {
    if (memDc_) {
        if (memOld_) ::SelectObject(memDc_, memOld_);
        ::DeleteDC(memDc_);
        memDc_ = nullptr;
        memOld_ = nullptr;
    }
    if (memBmp_) {
        ::DeleteObject(memBmp_);
        memBmp_ = nullptr;
    }
    memSize_ = SIZE{0, 0};
}

void ZoneOverlay::Render(const RECT* dirty) {
    if (!EnsureBuffer()) return;

    const RECT whole{0, 0, memSize_.cx, memSize_.cy};
    const RECT& region = dirty ? *dirty : whole;

    // Everything that stays this color disappears: the color key is the
    // transparency of this window.
    HBRUSH key = ::CreateSolidBrush(kColorKey);
    ::FillRect(memDc_, &region, key);
    ::DeleteObject(key);

    // Clip to the region so a partial repaint cannot spill over a neighbour.
    const int saved = ::SaveDC(memDc_);
    ::IntersectClipRect(memDc_, region.left, region.top, region.right, region.bottom);

    const Palette pal = MakePalette(dark_);
    ::SetBkMode(memDc_, TRANSPARENT);

    if (phase_ == Phase::Trigger) {
        PaintTrigger(memDc_, pal);
    } else if (phase_ == Phase::Zones) {
        PaintZones(memDc_, pal);
    }

    ::RestoreDC(memDc_, saved);
}

void ZoneOverlay::Paint(HDC target) {
    // The surface is rendered when something changes, not when Windows asks -
    // WM_PAINT only copies. That is what keeps a highlight change during the
    // drag down to two tiles instead of a whole screen.
    if (phase_ == Phase::None) return;  // a paint that arrived after Hide
    if (!memDc_) Render(nullptr);
    if (!memDc_) return;

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    ::BitBlt(target, 0, 0, client.right, client.bottom, memDc_, 0, 0, SRCCOPY);
}

void ZoneOverlay::PaintTrigger(HDC dc, const Palette& pal) {
    const RECT field = ToClient(trigger_);
    const int radius = Scale(kFieldRad, dpi_);

    FillRounded(dc, field, pal.background, radius);
    FrameRounded(dc, field, pal.accent, radius, Scale(kTileBorder, dpi_));

    // Inside the field a miniature of the layout, so the user sees what the
    // zones will look like before committing to them.
    const int pad = Scale(kFieldPad, dpi_);
    const int labelH = Scale(kFieldLabelH, dpi_);
    RECT mini{field.left + pad, field.top + pad,
              field.right - pad, field.bottom - pad - labelH};

    if (mini.right - mini.left > 8 && mini.bottom - mini.top > 8) {
        const RectI miniArea = AsRectI(mini);
        FillRounded(dc, mini, pal.miniBack, Scale(4, dpi_));

        const int gap = Scale(kMiniGap, dpi_);
        for (const Zone& zone : layout_.zones) {
            const RECT tile =
                AsRect(ShrunkBy(PercentToPixels(miniArea, AsPercent(zone)), gap));
            if (tile.right <= tile.left || tile.bottom <= tile.top) continue;
            FillRounded(dc, tile, pal.zone, Scale(3, dpi_));
        }
        FrameRounded(dc, mini, pal.border, Scale(4, dpi_));
    }

    if (!layout_.name.empty() && font_) {
        HGDIOBJ oldFont = ::SelectObject(dc, font_);
        ::SetTextColor(dc, pal.text);
        RECT label{field.left + pad, field.bottom - pad - labelH,
                   field.right - pad, field.bottom - pad};
        ::DrawTextW(dc, layout_.name.c_str(), static_cast<int>(layout_.name.size()),
                    &label,
                    DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        ::SelectObject(dc, oldFont);
    }
}

void ZoneOverlay::PaintZones(HDC dc, const Palette& pal) {
    const int radius = Scale(kTileRad, dpi_);

    // Two passes, the highlighted tile last - the same order the flyout draws
    // its miniatures in. Its outline is drawn thicker than the others and a
    // pen straddles the path it follows, so half of that width lands outside
    // the tile. Drawn in sequence, the next neighbour's fill would shave that
    // half off on one side and leave the highlight looking lopsided.
    for (size_t i = 0; i < tiles_.size(); ++i) {
        if (tiles_[i].empty() || static_cast<int>(i) == hot_) continue;

        const RECT tile = ToClient(tiles_[i]);
        FillRounded(dc, tile, pal.zone, radius);
        FrameRounded(dc, tile, pal.border, radius, Scale(kTileBorder, dpi_));
    }

    if (hot_ >= 0 && static_cast<size_t>(hot_) < tiles_.size() &&
        !tiles_[static_cast<size_t>(hot_)].empty()) {
        const RECT tile = ToClient(tiles_[static_cast<size_t>(hot_)]);
        FillRounded(dc, tile, pal.accent, radius);
        FrameRounded(dc, tile, pal.text, radius, Scale(kHotBorder, dpi_));
    }
}

LRESULT CALLBACK ZoneOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ZoneOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ZoneOverlay*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ZoneOverlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT ZoneOverlay::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // never take the focus from the dragged window

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = ::BeginPaint(hwnd_, &ps);
        Paint(hdc);
        ::EndPaint(hwnd_, &ps);
        return 0;
    }

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace mfly
