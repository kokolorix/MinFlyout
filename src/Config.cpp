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
    "// allowed. Changes are applied through the tray menu command\n"
    "// \"Reload configuration\".\n"
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
    out.traceDetection = root.boolean(L"traceDetection", false);

    out.showAllMonitors = root.boolean(L"showAllMonitors", true);

    const std::wstring detection = root.text(L"buttonDetection", L"auto");
    ProbeMode mode = ProbeMode::Auto;
    if (ParseProbeMode(detection, mode)) {
        out.buttonDetection = detection;
    } else {
        out.error = L"\"buttonDetection\" must be \"auto\", \"hittest\" or \"computed\".";
        return false;
    }

    const json::Value* layouts = root.find(L"layouts");
    if (!layouts) return true;  // no section: keep the defaults
    if (!layouts->is(json::Value::Type::Array)) {
        out.error = L"\"layouts\" must be an array.";
        return false;
    }

    std::vector<Layout> parsed;
    for (const json::Value& entry : layouts->elements()) {
        if (!entry.is(json::Value::Type::Object)) continue;

        Layout layout;
        layout.name = entry.text(L"name");
        ReadMonitorSelectors(entry, layout);

        const json::Value* zones = entry.find(L"zones");
        if (!zones || !zones->is(json::Value::Type::Array)) continue;

        for (const json::Value& z : zones->elements()) {
            if (!z.is(json::Value::Type::Object)) continue;

            Zone zone;
            zone.left   = ClampPercent(z.number(L"left", 0.0), 0.0, 99.0);
            zone.top    = ClampPercent(z.number(L"top", 0.0), 0.0, 99.0);
            zone.width  = ClampPercent(z.number(L"width", 50.0), 1.0, 100.0);
            zone.height = ClampPercent(z.number(L"height", 100.0), 1.0, 100.0);
            if (zone.valid()) layout.zones.push_back(zone);
        }
        if (!layout.zones.empty()) parsed.push_back(std::move(layout));
    }

    // An explicitly empty list is a valid setting - then only the text items show.
    out.layouts = std::move(parsed);
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
