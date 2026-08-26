/**
 * \file Json.cpp
 * \ingroup config
 * \brief Recursive-descent JSONC parser.
 */
#include "Json.h"

#include <cwchar>
#include <cwctype>
#include <cstdlib>

namespace mfly::json {
namespace {

/// Maximum nesting depth - protects the stack against malicious files.
constexpr int kMaxDepth = 64;

/**
 * \brief State of a parser run.
 */
class Parser {
public:
    /**
     * \brief Creates a parser for the given text.
     * \param text Text to read.
     */
    explicit Parser(const std::wstring& text) : text_(text) {}

    /**
     * \brief Reads exactly one value and checks that only whitespace follows.
     * \param[out] result Success or error position.
     * \return The value that was read.
     */
    Value ParseDocument(ParseResult& result) {
        Value value = ParseValue(0);
        if (ok_) {
            SkipTrivia();
            if (pos_ < text_.size()) Fail(L"Unexpected characters after the value");
        }
        result.ok = ok_;
        result.line = line_;
        result.column = column_;
        result.message = message_;
        return ok_ ? value : Value::MakeNull();
    }

private:
    /// Reports an error at the current position (the first one wins).
    void Fail(const wchar_t* message) {
        if (!ok_) return;
        ok_ = false;
        message_ = message;
    }

    /// \return \c true if the end of the text has been reached.
    bool AtEnd() const { return pos_ >= text_.size(); }

    /// \return The current character, or \c 0 at the end of the text.
    wchar_t Peek() const { return AtEnd() ? L'\0' : text_[pos_]; }

    /// \return The character \p offset positions ahead, or \c 0.
    wchar_t PeekAt(size_t offset) const {
        return (pos_ + offset < text_.size()) ? text_[pos_ + offset] : L'\0';
    }

    /// Consumes one character and updates the line/column counters.
    wchar_t Take() {
        wchar_t c = text_[pos_++];
        if (c == L'\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    /// Skips whitespace and comments.
    void SkipTrivia() {
        while (!AtEnd()) {
            const wchar_t c = Peek();
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                Take();
            } else if (c == L'/' && PeekAt(1) == L'/') {
                while (!AtEnd() && Peek() != L'\n') Take();
            } else if (c == L'/' && PeekAt(1) == L'*') {
                Take();
                Take();
                bool closed = false;
                while (!AtEnd()) {
                    if (Peek() == L'*' && PeekAt(1) == L'/') {
                        Take();
                        Take();
                        closed = true;
                        break;
                    }
                    Take();
                }
                if (!closed) {
                    Fail(L"Unterminated block comment");
                    return;
                }
            } else {
                return;
            }
        }
    }

    /// Expects a specific character and consumes it.
    bool Expect(wchar_t expected, const wchar_t* message) {
        SkipTrivia();
        if (!ok_) return false;
        if (Peek() != expected) {
            Fail(message);
            return false;
        }
        Take();
        return true;
    }

    /// Reads any value.
    Value ParseValue(int depth) {
        if (depth > kMaxDepth) {
            Fail(L"Nesting too deep");
            return Value::MakeNull();
        }
        SkipTrivia();
        if (!ok_) return Value::MakeNull();
        if (AtEnd()) {
            Fail(L"Unexpected end of input");
            return Value::MakeNull();
        }

        switch (Peek()) {
        case L'{': return ParseObject(depth);
        case L'[': return ParseArray(depth);
        case L'"': return ParseString();
        case L't': return ParseLiteral(L"true", Value::MakeBool(true));
        case L'f': return ParseLiteral(L"false", Value::MakeBool(false));
        case L'n': return ParseLiteral(L"null", Value::MakeNull());
        default:   return ParseNumber();
        }
    }

    /// Reads an object, allowing a trailing comma.
    Value ParseObject(int depth) {
        std::vector<std::pair<std::wstring, Value>> members;
        Take();  // '{'

        while (true) {
            SkipTrivia();
            if (!ok_) return Value::MakeNull();
            if (AtEnd()) {
                Fail(L"Unterminated object");
                return Value::MakeNull();
            }
            if (Peek() == L'}') {
                Take();
                break;
            }
            if (Peek() != L'"') {
                Fail(L"Expected a quoted key");
                return Value::MakeNull();
            }

            Value key = ParseString();
            if (!ok_) return Value::MakeNull();
            if (!Expect(L':', L"Expected ':' after the key")) {
                return Value::MakeNull();
            }

            Value item = ParseValue(depth + 1);
            if (!ok_) return Value::MakeNull();
            members.emplace_back(key.asString(), std::move(item));

            SkipTrivia();
            if (Peek() == L',') {
                Take();
                continue;  // a trailing comma is allowed
            }
            if (Peek() == L'}') {
                Take();
                break;
            }
            Fail(L"Expected ',' or '}'");
            return Value::MakeNull();
        }
        return Value::MakeObject(std::move(members));
    }

    /// Reads an array, allowing a trailing comma.
    Value ParseArray(int depth) {
        std::vector<Value> elements;
        Take();  // '['

        while (true) {
            SkipTrivia();
            if (!ok_) return Value::MakeNull();
            if (AtEnd()) {
                Fail(L"Unterminated array");
                return Value::MakeNull();
            }
            if (Peek() == L']') {
                Take();
                break;
            }

            elements.push_back(ParseValue(depth + 1));
            if (!ok_) return Value::MakeNull();

            SkipTrivia();
            if (Peek() == L',') {
                Take();
                continue;
            }
            if (Peek() == L']') {
                Take();
                break;
            }
            Fail(L"Expected ',' or ']'");
            return Value::MakeNull();
        }
        return Value::MakeArray(std::move(elements));
    }

    /// Reads a string including its escape sequences.
    Value ParseString() {
        std::wstring out;
        Take();  // '"'

        while (true) {
            if (AtEnd()) {
                Fail(L"Unterminated string");
                return Value::MakeNull();
            }
            wchar_t c = Take();
            if (c == L'"') break;
            if (c == L'\n') {
                Fail(L"Line break inside a string");
                return Value::MakeNull();
            }
            if (c != L'\\') {
                out.push_back(c);
                continue;
            }

            if (AtEnd()) {
                Fail(L"Incomplete escape sequence");
                return Value::MakeNull();
            }
            const wchar_t esc = Take();
            switch (esc) {
            case L'"':  out.push_back(L'"');  break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/':  out.push_back(L'/');  break;
            case L'b':  out.push_back(L'\b'); break;
            case L'f':  out.push_back(L'\f'); break;
            case L'n':  out.push_back(L'\n'); break;
            case L'r':  out.push_back(L'\r'); break;
            case L't':  out.push_back(L'\t'); break;
            case L'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (AtEnd()) {
                        Fail(L"Incomplete \\u escape");
                        return Value::MakeNull();
                    }
                    const wchar_t h = Take();
                    unsigned digit = 0;
                    if (h >= L'0' && h <= L'9')      digit = static_cast<unsigned>(h - L'0');
                    else if (h >= L'a' && h <= L'f') digit = static_cast<unsigned>(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') digit = static_cast<unsigned>(h - L'A' + 10);
                    else {
                        Fail(L"Invalid digit in \\u escape");
                        return Value::MakeNull();
                    }
                    code = code * 16 + digit;
                }
                // On Windows wchar_t is UTF-16, so surrogates fit directly.
                out.push_back(static_cast<wchar_t>(code));
                break;
            }
            default:
                Fail(L"Unknown escape sequence");
                return Value::MakeNull();
            }
        }
        return Value::MakeString(std::move(out));
    }

    /// Reads a keyword (\c true, \c false, \c null).
    Value ParseLiteral(const wchar_t* literal, Value value) {
        for (const wchar_t* p = literal; *p; ++p) {
            if (AtEnd() || Peek() != *p) {
                Fail(L"Unknown keyword");
                return Value::MakeNull();
            }
            Take();
        }
        return value;
    }

    /// Reads a number.
    Value ParseNumber() {
        const size_t start = pos_;
        if (Peek() == L'-' || Peek() == L'+') Take();

        bool digits = false;
        while (!AtEnd() && Peek() >= L'0' && Peek() <= L'9') {
            Take();
            digits = true;
        }
        if (!AtEnd() && Peek() == L'.') {
            Take();
            while (!AtEnd() && Peek() >= L'0' && Peek() <= L'9') {
                Take();
                digits = true;
            }
        }
        if (digits && !AtEnd() && (Peek() == L'e' || Peek() == L'E')) {
            Take();
            if (!AtEnd() && (Peek() == L'+' || Peek() == L'-')) Take();
            while (!AtEnd() && Peek() >= L'0' && Peek() <= L'9') Take();
        }
        if (!digits) {
            Fail(L"Expected a number");
            return Value::MakeNull();
        }

        const std::wstring literal = text_.substr(start, pos_ - start);
        return Value::MakeNumber(std::wcstod(literal.c_str(), nullptr));
    }

    const std::wstring& text_;      ///< Text to read.
    size_t   pos_ = 0;              ///< Current read position.
    size_t   line_ = 1;             ///< Current line.
    size_t   column_ = 1;           ///< Current column.
    bool     ok_ = true;            ///< No error has occurred yet.
    std::wstring message_;          ///< Error description.
};

}  // namespace

const Value* Value::find(const wchar_t* key) const {
    if (type_ != Type::Object || !key) return nullptr;
    for (const auto& member : object_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

bool Value::boolean(const wchar_t* key, bool fallback) const {
    const Value* v = find(key);
    return v ? v->asBool(fallback) : fallback;
}

double Value::number(const wchar_t* key, double fallback) const {
    const Value* v = find(key);
    return v ? v->asNumber(fallback) : fallback;
}

std::wstring Value::text(const wchar_t* key, const wchar_t* fallback) const {
    const Value* v = find(key);
    return v ? v->asString(fallback) : std::wstring(fallback);
}

Value Value::MakeNull() { return Value(); }

Value Value::MakeBool(bool v) {
    Value out;
    out.type_ = Type::Bool;
    out.bool_ = v;
    return out;
}

Value Value::MakeNumber(double v) {
    Value out;
    out.type_ = Type::Number;
    out.number_ = v;
    return out;
}

Value Value::MakeString(std::wstring v) {
    Value out;
    out.type_ = Type::String;
    out.string_ = std::move(v);
    return out;
}

Value Value::MakeArray(std::vector<Value> v) {
    Value out;
    out.type_ = Type::Array;
    out.array_ = std::move(v);
    return out;
}

Value Value::MakeObject(std::vector<std::pair<std::wstring, Value>> v) {
    Value out;
    out.type_ = Type::Object;
    out.object_ = std::move(v);
    return out;
}

Value Parse(const std::wstring& text, ParseResult& result) {
    Parser parser(text);
    return parser.ParseDocument(result);
}

}  // namespace mfly::json
