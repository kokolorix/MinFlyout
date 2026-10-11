/**
 * \file Log.cpp
 * \ingroup app
 * \brief Sinks of the debug log: debugger output and optional log file.
 */
#include "Log.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cwchar>
#include <mutex>
#include <string_view>

namespace mfly::log {
namespace {

/// Rotate the log file once it grows past this size.
constexpr LONGLONG kMaxFileBytes = 1LL << 20;  // 1 MiB

std::mutex g_mutex;              ///< Serializes writers from UI and hook thread.
bool       g_fileLogging = false;///< File sink switched on.
std::wstring g_filePath;         ///< Cached path of the log file.

/**
 * First line of every log file. Debug-Log.ps1 reads the file with
 * ConvertFrom-Csv and finds the columns by these names (the ClipWatch
 * spelling); LogViewPlus and Excel show them as column titles.
 */
constexpr char kHeader[] =
    "Zeit\tThread\tLogLevel\tDuration(ms)\tMessage\tFunction\tLogSource\tDetail\r\n";

/// UTF-8 byte order mark.
constexpr char kBom[] = "\xEF\xBB\xBF";

/**
 * \brief Widens a narrow literal (\c __FILE__, \c __FUNCTION__).
 * \param text Source text, interpreted as UTF-8.
 * \return The widened text.
 */
std::wstring Widen(const char* text) {
    if (!text || !*text) return std::wstring();

    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1) return std::wstring();

    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), needed);
    return out;
}

/**
 * \brief Reduces a source path to its file name.
 *
 * \c __FILE__ is an absolute path under MSVC; the full path would push the
 * interesting columns off screen.
 *
 * \param path Path from \c __FILE__.
 * \return The file name without directories.
 */
std::wstring BaseName(const char* path) {
    std::wstring full = Widen(path);
    const size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? full : full.substr(slash + 1);
}

/// \return Local time as "yyyy-MM-dd HH:mm:ss.fff".
std::wstring Timestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return dformat(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/**
 * \brief Determines the path of the log file (next to the configuration).
 * \return Full path, or empty when \c %APPDATA% cannot be resolved.
 */
std::wstring ResolveFilePath() {
    PWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        return std::wstring();
    }
    std::wstring path(appData);
    ::CoTaskMemFree(appData);

    path += L"\\MinFlyout";
    ::CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\minflyout.log";
    return path;
}

/**
 * \brief Renames the current log to ".1" once it is too large.
 * \param path Path of the log file.
 */
void RotateIfNeeded(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;

    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(info.nFileSizeHigh);
    size.LowPart = info.nFileSizeLow;
    if (size.QuadPart < kMaxFileBytes) return;

    const std::wstring previous = path + L".1";
    ::DeleteFileW(previous.c_str());
    ::MoveFileW(path.c_str(), previous.c_str());
}

/**
 * \brief Appends one line to the log file as UTF-8.
 * \param line Line without a trailing newline.
 */
void AppendToFile(const std::wstring& line) {
    if (g_filePath.empty()) return;
    RotateIfNeeded(g_filePath);

    HANDLE file = ::CreateFileW(g_filePath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    const std::wstring wide = line + L"\r\n";
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                             static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        std::string utf8(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                              utf8.data(), needed, nullptr, nullptr);

        // A BOM in a fresh file makes Notepad and Excel pick UTF-8 by themselves;
        // the header names the columns for Debug-Log.ps1 and the viewers.
        if (::SetFilePointer(file, 0, nullptr, FILE_END) == 0) {
            DWORD written = 0;
            ::WriteFile(file, kBom, sizeof(kBom) - 1, &written, nullptr);
            ::WriteFile(file, kHeader, sizeof(kHeader) - 1, &written, nullptr);
        }
        DWORD written = 0;
        ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    ::CloseHandle(file);
}

/**
 * \brief Moves a log file written before the header existed out of the way.
 *
 * Debug-Log.ps1 takes the first line as column names, so a file without the
 * header would lose its first entry and show every column wrongly. It is
 * renamed to ".1" like a rotated file and stays available there.
 *
 * \param path Path of the log file.
 */
void RetireFileWithoutHeader(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;  // no file yet

    char start[sizeof(kBom) - 1 + 5] = {};  // BOM + "Zeit\t"
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, start, sizeof(start), &read, nullptr);
    ::CloseHandle(file);
    if (!ok || read == 0) return;  // empty: the next write adds the header

    const std::string_view expected = std::string_view(kBom, sizeof(kBom) - 1);
    const std::string_view actual(start, read);
    if (actual.size() == sizeof(start) &&
        actual.substr(0, expected.size()) == expected &&
        actual.substr(expected.size()) == std::string_view(kHeader, 5)) {
        return;
    }

    const std::wstring previous = path + L".1";
    ::DeleteFileW(previous.c_str());
    ::MoveFileW(path.c_str(), previous.c_str());
}

}  // namespace

void writeDebugLog(const std::wstring& line) {
    ::OutputDebugStringW((line + L"\r\n").c_str());

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_fileLogging) AppendToFile(line);
}

void SetFileLogging(bool enabled) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (enabled && g_filePath.empty()) g_filePath = ResolveFilePath();
    if (enabled && !g_fileLogging && !g_filePath.empty()) RetireFileWithoutHeader(g_filePath);
    g_fileLogging = enabled && !g_filePath.empty();
}

std::wstring FilePath() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_filePath.empty() ? ResolveFilePath() : g_filePath;
}

// ---------------------------------------------------------------------------
// Opening the log (tray menu). Same lookup order as ClipWatch.
// ---------------------------------------------------------------------------
namespace {

/// \return \c true when \p path names an existing file (not a directory).
bool IsFile(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/// \return Everything before the last backslash, or empty.
std::wstring ParentFolder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

/// \return \p path in double quotes, for a command line.
std::wstring Quote(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

/**
 * \brief Reads a REG_SZ value, without surrounding quotes.
 * \param value Value name, \c nullptr for the default value.
 * \return The string, or empty when the value is missing.
 */
std::wstring ReadRegistryString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD bytes = 0;
    if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
        return std::wstring();
    }

    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, result.data(), &bytes) != ERROR_SUCCESS) {
        return std::wstring();
    }

    result.resize(std::wcslen(result.c_str()));
    if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"') {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

/**
 * \brief Looks an executable up in the registry's App Paths.
 * \return Full path, or empty when the program is not installed.
 */
std::wstring FindInstalledProgram(const wchar_t* executable) {
    const std::wstring key =
        std::wstring(LR"(Software\Microsoft\Windows\CurrentVersion\App Paths\)") + executable;

    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        const std::wstring path = ReadRegistryString(root, key.c_str(), nullptr);
        if (IsFile(path)) return path;
    }
    return std::wstring();
}

/**
 * \brief Looks a program up in the uninstall entries of the registry.
 *
 * Every installer registers there, wherever it puts the files, so this also
 * finds per-user installations below \c %LOCALAPPDATA%. The executable is
 * taken from \c InstallLocation, or else from \c DisplayIcon, which usually
 * points at it ("C:\...\App.exe,0").
 *
 * \param displayName Start of the entry's DisplayName, case-insensitive.
 * \param executable  File name of the program.
 * \return Full path, or empty.
 */
std::wstring FindUninstallEntry(std::wstring_view displayName, const wchar_t* executable) {
    struct Root {
        HKEY           key;
        const wchar_t* path;
    };
    static const Root kRoots[] = {
        {HKEY_CURRENT_USER,  LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)"},
        {HKEY_LOCAL_MACHINE, LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)"},
        {HKEY_LOCAL_MACHINE, LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)"},
    };

    for (const Root& root : kRoots) {
        HKEY uninstall = nullptr;
        if (::RegOpenKeyExW(root.key, root.path, 0, KEY_READ, &uninstall) != ERROR_SUCCESS) continue;

        std::wstring found;
        wchar_t name[256];
        for (DWORD index = 0; found.empty(); ++index) {
            DWORD length = ARRAYSIZE(name);
            const LSTATUS status = ::RegEnumKeyExW(uninstall, index, name, &length,
                                                   nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) continue;

            const std::wstring entry = std::wstring(root.path) + L"\\" + name;
            const std::wstring title = ReadRegistryString(root.key, entry.c_str(), L"DisplayName");
            if (title.size() < displayName.size() ||
                ::CompareStringOrdinal(title.c_str(), static_cast<int>(displayName.size()),
                                       displayName.data(), static_cast<int>(displayName.size()),
                                       TRUE) != CSTR_EQUAL) {
                continue;
            }

            std::wstring location = ReadRegistryString(root.key, entry.c_str(), L"InstallLocation");
            if (!location.empty()) {
                if (location.back() != L'\\') location += L'\\';
                if (IsFile(location + executable)) found = location + executable;
            }
            if (found.empty()) {
                std::wstring icon = ReadRegistryString(root.key, entry.c_str(), L"DisplayIcon");
                const size_t comma = icon.rfind(L',');
                if (comma != std::wstring::npos &&
                    icon.find_first_not_of(L"-0123456789", comma + 1) == std::wstring::npos) {
                    icon.erase(comma);  // icon index
                }
                if (icon.size() >= 2 && icon.front() == L'"' && icon.back() == L'"') {
                    icon = icon.substr(1, icon.size() - 2);
                }
                const size_t slash = icon.find_last_of(L"\\/");
                const wchar_t* file = icon.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
                if (::_wcsicmp(file, executable) == 0 && IsFile(icon)) found = icon;
            }
        }

        ::RegCloseKey(uninstall);
        if (!found.empty()) return found;
    }
    return std::wstring();
}

/// \return A known folder, or empty.
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &folder))) return std::wstring();
    std::wstring path(folder);
    ::CoTaskMemFree(folder);
    return path;
}

/**
 * \brief LogViewPlus, which often installs outside the App Paths key.
 *
 * Tried in turn: App Paths, the uninstall entries, then the folders the
 * per-user and per-machine installers use.
 */
std::wstring FindLogViewPlus() {
    constexpr wchar_t kExecutable[] = L"LogViewPlus.exe";

    if (std::wstring path = FindInstalledProgram(kExecutable); !path.empty()) return path;
    if (std::wstring path = FindUninstallEntry(L"LogViewPlus", kExecutable); !path.empty()) return path;

    const std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
    const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
    const std::wstring programFilesX86 = KnownFolder(FOLDERID_ProgramFilesX86);
    const std::wstring candidates[] = {
        localAppData.empty()    ? std::wstring() : localAppData + L"\\LogViewPlus",
        localAppData.empty()    ? std::wstring() : localAppData + L"\\Programs\\LogViewPlus",
        programFiles.empty()    ? std::wstring() : programFiles + L"\\LogViewPlus",
        programFilesX86.empty() ? std::wstring() : programFilesX86 + L"\\LogViewPlus",
    };
    for (const std::wstring& folder : candidates) {
        if (folder.empty()) continue;
        const std::wstring path = folder + L"\\" + kExecutable;
        if (IsFile(path)) return path;
    }
    return std::wstring();
}

/// \return Folder of the running executable, or empty.
std::wstring ExecutableFolder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(),
                                                  static_cast<DWORD>(path.size()));
        if (length == 0) return std::wstring();
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);  // truncated: long path
    }
    return ParentFolder(path);
}

/// Starts \p application with \p arguments; \return \c true on success.
bool Launch(HWND owner, const std::wstring& application, const std::wstring& arguments,
            const std::wstring& workingDirectory) {
    const HINSTANCE result = ::ShellExecuteW(
        owner, L"open", application.c_str(),
        arguments.empty() ? nullptr : arguments.c_str(),
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

}  // namespace

bool OpenLog(HWND owner) {
    const std::wstring path = FilePath();
    if (!IsFile(path)) return false;

    const std::wstring folder = ParentFolder(path);

    if (const std::wstring viewer = FindLogViewPlus(); !viewer.empty()) {
        return Launch(owner, viewer, Quote(path), folder);
    }
    if (const std::wstring excel = FindInstalledProgram(L"excel.exe"); !excel.empty()) {
        return Launch(owner, excel, Quote(path), folder);
    }
    // Neither is installed: hand the file to whatever is associated with it.
    return Launch(owner, path, std::wstring(), folder);
}

HtmlViewResult OpenLogHtml(HWND owner) {
    const std::wstring path = FilePath();
    if (!IsFile(path)) return HtmlViewResult::NoLogFile;

    const std::wstring folder = ExecutableFolder();
    if (folder.empty()) return HtmlViewResult::ScriptMissing;

    const std::wstring command = folder + L"\\Debug-Log.cmd";
    if (IsFile(command)) {
        return Launch(owner, command, Quote(path), folder) ? HtmlViewResult::Started
                                                           : HtmlViewResult::Failed;
    }

    // Fall back to the script itself, through PowerShell 7 or Windows PowerShell.
    // Without the .cmd nobody opens the browser; the script prints the URL.
    const std::wstring script = folder + L"\\Debug-Log.ps1";
    if (!IsFile(script)) return HtmlViewResult::ScriptMissing;

    std::wstring shell = FindInstalledProgram(L"pwsh.exe");
    if (shell.empty()) shell = L"powershell.exe";

    const std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -File " +
                                   Quote(script) + L" -Serve " + Quote(path);
    return Launch(owner, shell, arguments, folder) ? HtmlViewResult::Started
                                                   : HtmlViewResult::Failed;
}

bool OpenLogFolder(HWND owner) {
    const std::wstring path = FilePath();
    if (IsFile(path)) {
        return Launch(owner, L"explorer.exe", L"/select," + Quote(path), std::wstring());
    }

    // No file yet: the folder itself (ResolveFilePath has created it).
    const std::wstring folder = ParentFolder(path);
    return !folder.empty() && Launch(owner, folder, std::wstring(), std::wstring());
}

std::wstring Describe(HWND window) {
    if (!window || !::IsWindow(window)) return L"(invalid window)";

    wchar_t title[128] = {};
    wchar_t cls[64] = {};
    // GetWindowText would block on a hung window - the timeout variant does not.
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(window, WM_GETTEXT, ARRAYSIZE(title),
                          reinterpret_cast<LPARAM>(title),
                          SMTO_ABORTIFHUNG, kSendTimeoutMs, &result);
    ::GetClassNameW(window, cls, ARRAYSIZE(cls));

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    return dformat(L"'{}' [{}] pid {}", title, cls, pid);
}

Stopwatch::Stopwatch() {
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    start_ = now.QuadPart;
}

int Stopwatch::ElapsedMs() const {
    LARGE_INTEGER now{}, frequency{};
    ::QueryPerformanceCounter(&now);
    ::QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) return 0;

    const double ms = static_cast<double>(now.QuadPart - start_) * 1000.0 /
                      static_cast<double>(frequency.QuadPart);
    return static_cast<int>(ms + 0.5);
}

namespace {

/// Placeholder for a column that has no value in this call.
constexpr const wchar_t* kEmpty = L"-";

/**
 * \brief Assembles one log line.
 *
 * Column order: time, thread, level, duration, message, function, file(line),
 * detail. Every column is always filled - a log viewer parsing this as a
 * delimiter separated file would otherwise have to cope with empty fields.
 *
 * \param w          Call site.
 * \param duration   Duration column, already formatted.
 * \param msg        Message.
 * \param detail     Detail column.
 */
std::wstring Compose(const Writer& w, std::wstring_view duration,
                     std::wstring_view msg, std::wstring_view detail) {
    return dformat(L"{}\t{}\t{}\t{}\t{}\t{}\t{}({})\t{}",
                   Timestamp(), ::GetCurrentThreadId(), w.level, duration, msg,
                   Widen(w.function), BaseName(w.file), w.line, detail);
}

}  // namespace

void Writer::operator()(std::wstring_view msg) const {
    writeDebugLog(Compose(*this, kEmpty, msg, kEmpty));
}

void Writer::operator()(std::wstring_view msg, int durationMs) const {
    writeDebugLog(Compose(*this, std::to_wstring(durationMs), msg, kEmpty));
}

void Writer::operator()(std::wstring_view msg, std::wstring_view detail) const {
    writeDebugLog(Compose(*this, kEmpty, msg, detail.empty() ? kEmpty : detail));
}

void Writer::operator()(std::wstring_view msg, std::wstring_view detail,
                        int durationMs) const {
    writeDebugLog(Compose(*this, std::to_wstring(durationMs), msg,
                          detail.empty() ? kEmpty : detail));
}

}  // namespace mfly::log
