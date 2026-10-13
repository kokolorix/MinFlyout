/**
 * \file Hotkey.h
 * \ingroup config
 * \brief Key combinations as the configuration file spells them - pure text work.
 *
 * \c "Ctrl+Alt+Z" in the file, a modifier mask and a virtual key code for
 * \c RegisterHotKey in the program. Like OverlayGeometry.h this file is free of
 * Windows dependencies so the rules can be tested without a desktop: the
 * modifier bits carry the values of \c MOD_ALT, \c MOD_CONTROL, \c MOD_SHIFT
 * and \c MOD_WIN, and the key codes those of the \c VK_* constants, so both go
 * to \c RegisterHotKey unchanged.
 */
#pragma once

#include <string>
#include <string_view>

namespace mfly {

/** \name Modifier bits
 *  Identical to \c MOD_ALT, \c MOD_CONTROL, \c MOD_SHIFT and \c MOD_WIN.
 *  \{ */
constexpr unsigned kHotkeyAlt   = 0x0001;  ///< Alt.
constexpr unsigned kHotkeyCtrl  = 0x0002;  ///< Ctrl.
constexpr unsigned kHotkeyShift = 0x0004;  ///< Shift.
constexpr unsigned kHotkeyWin   = 0x0008;  ///< Windows key.
/** \} */

/**
 * \brief One key combination.
 *
 * An empty chord (\c key == 0) means "no hotkey" - that is what an empty
 * string in the file produces, and how a hotkey is switched off.
 */
struct HotkeyChord {
    unsigned modifiers = 0;  ///< Combination of the \c kHotkey* bits.
    unsigned key = 0;        ///< Virtual key code; 0 for "none".

    /// \return \c true if no key is set.
    bool empty() const { return key == 0; }

    /// \return \c true if both chords are the same combination.
    bool operator==(const HotkeyChord& other) const {
        return modifiers == other.modifiers && key == other.key;
    }
};

/**
 * \brief Reads a key combination such as \c "Ctrl+Alt+Z".
 *
 * Parts are separated by \c '+', surrounding blanks are ignored, and case does
 * not matter. Understood:
 *
 * * Modifiers: \c Ctrl (\c Control, \c Strg), \c Alt, \c Shift (\c Umschalt),
 *   \c Win (\c Windows).
 * * Keys: \c A - \c Z, \c 0 - \c 9, \c F1 - \c F24, \c Space, \c Enter,
 *   \c Tab, \c Backspace, \c Insert, \c Delete, \c Home, \c End, \c PageUp,
 *   \c PageDown, \c Up, \c Down, \c Left, \c Right, \c Pause. German names
 *   (\c Leertaste, \c Eingabe, \c Entf, \c Einfg, \c Pos1, \c Ende, \c BildAuf,
 *   \c BildAb, \c Oben, \c Unten, \c Links, \c Rechts) are accepted as well.
 *
 * Refused, because they would take something away from ordinary typing:
 *
 * * a key other than F1 - F24 without Ctrl, Alt or Win - \c "Shift+A" would
 *   swallow every capital A;
 * * \c Esc in any combination - it is what closes the flyout;
 * * more than one key, or a modifier twice.
 *
 * An empty or blank string is valid and yields an empty chord.
 *
 * \param[in]  text  The text from the file.
 * \param[out] out   Written only on \c true.
 * \param[out] error Reason on \c false; may be \c nullptr.
 * \return \c false if the text is not a usable combination.
 */
bool ParseHotkey(std::wstring_view text, HotkeyChord& out, std::wstring* error = nullptr);

/**
 * \brief Spells a chord the way the tray menu and the log show it.
 *
 * Modifiers in the fixed order Win, Ctrl, Alt, Shift - the order Windows
 * itself writes them in - then the key under its
 * English name: \c "Ctrl+Alt+Z". Parsing the result gives the same chord back.
 *
 * \param chord Chord to format.
 * \return The text; empty for an empty chord.
 */
std::wstring FormatHotkey(const HotkeyChord& chord);

}  // namespace mfly
