/**
 * \file FlyoutWindow.h
 * \ingroup ui
 * \brief The GDI-drawn popup below the minimize button.
 */
#pragma once

#include "Common.h"
#include "Config.h"
#include "Monitors.h"

namespace mfly {

/**
 * \brief One monitor together with the layouts offered on it.
 *
 * Layouts can be restricted to certain screens (Layout::monitors), so every
 * row carries its own list instead of a single global one. A monitor without a
 * single matching layout does not become a row at all.
 */
struct MonitorRow {
    MonitorEntry        monitor;  ///< The screen this row stands for.
    std::vector<Layout> layouts;  ///< Layouts offered on it, in file order.
};

/**
 * \brief What the flyout shows and what a click means.
 *
 * Assembled by the controller before every open: one row per offered monitor
 * and the text items below them.
 */
struct FlyoutContent {
    std::vector<MonitorRow> rows;   ///< Monitor rows to draw, in order.
    std::vector<Item>       items;  ///< Text items below the miniatures.
    bool useWorkArea = true;        ///< Miniatures show the work area.

    /**
     * \brief Row of the monitor the target window is on.
     *
     * Equal to \c rows.size() when that monitor is not among the rows - then no
     * row is marked as the current one.
     */
    size_t currentRow = 0;

    /// \return \c true if at least one row has a layout to draw.
    bool hasLayouts() const {
        for (const MonitorRow& row : rows) {
            if (!row.layouts.empty()) return true;
        }
        return false;
    }
};

/**
 * \brief One clickable zone inside a monitor miniature.
 */
struct ZoneHotspot {
    RECT     rect{};             ///< Position in client coordinates.
    HMONITOR monitor = nullptr;  ///< Monitor the zone belongs to.
    Zone     zone;               ///< The zone itself, in percent.
};

/**
 * \brief Popup that previews the monitors and their configured zones.
 *
 * The upper part draws one row per monitor: a miniature per layout, in the
 * aspect ratio of that monitor, with the zones as tiles inside it. Clicking a
 * tile means "put the window on this monitor, into this zone" - monitor and
 * position in a single click. Below that, separated by a line, the text items
 * (minimize, notification area, ...) follow.
 *
 * The window never activates itself: it carries \c WS_EX_NOACTIVATE and answers
 * \c WM_MOUSEACTIVATE with \c MA_NOACTIVATE, so the target window keeps focus.
 * Drawing is done with GDI into a memory DC (double buffering), colors follow
 * \ref SystemUsesDarkTheme and the system accent color, all metrics go through
 * \ref Scale.
 *
 * Clicks are not handled here: the window posts \ref WM_MFLY_ZONE or
 * \ref WM_MFLY_INVOKE with the index to the controller.
 */
class FlyoutWindow {
public:
    /// Window class name; excluded by \ref IsIgnoredWindow.
    static constexpr const wchar_t* kClassName = L"MinFlyout.Flyout";

    /**
     * \brief Registers the window class and creates the (hidden) window.
     * \param instance     Module instance.
     * \param notifyTarget Window that receives \ref WM_MFLY_ZONE, \ref WM_MFLY_INVOKE
     *                     and \ref WM_MFLY_CLOSED.
     * \return \c true on success.
     */
    bool Create(HINSTANCE instance, HWND notifyTarget);

    /// Destroys window and fonts.
    void Destroy();

    /**
     * \brief Lays the content out and shows the flyout.
     *
     * Positioned centered below \p anchor; if there is not enough room below,
     * it flips above the button. The extent is clamped to the work area of the
     * monitor involved.
     *
     * \param content Monitors, layouts and items (taken over).
     * \param anchor  Button rectangle in screen coordinates.
     * \param dpi     DPI of the target monitor.
     */
    void Show(FlyoutContent content, const RECT& anchor, UINT dpi);

    /// Hides the flyout; the content stays around.
    void Hide();

    /// \return \c true if the flyout is currently visible.
    bool IsVisible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }

    /// \return Window handle of the flyout.
    HWND hwnd() const { return hwnd_; }

    /// \return Window rectangle in screen coordinates (empty if not created).
    RECT ScreenRect() const;

    /// \return The text items most recently passed to \ref Show.
    const std::vector<Item>& items() const { return content_.items; }

    /// \return The clickable zones of the current layout.
    const std::vector<ZoneHotspot>& hotspots() const { return hotspots_; }

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

    /**
     * \brief Computes miniatures, hotspots and item rows.
     * \param[in]  dpi  Target DPI.
     * \param[out] size Required window size.
     */
    void Measure(UINT dpi, SIZE& size);

    /**
     * \brief Draws the whole content (through a memory DC).
     * \param hdc Target device context.
     */
    void Paint(HDC hdc);

    /**
     * \brief Draws one monitor row: caption plus one miniature per layout.
     * \param dc       Target device context.
     * \param rowIndex Index into FlyoutContent::rows.
     */
    void PaintMonitorRow(HDC dc, size_t rowIndex);

    /**
     * \brief Finds the item under a point.
     * \param clientPt Point in client coordinates.
     * \return Index, or -1 if no clickable item is there.
     */
    int HitTestItem(POINT clientPt) const;

    /**
     * \brief Finds the zone under a point.
     * \param clientPt Point in client coordinates.
     * \return Index into \ref hotspots_, or -1.
     */
    int HitTestZone(POINT clientPt) const;

    /**
     * \brief Sets the highlighted item and repaints only the rows involved.
     * \param index New index, or -1 for none.
     */
    void SetHotItem(int index);

    /**
     * \brief Sets the highlighted zone and repaints only the tiles involved.
     * \param index New index into \ref hotspots_, or -1 for none.
     */
    void SetHotZone(int index);

    /**
     * \brief Creates the UI font from the system metrics.
     * \param dpi   Target DPI.
     * \param bold  \c true for the semibold variant.
     * \param small \c true for the smaller caption font.
     * \return New font; the caller takes ownership.
     */
    HFONT CreateUiFont(UINT dpi, bool bold, bool small) const;

    HWND   hwnd_ = nullptr;      ///< Own window.
    HWND   notify_ = nullptr;    ///< Recipient of the notifications (controller).
    HFONT  font_ = nullptr;      ///< Normal UI font.
    HFONT  fontBold_ = nullptr;  ///< Semibold variant for \ref kItemDefault.
    HFONT  fontSmall_ = nullptr; ///< Small font for captions below the miniatures.
    UINT   dpi_ = 96;            ///< DPI the current layout was computed with.
    int    hotItem_ = -1;        ///< Index of the highlighted item, or -1.
    int    hotZone_ = -1;        ///< Index of the highlighted zone, or -1.
    bool   tracking_ = false;    ///< \c TrackMouseEvent is running.
    bool   dark_ = false;        ///< Dark mode active.

    FlyoutContent content_;              ///< Current content.
    std::vector<ZoneHotspot> hotspots_;  ///< Clickable zones in client coordinates.
    std::vector<RECT> itemRects_;        ///< Item rows in client coordinates.
    std::vector<RECT> monitorRects_;     ///< Row rectangle per monitor row.
    std::vector<RECT> miniRects_;        ///< Miniature rectangles, row-major.
    std::vector<size_t> miniFirst_;      ///< First index in \ref miniRects_ per row.
    std::vector<size_t> zoneFirst_;      ///< First index in \ref hotspots_ per miniature.
    int    panelBottom_ = 0;             ///< Lower edge of the miniature area.
};

}  // namespace mfly
