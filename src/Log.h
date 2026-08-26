/**
 * \file Log.h
 * \ingroup app
 * \brief Debug logging: WRITE_DEBUG_LOG and friends.
 *
 * The macros expand to a functor that carries level, function, file and line,
 * so the call site stays short:
 *
 * \code{.cpp}
 * WRITE_INFO_LOG(L"Flyout opened");
 * WRITE_DEBUG_LOG(L"Button probed", elapsed.ElapsedMs());        // with duration
 * WRITE_WARNING_LOG(L"Config rejected", config.error);           // with detail
 * WRITE_ERROR_LOG(L"SetWindowPos failed", detail, elapsed.ElapsedMs());
 * \endcode
 *
 * Logging is compiled in when \c _DEBUG or \c _RELEASE_WITH_DEBUG_LOG is
 * defined. Otherwise the macros swallow their arguments without evaluating
 * them; see the disabled branch at the bottom of this file.
 *
 * Every line is tab separated, has exactly eight columns and never leaves one
 * empty ("-" stands in), so the file can be read by a log viewer or pasted
 * into a spreadsheet as it is:
 *
 * ```
 * time  thread  level  duration  message  function  file(line)  detail
 * ```
 */
#pragma once

#include "Common.h"

#include <format>
#include <string_view>

namespace mfly::log {

/**
 * \brief Thin wrapper around \c std::format that always yields a wide string.
 *
 * Exists so call sites read the same everywhere and the formatting library can
 * be swapped in one place.
 *
 * \param fmt  Format string.
 * \param args Arguments for the placeholders.
 * \return The formatted string.
 */
template <class... Args>
std::wstring dformat(std::wformat_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

/**
 * \brief Writes one finished line to all active sinks.
 *
 * Always goes to \c OutputDebugStringW; additionally to the log file when
 * \ref mfly::log::SetFileLogging enabled it. The function is thread-safe - the hook
 * thread and the UI thread both log.
 *
 * \param line Line to write (without a trailing newline).
 */
void writeDebugLog(const std::wstring& line);

/**
 * \brief Switches file logging on or off.
 *
 * The file lives next to the configuration in
 * \c %APPDATA%\\MinFlyout\\minflyout.log and is rotated to \c .1 once it
 * exceeds one megabyte.
 *
 * \param enabled \c true also writes to the file.
 */
void SetFileLogging(bool enabled);

/// \return Full path of the log file (empty if \c %APPDATA% is unavailable).
std::wstring FilePath();

/**
 * \brief Describes a foreign window for the detail column.
 *
 * Produces something like \c "'Untitled - Notepad' [Notepad] pid 1234" - class
 * and title are what actually identify a window when reading a log.
 *
 * \param window Window to describe (may be invalid).
 * \return One-line description.
 */
std::wstring Describe(HWND window);

/**
 * \brief Stopwatch for the duration overloads.
 *
 * Uses the performance counter, so short operations such as the caption probe
 * are measured accurately rather than in 16 ms ticks.
 */
class Stopwatch {
public:
    /// Starts the measurement.
    Stopwatch();

    /// \return Milliseconds since construction, rounded to the nearest integer.
    int ElapsedMs() const;

private:
    LONGLONG start_ = 0;  ///< Performance counter at construction.
};

/**
 * \brief Carries the call site and formats the final line.
 *
 * One instance is created per log statement by the \c WRITE_*_LOG macros.
 * Function and file come from the preprocessor as narrow literals and are only
 * widened when a line is actually written.
 */
struct Writer {
    const wchar_t* level;     ///< DEBUG, INFO, WARN or ERROR - names log viewers recognise.
    const char*    function;  ///< \c __FUNCTION__ of the call site.
    const char*    file;      ///< \c __FILE__ of the call site.
    int            line;      ///< \c __LINE__ of the call site.

    /**
     * \brief Logs a message.
     * \param msg Message text.
     */
    void operator()(std::wstring_view msg) const;

    /**
     * \brief Logs a message with a measured duration.
     * \param msg        Message text.
     * \param durationMs Duration in milliseconds.
     */
    void operator()(std::wstring_view msg, int durationMs) const;

    /**
     * \brief Logs a message with additional detail.
     * \param msg    Message text.
     * \param detail Detail column, e.g. a window title or an error text.
     */
    void operator()(std::wstring_view msg, std::wstring_view detail) const;

    /**
     * \brief Logs a message with detail and duration.
     * \param msg        Message text.
     * \param detail     Detail column.
     * \param durationMs Duration in milliseconds.
     */
    void operator()(std::wstring_view msg, std::wstring_view detail, int durationMs) const;
};

/**
 * \brief Accepts any arguments and does nothing.
 *
 * Used only inside the discarded branch of the disabled macros, so that the
 * arguments still count as used and keep compiling.
 *
 * \param args Arguments of the log statement; ignored.
 */
template <class... Args>
inline void Discard(const Args&... args) { (void)sizeof...(args); }

}  // namespace mfly::log

// ---------------------------------------------------------------------------
// The macros themselves
// ---------------------------------------------------------------------------
#if defined(_DEBUG) || defined(_RELEASE_WITH_DEBUG_LOG)

/// Logs at level DEBUG; call it like a function, see \ref mfly::log::Writer.
#define WRITE_DEBUG_LOG   ::mfly::log::Writer{L"DEBUG",   __FUNCTION__, __FILE__, __LINE__}
/// Logs at level INFO; call it like a function, see \ref mfly::log::Writer.
#define WRITE_INFO_LOG    ::mfly::log::Writer{L"INFO",    __FUNCTION__, __FILE__, __LINE__}
/// Logs at level WARNING; call it like a function, see \ref mfly::log::Writer.
#define WRITE_WARNING_LOG ::mfly::log::Writer{L"WARN",    __FUNCTION__, __FILE__, __LINE__}
/// Logs at level ERROR; call it like a function, see \ref mfly::log::Writer.
#define WRITE_ERROR_LOG   ::mfly::log::Writer{L"ERROR",   __FUNCTION__, __FILE__, __LINE__}

#else

// Variadic, so every overload (message, +detail, +duration) still compiles.
// "if constexpr (false)" discards the statement: nothing is evaluated at run
// time, yet the arguments are still type-checked and count as used - no
// "unreferenced local variable" warnings, and the logging code cannot rot.
/// Discards a log statement without evaluating its arguments.
#define MFLY_LOG_DISCARD(...)                                   \
    do {                                                        \
        if constexpr (false) {                                  \
            ::mfly::log::Discard(__VA_ARGS__);                  \
        }                                                       \
    } while (false)

/// Disabled counterpart of the DEBUG macro.
#define WRITE_DEBUG_LOG(...)   MFLY_LOG_DISCARD(__VA_ARGS__)
/// Disabled counterpart of the INFO macro.
#define WRITE_INFO_LOG(...)    MFLY_LOG_DISCARD(__VA_ARGS__)
/// Disabled counterpart of the WARNING macro.
#define WRITE_WARNING_LOG(...) MFLY_LOG_DISCARD(__VA_ARGS__)
/// Disabled counterpart of the ERROR macro.
#define WRITE_ERROR_LOG(...)   MFLY_LOG_DISCARD(__VA_ARGS__)

#endif  // _DEBUG || _RELEASE_WITH_DEBUG_LOG
