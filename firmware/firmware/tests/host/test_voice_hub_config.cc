/**
 * @file test_voice_hub_config.cc
 * @brief Host tests for the real voice_hub_config translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These two strings arrive over the network and end up in an HTTP request line
 * and an `Authorization` header. Most of what is asserted here is about
 * characters that would change the shape of a request rather than its content.
 */

#include "common/voice_hub_config.h"

#include <cstdio>
#include <cstring>
#include <string>

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
        std::printf("%-58s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

static const char* kToken = "0123456789abcdef0123456789abcdef";

// ------------------------------------------------------------------- urls --

static void test_ordinary_urls_are_accepted() {
    CHECK(ValidateHubUrl("http://192.168.0.10:8653") == HubConfigError::kNone);
    CHECK(ValidateHubUrl("http://hub.local:8653") == HubConfigError::kNone);
    CHECK(ValidateHubUrl("https://hub.example:8653") == HubConfigError::kNone);
    CHECK(ValidateHubUrl("http://10.0.0.1") == HubConfigError::kNone);
    CHECK(ValidateHubUrl("http://10.0.0.1/prefix") == HubConfigError::kNone);
}

static void test_an_empty_url_is_empty_not_malformed() {
    CHECK(ValidateHubUrl("") == HubConfigError::kEmpty);
}

static void test_other_schemes_are_refused() {
    // ws:// and file:// are the interesting ones: esp_http_client would not
    // handle either, and the failure would arrive at connect time looking like
    // a network problem rather than a configuration one.
    CHECK(ValidateHubUrl("ws://hub:8653") == HubConfigError::kBadScheme);
    CHECK(ValidateHubUrl("file:///etc/passwd") == HubConfigError::kBadScheme);
    CHECK(ValidateHubUrl("192.168.0.10:8653") == HubConfigError::kBadScheme);
    CHECK(ValidateHubUrl("HTTP://hub:8653") == HubConfigError::kBadScheme);
}

static void test_a_scheme_with_no_host_is_refused() {
    CHECK(ValidateHubUrl("http://") == HubConfigError::kNoHost);
    CHECK(ValidateHubUrl("https://") == HubConfigError::kNoHost);
    CHECK(ValidateHubUrl("http:///path") == HubConfigError::kNoHost);
    CHECK(ValidateHubUrl("http://:8653") == HubConfigError::kNoHost);
    CHECK(ValidateHubUrl("http://?x=1") == HubConfigError::kNoHost);
}

static void test_control_characters_in_a_url_are_refused() {
    // A newline here would split the request line and let the caller append a
    // header of their choosing.
    CHECK(ValidateHubUrl("http://hub\r\nX-Evil: 1") == HubConfigError::kControlCharacter);
    CHECK(ValidateHubUrl("http://hub\n") == HubConfigError::kControlCharacter);
    CHECK(ValidateHubUrl(std::string("http://hub\0:8653", 16)) ==
          HubConfigError::kControlCharacter);
    CHECK(ValidateHubUrl("http://hub\x7f") == HubConfigError::kControlCharacter);
}

static void test_a_space_in_a_url_is_refused() {
    // A space ends the request line early: everything after it becomes the
    // HTTP version, and the request means something else entirely.
    CHECK(ValidateHubUrl("http://hub host") == HubConfigError::kControlCharacter);
}

static void test_an_overlong_url_is_refused() {
    std::string url = "http://";
    url.append(kMaxHubUrlChars, 'a');
    CHECK(ValidateHubUrl(url) == HubConfigError::kTooLong);
    // Exactly at the cap is accepted: the boundary is pinned rather than left
    // to whichever side of it a future edit lands on.
    std::string edge = "http://";
    edge.append(kMaxHubUrlChars - edge.size(), 'a');
    CHECK(edge.size() == kMaxHubUrlChars);
    CHECK(ValidateHubUrl(edge) == HubConfigError::kNone);
}

// ----------------------------------------------------------------- tokens --

static void test_an_ordinary_token_is_accepted() {
    CHECK(ValidateHubToken(kToken) == HubConfigError::kNone);
}

static void test_an_empty_token_is_empty() {
    CHECK(ValidateHubToken("") == HubConfigError::kEmpty);
}

static void test_a_token_with_a_newline_is_refused() {
    // `Authorization: Bearer <token>` — a newline ends the header and starts
    // another one of the caller's choosing.
    CHECK(ValidateHubToken("abc\r\nX-Evil: 1") == HubConfigError::kControlCharacter);
    CHECK(ValidateHubToken("abc\n") == HubConfigError::kControlCharacter);
    CHECK(ValidateHubToken(std::string("abc\0def", 7)) == HubConfigError::kControlCharacter);
}

static void test_a_token_with_a_space_is_refused() {
    // Everything after the space becomes a separate, ignored parameter, and
    // the hub answers 401. That looks like a wrong token rather than a
    // malformed one, and the half hour goes on the wrong hypothesis.
    CHECK(ValidateHubToken("abc def") == HubConfigError::kControlCharacter);
    CHECK(ValidateHubToken(" abc") == HubConfigError::kControlCharacter);
}

static void test_an_overlong_token_is_refused() {
    CHECK(ValidateHubToken(std::string(kMaxHubTokenChars + 1, 'a')) ==
          HubConfigError::kTooLong);
    CHECK(ValidateHubToken(std::string(kMaxHubTokenChars, 'a')) == HubConfigError::kNone);
}

// ------------------------------------------------------------------ pairs --

static void test_a_valid_pair_is_normalised() {
    std::string url;
    std::string token;
    CHECK(ValidateHubPair("http://hub:8653/", kToken, &url, &token) ==
          HubConfigError::kNone);
    CHECK(url == "http://hub:8653");
    CHECK(token == kToken);
}

static void test_repeated_trailing_slashes_are_all_removed() {
    std::string url;
    std::string token;
    CHECK(ValidateHubPair("http://hub:8653///", kToken, &url, &token) ==
          HubConfigError::kNone);
    CHECK(url == "http://hub:8653");
}

static void test_both_empty_clears_the_configuration() {
    // The documented way to unconfigure a hub, and a success rather than an
    // error: the device goes back to the state it ships in.
    std::string url = "stale";
    std::string token = "stale";
    CHECK(ValidateHubPair("", "", &url, &token) == HubConfigError::kNone);
    CHECK(url.empty());
    CHECK(token.empty());
}

static void test_a_url_without_a_token_is_incomplete() {
    // Accepting this would leave the device reporting itself configured while
    // every upload came back 401.
    std::string url;
    std::string token;
    CHECK(ValidateHubPair("http://hub:8653", "", &url, &token) ==
          HubConfigError::kIncomplete);
}

static void test_a_token_without_a_url_is_incomplete() {
    std::string url;
    std::string token;
    CHECK(ValidateHubPair("", kToken, &url, &token) == HubConfigError::kIncomplete);
}

static void test_a_url_of_only_slashes_is_incomplete_not_a_bad_scheme() {
    // It normalises to empty, so with a token present it is the same mistake
    // as leaving the url out.
    std::string url;
    std::string token;
    CHECK(ValidateHubPair("///", kToken, &url, &token) == HubConfigError::kIncomplete);
}

static void test_a_rejected_pair_does_not_write_the_outputs() {
    // The caller passes these straight to NVS. Writing a half-validated URL
    // would persist a configuration the validator just refused.
    std::string url = "untouched";
    std::string token = "untouched";
    CHECK(ValidateHubPair("ws://hub", kToken, &url, &token) == HubConfigError::kBadScheme);
    CHECK(url == "untouched");
    CHECK(token == "untouched");
    CHECK(ValidateHubPair("http://hub", "bad token", &url, &token) ==
          HubConfigError::kControlCharacter);
    CHECK(url == "untouched");
    CHECK(token == "untouched");
}

static void test_null_outputs_are_safe() {
    CHECK(ValidateHubPair("http://hub:8653", kToken, nullptr, nullptr) ==
          HubConfigError::kNone);
    CHECK(ValidateHubPair("", "", nullptr, nullptr) == HubConfigError::kNone);
}

static void test_error_names_are_total_and_readable() {
    CHECK(std::strcmp(HubConfigErrorName(HubConfigError::kNone), "ok") == 0);
    CHECK(std::strstr(HubConfigErrorName(HubConfigError::kBadScheme), "http://") != nullptr);
    CHECK(std::strstr(HubConfigErrorName(HubConfigError::kIncomplete), "both") != nullptr);
}

int main() {
    RUN(test_ordinary_urls_are_accepted);
    RUN(test_an_empty_url_is_empty_not_malformed);
    RUN(test_other_schemes_are_refused);
    RUN(test_a_scheme_with_no_host_is_refused);
    RUN(test_control_characters_in_a_url_are_refused);
    RUN(test_a_space_in_a_url_is_refused);
    RUN(test_an_overlong_url_is_refused);

    RUN(test_an_ordinary_token_is_accepted);
    RUN(test_an_empty_token_is_empty);
    RUN(test_a_token_with_a_newline_is_refused);
    RUN(test_a_token_with_a_space_is_refused);
    RUN(test_an_overlong_token_is_refused);

    RUN(test_a_valid_pair_is_normalised);
    RUN(test_repeated_trailing_slashes_are_all_removed);
    RUN(test_both_empty_clears_the_configuration);
    RUN(test_a_url_without_a_token_is_incomplete);
    RUN(test_a_token_without_a_url_is_incomplete);
    RUN(test_a_url_of_only_slashes_is_incomplete_not_a_bad_scheme);
    RUN(test_a_rejected_pair_does_not_write_the_outputs);
    RUN(test_null_outputs_are_safe);
    RUN(test_error_names_are_total_and_readable);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
