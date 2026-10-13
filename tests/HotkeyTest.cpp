/**
 * \file HotkeyTest.cpp
 * \brief Checks for the key combination parser, without a test framework.
 *
 * Exit code 0 means every check passed; a failure prints file, line and the
 * expression and makes the build fail. Built as its own target
 * \c hotkey_tests for the same reason \c overlay_tests is - see the note in
 * OverlayGeometryTest.cpp.
 */
#include <cstdio>
#include <string>

#include "Hotkey.h"

namespace {

int g_failures = 0;  ///< Number of failed checks.
int g_checks = 0;    ///< Number of checks run.

/**
 * \brief Records the result of one check.
 * \param ok    Result of the expression.
 * \param text  The expression as written.
 * \param file  Source file.
 * \param line  Source line.
 */
void Check(bool ok, const char* text, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED  %s(%d): %s\n", file, line, text);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

using mfly::HotkeyChord;
using mfly::ParseHotkey;

/// \return The parsed chord, or an empty one with \p ok set to \c false.
HotkeyChord Parse(const wchar_t* text, bool& ok) {
    HotkeyChord chord{0xFFFF, 0xFFFF};
    ok = ParseHotkey(text, chord);
    return chord;
}

/// \return \c true if \p text parses.
bool Accepts(const wchar_t* text) {
    HotkeyChord chord;
    return ParseHotkey(text, chord);
}

/// The default and its spellings.
void TestBasics() {
    bool ok = false;
    HotkeyChord c = Parse(L"Ctrl+Alt+Z", ok);
    CHECK(ok);
    CHECK(c.modifiers == (mfly::kHotkeyCtrl | mfly::kHotkeyAlt));
    CHECK(c.key == 'Z');

    // Order, case and blanks do not matter; German names are fine.
    c = Parse(L"  alt + STRG +z ", ok);
    CHECK(ok && c.modifiers == (mfly::kHotkeyCtrl | mfly::kHotkeyAlt) && c.key == 'Z');

    c = Parse(L"Win+Shift+F10", ok);
    CHECK(ok && c.modifiers == (mfly::kHotkeyWin | mfly::kHotkeyShift) && c.key == 0x79);

    c = Parse(L"Ctrl+Alt+Leertaste", ok);
    CHECK(ok && c.key == 0x20);

    c = Parse(L"Ctrl+Alt+7", ok);
    CHECK(ok && c.key == '7');

    c = Parse(L"Ctrl+Alt+BildAb", ok);
    CHECK(ok && c.key == 0x22);

    // The bits are those RegisterHotKey expects.
    CHECK(mfly::kHotkeyAlt == 0x1 && mfly::kHotkeyCtrl == 0x2);
    CHECK(mfly::kHotkeyShift == 0x4 && mfly::kHotkeyWin == 0x8);
}

/// Empty switches the hotkey off.
void TestEmpty() {
    bool ok = false;
    HotkeyChord c = Parse(L"", ok);
    CHECK(ok && c.empty() && c.modifiers == 0);
    c = Parse(L"   ", ok);
    CHECK(ok && c.empty());
}

/// Function keys may stand alone, nothing else may.
void TestTypingStaysIntact() {
    CHECK(Accepts(L"F13"));
    CHECK(Accepts(L"Shift+F9"));
    CHECK(!Accepts(L"Z"));
    CHECK(!Accepts(L"Shift+Z"));
    CHECK(!Accepts(L"Space"));
    CHECK(Accepts(L"Alt+Space"));
}

/// What is refused, and that the reason is given.
void TestRefused() {
    CHECK(!Accepts(L"Ctrl+Alt"));          // no key
    CHECK(!Accepts(L"Ctrl+Alt+Z+X"));      // two keys
    CHECK(!Accepts(L"Ctrl+Ctrl+Z"));       // modifier twice
    CHECK(!Accepts(L"Ctrl++Z"));           // empty part
    CHECK(!Accepts(L"Ctrl+Alt+"));         // trailing '+'
    CHECK(!Accepts(L"Ctrl+Alt+Esc"));      // closes the flyout
    CHECK(!Accepts(L"Ctrl+Alt+F25"));      // no such key
    CHECK(!Accepts(L"Ctrl+Alt+Ä"));        // layout dependent, not offered
    CHECK(!Accepts(L"Hyper+Z"));

    HotkeyChord c{1, 2};
    std::wstring error;
    CHECK(!ParseHotkey(L"Ctrl+Alt+Banana", c, &error));
    CHECK(error.find(L"banana") != std::wstring::npos);
    // A refused text leaves the output alone.
    CHECK(c.modifiers == 1 && c.key == 2);
}

/// Formatting, and that it parses back.
void TestFormat() {
    CHECK(mfly::FormatHotkey(HotkeyChord{}).empty());

    const wchar_t* samples[] = {L"Ctrl+Alt+Z", L"Win+Shift+F10", L"Ctrl+Alt+Space",
                                L"Alt+PageDown", L"Win+Ctrl+Alt+Shift+Delete",
                                L"F13", L"Ctrl+Alt+0"};
    for (const wchar_t* sample : samples) {
        HotkeyChord parsed;
        CHECK(ParseHotkey(sample, parsed));
        CHECK(mfly::FormatHotkey(parsed) == sample);
    }

    // Fixed modifier order, whatever the file said.
    HotkeyChord c;
    CHECK(ParseHotkey(L"shift+alt+ctrl+pgup", c));
    CHECK(mfly::FormatHotkey(c) == L"Ctrl+Alt+Shift+PageUp");
}

}  // namespace

int main() {
    TestBasics();
    TestEmpty();
    TestTypingStaysIntact();
    TestRefused();
    TestFormat();

    std::printf("Hotkey: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
