/**
 * \file ConfigWatcher.h
 * \ingroup config
 * \brief Watches \c config.jsonc and reports every save.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Reports a saved configuration file to the controller window.
 *
 * The watch runs on its own thread, because \c ReadDirectoryChangesW blocks
 * until something happens and the UI thread has a message loop to serve. The
 * thread does nothing but \c PostMessage \ref WM_MFLY_CONFIG - reading and
 * parsing stays on the UI thread, exactly as with \ref HookThread.
 *
 * Three details make the difference between "it fires" and "it fires when it
 * should":
 *
 * - **Directory, not file.** A file handle would not survive the save: most
 *   editors write a temporary file and rename it over the original, so the
 *   watched object is gone afterwards. The directory stays, and the entries
 *   are filtered by name - which is necessary anyway, since
 *   \c minflyout.log and \c minflyout-diagnosis.txt live in the same folder
 *   and are written by this very process.
 * - **Debounce.** One save produces several notifications (size, write time,
 *   rename). \ref kDebounceMs of silence must pass before the message goes
 *   out, so the file is read once instead of three times.
 * - **Readability.** Right after the notification the editor may still hold
 *   the file open exclusively. The thread waits until it can be opened for
 *   reading, so the reload does not report a spurious error.
 *
 * \see AppController::ReloadConfig
 */
class ConfigWatcher {
public:
    ConfigWatcher() = default;

    /// Stops the thread if it is still running.
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    /**
     * \brief Starts watching the configuration file.
     *
     * A second call while the watch is running is a no-op. The directory has
     * to exist; \ref mfly::ConfigStore::FilePath creates it.
     *
     * \param owner Window that receives \ref WM_MFLY_CONFIG.
     * \param path  Full path of the file to watch.
     * \return \c true if the thread is up and the directory could be opened.
     */
    bool Start(HWND owner, const std::wstring& path);

    /**
     * \brief Stops the thread and closes all handles.
     *
     * Safe to call more than once; a no-op without a preceding \ref Start.
     */
    void Stop();

    /// \return \c true while the watch is running.
    bool watching() const { return thread_ != nullptr; }

private:
    /**
     * \brief Thread entry point, calls \ref Run.
     * \param param Pointer to the \ref ConfigWatcher instance.
     * \return Always 0.
     */
    static DWORD WINAPI ThreadMain(LPVOID param);

    /// Opens the directory and runs the notification loop until \ref Stop.
    void Run();

    /**
     * \brief Waits until the file can be opened for reading again.
     *
     * Returns early when \ref stop_ is signaled, and gives up after
     * \c kReadRetries attempts - a file that stays unreadable is reported
     * anyway, so the error lands in the reload where it can be shown.
     *
     * \return \c true if the file became readable.
     */
    bool WaitUntilReadable() const;

    HWND         owner_ = nullptr;   ///< Window the notification is posted to.
    std::wstring directory_;         ///< Folder that is watched.
    std::wstring fileName_;          ///< File name inside that folder.
    std::wstring path_;              ///< Full path, for the readability check.
    HANDLE       thread_ = nullptr;  ///< Handle of the watch thread.
    HANDLE       stop_ = nullptr;    ///< Manual-reset event that ends the loop.
};

}  // namespace mfly
