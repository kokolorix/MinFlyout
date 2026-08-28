/**
 * \file Config.h
 * \ingroup config
 * \brief User configuration from \c %APPDATA%\\MinFlyout\\config.jsonc.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief One target area inside a monitor, in percent.
 *
 * The values are percentages of the work area (or of the full monitor area,
 * see Config::useWorkArea) of the monitor the zone is drawn on. A zone is what
 * the user finally clicks in the flyout.
 */
struct Zone {
    double left = 0.0;    ///< Left edge in percent (0 - 100).
    double top = 0.0;     ///< Top edge in percent (0 - 100).
    double width = 50.0;  ///< Width in percent (1 - 100).
    double height = 100.0;///< Height in percent (1 - 100).

    /// \return \c true if the zone has a usable size.
    bool valid() const { return width > 0.0 && height > 0.0; }
};

/**
 * \brief A named set of zones - one miniature in the flyout.
 *
 * A layout is drawn as a miniature of the monitor: every zone becomes a tile
 * at its relative position. Zones are meant to tile the screen without
 * overlapping, the way the Windows snap layouts do.
 *
 * A layout can be restricted to certain monitors through \ref monitors - thirds
 * make sense on an ultrawide screen and not on a laptop panel. Which selectors
 * are understood is documented at \ref mfly::SelectorMatches.
 */
struct Layout {
    std::wstring name;         ///< Label, shown as a caption below the miniature.
    std::vector<Zone> zones;   ///< The zones, in drawing order.

    /**
     * \brief Monitors this layout is offered on.
     *
     * One or more selectors, see \ref mfly::SelectorMatches. An empty list
     * means "every monitor" - that is the default and what a missing
     * \c "monitors" key produces.
     */
    std::vector<std::wstring> monitors;

    /// \return \c true if the layout applies to every monitor.
    bool everywhere() const { return monitors.empty(); }
};

/**
 * \brief Everything about dragging a window into a zone by finger.
 *
 * Touch is not a mouse with a fat pointer, so it gets its own layouts rather
 * than a scaled version of the ones the flyout offers. Halves and quarters are
 * comfortable with a finger; the five-tile arrangements of a wide desktop are
 * not, and picking one of five miniatures while holding a window in mid-drag is
 * a different task from picking one with a mouse.
 *
 * Which layout appears is decided per monitor by Layout::monitors, exactly as
 * for the flyout - that is also how a Surface gets one arrangement in landscape
 * and another in portrait. The first matching layout wins; there is one drop
 * target, not a menu of them.
 */
struct TouchConfig {
    /// Drag-to-zone active at all.
    bool enabled = true;

    /**
     * \brief Also react to a window dragged with the mouse.
     *
     * Off by default - with a mouse the flyout is the better route. Worth
     * turning on to try the overlay out on a machine without a touchscreen.
     */
    bool alsoMouse = false;

    /// How long the finger must rest in the trigger field before the zones unfold.
    UINT dwellMs = kTouchDwellMs;

    /**
     * \brief Where the trigger field sits, in percent of the reference area.
     *
     * The default is a fifth of the screen in its middle. It only exists while
     * a window is actually being dragged, so it may be generous.
     */
    Zone trigger{40.0, 40.0, 20.0, 20.0};

    /// Layouts offered while dragging; empty falls back to Config::layouts.
    std::vector<Layout> layouts;
};

/**
 * \brief The complete user configuration.
 */
struct Config {
    UINT hoverDelayMs = kHoverDelayMs;   ///< Hover delay before the flyout opens.
    UINT closeGraceMs = kCloseGraceMs;   ///< Grace period before closing.
    bool showBuiltinItems = true;        ///< Show the built-in text items below the layouts.
    bool useWorkArea = true;             ///< Base percentages on the work area (instead of the full monitor).
    bool logToFile = false;              ///< Also write the debug log to \c %APPDATA%\\MinFlyout\\minflyout.log.
    bool watchConfig = true;             ///< Reload automatically as soon as the file is saved.
    bool showAllMonitors = true;         ///< Offer every monitor, not just the one the window is on.

    /**
     * \brief Size factor of the flyout, on top of the DPI scaling.
     *
     * Windows sizes the flyout for the resolution of the screen, which on a
     * small panel means small - correct in millimetres and awkward to hit. This
     * multiplies the DPI the flyout is laid out for, so fonts, paddings and
     * miniatures grow together (see \ref mfly::ScaledDpi). Clamped to
     * \ref mfly::kMinUiScale ... \ref mfly::kMaxUiScale, and reduced further
     * where the result would not fit on the screen.
     */
    double uiScale = 1.0;

    std::vector<Layout> layouts;         ///< Configured layouts.
    TouchConfig touch;                   ///< Drag-to-zone by finger.

    /**
     * \brief How the minimize button is located.
     *
     * \c "auto" asks the window first and computes the position when it says
     * nothing, \c "hittest" only asks, \c "computed" only computes. The names
     * are the ones \ref mfly::ParseProbeMode understands; an unknown value is
     * reported as a configuration error and leaves the default in place.
     */
    std::wstring buttonDetection = L"auto";

    /**
     * \brief Log every gate of the detection, not just the successful ones.
     *
     * Answers "where does the mouse movement die?" - at the window under the
     * cursor, at the ignore list, at the caption button region, or later. One
     * line per change, and only in a build with the logging macros compiled in.
     */
    bool traceDetection = false;

    std::wstring path;                   ///< Full path of the configuration file.
    std::wstring error;                  ///< Error text if the file could not be read.

    /// \return \c true if an error occurred during the last load.
    bool hasError() const { return !error.empty(); }
};

/**
 * \brief Loads and holds the user configuration.
 *
 * On first start a commented template is created. If the file is malformed the
 * application stays usable with the default values and reports the error
 * through Config::error.
 */
class ConfigStore {
public:
    /// \return The process-wide instance.
    static ConfigStore& Instance();

    /**
     * \brief Re-reads the configuration file.
     *
     * Creates directory and template if they do not exist yet.
     *
     * \return \c true if the file was read without errors.
     */
    bool Reload();

    /// \return The most recently loaded configuration.
    const Config& current() const { return config_; }

    /**
     * \brief Determines the path of the configuration file.
     * \return \c %APPDATA%\\MinFlyout\\config.jsonc, or empty on error.
     */
    static std::wstring FilePath();

    /**
     * \brief Opens the configuration file in the default editor.
     *
     * Creates it beforehand if it is missing.
     */
    void OpenInEditor() const;

private:
    /**
     * \brief Parses JSONC text that has already been read.
     * \param text Contents of the file.
     * \param[out] out Target structure; on errors the default values remain.
     * \return \c true if the text was error-free.
     */
    static bool ParseInto(const std::wstring& text, Config& out);

    Config config_;  ///< Currently effective configuration.
};

/**
 * \brief Returns the default layouts (also the contents of the template).
 *
 * None of them is restricted to a monitor, so they show up on every screen.
 *
 * \return Five layouts: halves, thirds, large plus two, quarters, full screen.
 */
std::vector<Layout> DefaultLayouts();

}  // namespace mfly
