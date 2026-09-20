/**
 * @file test_json_scan.cc
 * @brief Host tests for the strict JSON reader.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These compile main/common/json_scan.cc directly, so what is tested is the
 * translation unit the firmware links.
 *
 * The claims worth testing hardest here are the refusals, because a parser's
 * bugs are almost never in the documents it accepts. Every one of these was
 * chosen because accepting it would let two sides of a wire disagree about what
 * a document says while both believing they had parsed it:
 *
 *  - a duplicate key (caught by the caller's `seen` mask, exercised below),
 *  - a trailing comma, which JavaScript takes and JSON does not,
 *  - `60.0` where an integer was promised,
 *  - a literal newline inside a string,
 *  - a lone surrogate, which has no code point to stand for,
 *  - bytes after the root value, which mean the sender stopped somewhere the
 *    receiver did not.
 *
 * Run under ASan and UBSan by tests/host/run.sh: several of these drive the
 * reader off the end of a truncated buffer on purpose, which is exactly where
 * an off-by-one would otherwise hide.
 */

#include "common/json_scan.h"

#include <stdio.h>
#include <string.h>

#include <string>

using namespace json;

// ----------------------------------------------------------- tiny harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,         \
                   g_current, #cond);                                      \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const long long va = (long long)(a);                               \
        const long long vb = (long long)(b);                               \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n",        \
                   __FILE__, __LINE__, g_current, #a, #b, va, vb);         \
        }                                                                  \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        ++g_checks;                                                        \
        if (strcmp((a), (b)) != 0) {                                       \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: \"%s\" == \"%s\"\n", __FILE__,     \
                   __LINE__, g_current, (a), (b));                         \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

/// Walk a whole document as a schema-less object and report the first refusal.
/// Returns kErrNone when the document is well formed all the way through.
const char* ScanWholeDocument(const std::string& text) {
    Reader reader(text.data(), text.size());
    reader.SkipValue();
    reader.Finish();
    return reader.error();
}

}  // namespace

// ------------------------------------------------------------- well formed --

static void test_a_flat_object_walks_key_by_key() {
    const std::string doc = R"({"a":1,"b":"two","c":true,"d":null})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());

    char key[32];
    size_t key_len = 0;

    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "a");
    CHECK_EQ_INT(key_len, 1);
    int64_t n = 0;
    CHECK(reader.ReadInt(0, 100, &n));
    CHECK_EQ_INT(n, 1);

    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "b");
    char value[32];
    size_t value_len = 0;
    CHECK(reader.ReadString(value, sizeof(value), &value_len));
    CHECK_STR(value, "two");
    CHECK_EQ_INT(value_len, 3);

    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "c");
    bool flag = false;
    CHECK(reader.ReadBool(&flag));
    CHECK(flag);

    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "d");
    CHECK(reader.ReadNull());

    // The close brace is reported as "no more members", not as an error.
    CHECK(!reader.NextKey(key, sizeof(key), &key_len));
    CHECK(reader.ok());
    CHECK(reader.Finish());
    CHECK_STR(reader.error(), kErrNone);
}

static void test_an_empty_object_and_array_close_immediately() {
    {
        const std::string doc = "{}";
        Reader reader(doc.data(), doc.size());
        CHECK(reader.EnterObject());
        char key[8];
        size_t key_len = 0;
        CHECK(!reader.NextKey(key, sizeof(key), &key_len));
        CHECK(reader.ok());
        CHECK(reader.Finish());
    }
    {
        const std::string doc = "[]";
        Reader reader(doc.data(), doc.size());
        CHECK(reader.EnterArray());
        CHECK(!reader.NextElement());
        CHECK(reader.ok());
        CHECK(reader.Finish());
    }
}

static void test_nested_containers_walk_in_order() {
    const std::string doc = R"({"rows":[{"n":1},{"n":2},{"n":3}]})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "rows");
    CHECK(reader.EnterArray());

    int64_t seen[3] = {0, 0, 0};
    int count = 0;
    while (reader.NextElement()) {
        CHECK(reader.EnterObject());
        CHECK(reader.NextKey(key, sizeof(key), &key_len));
        CHECK_STR(key, "n");
        int64_t n = 0;
        CHECK(reader.ReadInt(0, 10, &n));
        if (count < 3) seen[count] = n;
        ++count;
        CHECK(!reader.NextKey(key, sizeof(key), &key_len));
    }
    CHECK(reader.ok());
    CHECK_EQ_INT(count, 3);
    CHECK_EQ_INT(seen[0], 1);
    CHECK_EQ_INT(seen[1], 2);
    CHECK_EQ_INT(seen[2], 3);
    CHECK(!reader.NextKey(key, sizeof(key), &key_len));
    CHECK(reader.Finish());
}

static void test_whitespace_between_every_token_is_accepted() {
    const std::string doc = "  {\n \"a\" :\t[ 1 , 2 ]\r\n }  ";
    CHECK_STR(ScanWholeDocument(doc), kErrNone);
}

// ------------------------------------------------------------------ numbers --

static void test_integers_round_trip_including_the_extremes() {
    struct Case {
        const char* text;
        int64_t expected;
    };
    const Case cases[] = {
        {"0", 0},
        {"-0", 0},
        {"7", 7},
        {"-7", -7},
        {"9223372036854775807", 9223372036854775807ll},
        {"-9223372036854775808", (-9223372036854775807ll - 1)},
    };
    for (const Case& c : cases) {
        const std::string doc(c.text);
        Reader reader(doc.data(), doc.size());
        int64_t out = 12345;
        CHECK(reader.ReadInt(-9223372036854775807ll - 1, 9223372036854775807ll, &out));
        CHECK_EQ_INT(out, c.expected);
        CHECK(reader.Finish());
    }
}

static void test_an_integer_outside_the_asked_range_is_refused() {
    const std::string doc = "1441";
    Reader reader(doc.data(), doc.size());
    int64_t out = 0;
    CHECK(!reader.ReadInt(15, 1440, &out));
    CHECK_STR(reader.error(), kErrNumberRange);
}

static void test_an_integer_that_overflows_is_refused_not_wrapped() {
    // One past INT64_MAX. Wrapping would turn a nonsense epoch into a
    // plausible-looking negative one.
    const std::string doc = "9223372036854775808";
    Reader reader(doc.data(), doc.size());
    int64_t out = 0;
    CHECK(!reader.ReadInt(-9223372036854775807ll - 1, 9223372036854775807ll, &out));
    CHECK_STR(reader.error(), kErrNumberRange);
}

static void test_a_fractional_number_is_not_an_integer() {
    const std::string doc = "60.0";
    Reader reader(doc.data(), doc.size());
    int64_t out = 0;
    CHECK(!reader.ReadInt(0, 1000, &out));
    CHECK_STR(reader.error(), kErrType);
}

static void test_an_exponent_is_not_an_integer_either() {
    const std::string doc = "6e1";
    Reader reader(doc.data(), doc.size());
    int64_t out = 0;
    CHECK(!reader.ReadInt(0, 1000, &out));
    CHECK_STR(reader.error(), kErrType);
}

static void test_leading_zeros_and_plus_signs_are_refused() {
    CHECK_STR(ScanWholeDocument("01"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("+1"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("-"), kErrType);
    CHECK_STR(ScanWholeDocument(".5"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("1."), kErrSyntax);
}

static void test_the_javascript_number_words_are_not_json() {
    CHECK_STR(ScanWholeDocument("NaN"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("Infinity"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("-Infinity"), kErrType);
}

static void test_doubles_carry_coordinates() {
    const std::string doc = "-73.56";
    Reader reader(doc.data(), doc.size());
    double out = 0;
    CHECK(reader.ReadDouble(-180.0, 180.0, &out));
    CHECK(out < -73.55 && out > -73.57);
    CHECK(reader.Finish());
}

static void test_a_double_outside_its_range_is_refused() {
    const std::string doc = "181.0";
    Reader reader(doc.data(), doc.size());
    double out = 0;
    CHECK(!reader.ReadDouble(-180.0, 180.0, &out));
    CHECK_STR(reader.error(), kErrNumberRange);
}

// ------------------------------------------------------------------ strings --

static void test_escapes_are_unescaped() {
    const std::string doc = R"("a\"b\\c\/d\be\ff\ng\rh\ti")";
    Reader reader(doc.data(), doc.size());
    char out[64];
    size_t len = 0;
    CHECK(reader.ReadString(out, sizeof(out), &len));
    CHECK_STR(out, "a\"b\\c/d\be\ff\ng\rh\ti");
    CHECK_EQ_INT(len, strlen("a\"b\\c/d\be\ff\ng\rh\ti"));
}

static void test_unicode_escapes_become_utf8() {
    // "Montréal" — the awkward spelling a JSON encoder may choose for a
    // label the tower wrote as plain UTF-8. Both spellings have to reach the
    // panel as the same nine bytes, or a read-back comparison would fail on a
    // document that says exactly what was sent.
    const std::string doc = "\"Montr\\u00e9al\"";
    Reader reader(doc.data(), doc.size());
    char out[32];
    size_t len = 0;
    CHECK(reader.ReadString(out, sizeof(out), &len));
    CHECK_STR(out, "Montr\xc3\xa9""al");
    CHECK_EQ_INT(len, 9);
}

static void test_literal_utf8_passes_through_unchanged() {
    const std::string doc = "\"Montr\xc3\xa9""al\"";
    Reader reader(doc.data(), doc.size());
    char out[32];
    size_t len = 0;
    CHECK(reader.ReadString(out, sizeof(out), &len));
    CHECK_STR(out, "Montr\xc3\xa9""al");
    CHECK_EQ_INT(len, 9);
}

static void test_a_surrogate_pair_becomes_one_code_point() {
    const std::string doc = "\"\\ud83d\\ude00\"";  // U+1F600
    Reader reader(doc.data(), doc.size());
    char out[16];
    size_t len = 0;
    CHECK(reader.ReadString(out, sizeof(out), &len));
    CHECK_EQ_INT(len, 4);
    CHECK_EQ_INT((unsigned char)out[0], 0xf0);
    CHECK_EQ_INT((unsigned char)out[1], 0x9f);
    CHECK_EQ_INT((unsigned char)out[2], 0x98);
    CHECK_EQ_INT((unsigned char)out[3], 0x80);
}

static void test_a_lone_surrogate_is_refused_not_substituted() {
    {
        const std::string doc = R"("\ud83d")";
        Reader reader(doc.data(), doc.size());
        char out[16];
        CHECK(!reader.ReadString(out, sizeof(out), nullptr));
        CHECK_STR(reader.error(), kErrEscape);
    }
    {
        const std::string doc = R"("\ude00")";
        Reader reader(doc.data(), doc.size());
        char out[16];
        CHECK(!reader.ReadString(out, sizeof(out), nullptr));
        CHECK_STR(reader.error(), kErrEscape);
    }
}

static void test_an_unknown_escape_is_refused() {
    const std::string doc = R"("a\qb")";
    Reader reader(doc.data(), doc.size());
    char out[16];
    CHECK(!reader.ReadString(out, sizeof(out), nullptr));
    CHECK_STR(reader.error(), kErrEscape);
}

static void test_a_literal_control_character_is_refused() {
    // A real newline inside the quotes, which is what a hand-edited file gets.
    const std::string doc = "\"line\nbreak\"";
    Reader reader(doc.data(), doc.size());
    char out[32];
    CHECK(!reader.ReadString(out, sizeof(out), nullptr));
    CHECK_STR(reader.error(), kErrControlChar);
}

static void test_a_string_longer_than_the_buffer_is_refused_not_truncated() {
    const std::string doc = "\"abcdefghij\"";
    Reader reader(doc.data(), doc.size());
    char out[8];  // 7 usable bytes plus a terminator
    size_t len = 999;
    CHECK(!reader.ReadString(out, sizeof(out), &len));
    CHECK_STR(reader.error(), kErrStringTooLong);
}

static void test_a_string_that_exactly_fills_the_buffer_is_accepted() {
    const std::string doc = "\"abcdefg\"";
    Reader reader(doc.data(), doc.size());
    char out[8];
    size_t len = 0;
    CHECK(reader.ReadString(out, sizeof(out), &len));
    CHECK_STR(out, "abcdefg");
    CHECK_EQ_INT(len, 7);
}

static void test_an_unterminated_string_is_refused() {
    const std::string doc = "\"abc";
    Reader reader(doc.data(), doc.size());
    char out[16];
    CHECK(!reader.ReadString(out, sizeof(out), nullptr));
    CHECK_STR(reader.error(), kErrSyntax);
}

static void test_an_escaped_key_still_matches_its_plain_spelling() {
    // "a" is a legal spelling of "a". A table lookup against the raw bytes
    // would miss it; unescaping the key first is why this passes.
    const std::string doc = R"({"a":1})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "a");
}

// ------------------------------------------------------------- the refusals --

static void test_a_trailing_comma_is_refused_in_objects_and_arrays() {
    CHECK_STR(ScanWholeDocument(R"({"a":1,})"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("[1,2,]"), kErrSyntax);
}

static void test_a_missing_comma_is_refused() {
    CHECK_STR(ScanWholeDocument(R"({"a":1 "b":2})"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("[1 2]"), kErrSyntax);
}

static void test_an_unquoted_key_is_refused() {
    CHECK_STR(ScanWholeDocument("{a:1}"), kErrType);
}

static void test_single_quotes_are_refused() {
    CHECK_STR(ScanWholeDocument("{'a':1}"), kErrType);
}

static void test_comments_are_refused() {
    CHECK_STR(ScanWholeDocument("{} // hello"), kErrTrailing);
    CHECK_STR(ScanWholeDocument("/* hi */ {}"), kErrSyntax);
}

static void test_bytes_after_the_root_value_are_refused() {
    CHECK_STR(ScanWholeDocument("{} {}"), kErrTrailing);
    CHECK_STR(ScanWholeDocument("1 2"), kErrTrailing);
    // Whitespace after the root is fine; that is the point of the distinction.
    CHECK_STR(ScanWholeDocument("{}   \n"), kErrNone);
}

static void test_an_unclosed_container_is_refused() {
    CHECK_STR(ScanWholeDocument(R"({"a":1)"), kErrSyntax);
    CHECK_STR(ScanWholeDocument("[1,2"), kErrSyntax);
    CHECK_STR(ScanWholeDocument(""), kErrSyntax);
}

static void test_nesting_past_the_depth_limit_is_refused_not_crashed() {
    // Deep enough to blow a recursive descent parser's stack if it had no
    // bound. The refusal is the whole point: ASan would report the alternative.
    std::string doc;
    for (int i = 0; i < 4096; ++i) doc += '[';
    for (int i = 0; i < 4096; ++i) doc += ']';
    CHECK_STR(ScanWholeDocument(doc), kErrDepth);
}

static void test_nesting_exactly_at_the_limit_is_accepted() {
    std::string doc;
    for (int i = 0; i < kMaxDepth; ++i) doc += '[';
    for (int i = 0; i < kMaxDepth; ++i) doc += ']';
    CHECK_STR(ScanWholeDocument(doc), kErrNone);
}

static void test_the_first_refusal_is_the_one_reported() {
    // Two things wrong: a bad escape, then a trailing comma. The reader must
    // keep the first, because that is the one that explains the rest.
    const std::string doc = R"({"a":"x\qy",})";
    Reader reader(doc.data(), doc.size());
    reader.SkipValue();
    reader.Finish();
    CHECK_STR(reader.error(), kErrEscape);
}

static void test_a_failed_reader_stays_failed_and_reads_nothing() {
    const std::string doc = R"({"a":})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    int64_t out = 42;
    CHECK(!reader.ReadInt(0, 100, &out));
    const char* first = reader.error();
    // Every subsequent call is a no-op that returns false and does not move the
    // latched reason, which is what lets a caller walk a schema and check once.
    bool flag = true;
    CHECK(!reader.ReadBool(&flag));
    CHECK(flag);
    CHECK(!reader.NextKey(key, sizeof(key), &key_len));
    CHECK(!reader.Finish());
    CHECK_STR(reader.error(), first);
    CHECK_EQ_INT(out, 42);
}

static void test_a_wrong_type_is_reported_as_a_type_error() {
    const std::string doc = R"("not a number")";
    Reader reader(doc.data(), doc.size());
    int64_t out = 0;
    CHECK(!reader.ReadInt(0, 10, &out));
    CHECK_STR(reader.error(), kErrType);
}

static void test_the_error_offset_points_at_the_problem() {
    const std::string doc = R"({"a":1,"b":tru})";
    Reader reader(doc.data(), doc.size());
    reader.SkipValue();
    CHECK(!reader.ok());
    // The refusal is at the `tru`, not at byte zero.
    CHECK(reader.error_offset() >= 11);
}

// ---------------------------------------------------------- caller patterns --

static void test_a_caller_detects_a_duplicate_key_with_a_seen_mask() {
    // The reader does not carry a key set; the schema layer does, with one bit
    // per known field. This is that pattern, and the reason it is tested here
    // is that every schema in the firmware depends on it working.
    const std::string doc = R"({"a":1,"b":2,"a":3})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    uint32_t seen = 0;
    bool duplicate = false;
    while (reader.NextKey(key, sizeof(key), &key_len)) {
        const uint32_t bit = (strcmp(key, "a") == 0) ? 1u : 2u;
        if (seen & bit) duplicate = true;
        seen |= bit;
        CHECK(reader.SkipValue());
    }
    CHECK(reader.ok());
    CHECK(duplicate);
}

static void test_skip_value_steps_over_whole_subtrees() {
    const std::string doc =
        R"({"skip":{"deep":[1,{"x":"y"},null]},"keep":5})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "skip");
    CHECK(reader.SkipValue());
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK_STR(key, "keep");
    int64_t n = 0;
    CHECK(reader.ReadInt(0, 10, &n));
    CHECK_EQ_INT(n, 5);
    CHECK(!reader.NextKey(key, sizeof(key), &key_len));
    CHECK(reader.Finish());
}

static void test_skip_value_walks_keys_longer_than_any_local_buffer() {
    // Skipping must not fail merely because a key was long; that would make an
    // unknown-field refusal depend on the length of the unknown field's name.
    std::string doc = R"({"skip":{")";
    doc.append(600, 'k');
    doc += R"(":1},"keep":5})";
    Reader reader(doc.data(), doc.size());
    CHECK(reader.EnterObject());
    char key[16];
    size_t key_len = 0;
    CHECK(reader.NextKey(key, sizeof(key), &key_len));
    CHECK(reader.SkipValue());
    CHECK(reader.ok());
}

static void test_a_truncated_document_never_reads_past_its_end() {
    // Every prefix of a valid document must be refused rather than read off the
    // end. ASan turns any mistake here into a failure rather than a maybe.
    const std::string full =
        R"({"profile_version":1,"rows":["a","b"],"w":{"lat":-73.56},"ok":true})";
    for (size_t cut = 0; cut < full.size(); ++cut) {
        const std::string prefix = full.substr(0, cut);
        Reader reader(prefix.data(), prefix.size());
        reader.SkipValue();
        reader.Finish();
        CHECK(!reader.ok());
    }
    Reader whole(full.data(), full.size());
    whole.SkipValue();
    CHECK(whole.Finish());
}

static void test_a_null_buffer_is_an_empty_document() {
    Reader reader(nullptr, 100);
    CHECK(!reader.SkipValue());
    CHECK_STR(reader.error(), kErrSyntax);
}

int main() {
    RUN(test_a_flat_object_walks_key_by_key);
    RUN(test_an_empty_object_and_array_close_immediately);
    RUN(test_nested_containers_walk_in_order);
    RUN(test_whitespace_between_every_token_is_accepted);

    RUN(test_integers_round_trip_including_the_extremes);
    RUN(test_an_integer_outside_the_asked_range_is_refused);
    RUN(test_an_integer_that_overflows_is_refused_not_wrapped);
    RUN(test_a_fractional_number_is_not_an_integer);
    RUN(test_an_exponent_is_not_an_integer_either);
    RUN(test_leading_zeros_and_plus_signs_are_refused);
    RUN(test_the_javascript_number_words_are_not_json);
    RUN(test_doubles_carry_coordinates);
    RUN(test_a_double_outside_its_range_is_refused);

    RUN(test_escapes_are_unescaped);
    RUN(test_unicode_escapes_become_utf8);
    RUN(test_literal_utf8_passes_through_unchanged);
    RUN(test_a_surrogate_pair_becomes_one_code_point);
    RUN(test_a_lone_surrogate_is_refused_not_substituted);
    RUN(test_an_unknown_escape_is_refused);
    RUN(test_a_literal_control_character_is_refused);
    RUN(test_a_string_longer_than_the_buffer_is_refused_not_truncated);
    RUN(test_a_string_that_exactly_fills_the_buffer_is_accepted);
    RUN(test_an_unterminated_string_is_refused);
    RUN(test_an_escaped_key_still_matches_its_plain_spelling);

    RUN(test_a_trailing_comma_is_refused_in_objects_and_arrays);
    RUN(test_a_missing_comma_is_refused);
    RUN(test_an_unquoted_key_is_refused);
    RUN(test_single_quotes_are_refused);
    RUN(test_comments_are_refused);
    RUN(test_bytes_after_the_root_value_are_refused);
    RUN(test_an_unclosed_container_is_refused);
    RUN(test_nesting_past_the_depth_limit_is_refused_not_crashed);
    RUN(test_nesting_exactly_at_the_limit_is_accepted);
    RUN(test_the_first_refusal_is_the_one_reported);
    RUN(test_a_failed_reader_stays_failed_and_reads_nothing);
    RUN(test_a_wrong_type_is_reported_as_a_type_error);
    RUN(test_the_error_offset_points_at_the_problem);

    RUN(test_a_caller_detects_a_duplicate_key_with_a_seen_mask);
    RUN(test_skip_value_steps_over_whole_subtrees);
    RUN(test_skip_value_walks_keys_longer_than_any_local_buffer);
    RUN(test_a_truncated_document_never_reads_past_its_end);
    RUN(test_a_null_buffer_is_an_empty_document);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
