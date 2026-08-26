/**
 * \file ConfigWatcher.cpp
 * \ingroup config
 * \brief Implementation of \ref mfly::ConfigWatcher.
 */
#include "ConfigWatcher.h"

#include <cstddef>

#include "Log.h"

namespace mfly {
namespace {

/// Size of the notification buffer; a folder with three files never fills it.
constexpr DWORD kBufferBytes = 8 * 1024;

/// Silence after the last notification before the reload is reported.
constexpr DWORD kDebounceMs = 300;

/// Pause between two attempts to open the file for reading.
constexpr DWORD kReadRetryMs = 80;

/// Attempts before the file is reported as changed anyway (~1 second).
constexpr int kReadRetries = 12;

/**
 * \brief The changes worth waking up for.
 *
 * The write time covers the plain "save in place", the size covers a
 * truncating write that keeps the time stamp within one tick, and the file
 * name covers the write-temporary-and-rename that most editors do.
 */
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_LAST_WRITE |
                          FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_FILE_NAME;

/**
 * \brief Compares two names the way the file system does.
 * \param a First name.
 * \param b Second name.
 * \return \c true if they only differ in case.
 */
bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                  b.data(), static_cast<int>(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

/**
 * \brief Searches the notification buffer for one file name.
 *
 * The entries form a singly linked list inside the buffer, the names are not
 * null terminated, and a rename delivers the old name as well - all of which
 * is why this is not a \c wcscmp.
 *
 * \param buffer Buffer filled by \c ReadDirectoryChangesW.
 * \param bytes  Number of bytes written into it.
 * \param name   File name to look for, without a path.
 * \return \c true if at least one entry names that file.
 */
bool MentionsFile(const BYTE* buffer, DWORD bytes, const std::wstring& name) {
    constexpr DWORD kHeader = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    DWORD offset = 0;

    while (offset + kHeader <= bytes) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        if (offset + kHeader + info->FileNameLength > bytes) break;  // truncated entry

        const std::wstring_view entry(info->FileName,
                                      info->FileNameLength / sizeof(WCHAR));
        if (EqualsNoCase(entry, name)) return true;

        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
    return false;
}

/**
 * \brief Splits a full path into folder and file name.
 * \param[in]  path Full path.
 * \param[out] dir  Folder, without the trailing separator.
 * \param[out] file File name.
 * \return \c false if the path carries no separator or no file name.
 */
bool SplitPath(const std::wstring& path, std::wstring& dir, std::wstring& file) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= path.size()) return false;

    dir = path.substr(0, slash);
    file = path.substr(slash + 1);
    return !dir.empty() && !file.empty();
}

}  // namespace

ConfigWatcher::~ConfigWatcher() {
    Stop();
}

bool ConfigWatcher::Start(HWND owner, const std::wstring& path) {
    if (thread_) return true;  // already watching
    if (!owner || !SplitPath(path, directory_, fileName_)) return false;

    owner_ = owner;
    path_ = path;

    stop_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
    if (!stop_) return false;

    thread_ = ::CreateThread(nullptr, 0, &ConfigWatcher::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        ::CloseHandle(stop_);
        stop_ = nullptr;
        return false;
    }

    WRITE_INFO_LOG(L"Watching the configuration file", path_);
    return true;
}

void ConfigWatcher::Stop() {
    if (stop_) ::SetEvent(stop_);

    if (thread_) {
        // The loop only ever waits, so it reacts to the event immediately. The
        // timeout is there so a shutdown cannot hang on a wedged file system.
        if (::WaitForSingleObject(thread_, 5000) == WAIT_TIMEOUT) {
            WRITE_WARNING_LOG(L"Config watcher did not stop in time");
        }
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }

    if (stop_) {
        ::CloseHandle(stop_);
        stop_ = nullptr;
    }
    owner_ = nullptr;
}

DWORD WINAPI ConfigWatcher::ThreadMain(LPVOID param) {
    static_cast<ConfigWatcher*>(param)->Run();
    return 0;
}

bool ConfigWatcher::WaitUntilReadable() const {
    for (int attempt = 0; attempt < kReadRetries; ++attempt) {
        HANDLE file = ::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file);
            return true;
        }
        // Also covers the moment during a rename in which the file does not
        // exist at all.
        if (::WaitForSingleObject(stop_, kReadRetryMs) == WAIT_OBJECT_0) return false;
    }
    return false;
}

void ConfigWatcher::Run() {
    // FILE_LIST_DIRECTORY on a directory needs FILE_FLAG_BACKUP_SEMANTICS; the
    // share flags let the editor do whatever it wants with the files inside.
    HANDLE dir = ::CreateFileW(directory_.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        WRITE_WARNING_LOG(L"Configuration folder could not be opened for watching",
                          log::dformat(L"{}, error {}", directory_, ::GetLastError()));
        return;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = ::CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        ::CloseHandle(dir);
        return;
    }

    // FILE_NOTIFY_INFORMATION contains DWORDs, so the buffer has to be aligned.
    alignas(DWORD) BYTE buffer[kBufferBytes];

    bool reading = false;  // A read is outstanding on `overlapped`.
    bool pending = false;  // Our file changed, the debounce is running.

    for (;;) {
        if (!reading) {
            ::ResetEvent(overlapped.hEvent);
            if (!::ReadDirectoryChangesW(dir, buffer, kBufferBytes, /*subtree=*/FALSE,
                                         kFilter, nullptr, &overlapped, nullptr)) {
                WRITE_WARNING_LOG(L"ReadDirectoryChanges failed, the file is no longer watched",
                                  log::dformat(L"error {}", ::GetLastError()));
                break;
            }
            reading = true;
        }

        const HANDLE handles[] = {stop_, overlapped.hEvent};
        const DWORD wait = ::WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE,
                                                    pending ? kDebounceMs : INFINITE);

        if (wait == WAIT_OBJECT_0) break;  // Stop()

        if (wait == WAIT_TIMEOUT) {
            // Nothing more has happened for kDebounceMs - the save is over.
            // The read stays outstanding, so changes during the wait below are
            // not lost.
            pending = false;
            if (!WaitUntilReadable() &&
                ::WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {
                break;
            }
            ::PostMessageW(owner_, WM_MFLY_CONFIG, 0, 0);
            continue;
        }

        if (wait != WAIT_OBJECT_0 + 1) {
            WRITE_ERROR_LOG(L"Wait of the config watcher failed",
                            log::dformat(L"error {}", ::GetLastError()));
            break;
        }

        DWORD bytes = 0;
        if (!::GetOverlappedResult(dir, &overlapped, &bytes, FALSE)) {
            WRITE_WARNING_LOG(L"Config watcher lost its notification",
                              log::dformat(L"error {}", ::GetLastError()));
            break;
        }
        reading = false;

        // bytes == 0 means the buffer overflowed and Windows dropped the
        // entries. What changed is then unknown, so we assume it was our file.
        if (bytes == 0 || MentionsFile(buffer, bytes, fileName_)) pending = true;
    }

    // buffer and overlapped live on this stack - the read must be finished
    // before it goes away.
    if (reading) {
        ::CancelIoEx(dir, &overlapped);
        DWORD ignored = 0;
        ::GetOverlappedResult(dir, &overlapped, &ignored, /*wait=*/TRUE);
    }
    ::CloseHandle(overlapped.hEvent);
    ::CloseHandle(dir);
}

}  // namespace mfly
