/**
 * \file Json.h
 * \ingroup config
 * \brief Compact JSONC parser (JSON with comments) without a third-party library.
 *
 * On top of strict JSON it additionally supports:
 * - line comments \c // and block comments
 * - trailing commas in objects and arrays
 *
 * The parser is deliberately free of Windows dependencies so that it can be
 * tested in isolation.
 */
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mfly::json {

/**
 * \brief A JSON value.
 *
 * Objects preserve the order of their keys - for a configuration file that is
 * the expected semantics.
 */
class Value {
public:
    /// Value type.
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    /// \return The type of the value.
    Type type() const { return type_; }

    /**
     * \brief Checks the type of the value.
     * \param t Expected type.
     * \return \c true if the value has this type.
     */
    bool is(Type t) const { return type_ == t; }

    /// \return \c true if the value is missing or \c null.
    bool isNull() const { return type_ == Type::Null; }

    /**
     * \brief Reads the value as a boolean.
     * \param fallback Returned if the value is not a bool.
     * \return The value or \p fallback.
     */
    bool asBool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }

    /**
     * \brief Reads the value as a number.
     * \param fallback Returned if the value is not a number.
     * \return The value or \p fallback.
     */
    double asNumber(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }

    /**
     * \brief Reads the value as a string.
     * \param fallback Returned if the value is not a string.
     * \return The value or \p fallback.
     */
    std::wstring asString(const wchar_t* fallback = L"") const {
        return type_ == Type::String ? string_ : std::wstring(fallback);
    }

    /// \return The elements of an array (empty if not an array).
    const std::vector<Value>& elements() const { return array_; }

    /**
     * \brief Looks up a key in an object.
     * \param key Key to look for.
     * \return Pointer to the value, or \c nullptr if not present.
     */
    const Value* find(const wchar_t* key) const;

    /**
     * \brief Reads a boolean from an object.
     * \param key      Key.
     * \param fallback Returned if the key is missing or has another type.
     * \return The value or \p fallback.
     */
    bool boolean(const wchar_t* key, bool fallback) const;

    /**
     * \brief Reads a number from an object.
     * \param key      Key.
     * \param fallback Returned if the key is missing or has another type.
     * \return The value or \p fallback.
     */
    double number(const wchar_t* key, double fallback) const;

    /**
     * \brief Reads a string from an object.
     * \param key      Key.
     * \param fallback Returned if the key is missing or has another type.
     * \return The value or \p fallback.
     */
    std::wstring text(const wchar_t* key, const wchar_t* fallback = L"") const;

    /// \return A \c null value.
    static Value MakeNull();

    /**
     * \brief Creates a boolean.
     * \param v Content.
     * \return The created value.
     */
    static Value MakeBool(bool v);

    /**
     * \brief Creates a number.
     * \param v Content.
     * \return The created value.
     */
    static Value MakeNumber(double v);

    /**
     * \brief Creates a string.
     * \param v Content.
     * \return The created value.
     */
    static Value MakeString(std::wstring v);

    /**
     * \brief Creates an array.
     * \param v Elements.
     * \return The created value.
     */
    static Value MakeArray(std::vector<Value> v);

    /**
     * \brief Creates an object.
     * \param v Key-value pairs in order.
     * \return The created value.
     */
    static Value MakeObject(std::vector<std::pair<std::wstring, Value>> v);

private:
    Type         type_ = Type::Null;   ///< Value type.
    bool         bool_ = false;        ///< Content for \c Type::Bool.
    double       number_ = 0.0;        ///< Content for \c Type::Number.
    std::wstring string_;              ///< Content for \c Type::String.
    std::vector<Value> array_;         ///< Content for \c Type::Array.
    std::vector<std::pair<std::wstring, Value>> object_;  ///< Content for \c Type::Object.
};

/**
 * \brief Result of a parser run.
 */
struct ParseResult {
    bool         ok = false;   ///< \c true if the text was read completely.
    size_t       line = 0;     ///< Error line (1-based).
    size_t       column = 0;   ///< Error column (1-based).
    std::wstring message;      ///< Error description in plain text.
};

/**
 * \brief Reads a JSONC text.
 *
 * \param[in]  text   Text to read (without BOM).
 * \param[out] result Success or error position.
 * \return The parsed value; a \c null value on errors.
 */
Value Parse(const std::wstring& text, ParseResult& result);

}  // namespace mfly::json
