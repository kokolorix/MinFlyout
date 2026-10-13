/**
 * \file Hotkey.cpp
 * \ingroup config
 * \brief Implementation of the key combination parser.
 */
#include "Hotkey.h"

#include <cwctype>
#include <iterator>

namespace mfly {
namespace {

/// A named key and its virtual key code.
struct NamedKey {
    const wchar_t* name;  ///< Lower-case name as written in the file.
    unsigned key;         ///< Virtual key code (the value of the \c VK_* constant).
};

/**
 * \brief Keys known by name. The first entry per code is the one
 *        \ref FormatHotkey writes.
 */
constexpr NamedKey kNamedKeys[] = {
    {L"space", 0x20},     {L"leertaste", 0x20},
    {L"enter", 0x0D},     {L"return", 0x0D},    {L"eingabe", 0x0D},
    {L"tab", 0x09},
    {L"backspace", 0x08}, {L"back", 0x08},
    {L"insert", 0x2D},    {L"ins", 0x2D},       {L"einfg", 0x2D},
    {L"delete", 0x2E},    {L"del", 0x2E},       {L"entf", 0x2E},
    {L"home", 0x24},      {L"pos1", 0x24},
    {L"end", 0x23},       {L"ende", 0x23},
    {L"pageup", 0x21},    {L"pgup", 0x21},      {L"bildauf", 0x21},
    {L"pagedown", 0x22},  {L"pgdn", 0x22},      {L"bildab", 0x22},
    {L"left", 0x25},      {L"links", 0x25},
    {L"up", 0x26},        {L"oben", 0x26},
    {L"right", 0x27},     {L"rechts", 0x27},
    {L"down", 0x28},      {L"unten", 0x28},
    {L"pause", 0x13},
};

/// Virtual key code of Esc - refused, see \ref ParseHotkey.
constexpr unsigned kKeyEscape = 0x1B;
/// Virtual key code of F1; F24 is <code>kKeyF1 + 23</code>.
constexpr unsigned kKeyF1 = 0x70;

/// \return \p text without leading and trailing blanks.
std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

/// \return \p text in lower case (ASCII is all the names need).
std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

/// \return The modifier bit for \p name, or 0 if it is no modifier.
unsigned ModifierOf(const std::wstring& name) {
    if (name == L"ctrl" || name == L"control" || name == L"strg") return kHotkeyCtrl;
    if (name == L"alt") return kHotkeyAlt;
    if (name == L"shift" || name == L"umschalt") return kHotkeyShift;
    if (name == L"win" || name == L"windows") return kHotkeyWin;
    return 0;
}

/// \return The virtual key code for \p name, or 0 if the name is unknown.
unsigned KeyOf(const std::wstring& name) {
    if (name.size() == 1) {
        const wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') return static_cast<unsigned>(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9') return static_cast<unsigned>(c);
    }

    if (name.size() >= 2 && name.size() <= 3 && name[0] == L'f') {
        unsigned number = 0;
        for (size_t i = 1; i < name.size(); ++i) {
            if (name[i] < L'0' || name[i] > L'9') return 0;
            number = number * 10 + static_cast<unsigned>(name[i] - L'0');
        }
        if (number >= 1 && number <= 24) return kKeyF1 + number - 1;
        return 0;
    }

    if (name == L"esc" || name == L"escape") return kKeyEscape;

    for (const NamedKey& named : kNamedKeys) {
        if (name == named.name) return named.key;
    }
    return 0;
}

/// \return \c true for F1 - F24.
bool IsFunctionKey(unsigned key) {
    return key >= kKeyF1 && key < kKeyF1 + 24;
}

/// Writes \p text to \p error if there is one; always returns \c false.
bool Fail(std::wstring* error, std::wstring text) {
    if (error) *error = std::move(text);
    return false;
}

}  // namespace

bool ParseHotkey(std::wstring_view text, HotkeyChord& out, std::wstring* error) {
    text = Trim(text);
    if (text.empty()) {
        out = HotkeyChord{};
        return true;
    }

    HotkeyChord chord;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t plus = text.find(L'+', start);
        const size_t end = plus == std::wstring_view::npos ? text.size() : plus;
        const std::wstring part = Lower(Trim(text.substr(start, end - start)));

        if (part.empty()) return Fail(error, L"an empty part between two '+'");

        if (const unsigned modifier = ModifierOf(part)) {
            if (chord.modifiers & modifier) {
                return Fail(error, L"'" + part + L"' appears twice");
            }
            chord.modifiers |= modifier;
        } else {
            const unsigned key = KeyOf(part);
            if (key == 0) return Fail(error, L"unknown key '" + part + L"'");
            if (chord.key != 0) return Fail(error, L"more than one key");
            chord.key = key;
        }

        if (plus == std::wstring_view::npos) break;
        start = plus + 1;
    }

    if (chord.key == 0) return Fail(error, L"no key, only modifiers");
    if (chord.key == kKeyEscape) return Fail(error, L"Esc closes the flyout");
    if (!IsFunctionKey(chord.key) &&
        (chord.modifiers & (kHotkeyCtrl | kHotkeyAlt | kHotkeyWin)) == 0) {
        return Fail(error, L"needs Ctrl, Alt or Win - otherwise typing that key "
                           L"would trigger it");
    }

    out = chord;
    return true;
}

std::wstring FormatHotkey(const HotkeyChord& chord) {
    if (chord.empty()) return {};

    std::wstring text;
    if (chord.modifiers & kHotkeyWin)   text += L"Win+";
    if (chord.modifiers & kHotkeyCtrl)  text += L"Ctrl+";
    if (chord.modifiers & kHotkeyAlt)   text += L"Alt+";
    if (chord.modifiers & kHotkeyShift) text += L"Shift+";

    const unsigned key = chord.key;
    if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9')) {
        text += static_cast<wchar_t>(key);
    } else if (IsFunctionKey(key)) {
        text += L"F" + std::to_wstring(key - kKeyF1 + 1);
    } else {
        for (const NamedKey& named : kNamedKeys) {
            if (named.key != key) continue;
            std::wstring name = named.name;
            name[0] = static_cast<wchar_t>(std::towupper(name[0]));
            if (name == L"Pageup") name = L"PageUp";
            if (name == L"Pagedown") name = L"PageDown";
            text += name;
            break;
        }
    }
    return text;
}

}  // namespace mfly
