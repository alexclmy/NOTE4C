/**
 * @file json_scan.h
 * @brief A strict, allocation-free JSON reader for the two documents the
 *        device has to understand on its own.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why this exists when cJSON is already linked
 * --------------------------------------------
 * cJSON is in the build and parses the config API's request bodies perfectly
 * well. It is the wrong tool for these two documents, for four reasons that are
 * each specific rather than stylistic:
 *
 *  1. **These parse outside a route.** The autonomy profile is re-read from
 *     SPIFFS at the start of every wake cycle, with no HTTP request in sight.
 *     That path runs before anything else and must not depend on a request
 *     buffer's lifetime or a route's error plumbing.
 *  2. **cJSON allocates a DOM.** A 16 KB profile becomes several tens of KB of
 *     nodes on a heap the wake cycle is trying not to touch. This reader holds
 *     one cursor and copies only the scalars the caller asks for, so parsing a
 *     profile costs the caller's own output struct and nothing else.
 *  3. **cJSON accepts duplicate keys**, silently keeping one of them. For a
 *     document the tower pushes, reads back and compares byte for byte, a
 *     duplicate key is a disagreement about what was even said, and the honest
 *     answer is a refusal naming the key. The same applies to a forecast:
 *     eMini's fetcher rejects duplicate keys for exactly this reason and it was
 *     right to.
 *  4. **It is not host-compilable.** cJSON arrives through an ESP-IDF
 *     component, so a validator built on it can only be tested by flashing a
 *     board. Everything in this directory is deliberately free of ESP-IDF
 *     headers so the host suite exercises the translation unit that ships.
 *
 * What "strict" means here, precisely
 * -----------------------------------
 * RFC 8259 and nothing beside it. No comments, no trailing commas, no single
 * quotes, no unquoted keys, no `NaN`, no `Infinity`, no leading `+`, no leading
 * zeros, no hex. Literal control characters inside strings are refused rather
 * than passed through, because the panel cannot draw them and a string carrying
 * one did not come from a text box. Depth is bounded, so a document made of ten
 * thousand open brackets costs a refusal rather than the stack.
 *
 * The reader never repairs anything. Every refusal carries a stable token and a
 * byte offset, which is what lets the profile route answer "unknown field
 * `wether` at byte 214" instead of "400".
 *
 * How it is meant to be used
 * --------------------------
 * Schema-directed, not DOM-style. The caller knows the shape it expects, walks
 * it with EnterObject/NextKey, and dispatches each key against a closed table;
 * a key not in the table is an error naming the key, which is the same
 * discipline device_config.h's closed field table already uses. Duplicate
 * detection is the caller's `seen` bitmask over that same table, so it costs
 * one bit per field and no allocation.
 */

#ifndef COMMON_JSON_SCAN_H
#define COMMON_JSON_SCAN_H

#include <stddef.h>
#include <stdint.h>

namespace json {

/// The six JSON value types, as the reader reports them from a peek.
enum class Type {
    kNull = 0,
    kBool,
    kNumber,
    kString,
    kObject,
    kArray,
};

/**
 * @brief Maximum nesting the reader will follow.
 *
 * The profile nests four deep at its worst (root → modules → module → when →
 * condition) and a forecast three. Sixteen leaves generous headroom while
 * keeping the refusal well clear of any real stack limit, which matters because
 * the alternative to a bound here is a crash rather than a 400.
 */
constexpr int kMaxDepth = 16;

/**
 * @brief Stable refusal tokens.
 *
 * Tokens rather than sentences because these cross the wire into the tower's
 * `CODE_COPY` table, which is where the human wording lives. A token that
 * changed spelling would silently become an unrecognised code, so these are
 * treated as part of the contract.
 */
extern const char* const kErrNone;          ///< no error
extern const char* const kErrSyntax;        ///< not JSON at all at this point
extern const char* const kErrDepth;         ///< nesting past kMaxDepth
extern const char* const kErrType;          ///< a value of the wrong type
extern const char* const kErrNumberRange;   ///< integer outside the asked range
extern const char* const kErrStringTooLong; ///< string longer than the caller's buffer
extern const char* const kErrControlChar;   ///< a literal control character in a string
extern const char* const kErrEscape;        ///< a malformed \u or unknown escape
extern const char* const kErrTrailing;      ///< non-whitespace after the root value

/**
 * @brief A cursor over one JSON document.
 *
 * Not copyable in spirit: one reader walks one document once, forwards. It
 * borrows the buffer and never writes to it, so the caller may keep the raw
 * bytes for the byte-exact re-serialisation the profile store needs.
 *
 * Every method returns false on failure and latches the first error. Once the
 * reader has failed it stays failed and every subsequent call returns false
 * without touching the cursor, so a caller may walk a whole schema and check
 * ok() once at the end rather than testing after every field. The latch is what
 * makes that safe: a later call cannot overwrite the first, most specific
 * reason with a vaguer one.
 */
class Reader {
public:
    Reader(const char* data, size_t len);

    bool ok() const { return error_ == kErrNone; }
    /// The first refusal, or kErrNone. Never overwritten once set.
    const char* error() const { return error_; }
    /// Byte offset the first refusal was noticed at.
    size_t error_offset() const { return error_offset_; }

    /// Byte offset of the cursor. Used by callers that keep raw sub-spans.
    size_t offset() const { return pos_; }

    /**
     * @brief What the next value is, without consuming it.
     * @return false at end of input, on a syntax error, or once failed.
     */
    bool PeekType(Type* out);

    /// Consume and discard the next value, whatever it is, including containers.
    bool SkipValue();

    /**
     * @brief Step into an object.
     *
     * Must be followed by NextKey() calls until it returns false with ok()
     * still true, which is the one signal that the object closed cleanly.
     */
    bool EnterObject();

    /**
     * @brief Advance to the next member of the object being walked.
     *
     * @param key     set to the *unescaped* key bytes, in the caller's buffer.
     * @param key_cap capacity of @p key.
     * @param key_len set to the key length.
     * @return true when a member was read and its value is now the pending
     *         value; false at the closing brace or on error. Callers
     *         distinguish the two with ok().
     *
     * The key is unescaped into the caller's buffer rather than returned as a
     * pointer into the document, because `"wake_interval_min"` is a legal
     * spelling of a field name and a table lookup against the raw bytes would
     * miss it. Refusing escaped keys instead would be defensible, but silently
     * failing to match one is not.
     */
    bool NextKey(char* key, size_t key_cap, size_t* key_len);

    /// Step into an array. Pair with NextElement().
    bool EnterArray();

    /**
     * @brief Advance to the next array element.
     * @return true when an element is now the pending value; false at the
     *         closing bracket or on error, distinguished with ok().
     */
    bool NextElement();

    bool ReadBool(bool* out);
    bool ReadNull();

    /**
     * @brief Read an integer, refusing anything that is not exactly one.
     *
     * A fractional part or an exponent is a refusal, not a rounding: a wake
     * interval of `59.9` is a document that disagrees with itself about what it
     * means, and the device is the wrong place to decide which way it meant.
     * @p min and @p max are inclusive and checked before the value is stored.
     */
    bool ReadInt(int64_t min, int64_t max, int64_t* out);

    /**
     * @brief Read a number that may have a fractional part.
     *
     * Used for coordinates only. Exponent form is accepted because it is legal
     * JSON, but the value is range-checked like any other.
     */
    bool ReadDouble(double min, double max, double* out);

    /**
     * @brief Read a string, unescaped, NUL-terminated, into @p out.
     *
     * @param cap     buffer size including the terminator.
     * @param len_out set to the byte length excluding the terminator.
     *
     * Fails with kErrStringTooLong rather than truncating. A truncated message
     * on a panel is a different message, and the device is not entitled to
     * decide that a shorter one will do.
     */
    bool ReadString(char* out, size_t cap, size_t* len_out);

    /**
     * @brief Assert the document ends here, with only whitespace left.
     *
     * Called once after the root value. Trailing bytes mean the sender and the
     * receiver disagree about where the document stopped, which for a store
     * that hashes what it keeps is worth a refusal.
     */
    bool Finish();

    /// Latch an error from the caller's own schema layer, sharing the offset
    /// machinery so a field-level refusal reports where it happened.
    void Fail(const char* token);

private:
    void SkipWhitespace();
    bool AtEnd() const { return pos_ >= len_; }
    char Cur() const { return pos_ < len_ ? data_[pos_] : '\0'; }
    bool Expect(char c);
    bool ReadStringInto(char* out, size_t cap, size_t* len_out);
    bool ScanNumber(const char** begin, size_t* count, bool* is_integer);
    bool Push();
    void Pop();
    /// Consume the comma-or-close bookkeeping shared by objects and arrays.
    bool NextMember(char close, bool* has_member);

    const char* data_ = nullptr;
    size_t len_ = 0;
    size_t pos_ = 0;
    const char* error_ = nullptr;
    size_t error_offset_ = 0;
    int depth_ = 0;
    /// Per-level flag: has this container yielded a member yet? Drives whether
    /// a comma is required or forbidden before the next one.
    bool started_[kMaxDepth] = {};
};

}  // namespace json

#endif  // COMMON_JSON_SCAN_H
