/**
 * \file AboutDialog.h
 * \ingroup app
 * \brief The About dialog: version, commit and every key MinFlyout knows.
 */
#pragma once

#include <string>
#include <vector>

#include "Common.h"

namespace mfly {

/**
 * \brief One line of the shortcut list in the About dialog.
 *
 * Lines with the same \ref group are shown under one heading, in the order the
 * groups first appear.
 */
struct ShortcutRow {
    std::wstring group;    ///< Heading, e.g. "Anywhere".
    std::wstring keys;     ///< The keys or gesture, e.g. "Ctrl+Alt+Z".
    std::wstring command;  ///< What it does.
};

/**
 * \brief Shows the About dialog, modal to \p owner.
 *
 * Built after the one of ClipWatch: the version, and the way GitKraken shows
 * it the abbreviated commit the binary was built from. Hovering the hash shows
 * the full one; the \c Copy button and a click on the hash put the full hash
 * on the clipboard. A build from a working tree with uncommitted changes says
 * so in red, because then the hash does not describe the binary exactly. The
 * values come from Version.h, which tools/GenerateVersion.ps1 writes before
 * every build.
 *
 * Below that, every key and gesture in a grouped list - the controller fills
 * it from what is actually registered, so a hotkey another program holds is
 * marked as taken instead of being promised.
 *
 * A second call while the dialog is open brings it to the front instead of
 * opening another one.
 *
 * \param instance Module instance holding the dialog template.
 * \param owner    Owner window.
 * \param rows     Contents of the shortcut list.
 */
void ShowAboutDialog(HINSTANCE instance, HWND owner, const std::vector<ShortcutRow>& rows);

/// \return The version as the About dialog shows it, e.g. \c "1.0.0.42".
const wchar_t* VersionString();

/// \return Branch, commit and dirty state, e.g. \c "master: 7a676ec (dirty)".
const wchar_t* CommitDescription();

}  // namespace mfly
