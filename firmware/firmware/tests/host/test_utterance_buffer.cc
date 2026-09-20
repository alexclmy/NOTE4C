/**
 * @file test_utterance_buffer.cc
 * @brief Host tests for the real utterance_buffer translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * No codec, no encoder, no device. The frames here are patterns of bytes, and
 * what is asserted is that the container round-trips them without losing a
 * length, and that every cap is enforced at the byte rather than after it.
 */

#include "common/utterance_buffer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace voice;

// ------------------------------------------------------------ mini harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current_test = "";

static void Check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", g_current_test, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define RUN(fn)                                \
    do {                                       \
        g_current_test = #fn;                  \
        const int before = g_failures;         \
        fn();                                  \
        std::printf("%-52s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

// ---------------------------------------------------------------- helpers --

static std::vector<uint8_t> Frame(size_t size, uint8_t seed) {
    std::vector<uint8_t> f(size);
    for (size_t i = 0; i < size; ++i) {
        f[i] = static_cast<uint8_t>(seed + i);
    }
    return f;
}

static UtteranceBuffer MakeBuffer(size_t max_bytes = 192 * 1024,
                                  uint32_t max_frames = 512) {
    UtteranceBuffer b;
    Limits l;
    l.max_bytes = max_bytes;
    l.max_frames = max_frames;
    b.Configure(16000, 60, 1, l);
    return b;
}

// -------------------------------------------------------------- the header --

static void test_a_fresh_buffer_is_a_bare_header() {
    UtteranceBuffer b = MakeBuffer();
    b.Finalize();
    CHECK(b.size() == kHeaderBytes);
    CHECK(b.empty());
    CHECK(!b.truncated());
    CHECK(b.frame_count() == 0);
    CHECK(b.duration_ms() == 0);
}

static void test_the_header_carries_what_a_decoder_needs() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(40, 1);
    CHECK(b.AppendFrame(f.data(), f.size()));
    b.Finalize();

    uint32_t rate = 0;
    uint16_t duration = 0;
    uint8_t channels = 0;
    uint32_t frames = 0;
    CHECK(ParseHeader(b.body(), b.size(), &rate, &duration, &channels, &frames));
    CHECK(rate == 16000);
    CHECK(duration == 60);
    CHECK(channels == 1);
    CHECK(frames == 1);
}

static void test_the_magic_is_checked() {
    UtteranceBuffer b = MakeBuffer();
    b.Finalize();
    std::vector<uint8_t> copy(b.body(), b.body() + b.size());
    copy[0] = 'X';
    CHECK(!ParseHeader(copy.data(), copy.size(), nullptr, nullptr, nullptr, nullptr));
}

static void test_a_future_version_is_refused_rather_than_guessed() {
    UtteranceBuffer b = MakeBuffer();
    b.Finalize();
    std::vector<uint8_t> copy(b.body(), b.body() + b.size());
    copy[4] = kContainerVersion + 1;
    CHECK(!ParseHeader(copy.data(), copy.size(), nullptr, nullptr, nullptr, nullptr));
}

static void test_a_truncated_header_is_not_parsed() {
    UtteranceBuffer b = MakeBuffer();
    b.Finalize();
    for (size_t n = 0; n < kHeaderBytes; ++n) {
        CHECK(!ParseHeader(b.body(), n, nullptr, nullptr, nullptr, nullptr));
    }
}

// --------------------------------------------------------------- the frames --

static void test_frames_round_trip_with_their_lengths() {
    UtteranceBuffer b = MakeBuffer();
    const std::vector<size_t> sizes = {1, 17, 200, 3, 1275};
    size_t expected = kHeaderBytes;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto f = Frame(sizes[i], static_cast<uint8_t>(i + 1));
        CHECK(b.AppendFrame(f.data(), f.size()));
        expected += sizes[i] + 2;
    }
    b.Finalize();
    CHECK(b.size() == expected);
    CHECK(b.frame_count() == sizes.size());
    CHECK(ValidateFrames(b.body(), b.size()) == static_cast<int64_t>(sizes.size()));

    // Walk it by hand: this is what a decoder on the other end would do.
    size_t offset = kHeaderBytes;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const uint16_t len = static_cast<uint16_t>(
            b.body()[offset] | (b.body()[offset + 1] << 8));
        CHECK(len == sizes[i]);
        offset += 2;
        const auto f = Frame(sizes[i], static_cast<uint8_t>(i + 1));
        CHECK(std::memcmp(b.body() + offset, f.data(), sizes[i]) == 0);
        offset += len;
    }
    CHECK(offset == b.size());
}

static void test_duration_follows_the_frames_actually_stored() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(50, 7);
    for (int i = 0; i < 10; ++i) {
        CHECK(b.AppendFrame(f.data(), f.size()));
    }
    CHECK(b.duration_ms() == 600);
}

static void test_an_empty_frame_is_refused_and_is_not_truncation() {
    UtteranceBuffer b = MakeBuffer();
    const uint8_t byte = 0;
    CHECK(!b.AppendFrame(&byte, 0));
    CHECK(!b.AppendFrame(nullptr, 10));
    CHECK(b.frame_count() == 0);
    // Nothing was lost, so the utterance is not truncated. The distinction
    // matters because truncated() is what the user is told about.
    CHECK(!b.truncated());
}

static void test_an_implausible_frame_is_refused_and_is_not_truncation() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(kMaxFrameBytes + 1, 3);
    CHECK(!b.AppendFrame(f.data(), f.size()));
    CHECK(b.frame_count() == 0);
    CHECK(!b.truncated());
    // A frame of exactly the cap is fine.
    const auto ok = Frame(kMaxFrameBytes, 3);
    CHECK(b.AppendFrame(ok.data(), ok.size()));
}

// ---------------------------------------------------------------- the caps --

static void test_the_byte_cap_is_enforced_at_the_byte() {
    // Room for the header plus exactly one 100-byte frame and its prefix.
    UtteranceBuffer b = MakeBuffer(kHeaderBytes + 102);
    const auto f = Frame(100, 1);
    CHECK(b.AppendFrame(f.data(), f.size()));
    CHECK(!b.truncated());
    CHECK(b.remaining_bytes() == 0);
    // One more byte would not fit, prefix included.
    const uint8_t one = 9;
    CHECK(!b.AppendFrame(&one, 1));
    CHECK(b.truncated());
    CHECK(b.frame_count() == 1);
    b.Finalize();
    CHECK(b.size() == kHeaderBytes + 102);
    CHECK(ValidateFrames(b.body(), b.size()) == 1);
}

static void test_the_prefix_counts_against_the_cap() {
    // Exactly enough for a 100-byte payload but not for its two-byte length.
    UtteranceBuffer b = MakeBuffer(kHeaderBytes + 101);
    const auto f = Frame(100, 1);
    CHECK(!b.AppendFrame(f.data(), f.size()));
    CHECK(b.truncated());
    CHECK(b.frame_count() == 0);
}

static void test_the_frame_cap_is_enforced() {
    UtteranceBuffer b = MakeBuffer(192 * 1024, 3);
    const auto f = Frame(10, 1);
    CHECK(b.AppendFrame(f.data(), f.size()));
    CHECK(b.AppendFrame(f.data(), f.size()));
    CHECK(b.AppendFrame(f.data(), f.size()));
    CHECK(!b.truncated());
    CHECK(!b.AppendFrame(f.data(), f.size()));
    CHECK(b.truncated());
    CHECK(b.frame_count() == 3);
}

static void test_a_truncated_buffer_is_still_well_formed() {
    // Half of what somebody said is more useful than nothing, so a truncated
    // utterance is still uploaded. What must not happen is a body that a
    // decoder cannot walk.
    UtteranceBuffer b = MakeBuffer(kHeaderBytes + 300);
    const auto f = Frame(90, 4);
    for (int i = 0; i < 10; ++i) {
        b.AppendFrame(f.data(), f.size());
    }
    CHECK(b.truncated());
    b.Finalize();
    CHECK(ValidateFrames(b.body(), b.size()) ==
          static_cast<int64_t>(b.frame_count()));
}

static void test_a_cap_below_the_header_is_clamped_rather_than_underflowing() {
    // remaining_bytes() is a subtraction. A cap smaller than the header would
    // wrap it and make every frame appear to fit.
    UtteranceBuffer b = MakeBuffer(4);
    CHECK(b.remaining_bytes() == 0);
    const auto f = Frame(10, 1);
    CHECK(!b.AppendFrame(f.data(), f.size()));
    CHECK(b.frame_count() == 0);
}

// ----------------------------------------------------------------- reuse --

static void test_reset_empties_the_buffer_and_clears_truncation() {
    UtteranceBuffer b = MakeBuffer(kHeaderBytes + 20);
    const auto f = Frame(100, 1);
    CHECK(!b.AppendFrame(f.data(), f.size()));
    CHECK(b.truncated());
    b.Reset();
    CHECK(!b.truncated());
    CHECK(b.frame_count() == 0);
    b.Finalize();
    CHECK(b.size() == kHeaderBytes);
}

static void test_reset_does_not_leak_the_previous_utterance() {
    // A cancelled recording is discarded. If Reset() left the old frames in
    // place, the next utterance would carry audio nobody meant to send.
    UtteranceBuffer b = MakeBuffer();
    const auto secret = Frame(200, 0xAB);
    CHECK(b.AppendFrame(secret.data(), secret.size()));
    b.Reset();
    const auto next = Frame(10, 1);
    CHECK(b.AppendFrame(next.data(), next.size()));
    b.Finalize();
    CHECK(b.size() == kHeaderBytes + 12);
    CHECK(b.frame_count() == 1);
    // The old payload is nowhere in the serialised body.
    const std::string body(reinterpret_cast<const char*>(b.body()), b.size());
    const std::string needle(reinterpret_cast<const char*>(secret.data()), 16);
    CHECK(body.find(needle) == std::string::npos);
}

static void test_appending_after_finalize_repatches_the_count() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(10, 1);
    CHECK(b.AppendFrame(f.data(), f.size()));
    b.Finalize();
    CHECK(b.AppendFrame(f.data(), f.size()));
    b.Finalize();
    uint32_t frames = 0;
    CHECK(ParseHeader(b.body(), b.size(), nullptr, nullptr, nullptr, &frames));
    CHECK(frames == 2);
    CHECK(ValidateFrames(b.body(), b.size()) == 2);
}

// --------------------------------------------------------- malformed input --

static void test_validate_rejects_a_dangling_length_prefix() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(10, 1);
    b.AppendFrame(f.data(), f.size());
    b.Finalize();
    std::vector<uint8_t> copy(b.body(), b.body() + b.size());
    copy.push_back(0x05);  // half a length
    CHECK(ValidateFrames(copy.data(), copy.size()) == -1);
}

static void test_validate_rejects_a_length_that_runs_past_the_end() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(10, 1);
    b.AppendFrame(f.data(), f.size());
    b.Finalize();
    std::vector<uint8_t> copy(b.body(), b.body() + b.size());
    copy[kHeaderBytes] = 0xff;
    copy[kHeaderBytes + 1] = 0x00;
    CHECK(ValidateFrames(copy.data(), copy.size()) == -1);
}

static void test_validate_rejects_a_count_that_disagrees_with_the_frames() {
    UtteranceBuffer b = MakeBuffer();
    const auto f = Frame(10, 1);
    b.AppendFrame(f.data(), f.size());
    b.Finalize();
    std::vector<uint8_t> copy(b.body(), b.body() + b.size());
    copy[12] = 7;  // claims seven frames, carries one
    CHECK(ValidateFrames(copy.data(), copy.size()) == -1);
}

static void test_validate_rejects_a_zero_length_frame() {
    std::vector<uint8_t> body(kHeaderBytes + 2, 0);
    std::memcpy(body.data(), "N4CO", 4);
    body[4] = kContainerVersion;
    body[5] = 1;
    body[12] = 1;
    CHECK(ValidateFrames(body.data(), body.size()) == -1);
}

static void test_the_mime_is_not_audio_opus() {
    // The container is not Ogg. Declaring it as audio/opus would send whoever
    // writes the first real transcriber to a decoder that cannot read it.
    CHECK(std::strcmp(kUtteranceMime, "audio/opus") != 0);
    CHECK(std::strstr(kUtteranceMime, "note4c") != nullptr);
}

int main() {
    RUN(test_a_fresh_buffer_is_a_bare_header);
    RUN(test_the_header_carries_what_a_decoder_needs);
    RUN(test_the_magic_is_checked);
    RUN(test_a_future_version_is_refused_rather_than_guessed);
    RUN(test_a_truncated_header_is_not_parsed);

    RUN(test_frames_round_trip_with_their_lengths);
    RUN(test_duration_follows_the_frames_actually_stored);
    RUN(test_an_empty_frame_is_refused_and_is_not_truncation);
    RUN(test_an_implausible_frame_is_refused_and_is_not_truncation);

    RUN(test_the_byte_cap_is_enforced_at_the_byte);
    RUN(test_the_prefix_counts_against_the_cap);
    RUN(test_the_frame_cap_is_enforced);
    RUN(test_a_truncated_buffer_is_still_well_formed);
    RUN(test_a_cap_below_the_header_is_clamped_rather_than_underflowing);

    RUN(test_reset_empties_the_buffer_and_clears_truncation);
    RUN(test_reset_does_not_leak_the_previous_utterance);
    RUN(test_appending_after_finalize_repatches_the_count);

    RUN(test_validate_rejects_a_dangling_length_prefix);
    RUN(test_validate_rejects_a_length_that_runs_past_the_end);
    RUN(test_validate_rejects_a_count_that_disagrees_with_the_frames);
    RUN(test_validate_rejects_a_zero_length_frame);
    RUN(test_the_mime_is_not_audio_opus);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
