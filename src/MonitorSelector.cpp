/**
 * \file MonitorSelector.cpp
 * \ingroup config
 * \brief Implementation of the monitor selectors.
 */
#include "MonitorSelector.h"

#include <algorithm>
#include <cwchar>

namespace mfly {
namespace {

/**
 * \brief Lowercases an ASCII letter.
 *
 * Only A-Z; everything else is passed through unchanged. Keywords and device
 * names are ASCII, and a table-free mapping keeps the result independent of any
 * locale.
 *
 * \param c Character to map.
 * \return The lowercase letter, or \p c.
 */
constexpr wchar_t Lower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

/**
 * \brief Compares two strings ignoring ASCII case.
 * \param a First string.
 * \param b Second string.
 * \return \c true if both are equal apart from the case of ASCII letters.
 */
bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

/**
 * \brief Strips leading and trailing blanks.
 * \param text Input.
 * \return The trimmed copy.
 */
std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return std::wstring();
    const size_t last = text.find_last_not_of(L" \t");
    return text.substr(first, last - first + 1);
}

/**
 * \brief Checks that a string consists of digits only.
 * \param text Input; an empty string is not a number.
 * \return \c true if every character is 0-9.
 */
bool IsNumber(const std::wstring& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(),
                       [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

/**
 * \brief Reads a selector of the form \c "1920x1080".
 * \param[in]  text   Selector to read (already trimmed).
 * \param[out] width  Width in pixels.
 * \param[out] height Height in pixels.
 * \return \c true if the whole text was a resolution.
 */
bool ParseResolution(const std::wstring& text, long& width, long& height) {
    const size_t sep = text.find_first_of(L"xX\u00D7");
    if (sep == std::wstring::npos || sep == 0 || sep + 1 >= text.size()) return false;

    const std::wstring left = Trim(text.substr(0, sep));
    const std::wstring right = Trim(text.substr(sep + 1));
    if (!IsNumber(left) || !IsNumber(right)) return false;

    width = std::wcstol(left.c_str(), nullptr, 10);
    height = std::wcstol(right.c_str(), nullptr, 10);
    return width > 0 && height > 0;
}

/**
 * \brief Drops everything up to and including the last backslash.
 * \param device Device name.
 * \return \c DISPLAY2 for <code>\\.\DISPLAY2</code>.
 */
std::wstring LastComponent(const std::wstring& device) {
    const size_t slash = device.find_last_of(L'\\');
    return slash == std::wstring::npos ? device : device.substr(slash + 1);
}

}  // namespace

bool SelectorMatches(const MonitorFacts& monitor, const std::wstring& selector) {
    const std::wstring token = Trim(selector);
    if (token.empty()) return false;

    if (token == L"*" || EqualsNoCase(token, L"all")) return true;
    if (EqualsNoCase(token, L"primary")) return monitor.primary;
    if (EqualsNoCase(token, L"secondary")) return !monitor.primary;

    // A plain number is the index the flyout caption shows.
    if (IsNumber(token)) {
        return std::wcstol(token.c_str(), nullptr, 10) == monitor.index;
    }

    long width = 0, height = 0;
    if (ParseResolution(token, width, height)) {
        return monitor.width == width && monitor.height == height;
    }

    // Whatever is left is a device name, with or without the "\\.\" prefix.
    return EqualsNoCase(token, monitor.device) ||
           EqualsNoCase(token, LastComponent(monitor.device));
}

}  // namespace mfly
