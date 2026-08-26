/**
 * \file Diagnostics.h
 * \ingroup detect
 * \brief A full dump of why a window is or is not detected.
 *
 * When a window stays undetected the interesting question is *where* the chain
 * broke: at the window styles, at the frame, at the hit test, or at the
 * computed rectangle. Guessing that from the outside is hopeless, so
 * \ref mfly::DiagnoseWindow walks the whole chain once and writes down every
 * intermediate value.
 *
 * Deliberately independent of the \c WRITE_*_LOG macros: those are compiled out
 * of a plain release build, and a diagnosis that only works in a debug build is
 * no use when the problem shows up in the release one.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Date and time this binary was compiled.
 *
 * The cheapest answer to "am I even running the build I just made?", which is
 * why it also ends up in the tray tooltip.
 *
 * \return A static string, never \c nullptr.
 */
const wchar_t* BuildStamp();

/**
 * \brief Reports whether the logging macros are compiled into this binary.
 * \return \c true for a debug build or a release build with
 *         \c _RELEASE_WITH_DEBUG_LOG.
 */
bool LoggingCompiledIn();

/**
 * \brief Walks the detection chain for one window and writes down every step.
 *
 * Window styles, frame, DPI, hit-test answer, what \c GetTitleBarInfo and DWM
 * report, the computed button rectangle and the region - followed by a verdict
 * naming the step that failed.
 *
 * \param window Top-level window to examine (may be \c nullptr).
 * \param pt     Cursor position in screen coordinates.
 * \return A multi-line report, ready to be read by a human.
 */
std::wstring DiagnoseWindow(HWND window, POINT pt);

/**
 * \brief Diagnoses whatever lies under the cursor right now.
 * \return The report from \ref DiagnoseWindow.
 */
std::wstring DiagnoseWindowUnderCursor();

/**
 * \brief Puts text on the clipboard as Unicode.
 * \param text Text to copy.
 * \return \c false if the clipboard could not be opened.
 */
bool CopyToClipboard(const std::wstring& text);

/**
 * \brief Appends a report to \c minflyout-diagnosis.txt next to the configuration.
 * \param[in]  text Report to append.
 * \param[out] path Full path of the file, also filled when writing failed.
 * \return \c false if the file could not be written.
 */
bool AppendDiagnosisFile(const std::wstring& text, std::wstring& path);

}  // namespace mfly
