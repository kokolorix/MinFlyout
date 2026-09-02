/**
 * \file SettingsBackup.h
 * \ingroup config
 * \brief Copying the configuration into a shared settings folder, and diffing it.
 *
 * A machine keeps its configuration in \c %APPDATA%; a person keeps several
 * machines. The two entries this adds to the tray menu are the bridge: one
 * writes the current configuration into a shared folder under the name of this
 * computer, the other opens that folder and the local one side by side in
 * Beyond Compare.
 *
 * Both are offered only where they can work, and both questions - is the share
 * mounted, is the comparer installed - are answered by \ref RefreshBackupStatus
 * on a worker thread rather than while a menu is being built. The share is a
 * mapped network drive, and \c GetFileAttributesW on one whose host has gone
 * away can sit there for seconds. Nothing in this application is allowed to
 * make the user wait like that, least of all the tray menu.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Root of the shared settings tree.
 *
 * Site-specific and deliberately a single constant: everything else here is
 * derived from it, so moving the share is a one-line change.
 */
constexpr const wchar_t* kSettingsRoot = L"P:\\Sachen\\Settings";

/**
 * \brief What the last probe found.
 *
 * Both flags gate a menu entry. \ref comparer is only ever \c true together
 * with \ref folder, because comparing against a share that is not there has
 * nothing to show.
 */
struct BackupAvailability {
    bool folder = false;    ///< \ref kSettingsRoot exists and is a directory.
    bool comparer = false;  ///< Beyond Compare 5 was found on this machine.
};

/**
 * \brief The most recent probe result.
 *
 * Never blocks and never asks the file system - it hands out what
 * \ref RefreshBackupStatus last stored, which is what makes it safe to call
 * while building a menu.
 *
 * \return The availability flags; all \c false until the first probe finishes.
 */
BackupAvailability BackupStatus();

/**
 * \brief Starts a probe on a worker thread, unless one is already running.
 *
 * Returns immediately. Call it once at startup and again after showing the
 * menu, so the answer is at most one menu-open old: a share mounted while the
 * application runs shows up the next time the menu is opened rather than
 * needing a restart.
 */
void RefreshBackupStatus();

/// \return \c %kSettingsRoot%\\MinFlyout - where the backups live.
std::wstring BackupFolder();

/**
 * \brief The file name this computer's configuration is stored under.
 *
 * The computer name plus \c ".config.jsonc", so one share holds one file per
 * machine without them ever colliding.
 *
 * \return The bare file name, or empty if the computer name is unavailable.
 */
std::wstring BackupFileName();

/// \return The full target path, or empty if \ref BackupFileName is.
std::wstring BackupFilePath();

/**
 * \brief Copies the configuration file into the shared folder.
 *
 * The file is copied rather than written afresh from the parsed values, so the
 * comments and the layout of the file survive the round trip - the backup is
 * meant to be readable, and half of a JSONC configuration is its comments. An
 * existing backup for this computer is overwritten.
 *
 * \param[out] target Full path that was written; set on success.
 * \param[out] error  Why it failed; set on failure.
 * \return \c true if the file was copied.
 */
bool SaveConfigurationBackup(std::wstring& target, std::wstring& error);

/**
 * \brief Opens the shared folder and the local one in Beyond Compare.
 *
 * Left is \ref BackupFolder, right is the folder the configuration lives in,
 * so the two panes read "what is on the share" against "what this machine has".
 *
 * \param[out] error Why it failed; set on failure.
 * \return \c true if the comparer was started.
 */
bool CompareConfigurationFolders(std::wstring& error);

}  // namespace mfly
