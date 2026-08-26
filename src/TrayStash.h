/**
 * \file TrayStash.h
 * \ingroup ui
 * \brief "Minimize to notification area" for foreign windows.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Hides foreign windows and represents them by a tray icon.
 *
 * The window is hidden with \c SW_HIDE and gets an icon in the notification
 * area; clicking it brings the window back.
 *
 * \warning The hidden windows belong to foreign processes. When MinFlyout
 *          exits, they \b must be made visible again - otherwise they are
 *          unreachable for the user. \ref RestoreAll is therefore called on
 *          shutdown and on \c WM_QUERYENDSESSION. A crash of the host would
 *          leave them invisible instead.
 */
class TrayStash {
public:
    /// \return The process-wide instance.
    static TrayStash& Instance();

    /**
     * \brief Sets the window that receives the tray notifications.
     * \param owner Controller window; receives \ref WM_MFLY_TRAY.
     */
    void Init(HWND owner);

    /**
     * \brief Hides a window and creates a tray icon for it.
     *
     * Icon and label come from the target window (\c WM_GETICON, window title).
     *
     * \param target Window to hide.
     * \return \c true on success; \c false if the window is invalid or has
     *         already been stashed.
     */
    bool Stash(HWND target);

    /// Brings all stashed windows back and removes their icons.
    void RestoreAll();

    /**
     * \brief Processes a notification from a tray icon.
     * \param wParam Icon ID from \ref WM_MFLY_TRAY.
     * \param lParam Mouse event from \ref WM_MFLY_TRAY.
     */
    void OnTrayMessage(WPARAM wParam, LPARAM lParam);

    /**
     * \brief Cleans up entries whose window has been destroyed in the meantime.
     *
     * Called periodically by the controller; the window is not restored in that
     * case, only the orphaned icon is removed.
     */
    void DropDeadWindows();

    /// \return \c true if no window is currently stashed.
    bool empty() const { return entries_.empty(); }

private:
    /// A stashed window along with its tray icon.
    struct Entry {
        HWND  window = nullptr;  ///< Hidden foreign window.
        UINT  id = 0;            ///< Icon ID in the notification area.
        HICON icon = nullptr;    ///< Displayed icon.
        bool  ownsIcon = false;  ///< \c true if the icon has to be destroyed.
    };

    /**
     * \brief Removes an entry along with its icon.
     * \param index          Position in \ref entries_.
     * \param restoreWindow  \c true shows the window again.
     */
    void RemoveEntry(size_t index, bool restoreWindow);

    HWND owner_ = nullptr;        ///< Recipient of the tray notifications.
    UINT nextId_ = 1;             ///< Next icon ID (0 belongs to the app icon).
    std::vector<Entry> entries_;  ///< All stashed windows.
};

}  // namespace mfly
