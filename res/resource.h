/**
 * \file resource.h
 * \ingroup app
 * \brief Resource IDs (also read by the resource compiler).
 */
#pragma once

/// Application icon; first resource, so that Explorer uses it as the file icon.
#define IDI_APPICON 101

/// The About dialog, see AboutDialog.h.
#define IDD_ABOUT 200

/** \name Controls of the About dialog
 *  \{ */
#define IDC_ABOUT_VERSION   1000  ///< "MinFlyout 1.0.0.42".
#define IDC_ABOUT_COMMIT    1001  ///< Abbreviated commit hash; a click copies the full one.
#define IDC_ABOUT_COPY      1002  ///< Button that copies the full commit hash.
#define IDC_ABOUT_DIRTY     1003  ///< Warning about a build with uncommitted changes.
#define IDC_ABOUT_SHORTCUTS 1004  ///< List view of every key and gesture.
#define IDC_ABOUT_BUILT     1005  ///< Branch and build time.
/** \} */

#ifndef IDC_STATIC
/// Static controls that are never addressed (winres.h is not used here).
#define IDC_STATIC (-1)
#endif
