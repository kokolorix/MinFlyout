/**
 * \file Config.cpp
 * \ingroup config
 * \brief Loading, creating and evaluating the configuration file.
 */
#include "Config.h"

#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>

#include "CaptionProbe.h"
#include "Json.h"

namespace mfly {
namespace {

/// Folder name below \c %APPDATA%.
constexpr const wchar_t* kFolderName = L"MinFlyout";
/// File name of the configuration.
constexpr const wchar_t* kFileName = L"config.jsonc";

/// Commented template written on the first run.
constexpr const char* kTemplate =
    "// MinFlyout - configuration\n"
    "//\n"
    "// This file is JSONC: comments (// and /* */) and trailing commas are\n"
    "// allowed. Saving the file applies the changes right away; the tray menu\n"
    "// command \"Reload configuration\" does the same by hand.\n"
    "{\n"
    "  // How long the pointer must rest on the minimize button before the flyout opens.\n"
    "  \"hoverDelayMs\": 350,\n"
    "\n"
    "  // Grace period before the flyout closes again after the pointer leaves.\n"
    "  \"closeGraceMs\": 260,\n"
    "\n"
    "  // Show the built-in items (minimize, notification area, ...) below the layouts.\n"
    "  \"showBuiltinItems\": true,\n"
    "\n"
    "  // true  = percentages refer to the work area (without the taskbar)\n"
    "  // false = percentages refer to the full monitor area\n"
    "  \"useWorkArea\": true,\n"
    "\n"
    "  // true  = show every monitor, so a window can be moved across screens\n"
    "  // false = only show the monitor the window is currently on\n"
    "  \"showAllMonitors\": true,\n"
    "\n"
    "  // Size factor of the flyout, on top of the scaling Windows already does.\n"
    "  // Windows sizes it for the resolution of the screen, which on a small\n"
    "  // panel means small - correct in millimetres and awkward to hit. 1.0 is\n"
    "  // the normal size; 1.4 is a good starting point on a laptop screen.\n"
    "  // Fonts, paddings and miniatures grow together, and the factor is walked\n"
    "  // back where the result would not fit on the screen. Range 0.75 to 3.\n"
    "  \"uiScale\": 1.0,\n"
    "\n"
    "  // How the minimize button is located.\n"
    "  //   \"auto\"      ask the window, and compute the position when it stays\n"
    "  //               silent - which is what apps with their own title bar do\n"
    "  //   \"hittest\"   only ask; a window that does not answer is not detected\n"
    "  //   \"computed\"  never ask, always compute\n"
    "  \"buttonDetection\": \"auto\",\n"
    "\n"
    "  // Log every gate of the detection, not just the successful ones. Answers\n"
    "  // \"where does the mouse movement die?\" - one line per change.\n"
    "  \"traceDetection\": false,\n"
    "\n"
    "  // Write the debug log to minflyout.log next to this file. The log always\n"
    "  // goes to the debugger output; this adds the file (rotated at 1 MB).\n"
    "  // Only has an effect in builds with logging compiled in.\n"
    "  \"logToFile\": false,\n"
    "\n"
    "  // Watch this file and reload it as soon as it is saved. Turn it off if\n"
    "  // %APPDATA% lives on a network share that reports no changes.\n"
    "  \"watchConfig\": true,\n"
    "\n"
    "  // Layouts. Each layout is drawn as a miniature of the monitor, each of its\n"
    "  // zones as a tile inside it. Clicking a tile moves the window to that zone\n"
    "  // on that monitor.\n"
    "  //\n"
    "  //   name      Caption below the miniature\n"
    "  //   zones     left / top / width / height, each in percent of the monitor\n"
    "  //   monitors  Which screens the layout is offered on. Omit it for all of\n"
    "  //             them. Allowed entries, mixable in one list:\n"
    "  //               1, 2, ...      the monitor number the flyout caption shows\n"
    "  //               \"primary\"      the primary screen\n"
    "  //               \"secondary\"    every screen that is not the primary one\n"
    "  //               \"DISPLAY2\"     the device name from the caption\n"
    "  //               \"3840x2160\"    every screen with that resolution\n"
    "  //             A screen without a single matching layout is left out of\n"
    "  //             the flyout.\n"
    "  //\n"
    "  // Zones are meant to tile the screen without overlapping, the way the\n"
    "  // Windows snap layouts do. Add or remove layouts as you like.\n"
    "  \"layouts\": [\n"
    "    {\n"
    "      \"name\": \"Halves\",\n"
    "      \"zones\": [\n"
    "        { \"left\":  0, \"top\": 0, \"width\": 50, \"height\": 100 },\n"
    "        { \"left\": 50, \"top\": 0, \"width\": 50, \"height\": 100 },\n"
    "      ],\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"Thirds\",\n"
    "      // Thirds are cramped on a laptop panel - uncomment to keep them on\n"
    "      // the wide screen only:\n"
    "      // \"monitors\": [\"3840x2160\"],\n"
    "      \"zones\": [\n"
    "        { \"left\":  0,    \"top\": 0, \"width\": 33.34, \"height\": 100 },\n"
    "        { \"left\": 33.33, \"top\": 0, \"width\": 33.34, \"height\": 100 },\n"
    "        { \"left\": 66.66, \"top\": 0, \"width\": 33.34, \"height\": 100 },\n"
    "      ],\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"Large + two\",\n"
    "      \"zones\": [\n"
    "        { \"left\":  0, \"top\":  0, \"width\": 66, \"height\": 100 },\n"
    "        { \"left\": 66, \"top\":  0, \"width\": 34, \"height\":  50 },\n"
    "        { \"left\": 66, \"top\": 50, \"width\": 34, \"height\":  50 },\n"
    "      ],\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"Quarters\",\n"
    "      \"zones\": [\n"
    "        { \"left\":  0, \"top\":  0, \"width\": 50, \"height\": 50 },\n"
    "        { \"left\": 50, \"top\":  0, \"width\": 50, \"height\": 50 },\n"
    "        { \"left\":  0, \"top\": 50, \"width\": 50, \"height\": 50 },\n"
    "        { \"left\": 50, \"top\": 50, \"width\": 50, \"height\": 50 },\n"
    "      ],\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"Full screen\",\n"
    "      \"zones\": [\n"
    "        { \"left\": 0, \"top\": 0, \"width\": 100, \"height\": 100 },\n"
    "      ],\n"
    "    },\n"
    "  ],\n"
    "\n"
    "  // Dragging a window into a zone with the finger.\n"
    "  //\n"
    "  // Pick up a window by its title bar and move it. A field appears where\n"
    "  // \"trigger\" puts it; rest the finger in it for \"dwellMs\" and the zones\n"
    "  // below unfold across the screen. Let go over one of them and the window\n"
    "  // lands there; let go anywhere else and nothing happens.\n"
    "  //\n"
    "  // Touch gets its own layouts because the task is a different one: with a\n"
    "  // window already in mid-drag there is exactly one arrangement on offer,\n"
    "  // not a menu of five. The first layout matching the monitor is the one\n"
    "  // that shows - which is also how a tablet gets one arrangement upright\n"
    "  // and another on its side, through \"monitors\". Leave \"layouts\" out\n"
    "  // entirely and the ones above are used instead.\n"
    "  \"touch\": {\n"
    "    \"enabled\": true,\n"
    "\n"
    "    // React to a window dragged with the mouse as well. Off by default -\n"
    "    // with a mouse the flyout is the shorter way. Turn it on to try the\n"
    "    // overlay out on a machine without a touchscreen.\n"
    "    \"alsoMouse\": false,\n"
    "\n"
    "    // How long the finger has to rest in the field before the zones unfold.\n"
    "    // Too short and merely carrying a window across the middle of the screen\n"
    "    // opens them.\n"
    "    \"dwellMs\": 250,\n"
    "\n"
    "    // The field, in percent of the same reference area the zones use. The\n"
    "    // default is a fifth of the screen in its middle; it is only on screen\n"
    "    // while a window is actually being dragged, so it may be generous.\n"
    "    \"trigger\": { \"left\": 40, \"top\": 40, \"width\": 20, \"height\": 20 },\n"
    "\n"
    "    \"layouts\": [\n"
    "      {\n"
    "        \"name\": \"Touch\",\n"
    "        \"zones\": [\n"
    "          { \"left\":  0, \"top\":  0, \"width\": 50, \"height\": 100 },\n"
    "          { \"left\": 50, \"top\":  0, \"width\": 50, \"height\":  50 },\n"
    "          { \"left\": 50, \"top\": 50, \"width\": 50, \"height\":  50 },\n"
    "        ],\n"
    "      },\n"
    "    ],\n"
    "  },\n"
    "\n"
    "  // Resizing a window in steps.\n"
    "  //\n"
    "  // Windows can put a window into half a screen and it can maximize one\n"
    "  // vertically; between those there is nothing that means \"a bit wider\".\n"
    "  // These are four ways to say it, and they all move the edges by\n"
    "  // \"stepPx\" pixels:\n"
    "  //\n"
    "  //   border   Ctrl+click on a window border grows it, Shift+click shrinks\n"
    "  //            it. On the left or right border that changes the width and\n"
    "  //            leaves the centre alone, on the top or bottom the height.\n"
    "  //            Hold Alt as well and only the edge you clicked moves.\n"
    "  //   wheel    Turn the wheel over a border and that one edge follows it.\n"
    "  //   hotkeys  Ctrl+Alt+Left / Right narrow and widen the foreground\n"
    "  //            window, Ctrl+Alt+Up / Down shorten and lengthen it, and\n"
    "  //            Ctrl+Alt+Shift+Right toggles the full screen width.\n"
    "  //   toolbar  The row of buttons at the top of the flyout.\n"
    "  //\n"
    "  // After a step from the border or the wheel the pointer travels along\n"
    "  // with the edge, so the next click or notch lands on it without aiming\n"
    "  // again - by however far the edge really went, which is less than a step\n"
    "  // where the screen stopped it. \"followEdge\": false leaves it alone.\n"
    "  //\n"
    "  // Double clicking the left or right border stretches the window across\n"
    "  // the full width of the screen and back - the counterpart of what\n"
    "  // Windows already does on the upper and lower border.\n"
    "  \"resize\": {\n"
    "    \"enabled\": true,\n"
    "    \"stepPx\": 10,\n"
    "    \"borderModifiers\": true,\n"
    "    \"wheel\": true,\n"
    "    \"followEdge\": true,\n"
    "    \"doubleClickMaximizes\": true,\n"
    "    \"hotkeys\": true,\n"
    "    \"toolbar\": true,\n"
    "  },\n"
    "}\n";

/**
 * \brief Converts UTF-8 bytes into a \c std::wstring.
 * \param bytes Input (a BOM is skipped).
 * \return The converted string.
 */
std::wstring Utf8ToWide(const std::string& bytes) {
    size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;  // BOM
    }
    const int len = static_cast<int>(bytes.size() - offset);
    if (len <= 0) return std::wstring();

    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset, len, nullptr, 0);
    if (needed <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset, len, out.data(), needed);
    return out;
}

/**
 * \brief Reads a file completely as bytes.
 * \param[in]  path File path.
 * \param[out] out  File contents.
 * \return \c true on success.
 */
bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart > (8 << 20)) {
        ::CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ||
                    ::ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    ::CloseHandle(file);
    if (!ok) return false;
    out.resize(read);
    return true;
}

/**
 * \brief Writes bytes to a file (overwriting existing contents).
 * \param path  File path.
 * \param bytes Contents to write.
 * \return \c true on success.
 */
bool WriteWholeFile(const std::wstring& path, const char* bytes) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const DWORD length = static_cast<DWORD>(::lstrlenA(bytes));
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes, length, &written, nullptr);
    ::CloseHandle(file);
    return ok && written == length;
}

/**
 * \brief Clamps a percentage to a sensible range.
 * \param value Raw value from the file.
 * \param lo    Lower bound.
 * \param hi    Upper bound.
 * \return The clamped value.
 */
double ClampPercent(double value, double lo, double hi) {
    if (!(value == value)) return lo;  // NaN
    return std::clamp(value, lo, hi);
}

/**
 * \brief Turns one entry of \c "monitors" into a selector string.
 *
 * Numbers are allowed so that <code>"monitors": [1, 2]</code> reads naturally;
 * they are converted to their decimal text, which
 * \ref mfly::MonitorMatchesSelector understands as the monitor index.
 *
 * \param[in]  value Array element or scalar from the file.
 * \param[out] out   The selector.
 * \return \c true if the value produced a usable selector.
 */
bool SelectorFromValue(const json::Value& value, std::wstring& out) {
    if (value.is(json::Value::Type::String)) {
        out = value.asString();
    } else if (value.is(json::Value::Type::Number)) {
        wchar_t buffer[32] = {};
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%d",
                   static_cast<int>(value.asNumber(0.0)));
        out = buffer;
    } else {
        return false;
    }
    return !out.empty();
}

/**
 * \brief Reads the \c "monitors" property of a layout.
 *
 * Accepts a single value as well as an array, so both
 * <code>"monitors": "primary"</code> and <code>"monitors": [1, 2]</code> work.
 * A missing property leaves the list empty, which means "every monitor".
 *
 * \param[in]  entry  The layout object.
 * \param[out] layout Layout whose Layout::monitors is filled.
 */
void ReadMonitorSelectors(const json::Value& entry, Layout& layout) {
    const json::Value* monitors = entry.find(L"monitors");
    if (!monitors) return;

    std::wstring selector;
    if (monitors->is(json::Value::Type::Array)) {
        for (const json::Value& element : monitors->elements()) {
            if (SelectorFromValue(element, selector)) {
                layout.monitors.push_back(std::move(selector));
            }
        }
    } else if (SelectorFromValue(*monitors, selector)) {
        layout.monitors.push_back(std::move(selector));
    }
}

/**
 * \brief Reads one zone object.
 *
 * The ranges are the ones \ref mfly::ZoneFromWindow clamps a measured zone to,
 * so a line written by Ctrl+Alt+F11 comes back out of the file unchanged.
 *
 * \param[in]  value The object.
 * \param[out] zone  Written only on \c true.
 * \return \c false for anything that is not a usable zone.
 */
bool ReadZone(const json::Value& value, Zone& zone) {
    if (!value.is(json::Value::Type::Object)) return false;

    zone.left   = ClampPercent(value.number(L"left", 0.0), 0.0, 99.0);
    zone.top    = ClampPercent(value.number(L"top", 0.0), 0.0, 99.0);
    zone.width  = ClampPercent(value.number(L"width", 50.0), 1.0, 100.0);
    zone.height = ClampPercent(value.number(L"height", 100.0), 1.0, 100.0);
    return zone.valid();
}

/**
 * \brief Reads an array of layout objects.
 *
 * Shared by \c "layouts" and \c "touch.layouts": the two sections have the same
 * shape, and a zone written for the one has to mean the same thing in the other.
 * Entries without a single usable zone are dropped silently, as before - a
 * malformed layout is not worth refusing the whole file over.
 *
 * \param[in]  array The array value.
 * \param[out] out   Replaced by what was read.
 */
void ReadLayouts(const json::Value& array, std::vector<Layout>& out) {
    std::vector<Layout> parsed;

    for (const json::Value& entry : array.elements()) {
        if (!entry.is(json::Value::Type::Object)) continue;

        Layout layout;
        layout.name = entry.text(L"name");
        ReadMonitorSelectors(entry, layout);

        const json::Value* zones = entry.find(L"zones");
        if (!zones || !zones->is(json::Value::Type::Array)) continue;

        for (const json::Value& z : zones->elements()) {
            Zone zone;
            if (ReadZone(z, zone)) layout.zones.push_back(zone);
        }
        if (!layout.zones.empty()) parsed.push_back(std::move(layout));
    }
    out = std::move(parsed);
}

/**
 * \brief Reads the \c "touch" section.
 *
 * A missing section leaves the defaults in place, so an existing configuration
 * keeps working and gains the drag-to-zone behaviour with the built-in trigger
 * field in the middle of the screen.
 *
 * \param[in]  root The configuration object.
 * \param[out] out  Target configuration.
 * \return \c false with Config::error set if the section is malformed.
 */
bool ReadTouch(const json::Value& root, Config& out) {
    const json::Value* touch = root.find(L"touch");
    if (!touch) return true;
    if (!touch->is(json::Value::Type::Object)) {
        out.error = L"\"touch\" must be an object.";
        return false;
    }

    out.touch.enabled = touch->boolean(L"enabled", true);
    out.touch.alsoMouse = touch->boolean(L"alsoMouse", false);
    out.touch.dwellMs = static_cast<UINT>(
        ClampPercent(touch->number(L"dwellMs", kTouchDwellMs), 0, 5000));

    if (const json::Value* trigger = touch->find(L"trigger")) {
        Zone zone;
        if (!ReadZone(*trigger, zone)) {
            out.error = L"\"touch.trigger\" must be an object with left, top, "
                        L"width and height.";
            return false;
        }
        out.touch.trigger = zone;
    }

    if (const json::Value* layouts = touch->find(L"layouts")) {
        if (!layouts->is(json::Value::Type::Array)) {
            out.error = L"\"touch.layouts\" must be an array.";
            return false;
        }
        ReadLayouts(*layouts, out.touch.layouts);
    }
    return true;
}

/**
 * \brief Reads the \c "resize" section.
 *
 * A missing section leaves the defaults in place, so a configuration written
 * before step resizing existed gains it without being touched.
 *
 * \param[in]  root The configuration object.
 * \param[out] out  Target configuration.
 * \return \c false with Config::error set if the section is malformed.
 */
bool ReadResize(const json::Value& root, Config& out) {
    const json::Value* resize = root.find(L"resize");
    if (!resize) return true;
    if (!resize->is(json::Value::Type::Object)) {
        out.error = L"\"resize\" must be an object.";
        return false;
    }

    out.resize.enabled = resize->boolean(L"enabled", true);
    out.resize.stepPx = static_cast<int>(
        ClampPercent(resize->number(L"stepPx", 10), 1, 200));
    out.resize.borderModifiers = resize->boolean(L"borderModifiers", true);
    out.resize.wheel = resize->boolean(L"wheel", true);
    out.resize.followEdge = resize->boolean(L"followEdge", true);
    out.resize.doubleClickMaximizes = resize->boolean(L"doubleClickMaximizes", true);
    out.resize.hotkeys = resize->boolean(L"hotkeys", true);
    out.resize.toolbar = resize->boolean(L"toolbar", true);
    return true;
}

}  // namespace

std::vector<Layout> DefaultLayouts() {
    // The empty third member is Layout::monitors: no restriction, every screen.
    return {
        Layout{L"Halves", {Zone{0, 0, 50, 100}, Zone{50, 0, 50, 100}}, {}},
        Layout{L"Thirds", {Zone{0, 0, 33.34, 100}, Zone{33.33, 0, 33.34, 100},
                           Zone{66.66, 0, 33.34, 100}}, {}},
        Layout{L"Large + two", {Zone{0, 0, 66, 100}, Zone{66, 0, 34, 50},
                                Zone{66, 50, 34, 50}}, {}},
        Layout{L"Quarters", {Zone{0, 0, 50, 50}, Zone{50, 0, 50, 50},
                             Zone{0, 50, 50, 50}, Zone{50, 50, 50, 50}}, {}},
        Layout{L"Full screen", {Zone{0, 0, 100, 100}}, {}},
    };
}

ConfigStore& ConfigStore::Instance() {
    static ConfigStore instance;
    return instance;
}

std::wstring ConfigStore::FilePath() {
    PWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        return std::wstring();
    }
    std::wstring path(appData);
    ::CoTaskMemFree(appData);

    path += L'\\';
    path += kFolderName;
    ::CreateDirectoryW(path.c_str(), nullptr);  // usually exists already

    path += L'\\';
    path += kFileName;
    return path;
}

bool ConfigStore::Reload() {
    Config fresh;
    fresh.layouts = DefaultLayouts();
    fresh.path = FilePath();

    if (fresh.path.empty()) {
        fresh.error = L"Could not determine the %APPDATA% folder.";
        config_ = std::move(fresh);
        return false;
    }

    if (::GetFileAttributesW(fresh.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First run: create the template and work with the default values.
        if (!WriteWholeFile(fresh.path, kTemplate)) {
            fresh.error = L"Could not create the configuration file.";
        }
        config_ = std::move(fresh);
        return !config_.hasError();
    }

    std::string bytes;
    if (!ReadWholeFile(fresh.path, bytes)) {
        fresh.error = L"Could not read the configuration file.";
        config_ = std::move(fresh);
        return false;
    }

    ParseInto(Utf8ToWide(bytes), fresh);
    config_ = std::move(fresh);
    return !config_.hasError();
}

bool ConfigStore::ParseInto(const std::wstring& text, Config& out) {
    json::ParseResult result;
    const json::Value root = json::Parse(text, result);

    if (!result.ok) {
        wchar_t buffer[256] = {};
        ::swprintf(buffer, ARRAYSIZE(buffer), L"Line %zu, column %zu: %s",
                   result.line, result.column, result.message.c_str());
        out.error = buffer;
        return false;  // the default values stay in place
    }
    if (!root.is(json::Value::Type::Object)) {
        out.error = L"The configuration must be a JSON object.";
        return false;
    }

    out.hoverDelayMs = static_cast<UINT>(
        ClampPercent(root.number(L"hoverDelayMs", kHoverDelayMs), 0, 5000));
    out.closeGraceMs = static_cast<UINT>(
        ClampPercent(root.number(L"closeGraceMs", kCloseGraceMs), 0, 5000));
    out.showBuiltinItems = root.boolean(L"showBuiltinItems", true);
    out.useWorkArea = root.boolean(L"useWorkArea", true);
    out.logToFile = root.boolean(L"logToFile", false);
    out.watchConfig = root.boolean(L"watchConfig", true);
    out.traceDetection = root.boolean(L"traceDetection", false);

    out.showAllMonitors = root.boolean(L"showAllMonitors", true);
    out.uiScale = ClampPercent(root.number(L"uiScale", 1.0), kMinUiScale, kMaxUiScale);

    const std::wstring detection = root.text(L"buttonDetection", L"auto");
    ProbeMode mode = ProbeMode::Auto;
    if (ParseProbeMode(detection, mode)) {
        out.buttonDetection = detection;
    } else {
        out.error = L"\"buttonDetection\" must be \"auto\", \"hittest\" or \"computed\".";
        return false;
    }

    if (!ReadTouch(root, out)) return false;
    if (!ReadResize(root, out)) return false;

    const json::Value* layouts = root.find(L"layouts");
    if (!layouts) return true;  // no section: keep the defaults
    if (!layouts->is(json::Value::Type::Array)) {
        out.error = L"\"layouts\" must be an array.";
        return false;
    }

    // An explicitly empty list is a valid setting - then only the text items show.
    ReadLayouts(*layouts, out.layouts);
    return true;
}

void ConfigStore::OpenInEditor() const {
    std::wstring path = config_.path.empty() ? FilePath() : config_.path;
    if (path.empty()) return;

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteWholeFile(path, kTemplate);
    }
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace mfly
