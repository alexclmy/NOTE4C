/**
 * @file json_scan.cc
 * @brief Implementation of the strict, allocation-free JSON reader.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See json_scan.h for why this exists rather than cJSON. No ESP-IDF headers
 * here: the host suite compiles this exact translation unit.
 */

#include "json_scan.h"

#include <stdlib.h>
#include <string.h>

namespace json {

const char* const kErrNone = "none";
const char* const kErrSyntax = "json_syntax";
const char* const kErrDepth = "json_too_deep";
const char* const kErrType = "json_wrong_type";
const char* const kErrNumberRange = "json_number_range";
const char* const kErrStringTooLong = "json_string_too_long";
const char* const kErrControlChar = "json_control_char";
const char* const kErrEscape = "json_bad_escape";
const char* const kErrTrailing = "json_trailing_bytes";

namespace {

inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

/// JSON whitespace is exactly these four bytes. Notably not a vertical tab or a
/// form feed, both of which some parsers accept and which would then be legal
/// in a document the tower would refuse to produce.
inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Append one code point as UTF-8.
 * @return bytes written, or 0 when the buffer could not take it.
 *
 * The caller has already bounded the buffer; this returns 0 rather than writing
 * a partial sequence, because half a code point on a panel is a replacement
 * glyph that nobody can trace back to a truncation.
 */
size_t AppendUtf8(uint32_t cp, char* out, size_t cap) {
    if (cp < 0x80) {
        if (cap < 1) return 0;
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) return 0;
        out[0] = static_cast<char>(0xc0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) return 0;
        out[0] = static_cast<char>(0xe0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out[2] = static_cast<char>(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = static_cast<char>(0xf0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out[3] = static_cast<char>(0x80 | (cp & 0x3f));
    return 4;
}

}  // namespace

Reader::Reader(const char* data, size_t len)
    : data_(data), len_(data == nullptr ? 0 : len), error_(kErrNone) {}

void Reader::Fail(const char* token) {
    // First refusal wins. A schema walk keeps going after a field-level error
    // in some callers, and the last error it stumbles into is almost always
    // less informative than the one that started it.
    if (error_ == kErrNone) {
        error_ = token;
        error_offset_ = pos_;
    }
}

void Reader::SkipWhitespace() {
    while (pos_ < len_ && IsSpace(data_[pos_])) ++pos_;
}

bool Reader::Expect(char c) {
    SkipWhitespace();
    if (AtEnd() || data_[pos_] != c) {
        Fail(kErrSyntax);
        return false;
    }
    ++pos_;
    return true;
}

bool Reader::Push() {
    if (depth_ >= kMaxDepth) {
        Fail(kErrDepth);
        return false;
    }
    started_[depth_] = false;
    ++depth_;
    return true;
}

void Reader::Pop() {
    if (depth_ > 0) --depth_;
}

bool Reader::PeekType(Type* out) {
    if (!ok()) return false;
    SkipWhitespace();
    if (AtEnd()) {
        Fail(kErrSyntax);
        return false;
    }
    switch (Cur()) {
        case 'n': *out = Type::kNull; return true;
        case 't':
        case 'f': *out = Type::kBool; return true;
        case '"': *out = Type::kString; return true;
        case '{': *out = Type::kObject; return true;
        case '[': *out = Type::kArray; return true;
        default:
            if (Cur() == '-' || IsDigit(Cur())) {
                *out = Type::kNumber;
                return true;
            }
            Fail(kErrSyntax);
            return false;
    }
}

bool Reader::EnterObject() {
    if (!ok()) return false;
    if (!Expect('{')) return false;
    return Push();
}

bool Reader::EnterArray() {
    if (!ok()) return false;
    if (!Expect('[')) return false;
    return Push();
}

bool Reader::NextMember(char close, bool* has_member) {
    *has_member = false;
    if (!ok()) return false;
    if (depth_ <= 0) {
        Fail(kErrSyntax);
        return false;
    }
    SkipWhitespace();
    if (AtEnd()) {
        Fail(kErrSyntax);
        return false;
    }
    if (Cur() == close) {
        ++pos_;
        Pop();
        return true;
    }
    if (started_[depth_ - 1]) {
        if (Cur() != ',') {
            Fail(kErrSyntax);
            return false;
        }
        ++pos_;
        SkipWhitespace();
        // A comma followed by the close brace is a trailing comma. Legal in
        // JavaScript, not in JSON, and accepting it here would mean the tower
        // and the device disagree about whether a document is well formed.
        if (AtEnd() || Cur() == close) {
            Fail(kErrSyntax);
            return false;
        }
    }
    started_[depth_ - 1] = true;
    *has_member = true;
    return true;
}

bool Reader::NextKey(char* key, size_t key_cap, size_t* key_len) {
    bool has_member = false;
    if (!NextMember('}', &has_member)) return false;
    if (!has_member) return false;
    if (!ReadStringInto(key, key_cap, key_len)) return false;
    return Expect(':');
}

bool Reader::NextElement() {
    bool has_member = false;
    if (!NextMember(']', &has_member)) return false;
    return has_member;
}

bool Reader::ReadStringInto(char* out, size_t cap, size_t* len_out) {
    if (!ok()) return false;
    SkipWhitespace();
    if (AtEnd() || Cur() != '"') {
        Fail(kErrType);
        return false;
    }
    ++pos_;

    size_t written = 0;
    // Reserve one byte for the terminator throughout, so the buffer is never
    // filled to capacity and then found to have nowhere to put the NUL.
    const size_t limit = (cap == 0) ? 0 : cap - 1;

    for (;;) {
        if (AtEnd()) {
            Fail(kErrSyntax);
            return false;
        }
        const unsigned char c = static_cast<unsigned char>(data_[pos_]);
        if (c == '"') {
            ++pos_;
            break;
        }
        if (c < 0x20) {
            // A literal newline or NUL inside a string. Invalid JSON, and on
            // this device also a value the panel could not draw.
            Fail(kErrControlChar);
            return false;
        }
        if (c != '\\') {
            if (written >= limit) {
                Fail(kErrStringTooLong);
                return false;
            }
            if (out != nullptr) out[written] = static_cast<char>(c);
            ++written;
            ++pos_;
            continue;
        }

        // Escape sequence.
        ++pos_;
        if (AtEnd()) {
            Fail(kErrSyntax);
            return false;
        }
        const char esc = data_[pos_];
        char simple = '\0';
        switch (esc) {
            case '"': simple = '"'; break;
            case '\\': simple = '\\'; break;
            case '/': simple = '/'; break;
            case 'b': simple = '\b'; break;
            case 'f': simple = '\f'; break;
            case 'n': simple = '\n'; break;
            case 'r': simple = '\r'; break;
            case 't': simple = '\t'; break;
            case 'u': break;
            default:
                Fail(kErrEscape);
                return false;
        }
        if (esc != 'u') {
            ++pos_;
            if (written >= limit) {
                Fail(kErrStringTooLong);
                return false;
            }
            if (out != nullptr) out[written] = simple;
            ++written;
            continue;
        }

        // \uXXXX, with surrogate pairing.
        ++pos_;
        if (pos_ + 4 > len_) {
            Fail(kErrEscape);
            return false;
        }
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const int v = HexValue(data_[pos_ + i]);
            if (v < 0) {
                Fail(kErrEscape);
                return false;
            }
            cp = (cp << 4) | static_cast<uint32_t>(v);
        }
        pos_ += 4;

        if (cp >= 0xd800 && cp <= 0xdbff) {
            // High surrogate: a low surrogate must follow, or this is not a
            // code point at all. Substituting U+FFFD here would put a glyph on
            // the panel that nobody wrote.
            if (pos_ + 6 > len_ || data_[pos_] != '\\' || data_[pos_ + 1] != 'u') {
                Fail(kErrEscape);
                return false;
            }
            uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
                const int v = HexValue(data_[pos_ + 2 + i]);
                if (v < 0) {
                    Fail(kErrEscape);
                    return false;
                }
                low = (low << 4) | static_cast<uint32_t>(v);
            }
            if (low < 0xdc00 || low > 0xdfff) {
                Fail(kErrEscape);
                return false;
            }
            pos_ += 6;
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            // A lone low surrogate.
            Fail(kErrEscape);
            return false;
        }

        char encoded[4];
        const size_t n = AppendUtf8(cp, encoded, sizeof(encoded));
        if (n == 0) {
            Fail(kErrEscape);
            return false;
        }
        if (written + n > limit) {
            Fail(kErrStringTooLong);
            return false;
        }
        if (out != nullptr) memcpy(out + written, encoded, n);
        written += n;
    }

    if (out != nullptr) out[written] = '\0';
    if (len_out != nullptr) *len_out = written;
    return true;
}

bool Reader::ReadString(char* out, size_t cap, size_t* len_out) {
    if (out == nullptr || cap == 0) {
        Fail(kErrStringTooLong);
        return false;
    }
    return ReadStringInto(out, cap, len_out);
}

bool Reader::ReadBool(bool* out) {
    if (!ok()) return false;
    SkipWhitespace();
    if (pos_ + 4 <= len_ && memcmp(data_ + pos_, "true", 4) == 0) {
        pos_ += 4;
        *out = true;
        return true;
    }
    if (pos_ + 5 <= len_ && memcmp(data_ + pos_, "false", 5) == 0) {
        pos_ += 5;
        *out = false;
        return true;
    }
    Fail(kErrType);
    return false;
}

bool Reader::ReadNull() {
    if (!ok()) return false;
    SkipWhitespace();
    if (pos_ + 4 <= len_ && memcmp(data_ + pos_, "null", 4) == 0) {
        pos_ += 4;
        return true;
    }
    Fail(kErrType);
    return false;
}

bool Reader::ScanNumber(const char** begin, size_t* count, bool* is_integer) {
    SkipWhitespace();
    const size_t start = pos_;
    *is_integer = true;

    if (!AtEnd() && Cur() == '-') ++pos_;
    if (AtEnd() || !IsDigit(Cur())) {
        pos_ = start;
        Fail(kErrType);
        return false;
    }
    if (Cur() == '0') {
        ++pos_;
        // `01` is not a JSON number. Accepting it would also mean accepting
        // `0123` as 123, which is a different value from what was written.
        if (!AtEnd() && IsDigit(Cur())) {
            Fail(kErrSyntax);
            return false;
        }
    } else {
        while (!AtEnd() && IsDigit(Cur())) ++pos_;
    }

    if (!AtEnd() && Cur() == '.') {
        *is_integer = false;
        ++pos_;
        if (AtEnd() || !IsDigit(Cur())) {
            Fail(kErrSyntax);
            return false;
        }
        while (!AtEnd() && IsDigit(Cur())) ++pos_;
    }

    if (!AtEnd() && (Cur() == 'e' || Cur() == 'E')) {
        *is_integer = false;
        ++pos_;
        if (!AtEnd() && (Cur() == '+' || Cur() == '-')) ++pos_;
        if (AtEnd() || !IsDigit(Cur())) {
            Fail(kErrSyntax);
            return false;
        }
        while (!AtEnd() && IsDigit(Cur())) ++pos_;
    }

    *begin = data_ + start;
    *count = pos_ - start;
    return true;
}

bool Reader::ReadInt(int64_t min, int64_t max, int64_t* out) {
    if (!ok()) return false;
    const char* begin = nullptr;
    size_t count = 0;
    bool is_integer = false;
    const size_t start = pos_;
    if (!ScanNumber(&begin, &count, &is_integer)) return false;
    if (!is_integer) {
        // `60.0` is refused rather than accepted as 60. The tower emits whole
        // numbers for every integer field, so a fractional one means the
        // document was produced by something else, and guessing what it meant
        // is how a wake interval quietly becomes a different wake interval.
        pos_ = start;
        Fail(kErrType);
        return false;
    }

    bool negative = false;
    size_t i = 0;
    if (count > 0 && begin[0] == '-') {
        negative = true;
        i = 1;
    }

    // Accumulate in unsigned so the overflow check is well defined; UBSan would
    // rightly object to letting a signed accumulator wrap first and checking
    // afterwards.
    uint64_t magnitude = 0;
    const uint64_t kLimit = negative ? 9223372036854775808ull : 9223372036854775807ull;
    for (; i < count; ++i) {
        const uint64_t digit = static_cast<uint64_t>(begin[i] - '0');
        if (magnitude > (kLimit - digit) / 10u) {
            pos_ = start;
            Fail(kErrNumberRange);
            return false;
        }
        magnitude = magnitude * 10u + digit;
    }

    const int64_t value =
        negative ? static_cast<int64_t>(~magnitude + 1u) : static_cast<int64_t>(magnitude);
    if (value < min || value > max) {
        pos_ = start;
        Fail(kErrNumberRange);
        return false;
    }
    *out = value;
    return true;
}

bool Reader::ReadDouble(double min, double max, double* out) {
    if (!ok()) return false;
    const char* begin = nullptr;
    size_t count = 0;
    bool is_integer = false;
    const size_t start = pos_;
    if (!ScanNumber(&begin, &count, &is_integer)) return false;

    // strtod needs a terminated string and the document is not ours to modify,
    // so the token is copied into a bounded local. A number literal longer than
    // this is not a coordinate; it is someone testing the parser.
    char buf[64];
    if (count >= sizeof(buf)) {
        pos_ = start;
        Fail(kErrNumberRange);
        return false;
    }
    memcpy(buf, begin, count);
    buf[count] = '\0';

    char* end = nullptr;
    const double value = strtod(buf, &end);
    if (end != buf + count) {
        pos_ = start;
        Fail(kErrSyntax);
        return false;
    }
    if (!(value >= min && value <= max)) {
        // Written as a positive test so a NaN, which compares false against
        // everything, is refused here rather than sailing through a `>` check.
        pos_ = start;
        Fail(kErrNumberRange);
        return false;
    }
    *out = value;
    return true;
}

bool Reader::SkipValue() {
    if (!ok()) return false;
    Type type;
    if (!PeekType(&type)) return false;
    switch (type) {
        case Type::kNull:
            return ReadNull();
        case Type::kBool: {
            bool ignored = false;
            return ReadBool(&ignored);
        }
        case Type::kNumber: {
            const char* begin = nullptr;
            size_t count = 0;
            bool is_integer = false;
            return ScanNumber(&begin, &count, &is_integer);
        }
        case Type::kString:
            // nullptr output with a capacity large enough that a long string is
            // walked rather than refused: skipping must not fail on length.
            return ReadStringInto(nullptr, static_cast<size_t>(-1), nullptr);
        case Type::kObject: {
            if (!EnterObject()) return false;
            for (;;) {
                bool has_member = false;
                if (!NextMember('}', &has_member)) return false;
                if (!has_member) break;
                // Keys are walked with no output buffer rather than read into
                // one: skipping is not allowed to fail because a key happened
                // to be longer than some local array.
                if (!ReadStringInto(nullptr, static_cast<size_t>(-1), nullptr)) return false;
                if (!Expect(':')) return false;
                if (!SkipValue()) return false;
            }
            return ok();
        }
        case Type::kArray: {
            if (!EnterArray()) return false;
            while (NextElement()) {
                if (!SkipValue()) return false;
            }
            return ok();
        }
    }
    Fail(kErrSyntax);
    return false;
}

bool Reader::Finish() {
    if (!ok()) return false;
    SkipWhitespace();
    if (!AtEnd()) {
        Fail(kErrTrailing);
        return false;
    }
    return true;
}

}  // namespace json
