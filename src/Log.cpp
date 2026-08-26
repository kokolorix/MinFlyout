/**
 * \file Log.cpp
 * \ingroup app
 * \brief Sinks of the debug log: debugger output and optional log file.
 */
#include "Log.h"

#include <shlobj.h>

#include <mutex>

namespace mfly::log {
namespace {

/// Rotate the log file once it grows past this size.
constexpr LONGLONG kMaxFileBytes = 1LL << 20;  // 1 MiB

std::mutex g_mutex;              ///< Serializes writers from UI and hook thread.
bool       g_fileLogging = false;///< File sink switched on.
std::wstring g_filePath;         ///< Cached path of the log file.

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

        // A BOM in a fresh file makes Notepad and Excel pick UTF-8 by themselves.
        if (::SetFilePointer(file, 0, nullptr, FILE_END) == 0) {
            DWORD written = 0;
            ::WriteFile(file, "\xEF\xBB\xBF", 3, &written, nullptr);
        }
        DWORD written = 0;
        ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    ::CloseHandle(file);
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
    g_fileLogging = enabled && !g_filePath.empty();
}

std::wstring FilePath() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_filePath.empty() ? ResolveFilePath() : g_filePath;
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
