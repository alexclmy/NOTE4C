/**
 * @file test_voice_transport.cc
 * @brief Host tests for the real voice_transport translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * No sockets. The "network" here is a recorder that counts attempts and hands
 * back whichever outcome the test wants, so a twenty-second retry ladder runs
 * in microseconds and the interleavings that matter (a result arriving after a
 * cancel, a timeout landing on the last attempt) are driven deliberately
 * rather than waited for.
 */

#include "common/voice_transport.h"

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
        std::printf("%-56s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

static void CheckState(TransportState actual, TransportState expected, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] expected %s, got %s\n", g_current_test, line,
                    TransportStateName(expected), TransportStateName(actual));
    }
}

#define CHECK_STATE(t, expected) CheckState((t).state(), (expected), __LINE__)

// --------------------------------------------------------------- the fakes --

struct Wire {
    int sends = 0;
    int aborts = 0;
    int finishes = 0;
    bool last_ok = false;
    std::vector<uint8_t> attempt_numbers;
    std::vector<std::string> request_ids;
    std::vector<std::string> keys;

    TransportHooks MakeHooks() {
        TransportHooks h;
        h.send = [this](const Identity& id, uint8_t attempt) {
            ++sends;
            attempt_numbers.push_back(attempt);
            request_ids.push_back(id.request_id);
            keys.push_back(id.idempotency_key);
        };
        h.abort = [this]() { ++aborts; };
        h.finished = [this](bool ok) { ++finishes; last_ok = ok; };
        return h;
    }
};

static Identity MakeIdentity(const char* rid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
                             const char* key = "utterance-3f2504e0") {
    Identity id{};
    std::snprintf(id.request_id, sizeof(id.request_id), "%s", rid);
    std::snprintf(id.idempotency_key, sizeof(id.idempotency_key), "%s", key);
    return id;
}

static TransportConfig TestConfig() {
    TransportConfig c;
    c.max_attempts = 3;
    c.attempt_timeout_ms = 6000;
    c.backoff_ms = 1000;
    c.total_deadline_ms = 22000;
    return c;
}

static void Make(VoiceTransport& t, Wire& wire) {
    t.SetConfig(TestConfig());
    t.SetHooks(wire.MakeHooks());
}

// ------------------------------------------------------------ classification --

static void test_success_statuses() {
    CHECK(ClassifyHttpStatus(200) == Outcome::kSuccess);
    CHECK(ClassifyHttpStatus(201) == Outcome::kSuccess);
    CHECK(ClassifyHttpStatus(204) == Outcome::kSuccess);
}

static void test_no_answer_is_retryable() {
    CHECK(ClassifyHttpStatus(0) == Outcome::kRetryable);
}

static void test_server_errors_and_throttling_are_retryable() {
    CHECK(ClassifyHttpStatus(500) == Outcome::kRetryable);
    CHECK(ClassifyHttpStatus(502) == Outcome::kRetryable);
    CHECK(ClassifyHttpStatus(503) == Outcome::kRetryable);
    CHECK(ClassifyHttpStatus(429) == Outcome::kRetryable);
}

static void test_the_hubs_own_refusals_are_permanent() {
    // Every one of these is a documented answer in hub/CONTRACTS.md, and not
    // one of them can be fixed by sending the same bytes again.
    CHECK(ClassifyHttpStatus(400) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(401) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(409) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(413) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(404) == Outcome::kPermanent);
}

static void test_a_redirect_is_not_followed() {
    // Following one would send household speech to an address nobody
    // configured, and the hub does not redirect in the first place.
    CHECK(ClassifyHttpStatus(301) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(302) == Outcome::kPermanent);
    CHECK(ClassifyHttpStatus(307) == Outcome::kPermanent);
}

// ---------------------------------------------------------------- happy path --

static void test_a_first_attempt_that_succeeds_stops_there() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    CHECK(t.Begin(MakeIdentity(), 1000));
    CHECK_STATE(t, TransportState::kSending);
    CHECK(wire.sends == 1);
    t.OnAttemptResult(Outcome::kSuccess, 1200);
    CHECK_STATE(t, TransportState::kSucceeded);
    CHECK(t.failure() == FailureKind::kNone);
    CHECK(wire.sends == 1);
    CHECK(wire.aborts == 0);
    CHECK(wire.finishes == 1);
    CHECK(wire.last_ok);
    CHECK(t.attempts_made() == 1);
}

static void test_the_attempt_number_is_one_based() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    CHECK(wire.attempt_numbers.size() == 1);
    CHECK(wire.attempt_numbers[0] == 1);
}

// ------------------------------------------------------------------ identity --

static void test_the_identity_is_constant_across_retries() {
    // The property that makes a retry safe rather than a second utterance:
    // hub/CONTRACTS.md answers a repeated delivery with the stored result.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    t.OnAttemptResult(Outcome::kRetryable, 100);
    t.Tick(1100);
    t.OnAttemptResult(Outcome::kRetryable, 1200);
    t.Tick(2200);
    CHECK(wire.sends == 3);
    for (size_t i = 1; i < wire.request_ids.size(); ++i) {
        CHECK(wire.request_ids[i] == wire.request_ids[0]);
        CHECK(wire.keys[i] == wire.keys[0]);
    }
    CHECK(wire.request_ids[0] == "3f2504e0-4f89-41d3-9a0c-0305e82c3301");
    CHECK(wire.keys[0] == "utterance-3f2504e0");
}

static void test_an_unterminated_identity_is_terminated() {
    // The glue builds these from a formatter, but a missing terminator would
    // be read past the end of the struct by every header and log call.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    Identity id{};
    std::memset(id.request_id, 'a', sizeof(id.request_id));
    std::memset(id.idempotency_key, 'b', sizeof(id.idempotency_key));
    t.Begin(id, 0);
    CHECK(std::strlen(t.identity().request_id) == kRequestIdChars - 1);
    CHECK(std::strlen(t.identity().idempotency_key) == kIdempotencyKeyChars - 1);
}

// ------------------------------------------------------------- the retry ladder --

static void test_a_retryable_outcome_backs_off_before_trying_again() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.OnAttemptResult(Outcome::kRetryable, 1500);
    CHECK_STATE(t, TransportState::kBackoff);
    CHECK(wire.sends == 1);
    t.Tick(2499);
    CHECK_STATE(t, TransportState::kBackoff);
    CHECK(wire.sends == 1);
    t.Tick(2500);
    CHECK_STATE(t, TransportState::kSending);
    CHECK(wire.sends == 2);
    CHECK(wire.attempt_numbers[1] == 2);
}

static void test_three_failures_exhaust_the_ladder() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    t.OnAttemptResult(Outcome::kRetryable, 100);
    t.Tick(1100);
    t.OnAttemptResult(Outcome::kRetryable, 1200);
    t.Tick(2200);
    CHECK(wire.sends == 3);
    CHECK_STATE(t, TransportState::kSending);
    t.OnAttemptResult(Outcome::kRetryable, 2300);
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kAttemptsExhausted);
    CHECK(wire.sends == 3);
    CHECK(wire.finishes == 1);
    CHECK(!wire.last_ok);
}

static void test_a_permanent_rejection_does_not_retry() {
    // Retrying a 401 three times produces three log lines and one error, and
    // delays the error the user is waiting for.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    t.OnAttemptResult(Outcome::kPermanent, 100);
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kPermanentRejection);
    CHECK(wire.sends == 1);
    // Time passing changes nothing.
    for (uint64_t now = 200; now < 60000; now += 250) {
        t.Tick(now);
    }
    CHECK(wire.sends == 1);
    CHECK(wire.finishes == 1);
}

static void test_a_late_success_after_two_failures_still_succeeds() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    t.OnAttemptResult(Outcome::kRetryable, 100);
    t.Tick(1100);
    t.OnAttemptResult(Outcome::kRetryable, 1200);
    t.Tick(2200);
    t.OnAttemptResult(Outcome::kSuccess, 2300);
    CHECK_STATE(t, TransportState::kSucceeded);
    CHECK(wire.last_ok);
    CHECK(t.attempts_made() == 3);
}

// ------------------------------------------------------------------ timeouts --

static void test_an_attempt_that_times_out_is_aborted_and_retried() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.Tick(1000 + 5999);
    CHECK_STATE(t, TransportState::kSending);
    CHECK(wire.aborts == 0);
    t.Tick(1000 + 6000);
    CHECK(wire.aborts == 1);
    CHECK_STATE(t, TransportState::kBackoff);
    t.Tick(1000 + 7000);
    CHECK_STATE(t, TransportState::kSending);
    CHECK(wire.sends == 2);
}

static void test_a_timeout_counts_as_an_attempt() {
    // Otherwise a hub that never answers is retried forever.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    uint64_t now = 0;
    t.Begin(MakeIdentity(), now);
    for (int i = 0; i < 3; ++i) {
        now += 6000;
        t.Tick(now);
        now += 1000;
        t.Tick(now);
    }
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.attempts_made() == 3);
    CHECK(wire.sends == 3);
    CHECK(wire.aborts == 3);
}

static void test_the_whole_ladder_has_a_deadline() {
    // A configuration whose arithmetic allows more time than the deadline must
    // still stop at the deadline, and must not start one more attempt on the
    // way out.
    VoiceTransport t;
    Wire wire;
    TransportConfig c;
    c.max_attempts = 10;
    c.attempt_timeout_ms = 3000;
    c.backoff_ms = 500;
    c.total_deadline_ms = 8000;
    t.SetConfig(c);
    t.SetHooks(wire.MakeHooks());
    uint64_t now = 1000;
    t.Begin(MakeIdentity(), now);
    for (int i = 0; i < 40 && t.busy(); ++i) {
        now += 500;
        t.Tick(now);
    }
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kDeadlineExceeded);
    CHECK(now <= 1000 + 8000 + 500);
    CHECK(wire.finishes == 1);
}

static void test_a_retryable_result_past_the_deadline_stops() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 0);
    t.OnAttemptResult(Outcome::kRetryable, 22000);
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kDeadlineExceeded);
    CHECK(wire.sends == 1);
}

// -------------------------------------------------------------- cancellation --

static void test_cancel_aborts_the_attempt_in_flight() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.Cancel(1500);
    CHECK_STATE(t, TransportState::kCancelled);
    CHECK(t.failure() == FailureKind::kCancelled);
    CHECK(wire.aborts == 1);
    CHECK(wire.finishes == 1);
    CHECK(!wire.last_ok);
}

static void test_cancel_during_backoff_does_not_abort_a_dead_attempt() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.OnAttemptResult(Outcome::kRetryable, 1100);
    CHECK_STATE(t, TransportState::kBackoff);
    t.Cancel(1200);
    CHECK_STATE(t, TransportState::kCancelled);
    CHECK(wire.aborts == 0);
}

static void test_a_result_arriving_after_a_cancel_is_dropped() {
    // Two tasks on the device: the HTTP call completing and the button task
    // cancelling. Acting on the late result would resurrect a cancelled
    // upload and play an answer the user asked not to hear.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.Cancel(1500);
    t.OnAttemptResult(Outcome::kSuccess, 1600);
    CHECK_STATE(t, TransportState::kCancelled);
    CHECK(wire.finishes == 1);
}

static void test_cancelling_something_finished_does_not_rewrite_it() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.OnAttemptResult(Outcome::kPermanent, 1100);
    CHECK_STATE(t, TransportState::kFailed);
    t.Cancel(1200);
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kPermanentRejection);
    CHECK(wire.finishes == 1);
}

static void test_a_cancel_stops_the_ticking() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.Cancel(1500);
    for (uint64_t now = 1600; now < 60000; now += 250) {
        t.Tick(now);
    }
    CHECK(wire.sends == 1);
    CHECK_STATE(t, TransportState::kCancelled);
}

// ------------------------------------------------------------------ one at a time --

static void test_a_second_begin_while_busy_is_refused() {
    // One utterance in flight. Starting a second would leave the first with
    // nobody to report to and two uploads racing for one state machine.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    CHECK(t.Begin(MakeIdentity("11111111-1111-4111-8111-111111111111", "k1"), 1000));
    CHECK(!t.Begin(MakeIdentity("22222222-2222-4222-8222-222222222222", "k2"), 1100));
    CHECK(wire.sends == 1);
    CHECK(std::strcmp(t.identity().idempotency_key, "k1") == 0);
}

static void test_release_frees_the_slot_only_when_not_busy() {
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.Release();
    CHECK_STATE(t, TransportState::kSending);  // still owed a result
    t.OnAttemptResult(Outcome::kSuccess, 1100);
    t.Release();
    CHECK_STATE(t, TransportState::kIdle);
    CHECK(t.attempts_made() == 0);
    CHECK(t.Begin(MakeIdentity("22222222-2222-4222-8222-222222222222", "k2"), 2000));
    CHECK(wire.sends == 2);
}

static void test_release_forgets_the_previous_identity() {
    // The identity is what makes a redelivery a duplicate. Leaving the last
    // one lying around invites a future edit to reuse it for a new utterance,
    // which the hub would answer with the wrong stored result.
    VoiceTransport t;
    Wire wire;
    Make(t, wire);
    t.Begin(MakeIdentity(), 1000);
    t.OnAttemptResult(Outcome::kSuccess, 1100);
    t.Release();
    CHECK(t.identity().request_id[0] == '\0');
    CHECK(t.identity().idempotency_key[0] == '\0');
}

// ------------------------------------------------------------- configuration --

static void test_zero_attempts_fails_rather_than_hanging() {
    VoiceTransport t;
    Wire wire;
    TransportConfig c = TestConfig();
    c.max_attempts = 0;
    t.SetConfig(c);
    t.SetHooks(wire.MakeHooks());
    CHECK(t.Begin(MakeIdentity(), 1000));
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(wire.sends == 0);
    CHECK(wire.finishes == 1);
    CHECK(!t.busy());
}

static void test_one_attempt_never_backs_off() {
    VoiceTransport t;
    Wire wire;
    TransportConfig c = TestConfig();
    c.max_attempts = 1;
    t.SetConfig(c);
    t.SetHooks(wire.MakeHooks());
    t.Begin(MakeIdentity(), 1000);
    t.OnAttemptResult(Outcome::kRetryable, 1100);
    CHECK_STATE(t, TransportState::kFailed);
    CHECK(t.failure() == FailureKind::kAttemptsExhausted);
    CHECK(wire.sends == 1);
}

static void test_null_hooks_are_safe() {
    VoiceTransport t;
    t.SetConfig(TestConfig());
    t.Begin(MakeIdentity(), 1000);
    t.Tick(7100);
    t.OnAttemptResult(Outcome::kRetryable, 8200);
    t.Cancel(9000);
    CHECK_STATE(t, TransportState::kCancelled);
}

static void test_names_are_total() {
    CHECK(std::strcmp(TransportStateName(TransportState::kBackoff), "Backoff") == 0);
    CHECK(std::strcmp(OutcomeName(Outcome::kPermanent), "permanent") == 0);
    CHECK(std::strstr(FailureKindName(FailureKind::kDeadlineExceeded), "time") != nullptr);
}

int main() {
    RUN(test_success_statuses);
    RUN(test_no_answer_is_retryable);
    RUN(test_server_errors_and_throttling_are_retryable);
    RUN(test_the_hubs_own_refusals_are_permanent);
    RUN(test_a_redirect_is_not_followed);

    RUN(test_a_first_attempt_that_succeeds_stops_there);
    RUN(test_the_attempt_number_is_one_based);

    RUN(test_the_identity_is_constant_across_retries);
    RUN(test_an_unterminated_identity_is_terminated);

    RUN(test_a_retryable_outcome_backs_off_before_trying_again);
    RUN(test_three_failures_exhaust_the_ladder);
    RUN(test_a_permanent_rejection_does_not_retry);
    RUN(test_a_late_success_after_two_failures_still_succeeds);

    RUN(test_an_attempt_that_times_out_is_aborted_and_retried);
    RUN(test_a_timeout_counts_as_an_attempt);
    RUN(test_the_whole_ladder_has_a_deadline);
    RUN(test_a_retryable_result_past_the_deadline_stops);

    RUN(test_cancel_aborts_the_attempt_in_flight);
    RUN(test_cancel_during_backoff_does_not_abort_a_dead_attempt);
    RUN(test_a_result_arriving_after_a_cancel_is_dropped);
    RUN(test_cancelling_something_finished_does_not_rewrite_it);
    RUN(test_a_cancel_stops_the_ticking);

    RUN(test_a_second_begin_while_busy_is_refused);
    RUN(test_release_frees_the_slot_only_when_not_busy);
    RUN(test_release_forgets_the_previous_identity);

    RUN(test_zero_attempts_fails_rather_than_hanging);
    RUN(test_one_attempt_never_backs_off);
    RUN(test_null_hooks_are_safe);
    RUN(test_names_are_total);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
