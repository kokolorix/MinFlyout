/**
 * \file SettingsBackup.cpp
 * \ingroup config
 * \brief Implementation of the settings backup and the Beyond Compare lookup.
 */
#include "SettingsBackup.h"

#include <shellapi.h>

#include <atomic>
#include <format>
#include <mutex>

#include "Config.h"
#include "Log.h"

namespace mfly {
namespace {

/// Suffix of a backup file; the computer name goes in front of it.
constexpr const wchar_t* kBackupSuffix = L".config.jsonc";

/// Sub-folder of \ref kSettingsRoot the backups are written into.
constexpr const wchar_t* kBackupSubFolder = L"MinFlyout";

/** \name Where Beyond Compare 5 might have registered itself
 *  @{ */
constexpr const wchar_t* kScooterKey  = L"SOFTWARE\\Scooter Software\\Beyond Compare 5";
constexpr const wchar_t* kAppPathsKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\BCompare.exe";
/** @} */

/// The probe result and the comparer path that goes with it.
struct ProbeState {
    std::mutex lock;               ///< Guards the two below.
    BackupAvailability status;     ///< What the last probe found.
    std::wstring comparerPath;     ///< Where it found Beyond Compare.
};

/**
 * \brief The probe state, deliberately never destroyed.
 *
 * The worker is not joined anywhere - it cannot be, because the whole point of
 * it is that nobody waits for a network drive. So it can still be inside
 * \c GetFileAttributesW when the message loop ends, and a plain file-scope
 * object would by then have run its destructor under the worker's feet. Leaking
 * it once is the price of never having to wait; the process is about to end
 * anyway, and Windows tears the thread down with it.
 *
 * \return The single instance.
 */
ProbeState& State() {
    static ProbeState* state = new ProbeState();
    return *state;
}

/// A probe is in flight; a second one would only ask the same questions.
/// Trivially destructible, so this one may live at file scope.
std::atomic<bool> g_probing{false};

/**
 * \brief Checks for a directory without following up on why it is missing.
 * \param path Path to test.
 * \return \c true if it exists and is a directory.
 */
bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * \brief Checks for a regular file.
 * \param path Path to test.
 * \return \c true if it exists and is not a directory.
 */
bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/**
 * \brief Reads one string value from the registry.
 *
 * \param root    Hive, \c HKEY_LOCAL_MACHINE or \c HKEY_CURRENT_USER.
 * \param subKey  Key below it.
 * \param value   Value name; empty reads the default value of the key.
 * \param view    \c KEY_WOW64_64KEY or \c KEY_WOW64_32KEY - a 64-bit build does
 *        not see the 32-bit view of the registry unless it asks for it, and an
 *        older Beyond Compare may well be the 32-bit build.
 * \return The string, expanded if it was \c REG_EXPAND_SZ; empty on any failure.
 */
std::wstring RegistryString(HKEY root, const wchar_t* subKey, const wchar_t* value,
                            REGSAM view) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return std::wstring();
    }

    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);  // room for a terminator of our own
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, value, nullptr, &type,
                                              reinterpret_cast<BYTE*>(buffer), &size);
    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::wstring();
    }
    // RegQueryValueEx does not promise the data is terminated.
    buffer[ARRAYSIZE(buffer) - 1] = L'\0';
    std::wstring text = buffer;
    if (text.empty() || type != REG_EXPAND_SZ) return text;

    wchar_t expanded[1024] = {};
    const DWORD written = ::ExpandEnvironmentStringsW(text.c_str(), expanded,
                                                      ARRAYSIZE(expanded));
    return (written > 0 && written <= ARRAYSIZE(expanded)) ? std::wstring(expanded) : text;
}

/**
 * \brief Reads the major file version of an executable.
 *
 * \c version.dll is resolved at run time, the way this application treats every
 * optional dependency: a machine without it loses the version check, not the
 * feature.
 *
 * \param[in]  file  Full path of the executable.
 * \param[out] major Major version; written only on \c true.
 * \return \c false if the file carries no version resource.
 */
bool MajorFileVersion(const std::wstring& file, WORD& major) {
    using PFN_GetSize  = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using PFN_GetInfo  = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using PFN_Query    = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

    struct Api {
        PFN_GetSize getSize = nullptr;
        PFN_GetInfo getInfo = nullptr;
        PFN_Query   query = nullptr;
    };
    static const Api api = [] {
        Api resolved;
        HMODULE module = ::LoadLibraryW(L"version.dll");
        if (!module) return resolved;
        resolved.getSize = reinterpret_cast<PFN_GetSize>(
            reinterpret_cast<void*>(::GetProcAddress(module, "GetFileVersionInfoSizeW")));
        resolved.getInfo = reinterpret_cast<PFN_GetInfo>(
            reinterpret_cast<void*>(::GetProcAddress(module, "GetFileVersionInfoW")));
        resolved.query = reinterpret_cast<PFN_Query>(
            reinterpret_cast<void*>(::GetProcAddress(module, "VerQueryValueW")));
        return resolved;
    }();

    if (!api.getSize || !api.getInfo || !api.query) return false;

    DWORD ignored = 0;
    const DWORD size = api.getSize(file.c_str(), &ignored);
    if (size == 0) return false;

    std::vector<BYTE> block(size);
    if (!api.getInfo(file.c_str(), 0, size, block.data())) return false;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!api.query(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
        !info || length < sizeof(VS_FIXEDFILEINFO)) {
        return false;
    }
    major = HIWORD(info->dwFileVersionMS);
    return true;
}

/// One place Beyond Compare might be, and how much its location already tells us.
struct ComparerCandidate {
    std::wstring path;         ///< Full path of \c BCompare.exe, may be empty.
    bool nameImpliesFive;      ///< The place itself names version 5.
};

/**
 * \brief Expands an environment variable.
 * \param name Variable name without the percent signs.
 * \return Its value, or empty if it is not set.
 */
std::wstring Environment(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD written = ::GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    return (written > 0 && written < ARRAYSIZE(buffer)) ? std::wstring(buffer) : std::wstring();
}

/**
 * \brief Locates Beyond Compare 5.
 *
 * Generous about where it looks and strict about what it accepts. The places
 * that name the version in so many words - the installer's own registry key,
 * the default install folder - are taken at their word when the executable
 * carries no version resource; the one that does not, the shared \c App \c Paths
 * entry, is only accepted once the file itself says 5. That entry is how a
 * machine with Beyond Compare 4 installed would otherwise light up a menu item
 * for a program it does not have.
 *
 * \note Runs on the probe worker as well as on the UI thread, so it does not
 *       log: the logging sink is a static of its own, and the worker may still
 *       be here while the process tears down. The UI-thread caller logs the
 *       result instead.
 *
 * \param[out] out Full path of the executable; written only on \c true.
 * \return \c true if a Beyond Compare 5 was found.
 */
bool FindBeyondCompareFive(std::wstring& out) {
    const std::wstring programFiles = Environment(L"ProgramFiles");
    const std::wstring programFilesX86 = Environment(L"ProgramFiles(x86)");

    const ComparerCandidate candidates[] = {
        {RegistryString(HKEY_LOCAL_MACHINE, kScooterKey, L"ExePath", KEY_WOW64_64KEY), true},
        {RegistryString(HKEY_LOCAL_MACHINE, kScooterKey, L"ExePath", KEY_WOW64_32KEY), true},
        {RegistryString(HKEY_CURRENT_USER,  kScooterKey, L"ExePath", KEY_WOW64_64KEY), true},
        {RegistryString(HKEY_CURRENT_USER,  kScooterKey, L"ExePath", KEY_WOW64_32KEY), true},
        {programFiles.empty() ? std::wstring()
                              : programFiles + L"\\Beyond Compare 5\\BCompare.exe", true},
        {programFilesX86.empty() ? std::wstring()
                                 : programFilesX86 + L"\\Beyond Compare 5\\BCompare.exe", true},
        {RegistryString(HKEY_LOCAL_MACHINE, kAppPathsKey, L"", KEY_WOW64_64KEY), false},
        {RegistryString(HKEY_LOCAL_MACHINE, kAppPathsKey, L"", KEY_WOW64_32KEY), false},
    };

    for (const ComparerCandidate& candidate : candidates) {
        if (!FileExists(candidate.path)) continue;

        WORD major = 0;
        if (MajorFileVersion(candidate.path, major)) {
            if (major != 5) continue;  // an older Beyond Compare is not this one
        } else if (!candidate.nameImpliesFive) {
            continue;
        }

        out = candidate.path;
        return true;
    }
    return false;
}

/// Worker that answers both questions and stores the answers.
DWORD WINAPI ProbeWork(LPVOID) {
    // The one call that can take its time: a mapped drive whose host is gone.
    const bool folder = DirectoryExists(kSettingsRoot);

    // Only asked when the share is there - a comparer with nothing to compare
    // against would light up a menu entry that cannot do anything.
    std::wstring comparer;
    const bool haveComparer = folder && FindBeyondCompareFive(comparer);

    ProbeState& state = State();
    {
        std::lock_guard<std::mutex> guard(state.lock);
        state.status = BackupAvailability{folder, haveComparer};
        state.comparerPath = comparer;
    }
    g_probing.store(false, std::memory_order_release);
    return 0;
}

/**
 * \brief The folder the configuration file lives in.
 * \return \c %APPDATA%\\MinFlyout, or empty if that cannot be determined.
 */
std::wstring ConfigurationFolder() {
    std::wstring path = ConfigStore::Instance().current().path;
    if (path.empty()) path = ConfigStore::FilePath();

    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

/**
 * \brief Makes sure the backup folder is there.
 * \param[out] error Why it could not be created.
 * \return \c true if the folder exists afterwards.
 */
bool EnsureBackupFolder(std::wstring& error) {
    if (!DirectoryExists(kSettingsRoot)) {
        error = std::format(L"{} is not reachable.", kSettingsRoot);
        return false;
    }
    const std::wstring folder = BackupFolder();
    if (DirectoryExists(folder)) return true;

    if (!::CreateDirectoryW(folder.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        error = std::format(L"Could not create {} (error {}).", folder, ::GetLastError());
        return false;
    }
    return true;
}

}  // namespace

BackupAvailability BackupStatus() {
    ProbeState& state = State();
    std::lock_guard<std::mutex> guard(state.lock);
    return state.status;
}

void RefreshBackupStatus() {
    bool idle = false;
    if (!g_probing.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
        return;  // one is already asking
    }
    if (!::QueueUserWorkItem(&ProbeWork, nullptr, WT_EXECUTELONGFUNCTION)) {
        g_probing.store(false, std::memory_order_release);
        WRITE_WARNING_LOG(L"Settings share could not be probed",
                          log::dformat(L"error {}", ::GetLastError()));
    }
}

std::wstring BackupFolder() {
    return std::wstring(kSettingsRoot) + L'\\' + kBackupSubFolder;
}

std::wstring BackupFileName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(name);
    if (!::GetComputerNameW(name, &size) || size == 0) return std::wstring();

    // Windows does not allow anything problematic in a computer name, so this
    // only ever matters for the machine that manages it anyway.
    std::wstring text(name, size);
    for (wchar_t& c : text) {
        if (::wcschr(L"\\/:*?\"<>|", c) != nullptr) c = L'_';
    }
    return text + kBackupSuffix;
}

std::wstring BackupFilePath() {
    const std::wstring file = BackupFileName();
    return file.empty() ? std::wstring() : BackupFolder() + L'\\' + file;
}

bool SaveConfigurationBackup(std::wstring& target, std::wstring& error) {
    const std::wstring source = ConfigStore::Instance().current().path;
    if (source.empty() || !FileExists(source)) {
        error = L"There is no configuration file to back up yet.";
        return false;
    }
    if (!EnsureBackupFolder(error)) return false;

    target = BackupFilePath();
    if (target.empty()) {
        error = L"The name of this computer could not be determined.";
        return false;
    }

    // Overwriting is the point: one file per machine, kept current.
    if (!::CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        error = std::format(L"Could not write {} (error {}).", target, ::GetLastError());
        WRITE_ERROR_LOG(L"Configuration backup failed", error);
        return false;
    }

    WRITE_INFO_LOG(L"Configuration backed up", target);
    return true;
}

bool CompareConfigurationFolders(std::wstring& error) {
    std::wstring comparer;
    {
        ProbeState& state = State();
        std::lock_guard<std::mutex> guard(state.lock);
        comparer = state.comparerPath;
    }
    // The probe may be a menu-open old, and Beyond Compare may have been
    // uninstalled since. Asking again costs a few registry reads.
    if (!FileExists(comparer) && !FindBeyondCompareFive(comparer)) {
        error = L"Beyond Compare 5 was not found on this computer.";
        return false;
    }
    WRITE_DEBUG_LOG(L"Beyond Compare 5", comparer);

    if (!EnsureBackupFolder(error)) return false;

    const std::wstring left = BackupFolder();
    const std::wstring right = ConfigurationFolder();
    if (right.empty()) {
        error = L"The configuration folder could not be determined.";
        return false;
    }

    const std::wstring parameters = L'"' + left + L"\" \"" + right + L'"';
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", comparer.c_str(),
                                             parameters.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        error = std::format(L"Beyond Compare could not be started (error {}).",
                            reinterpret_cast<INT_PTR>(result));
        WRITE_ERROR_LOG(L"Comparer could not be started", error);
        return false;
    }

    WRITE_INFO_LOG(L"Comparing the configuration folders",
                   log::dformat(L"{}  <->  {}", left, right));
    return true;
}

}  // namespace mfly
