/**
 * \file ZoneOverlay.h
 * \ingroup ui
 * \brief Full-screen drop target for dragging a window into a zone by finger.
 *
 * The counterpart of \ref mfly::FlyoutWindow for touch. The flyout is a menu:
 * it is pointed at and clicked. This one is a target: it appears underneath a
 * window that is already in motion and says where the window may be let go.
 *
 * It shows two phases on one window:
 *
 * ```
 *   Phase::Trigger   one field, wherever the configuration puts it, with a
 *                    miniature of the touch layout inside it
 *          │  finger rests in the field for "dwellMs"
 *          ▼
 *   Phase::Zones     the whole monitor, one tile per zone, the tile under the
 *                    finger highlighted; letting go there applies the zone
 * ```
 *
 * Two properties make this work while a foreign window is in its modal move
 * loop:
 *
 * * **It never takes input.** \c WS_EX_TRANSPARENT plus \c WS_EX_NOACTIVATE:
 *   every touch point goes straight through to the window being dragged. The
 *   overlay learns where the finger is from the low-level hook the controller
 *   already owns, in screen coordinates - it does not need a single message of
 *   its own. Nothing it does can therefore interrupt the drag.
 * * **It is composited, not painted over.** \c WS_EX_LAYERED hands the surface
 *   to DWM, so the window travelling underneath does not make it flicker.
 *
 * Transparency uses \c SetLayeredWindowAttributes with a color key plus one
 * constant alpha, not \c UpdateLayeredWindow with per-pixel alpha. That keeps
 * ordinary \c WM_PAINT drawing - and with it the shared helpers in Painting.h -
 * working unchanged; GDI text does not write an alpha channel, so per-pixel
 * alpha would need every label composed by hand. The price is that everything
 * drawn shares one opacity, which for a drop target is no loss.
 */
#pragma once

#include <vector>

#include "Common.h"
#include "Config.h"
#include "Monitors.h"
#include "OverlayGeometry.h"
#include "Painting.h"

namespace mfly {

/**
 * \brief The drop target overlay.
 *
 * Lives on one monitor at a time: the one the finger is on. Moving to another
 * screen during the drag re-shows it there, which is also how it picks up that
 * monitor's DPI and its own layout.
 */
class ZoneOverlay {
public:
    /// Window class name; our own process is skipped by \ref IsIgnoredWindow anyway.
    static constexpr const wchar_t* kClassName = L"MinFlyout.Overlay";

    /// What the overlay is currently showing.
    enum class Phase {
        None,     ///< Hidden.
        Trigger,  ///< The single field that unfolds the zones.
        Zones,    ///< One tile per zone of the layout.
    };

    /**
     * \brief Registers the window class and creates the (hidden) window.
     * \param instance Module instance.
     * \return \c true on success.
     */
    bool Create(HINSTANCE instance);

    /// Destroys window and font.
    void Destroy();

    /**
     * \brief Shows the trigger field on one monitor.
     *
     * Also stores layout and reference area, so \ref SwitchToZones needs no
     * arguments and cannot disagree with what was measured here.
     *
     * \param monitor     Monitor to cover.
     * \param layout      Touch layout offered on it; its zones appear as a
     *                    miniature inside the field and later as the tiles.
     * \param trigger     Position of the field, in percent of the reference area.
     * \param useWorkArea \c true bases the percentages on the work area.
     * \return \c false if the layout has no zone or the field would be empty.
     */
    bool ShowTrigger(const MonitorEntry& monitor, const Layout& layout,
                     const Zone& trigger, bool useWorkArea);

    /// Unfolds the zones of the stored layout on the same monitor.
    void SwitchToZones();

    /// Hides the overlay and forgets the highlight.
    void Hide();

    /// \return What is currently shown.
    Phase phase() const { return phase_; }

    /// \return \c true if the overlay is on screen.
    bool visible() const { return phase_ != Phase::None; }

    /// \return The monitor the overlay currently covers.
    HMONITOR monitor() const { return monitor_; }

    /// \return The trigger field in screen coordinates; empty outside \ref Phase::Trigger.
    RectI triggerRect() const;

    /**
     * \brief Moves the highlight to the zone under a screen point.
     *
     * Cheap enough to call on every mouse movement: it repaints only when the
     * highlighted tile actually changes.
     *
     * \param screenPt Point in screen coordinates.
     * \return \c true if the highlight changed.
     */
    bool Track(POINT screenPt);

    /// \return Index of the highlighted zone, or \c -1.
    int hot() const { return hot_; }

    /**
     * \brief The zone behind a tile.
     * \param index Index into the tiles, as returned by \ref hot.
     * \return The zone in percent; a default zone for an invalid index.
     */
    const Zone& zoneAt(size_t index) const;

    /// \return Name of the layout currently offered.
    const std::wstring& layoutName() const { return layout_.name; }

private:
    /**
     * \brief Static window procedure; forwards to \ref HandleMessage.
     * \param hwnd   Window.
     * \param msg    Message.
     * \param wParam First parameter.
     * \param lParam Second parameter.
     * \return Result of the message handling.
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * \brief Per-instance message handling.
     * \param msg    Message.
     * \param wParam First parameter.
     * \param lParam Second parameter.
     * \return Result of the message handling.
     */
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /// Computes \ref tiles_ from layout and reference area.
    void MeasureZones();

    /**
     * \brief Creates the back buffer for the current monitor size.
     * \return \c false if it could not be created.
     */
    bool EnsureBuffer();

    /// Frees the back buffer.
    void ReleaseBuffer();

    /**
     * \brief Draws the current phase into the back buffer.
     *
     * Called when something changes, not from \c WM_PAINT - the surface covers
     * a whole monitor, and redrawing all of it on every finger movement would
     * be felt during the drag.
     *
     * \param dirty Region to redraw in client coordinates, or \c nullptr for
     *        the whole surface.
     */
    void Render(const RECT* dirty);

    /**
     * \brief Copies the back buffer to the screen.
     * \param target Device context from \c BeginPaint.
     */
    void Paint(HDC target);

    /**
     * \brief Draws the trigger field with the layout miniature inside it.
     * \param dc  Target device context.
     * \param pal Palette of the current theme.
     */
    void PaintTrigger(HDC dc, const Palette& pal);

    /**
     * \brief Draws one tile per zone.
     * \param dc  Target device context.
     * \param pal Palette of the current theme.
     */
    void PaintZones(HDC dc, const Palette& pal);

    /**
     * \brief Converts a screen rectangle into client coordinates of the overlay.
     * \param r Rectangle in screen coordinates.
     * \return The same rectangle relative to the window origin.
     */
    RECT ToClient(const RectI& r) const;

    /// Puts the window on the monitor and shows it topmost, without activating.
    void PlaceOnMonitor();

    HWND  hwnd_ = nullptr;     ///< Own window.
    HFONT font_ = nullptr;     ///< Label font of the trigger field.
    HDC     memDc_ = nullptr;  ///< Back buffer DC, monitor sized.
    HBITMAP memBmp_ = nullptr; ///< Bitmap selected into \ref memDc_.
    HGDIOBJ memOld_ = nullptr; ///< Bitmap that was selected before.
    SIZE    memSize_{0, 0};    ///< Size of \ref memBmp_.
    UINT  dpi_ = 96;           ///< DPI of the monitor currently covered.
    bool  dark_ = false;       ///< Dark mode active.
    Phase phase_ = Phase::None;///< What is shown.
    int   hot_ = -1;           ///< Highlighted tile, or -1.

    HMONITOR monitor_ = nullptr;  ///< Monitor covered.
    RectI    screen_{};           ///< Full monitor area in screen coordinates.
    RectI    area_{};             ///< Reference area of the percentages.
    RectI    trigger_{};          ///< Trigger field in screen coordinates.
    Layout   layout_;             ///< Layout whose zones are offered.
    std::vector<RectI> tiles_;    ///< Zone tiles in screen coordinates.
};

}  // namespace mfly
