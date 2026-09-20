/**
 * @file test_dashboard_service.cc
 * @brief Host tests for the real dashboard_service translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Links main/common/dashboard_service.cc and dashboard_slot.cc directly. The
 * last group is an integration test over both: it is the one that decides
 * whether "coalescing" actually lands on the newest frame rather than merely
 * not crashing.
 */

#include "common/dashboard_service.h"
#include "common/dashboard_slot.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace dashboard;

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

#define RUN(fn)                                                    \
    do {                                                           \
        g_current_test = #fn;                                      \
        const int before = g_failures;                             \
        fn();                                                      \
        std::printf("%-48s %s\n", #fn,                             \
                    (g_failures == before) ? "ok" : "FAILED");     \
    } while (0)

// Fixed, obviously-fake test token. Not a secret and never used on a device:
// real tokens are generated on the device during local provisioning.
static const uint8_t kTestToken[kTokenBytes] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
};
static const char* kTestTokenHex =
    "00112233445566778899aabbccddeeff102132435465768798a9bacbdcedfe0f";

// ------------------------------------------------------------------- hex --

static void test_hex_roundtrip() {
    char hex[kTokenHexChars + 1];
    HexEncode(kTestToken, kTokenBytes, hex);
    CHECK(std::string(hex) == std::string(kTestTokenHex));

    uint8_t back[kTokenBytes];
    CHECK(HexDecode(hex, kTokenHexChars, back, kTokenBytes));
    CHECK(std::memcmp(back, kTestToken, kTokenBytes) == 0);
}

static void test_hex_decode_rejects_malformed() {
    uint8_t out[4];
    CHECK(!HexDecode("zzzzzzzz", 8, out, 4));      // non-hex characters
    CHECK(!HexDecode("0011", 4, out, 4));          // too short for out_len
    CHECK(!HexDecode("001122334455", 12, out, 4)); // too long for out_len
    CHECK(!HexDecode("0011223", 7, out, 4));       // odd length
    CHECK(!HexDecode(nullptr, 8, out, 4));
    CHECK(HexDecode("00AABBcc", 8, out, 4));       // mixed case accepted
    CHECK(out[1] == 0xAA && out[3] == 0xCC);
}

// ------------------------------------------------------------- FrameAuth --

/// The default posture: a device nobody has paired yet refuses writes. It does
/// not fall back to "allow", and there is no request that can install a token.
static void test_unprovisioned_denies_everything() {
    FrameAuth auth;
    CHECK(!auth.provisioned());
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 0) == AuthResult::kNotProvisioned);
    CHECK(auth.Check("", 0, 0) == AuthResult::kNotProvisioned);
    CHECK(auth.Check(nullptr, 0, 0) == AuthResult::kNotProvisioned);
}

static void test_set_token_requires_exact_length() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes - 1);
    CHECK(!auth.provisioned());
    auth.SetToken(nullptr, kTokenBytes);
    CHECK(!auth.provisioned());
    auth.SetToken(kTestToken, kTokenBytes);
    CHECK(auth.provisioned());
    // Clearing returns the device to the deny-by-default state.
    auth.SetToken(nullptr, 0);
    CHECK(!auth.provisioned());
}

static void test_correct_and_incorrect_tokens() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes);

    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 1000) == AuthResult::kOk);

    // Differs in the final nibble only.
    std::string almost(kTestTokenHex);
    almost[kTokenHexChars - 1] = '0';
    CHECK(auth.Check(almost.c_str(), kTokenHexChars, 1000) == AuthResult::kBadToken);

    CHECK(auth.Check("", 0, 1000) == AuthResult::kBadToken);
    CHECK(auth.Check(nullptr, 0, 1000) == AuthResult::kBadToken);
    CHECK(auth.Check("not-hex-at-all", 14, 1000) == AuthResult::kBadToken);
}

static void test_success_resets_failure_counter() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes);
    for (int i = 0; i < 5; ++i) {
        CHECK(auth.Check("00", 2, 1000) == AuthResult::kBadToken);
    }
    CHECK(auth.failure_count() == 5);
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 1000) == AuthResult::kOk);
    CHECK(auth.failure_count() == 0);
}

static void test_lockout_after_repeated_failures() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes);

    for (int i = 0; i < FrameAuth::kMaxFailures; ++i) {
        CHECK(auth.Check("00", 2, 1000) == AuthResult::kBadToken);
    }
    // The next attempt is refused without even looking at the token, and a
    // correct token does not get you past the lockout either.
    CHECK(auth.Check("00", 2, 1000) == AuthResult::kLockedOut);
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 1000) == AuthResult::kLockedOut);
    CHECK(auth.lockout_remaining_ms(1000) > 0);
}

static void test_lockout_expires_after_window() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes);
    for (int i = 0; i < FrameAuth::kMaxFailures; ++i) {
        auth.Check("00", 2, 1000);
    }
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 1000) == AuthResult::kLockedOut);

    const uint64_t after = 1000 + FrameAuth::kWindowMs + 1;
    CHECK(auth.lockout_remaining_ms(after) == 0);
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, after) == AuthResult::kOk);
}

/// A monotonic clock that jumps backwards (NTP step, counter wrap on reboot)
/// must not leave the device permanently locked out of its own screen.
static void test_backwards_clock_does_not_wedge_lockout() {
    FrameAuth auth;
    auth.SetToken(kTestToken, kTokenBytes);
    for (int i = 0; i < FrameAuth::kMaxFailures; ++i) {
        auth.Check("00", 2, 500000);
    }
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 500000) == AuthResult::kLockedOut);
    // Clock jumps back before the window started.
    CHECK(auth.Check(kTestTokenHex, kTokenHexChars, 10) == AuthResult::kOk);
}

// --------------------------------------------------------- PairingWindow --

static void test_pairing_window_starts_closed() {
    PairingWindow w;
    CHECK(w.state(0) == PairingWindow::State::kClosed);
    CHECK(w.remaining_ms(0) == 0);
    // Nothing can be claimed from a window nobody opened.
    CHECK(!w.Claim(0));
}

static void test_pairing_window_single_claim() {
    PairingWindow w;
    w.Open(1000);
    CHECK(w.state(1000) == PairingWindow::State::kOpen);
    CHECK(w.Claim(1500));
    CHECK(w.state(1500) == PairingWindow::State::kClaimed);
    // A second caller during the same window gets nothing.
    CHECK(!w.Claim(1600));
    CHECK(!w.Claim(1700));
}

static void test_pairing_window_expires() {
    PairingWindow w;
    w.Open(1000, 5000);
    CHECK(w.state(5999) == PairingWindow::State::kOpen);
    CHECK(w.remaining_ms(5999) == 1);
    CHECK(w.state(6000) == PairingWindow::State::kExpired);
    CHECK(w.remaining_ms(6000) == 0);
    CHECK(!w.Claim(6000));
    CHECK(w.state(6000) == PairingWindow::State::kExpired);
}

static void test_pairing_window_backwards_clock_expires() {
    PairingWindow w;
    w.Open(500000, 5000);
    // Clock jumped backwards: refuse rather than silently extend the window.
    CHECK(w.state(10) == PairingWindow::State::kExpired);
    CHECK(!w.Claim(10));
}

static void test_pairing_window_reopen_discards_previous() {
    PairingWindow w;
    w.Open(1000, 5000);
    CHECK(w.Claim(1100));
    CHECK(w.state(1100) == PairingWindow::State::kClaimed);

    // Re-pairing is always allowed and always starts a fresh window.
    w.Open(2000, 5000);
    CHECK(w.state(2000) == PairingWindow::State::kOpen);
    CHECK(w.Claim(2100));
}

static void test_pairing_window_close_is_immediate() {
    PairingWindow w;
    w.Open(1000, 60000);
    w.Close();
    CHECK(w.state(1001) == PairingWindow::State::kClosed);
    CHECK(!w.Claim(1001));
}

// ------------------------------------------------------ IdempotencyCache --

static void test_idempotency_records_and_recalls() {
    IdempotencyCache cache;
    CHECK(!cache.Seen("abc", 3));
    cache.Record("abc", 3);
    CHECK(cache.Seen("abc", 3));
    CHECK(!cache.Seen("abd", 3));
    // Prefix must not count as a match.
    CHECK(!cache.Seen("ab", 2));
}

static void test_idempotency_evicts_oldest() {
    IdempotencyCache cache;
    char key[8];
    for (size_t i = 0; i < IdempotencyCache::kDepth; ++i) {
        std::snprintf(key, sizeof(key), "k%zu", i);
        cache.Record(key, std::strlen(key));
    }
    CHECK(cache.Seen("k0", 2));

    // One more distinct key pushes the oldest out; the ring stays bounded.
    cache.Record("overflow", 8);
    CHECK(!cache.Seen("k0", 2));
    CHECK(cache.Seen("overflow", 8));
    CHECK(cache.Seen("k1", 2));
}

static void test_idempotency_ignores_degenerate_keys() {
    IdempotencyCache cache;
    cache.Record("", 0);
    CHECK(!cache.Seen("", 0));

    std::string huge(IdempotencyCache::kKeyMax + 1, 'x');
    cache.Record(huge.c_str(), huge.size());
    CHECK(!cache.Seen(huge.c_str(), huge.size()));
}

static void test_idempotency_duplicate_record_does_not_consume_slot() {
    IdempotencyCache cache;
    cache.Record("first", 5);
    for (int i = 0; i < 50; ++i) {
        cache.Record("first", 5);   // retried many times
    }
    CHECK(cache.Seen("first", 5));

    // Because duplicates did not consume ring slots, a handful of new keys
    // still fits alongside it.
    cache.Record("second", 6);
    CHECK(cache.Seen("first", 5));
    CHECK(cache.Seen("second", 6));
}

// ---------------------------------------------------- RefreshCoordinator --

static void ShaOf(uint8_t seed, uint8_t* out) {
    uint8_t buf[16];
    std::memset(buf, seed, sizeof(buf));
    Sha256(buf, sizeof(buf), out);
}

static void test_coordinator_single_flight() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes];
    ShaOf(1, a);

    CHECK(rc.state() == RefreshState::kIdle);
    CHECK(rc.Request(a, 1, false) == RenderDisposition::kStarted);
    CHECK(rc.state() == RefreshState::kRendering);
    CHECK(!rc.pending());
}

static void test_coordinator_queues_then_coalesces() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes], c[kShaBytes];
    ShaOf(1, a); ShaOf(2, b); ShaOf(3, c);

    CHECK(rc.Request(a, 1, false) == RenderDisposition::kStarted);
    CHECK(rc.Request(b, 2, false) == RenderDisposition::kQueued);
    CHECK(rc.pending());
    // Everything after the first pending request folds into it. The queue never
    // grows past one, no matter how hard the pusher hammers.
    CHECK(rc.Request(c, 3, false) == RenderDisposition::kCoalesced);
    for (int i = 0; i < 100; ++i) {
        CHECK(rc.Request(c, 4, false) == RenderDisposition::kCoalesced);
    }
    CHECK(rc.coalesced_count() == 101);
    CHECK(rc.pending());
}

static void test_coordinator_completion_drains_pending() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(1, a); ShaOf(2, b);

    rc.Request(a, 1, false);
    rc.Request(b, 2, false);
    // A pending request means the caller must immediately start another render.
    CHECK(rc.CompleteRender(a, 1, RenderOutcome::kDrawn));
    CHECK(rc.state() == RefreshState::kRendering);
    CHECK(!rc.pending());

    // No pending this time: settle to idle.
    CHECK(!rc.CompleteRender(b, 2, RenderOutcome::kDrawn));
    CHECK(rc.state() == RefreshState::kIdle);
    CHECK(rc.displayed_seq() == 2);
    CHECK(std::memcmp(rc.displayed_sha(), b, kShaBytes) == 0);
}

static void test_coordinator_skips_identical_frame() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes];
    ShaOf(1, a);

    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);

    CHECK(rc.Request(a, 1, false) == RenderDisposition::kSkipped);
    CHECK(rc.skipped_count() == 1);
    CHECK(rc.state() == RefreshState::kIdle);
}

/// An explicit user-initiated redraw must repaint even when nothing changed,
/// otherwise pressing the button on a ghosted panel appears to do nothing.
static void test_coordinator_force_overrides_skip() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes];
    ShaOf(1, a);
    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);

    CHECK(rc.Request(a, 1, true) == RenderDisposition::kStarted);
    CHECK(rc.state() == RefreshState::kRendering);
}

static void test_coordinator_failed_render_keeps_displayed_frame() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(1, a); ShaOf(2, b);

    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);
    CHECK(rc.displayed_seq() == 1);

    rc.Request(b, 2, false);
    rc.CompleteRender(b, 2, RenderOutcome::kFailed);   // panel refresh failed
    CHECK(rc.displayed_seq() == 1);
    CHECK(std::memcmp(rc.displayed_sha(), a, kShaBytes) == 0);
    CHECK(rc.state() == RefreshState::kIdle);
}

static void test_coordinator_counts_failed_renders() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(1, a); ShaOf(2, b);

    // Nothing has been attempted, so nothing may be reported as failed. A
    // fabricated non-zero here would be indistinguishable, from the tower's
    // side, from a device that really did drop a frame.
    CHECK(rc.failed_count() == 0);
    CHECK(!rc.last_render_failed());
    CHECK(rc.last_failed_seq() == 0);

    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);
    CHECK(rc.failed_count() == 0);
    CHECK(!rc.last_render_failed());

    rc.Request(b, 2, false);
    rc.CompleteRender(b, 2, RenderOutcome::kFailed);
    CHECK(rc.failed_count() == 1);
    CHECK(rc.last_render_failed());
    // The sequence that was dropped, not the one that is displayed.
    CHECK(rc.last_failed_seq() == 2);
    CHECK(rc.displayed_seq() == 1);
}

static void test_coordinator_success_clears_last_failed_but_keeps_total() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(3, a); ShaOf(4, b);

    rc.Request(a, 7, false);
    rc.CompleteRender(a, 7, RenderOutcome::kFailed);
    CHECK(rc.failed_count() == 1);
    CHECK(rc.last_render_failed());

    rc.Request(b, 8, false);
    rc.CompleteRender(b, 8, RenderOutcome::kDrawn);
    // "Is it broken right now" and "has it ever been broken" are different
    // questions, and the status route answers both.
    CHECK(!rc.last_render_failed());
    CHECK(rc.failed_count() == 1);
    // last_failed_seq is history, not a live flag: it still names the frame
    // that was dropped so an operator can go and look for it.
    CHECK(rc.last_failed_seq() == 7);

    rc.Reset();
    CHECK(rc.failed_count() == 0);
    CHECK(!rc.last_render_failed());
    CHECK(rc.last_failed_seq() == 0);
}

// A frame that arrives while the user is on another page is *deferred*, not
// failed. This is the whole point of change #2: the tower must be able to tell
// "your frame was dropped" (a fault) from "the user was reading another page"
// (benign). deferred_ is a separate counter and must not touch failed_.
static void test_coordinator_counts_deferred_apart_from_failed() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes], c[kShaBytes];
    ShaOf(1, a); ShaOf(2, b); ShaOf(3, c);

    // Nothing attempted: both counters start clean.
    CHECK(rc.deferred_count() == 0);
    CHECK(!rc.last_render_deferred());
    CHECK(rc.last_deferred_seq() == 0);

    // A drawn frame, then one deferred because another page owns the screen.
    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);

    rc.Request(b, 2, false);
    rc.CompleteRender(b, 2, RenderOutcome::kDeferred);
    // Counted as deferred, and NOT as a failure.
    CHECK(rc.deferred_count() == 1);
    CHECK(rc.last_render_deferred());
    CHECK(rc.last_deferred_seq() == 2);
    CHECK(rc.failed_count() == 0);
    CHECK(!rc.last_render_failed());
    // A deferral does not steal the screen: the displayed frame is unchanged.
    CHECK(rc.displayed_seq() == 1);
    CHECK(std::memcmp(rc.displayed_sha(), a, kShaBytes) == 0);

    // A genuine fault still lands on failed_, and clears last_deferred.
    rc.Request(c, 3, false);
    rc.CompleteRender(c, 3, RenderOutcome::kFailed);
    CHECK(rc.failed_count() == 1);
    CHECK(rc.last_render_failed());
    CHECK(rc.last_failed_seq() == 3);
    CHECK(rc.deferred_count() == 1);      // unchanged by the failure
    CHECK(!rc.last_render_deferred());    // the latest completion was a fault

    // A drawn frame clears last_failed too; both totals are history that Reset
    // clears.
    rc.Request(a, 4, false);
    rc.CompleteRender(a, 4, RenderOutcome::kDrawn);
    CHECK(!rc.last_render_failed());
    CHECK(!rc.last_render_deferred());
    CHECK(rc.failed_count() == 1);
    CHECK(rc.deferred_count() == 1);

    rc.Reset();
    CHECK(rc.deferred_count() == 0);
    CHECK(!rc.last_render_deferred());
    CHECK(rc.last_deferred_seq() == 0);
    CHECK(rc.failed_count() == 0);
}

static void test_coordinator_counts_each_failure_in_a_coalesced_run() {
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(5, a); ShaOf(6, b);

    CHECK(rc.Request(a, 1, false) == RenderDisposition::kStarted);
    CHECK(rc.Request(b, 2, false) == RenderDisposition::kQueued);

    // The successor is drained by the same loop, so a run of failures must not
    // collapse into a single count — a panel that drops ten frames and a panel
    // that drops one are not the same fault.
    CHECK(rc.CompleteRender(nullptr, 1, RenderOutcome::kFailed));
    CHECK(rc.failed_count() == 1);
    CHECK(!rc.CompleteRender(nullptr, 2, RenderOutcome::kFailed));
    CHECK(rc.failed_count() == 2);
    CHECK(rc.last_failed_seq() == 2);
    CHECK(!rc.has_displayed());
}

static void test_coordinator_has_nothing_displayed_initially() {
    RefreshCoordinator rc;
    CHECK(!rc.has_displayed());
    CHECK(rc.displayed_sha() == nullptr);
    CHECK(rc.displayed_seq() == 0);
    // With nothing on the glass yet, an identical digest cannot be "skipped".
    uint8_t a[kShaBytes];
    ShaOf(9, a);
    CHECK(rc.Request(a, 1, false) == RenderDisposition::kStarted);
}

// -------------------------------------------------------- RenderHandshake --

namespace {

/// Records what the handshake did, and lets each hook be steered per test.
struct FakePanel {
    int queued_signals = 0;      ///< stale completion signals waiting
    bool will_signal = true;     ///< does the refresh report back at all
    bool panel_clean = true;     ///< did BUSY release, or did read_busy give up
    bool triggered = false;
    int drain_calls = 0;
    int trigger_calls = 0;
    uint32_t last_timeout_ms = 0;
    std::vector<std::string> order;

    RenderHandshakeHooks Hooks() {
        RenderHandshakeHooks h;
        h.drain = [this]() {
            ++drain_calls;
            order.push_back("drain");
            const int n = queued_signals;
            queued_signals = 0;
            return n;
        };
        h.trigger = [this]() {
            ++trigger_calls;
            triggered = true;
            order.push_back("trigger");
        };
        h.wait = [this](uint32_t timeout_ms) {
            last_timeout_ms = timeout_ms;
            order.push_back("wait");
            // A signal only arrives if the refresh actually ran; a stale signal
            // that was drained cannot satisfy this wait.
            return triggered && will_signal;
        };
        h.panel_ok = [this]() {
            order.push_back("panel_ok");
            return panel_clean;
        };
        return h;
    }
};

}  // namespace

static void test_handshake_completes_cleanly() {
    RenderHandshake hs;
    FakePanel panel;
    CHECK(hs.Run(panel.Hooks(), 5000) == AckResult::kCompleted);
    CHECK(hs.completions() == 1);
    CHECK(hs.timeouts() == 0);
    CHECK(hs.panel_failures() == 0);
    CHECK(!hs.in_progress());
}

/// Rule 1: a leftover signal from a previous refresh must be discarded before
/// triggering, or this render is acknowledged before the panel has started.
static void test_handshake_drains_stale_signals_before_triggering() {
    RenderHandshake hs;
    FakePanel panel;
    panel.queued_signals = 3;

    CHECK(hs.Run(panel.Hooks(), 5000) == AckResult::kCompleted);
    CHECK(hs.stale_signals_discarded() == 3);
    CHECK(panel.queued_signals == 0);
    // Ordering matters as much as the drain itself.
    CHECK(panel.order.size() >= 3);
    CHECK(panel.order[0] == "drain");
    CHECK(panel.order[1] == "trigger");
    CHECK(panel.order[2] == "wait");
}

/// Rule 2: a panel that never reports back must not wedge the render task.
static void test_handshake_times_out_rather_than_blocking_forever() {
    RenderHandshake hs;
    FakePanel panel;
    panel.will_signal = false;

    CHECK(hs.Run(panel.Hooks(), 1234) == AckResult::kTimedOut);
    CHECK(hs.timeouts() == 1);
    CHECK(hs.completions() == 0);
    CHECK(panel.last_timeout_ms == 1234);
    // The handshake must release itself so later frames can still be attempted.
    CHECK(!hs.in_progress());
    CHECK(hs.Run(panel.Hooks(), 1234) == AckResult::kTimedOut);
    CHECK(hs.timeouts() == 2);
}

/// Rule 3: read_busy() gives up after its own timeout and the driver carries on,
/// so "the refresh task came back" does not mean "the panel drew the frame".
static void test_handshake_rejects_signal_when_panel_reports_failure() {
    RenderHandshake hs;
    FakePanel panel;
    panel.panel_clean = false;

    CHECK(hs.Run(panel.Hooks(), 5000) == AckResult::kPanelFailed);
    CHECK(hs.panel_failures() == 1);
    CHECK(hs.completions() == 0);
    // panel_ok must actually be consulted, after the wait.
    CHECK(panel.order.back() == "panel_ok");
}

/// Rule 4: two overlapping handshakes would race for a single signal.
static void test_handshake_refuses_overlapping_runs() {
    RenderHandshake hs;
    FakePanel outer;

    RenderHandshakeHooks hooks = outer.Hooks();
    RenderHandshake* self = &hs;
    AckResult inner = AckResult::kCompleted;
    // Re-enter from inside the wait, which is where a second render task would
    // realistically arrive.
    hooks.wait = [&](uint32_t) {
        FakePanel nested;
        inner = self->Run(nested.Hooks(), 100);
        CHECK(!nested.triggered);   // the nested run must not touch the panel
        return true;
    };

    CHECK(hs.Run(hooks, 5000) == AckResult::kCompleted);
    CHECK(inner == AckResult::kNotAttempted);
    CHECK(hs.overlaps_refused() == 1);
}

static void test_handshake_requires_all_hooks() {
    RenderHandshake hs;
    RenderHandshakeHooks empty;
    CHECK(hs.Run(empty, 1000) == AckResult::kNotAttempted);

    FakePanel panel;
    RenderHandshakeHooks partial = panel.Hooks();
    partial.panel_ok = nullptr;
    CHECK(hs.Run(partial, 1000) == AckResult::kNotAttempted);
    // A refused run must not have poked the hardware.
    CHECK(!panel.triggered);
    CHECK(!hs.in_progress());
}

/// A failed handshake must leave the coordinator's displayed frame untouched,
/// so status keeps reporting the frame that is genuinely on the glass.
static void test_failed_handshake_does_not_update_displayed_frame() {
    RenderHandshake hs;
    RefreshCoordinator rc;
    uint8_t a[kShaBytes], b[kShaBytes];
    ShaOf(1, a); ShaOf(2, b);

    rc.Request(a, 1, false);
    rc.CompleteRender(a, 1, RenderOutcome::kDrawn);
    CHECK(rc.displayed_seq() == 1);

    FakePanel panel;
    panel.panel_clean = false;
    rc.Request(b, 2, false);
    const AckResult ack = hs.Run(panel.Hooks(), 5000);
    CHECK(ack == AckResult::kPanelFailed);
    rc.CompleteRender(b, 2, ack == AckResult::kCompleted ? RenderOutcome::kDrawn
                                                         : RenderOutcome::kFailed);

    CHECK(rc.displayed_seq() == 1);
    CHECK(std::memcmp(rc.displayed_sha(), a, kShaBytes) == 0);
}


// ----------------------------------------------- rule 1a: a busy panel --

namespace {

/**
 * @brief A panel that is already refreshing something else.
 *
 * The shape is the device's: on hardware the clock tick, the status bar and the
 * navigation pump all trigger refreshes from the main task while the dashboard
 * render task is inside the handshake. The refresh that is already in flight
 * has *not* posted its completion signal yet, so draining cannot remove it, and
 * when it does post, it posts its own panel verdict with it.
 */
struct BusyPanel {
    bool foreign_running = true;    ///< a refresh is in flight when we arrive
    bool foreign_signals = true;    ///< does that refresh ever come back
    bool foreign_clean = true;      ///< did *it* release BUSY
    bool own_clean = true;          ///< did *our* refresh release BUSY
    bool triggered = false;
    bool last_completed_clean = false;
    int trigger_calls = 0;
    int drain_calls = 0;
    std::vector<std::string> order;

    RenderHandshakeHooks Hooks(bool wire_panel_busy) {
        RenderHandshakeHooks h;
        h.drain = [this]() {
            ++drain_calls;
            order.push_back("drain");
            return 0;   // the in-flight refresh has nothing queued yet
        };
        h.trigger = [this]() {
            ++trigger_calls;
            triggered = true;
            order.push_back("trigger");
        };
        h.wait = [this](uint32_t) {
            order.push_back("wait");
            if (foreign_running) {
                if (!foreign_signals) return false;
                foreign_running = false;
                last_completed_clean = foreign_clean;
                return true;
            }
            if (!triggered) return false;
            last_completed_clean = own_clean;
            return true;
        };
        h.panel_ok = [this]() {
            order.push_back("panel_ok");
            return last_completed_clean;
        };
        if (wire_panel_busy) {
            h.panel_busy = [this]() {
                order.push_back("busy?");
                return foreign_running;
            };
        }
        return h;
    }
};

}  // namespace

/**
 * The defect, stated as a test: with no way to ask whether the panel is already
 * busy, the wait is satisfied by the other refresh's signal within milliseconds
 * of the trigger, and panel_ok answers for that refresh too. The handshake then
 * reports a clean completion for a frame the panel has not drawn — which is how
 * a device comes to publish a displayed digest for an image nobody can see.
 */
static void test_a_busy_panel_lends_its_completion_to_the_wrong_frame() {
    RenderHandshake hs;
    BusyPanel panel;
    panel.foreign_clean = true;
    panel.own_clean = false;      // ours would have failed, had it been waited on

    CHECK(hs.Run(panel.Hooks(/*wire_panel_busy=*/false), 5000) ==
          AckResult::kCompleted);
    CHECK(hs.completions() == 1);
    // The verdict came from the other refresh: ours never got a wait of its own.
    CHECK(panel.order == std::vector<std::string>({"drain", "trigger", "wait",
                                                   "panel_ok"}));
}

/// Rule 1a with the hook wired: the stranger's refresh is waited out first, and
/// the verdict is then about our frame rather than about theirs.
static void test_handshake_waits_for_a_refresh_already_in_flight() {
    RenderHandshake hs;
    BusyPanel panel;
    panel.foreign_clean = true;
    panel.own_clean = false;

    CHECK(hs.Run(panel.Hooks(/*wire_panel_busy=*/true), 5000) ==
          AckResult::kPanelFailed);
    CHECK(hs.foreign_refreshes_awaited() == 1);
    CHECK(hs.completions() == 0);
    CHECK(hs.panel_failures() == 1);
    CHECK(panel.trigger_calls == 1);
    // Drain, find the panel busy, let it finish, drain again, and only then
    // trigger. The second drain matters: the foreign refresh may have posted
    // more than the one signal we consumed.
    CHECK(panel.order == std::vector<std::string>({"drain", "busy?", "wait",
                                                   "drain", "trigger", "wait",
                                                   "panel_ok"}));
    CHECK(panel.drain_calls == 2);
}

/// An idle panel is not made slower by the rule: no extra wait, no extra drain.
static void test_an_idle_panel_triggers_immediately() {
    RenderHandshake hs;
    BusyPanel panel;
    panel.foreign_running = false;

    CHECK(hs.Run(panel.Hooks(/*wire_panel_busy=*/true), 5000) ==
          AckResult::kCompleted);
    CHECK(hs.foreign_refreshes_awaited() == 0);
    CHECK(panel.drain_calls == 1);
    CHECK(panel.order == std::vector<std::string>({"drain", "busy?", "trigger",
                                                   "wait", "panel_ok"}));
}

/// A refresh that is in flight and never comes back must not be waited on for
/// ever, and must not cause a second frame to be pushed at a wedged panel.
static void test_a_wedged_foreign_refresh_times_out_without_triggering() {
    RenderHandshake hs;
    BusyPanel panel;
    panel.foreign_signals = false;

    CHECK(hs.Run(panel.Hooks(/*wire_panel_busy=*/true), 1500) ==
          AckResult::kTimedOut);
    CHECK(hs.timeouts() == 1);
    CHECK(panel.trigger_calls == 0);
    // Released, so the next frame still gets an attempt.
    CHECK(!hs.in_progress());
}

/**
 * The two halves joined up: a timeout is a failed render, and a failed render
 * is what the status route now has to report. Before this, the coordinator
 * recorded nothing and the wake cycle called the whole wake "unchanged".
 */
static void test_a_timed_out_handshake_is_recorded_as_a_failed_render() {
    RenderHandshake hs;
    RefreshCoordinator rc;
    BusyPanel panel;
    panel.foreign_signals = false;

    uint8_t b[kShaBytes];
    ShaOf(5, b);
    CHECK(rc.Request(b, 10, false) == RenderDisposition::kStarted);
    const AckResult ack = hs.Run(panel.Hooks(/*wire_panel_busy=*/true), 1500);
    CHECK(ack == AckResult::kTimedOut);
    const bool ok = (ack == AckResult::kCompleted);
    rc.CompleteRender(ok ? b : nullptr, 10,
                      ok ? RenderOutcome::kDrawn : RenderOutcome::kFailed);

    CHECK(rc.failed_count() == 1);
    CHECK(rc.last_render_failed());
    CHECK(rc.last_failed_seq() == 10);
    CHECK(!rc.has_displayed());
    CHECK(rc.state() == RefreshState::kIdle);
}

// ------------------------------------------------------------ integration --

namespace {

/// Minimal in-memory SlotIo, mirroring the device store for integration tests.
class MemIo : public SlotIo {
public:
    std::vector<uint8_t> slot[kSlotCount];
    bool present[kSlotCount] = {false, false};

    int ReadSlot(int s, uint8_t* buf, size_t max) override {
        if (!present[s]) return 0;
        const size_t n = slot[s].size() < max ? slot[s].size() : max;
        std::memcpy(buf, slot[s].data(), n);
        return static_cast<int>(n);
    }
    bool WriteSlot(int s, const uint8_t* data, size_t len) override {
        slot[s].assign(data, data + len);
        present[s] = true;
        return true;
    }
};

std::vector<uint8_t> Frame(uint32_t seed) {
    std::vector<uint8_t> v(kFrameBytes);
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < kFrameBytes; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = static_cast<uint8_t>((s >> 24) & 0xff);
    }
    return v;
}

}  // namespace

/**
 * The claim under test: when frames arrive faster than the panel can refresh,
 * the device ends up displaying the *newest* frame, and displays it exactly
 * once, rather than replaying every intermediate frame in order.
 */
static void test_burst_of_pushes_lands_on_newest_frame() {
    MemIo io;
    std::vector<uint8_t> store_scratch(kRecordBytes);
    DashboardSlot store(&io, store_scratch.data(), store_scratch.size());
    RefreshCoordinator rc;
    store.Load();

    std::vector<std::vector<uint8_t>> frames;
    for (uint32_t i = 1; i <= 6; ++i) frames.push_back(Frame(i * 1000));

    int renders = 0;
    std::vector<uint8_t> painted;

    // Frame 1 arrives and starts rendering.
    CHECK(store.Store(frames[0].data(), kFrameBytes, 1) == StoreResult::kOk);
    CHECK(rc.Request(store.active_sha(), store.active_seq(), false) == RenderDisposition::kStarted);

    // Frames 2..6 all arrive while that refresh is still on the glass.
    for (size_t i = 1; i < frames.size(); ++i) {
        CHECK(store.Store(frames[i].data(), kFrameBytes, static_cast<uint32_t>(i + 1)) ==
              StoreResult::kOk);
        const RenderDisposition d = rc.Request(store.active_sha(), store.active_seq(), false);
        CHECK(d == (i == 1 ? RenderDisposition::kQueued : RenderDisposition::kCoalesced));
    }

    // The first refresh completes, painting frame 1.
    painted = frames[0];
    ++renders;
    bool again = rc.CompleteRender(store.slot_status(0).sha, 1, RenderOutcome::kDrawn);
    // Rather than trusting the coordinator's bookkeeping, paint whatever the
    // store actually holds now, which is the whole point of depth-1 coalescing.
    while (again) {
        std::vector<uint8_t> current(kFrameBytes);
        CHECK(store.ReadFrame(current.data()));
        painted = current;
        ++renders;
        again = rc.CompleteRender(store.active_sha(), store.active_seq(), RenderOutcome::kDrawn);
    }

    // Two physical refreshes for six frames, and the last one is frame 6.
    CHECK(renders == 2);
    CHECK(painted == frames.back());
    CHECK(rc.state() == RefreshState::kIdle);
    CHECK(!rc.pending());
    CHECK(rc.displayed_seq() == 6);

    // A further push of the same bytes now changes nothing at all.
    CHECK(store.Store(frames.back().data(), kFrameBytes, 99) == StoreResult::kDuplicate);
    CHECK(rc.Request(store.active_sha(), store.active_seq(), false) == RenderDisposition::kSkipped);
}

/// Storage must not grow with traffic: a long burst still occupies two slots.
static void test_repeated_pushes_keep_storage_bounded() {
    MemIo io;
    std::vector<uint8_t> store_scratch(kRecordBytes);
    DashboardSlot store(&io, store_scratch.data(), store_scratch.size());
    store.Load();
    for (uint32_t i = 1; i <= 200; ++i) {
        CHECK(store.Store(Frame(i).data(), kFrameBytes, i) == StoreResult::kOk);
    }
    CHECK(io.slot[0].size() == kRecordBytes);
    CHECK(io.slot[1].size() == kRecordBytes);
    CHECK(store.active_seq() == 200);
}

// ---------------------------------------------------------- MutationGate --
//
// WHY THIS IS A CLASS AND NOT A BOOL
// ----------------------------------
// The rule — one mutating write at a time, refused rather than queued — was
// enforced by reading a `volatile bool` and then writing it. Two operations,
// with a window between them, on a dual-core part where the HTTP task and the
// task storing a locally composed frame both write frames. Two writers passing
// that check land inside the A/B store's write-inactive-then-swap sequence
// together.

static void test_the_gate_admits_exactly_one_holder() {
    dashboard::MutationGate gate;
    CHECK(!gate.busy());
    CHECK(gate.TryEnter());
    CHECK(gate.busy());
    CHECK(!gate.TryEnter());
    gate.Leave();
    CHECK(!gate.busy());
    CHECK(gate.TryEnter());
    gate.Leave();
}

static void test_the_claim_releases_on_every_path_out() {
    dashboard::MutationGate gate;
    {
        dashboard::MutationClaim claim(gate);
        CHECK(claim.entered());
        dashboard::MutationClaim second(gate);
        CHECK(!second.entered());
        // A refused claim must not release the gate when it goes out of scope.
    }
    CHECK(!gate.busy());
    {
        dashboard::MutationClaim claim(gate);
        CHECK(claim.entered());
    }
    CHECK(!gate.busy());
}

/**
 * The race, run for real. Eight threads hammer the gate; the count of
 * successful claims must equal the count of releases, and at no instant may two
 * holders be inside at once. A plain bool fails this under a sanitizer build,
 * which is the build this suite uses.
 */
static void test_two_threads_cannot_both_be_inside_the_gate() {
    dashboard::MutationGate gate;
    std::atomic<int> inside{0};
    std::atomic<int> admitted{0};
    std::atomic<int> refused{0};
    std::atomic<bool> overlap{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            for (int n = 0; n < 4000; ++n) {
                dashboard::MutationClaim claim(gate);
                if (!claim.entered()) {
                    refused.fetch_add(1);
                    continue;
                }
                admitted.fetch_add(1);
                if (inside.fetch_add(1) != 0) overlap.store(true);
                // A slice of "work" with the gate held.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (inside.fetch_sub(1) != 1) overlap.store(true);
            }
        });
    }
    for (std::thread& th : threads) th.join();

    CHECK(!overlap.load());
    CHECK(inside.load() == 0);
    CHECK(admitted.load() > 0);
    CHECK(admitted.load() + refused.load() == 8 * 4000);
}

// ------------------------------------------------------ displayed origin --

static void test_the_displayed_frame_is_the_stored_one_only_when_both_match() {
    const char* a = "aa00";
    const char* b = "bb11";
    // The ordinary steady state: stored, displayed, same frame.
    CHECK(dashboard::DisplayedFrameIsStoredFrame(true, 7, a, true, 7, a));
    // The store moved on: a push landed and the panel has not caught up.
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(true, 8, b, true, 7, a));
    // Same sequence, different bytes. A rewritten record is a different frame.
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(true, 7, b, true, 7, a));
}

static void test_a_reboot_leaves_the_displayed_origin_unknowable() {
    // The case that makes a single origin field wrong. E-paper keeps its image
    // across a deep sleep; the coordinator that knows what was drawn does not.
    // The device holds a frame, has displayed nothing this boot, and the only
    // honest answer about the glass is "unknown".
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(true, 7, "aa00", false, 0, ""));
}

static void test_nothing_stored_and_nothing_displayed_match_nothing() {
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(false, 0, "", false, 0, ""));
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(false, 0, "", true, 0, ""));
    // Two empty digests are two absences, not a match.
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(true, 0, "", true, 0, ""));
    CHECK(!dashboard::DisplayedFrameIsStoredFrame(true, 0, nullptr, true, 0, "aa"));
}

int main() {
    std::printf("dashboard_service host tests (real firmware translation units)\n\n");

    RUN(test_the_gate_admits_exactly_one_holder);
    RUN(test_the_claim_releases_on_every_path_out);
    RUN(test_two_threads_cannot_both_be_inside_the_gate);
    RUN(test_the_displayed_frame_is_the_stored_one_only_when_both_match);
    RUN(test_a_reboot_leaves_the_displayed_origin_unknowable);
    RUN(test_nothing_stored_and_nothing_displayed_match_nothing);

    RUN(test_hex_roundtrip);
    RUN(test_hex_decode_rejects_malformed);

    RUN(test_unprovisioned_denies_everything);
    RUN(test_set_token_requires_exact_length);
    RUN(test_correct_and_incorrect_tokens);
    RUN(test_success_resets_failure_counter);
    RUN(test_lockout_after_repeated_failures);
    RUN(test_lockout_expires_after_window);
    RUN(test_backwards_clock_does_not_wedge_lockout);

    RUN(test_pairing_window_starts_closed);
    RUN(test_pairing_window_single_claim);
    RUN(test_pairing_window_expires);
    RUN(test_pairing_window_backwards_clock_expires);
    RUN(test_pairing_window_reopen_discards_previous);
    RUN(test_pairing_window_close_is_immediate);

    RUN(test_idempotency_records_and_recalls);
    RUN(test_idempotency_evicts_oldest);
    RUN(test_idempotency_ignores_degenerate_keys);
    RUN(test_idempotency_duplicate_record_does_not_consume_slot);

    RUN(test_coordinator_single_flight);
    RUN(test_coordinator_queues_then_coalesces);
    RUN(test_coordinator_completion_drains_pending);
    RUN(test_coordinator_skips_identical_frame);
    RUN(test_coordinator_force_overrides_skip);
    RUN(test_coordinator_failed_render_keeps_displayed_frame);
    RUN(test_coordinator_counts_failed_renders);
    RUN(test_coordinator_success_clears_last_failed_but_keeps_total);
    RUN(test_coordinator_counts_deferred_apart_from_failed);
    RUN(test_coordinator_counts_each_failure_in_a_coalesced_run);
    RUN(test_coordinator_has_nothing_displayed_initially);

    RUN(test_handshake_completes_cleanly);
    RUN(test_handshake_drains_stale_signals_before_triggering);
    RUN(test_handshake_times_out_rather_than_blocking_forever);
    RUN(test_handshake_rejects_signal_when_panel_reports_failure);
    RUN(test_handshake_refuses_overlapping_runs);
    RUN(test_handshake_requires_all_hooks);
    RUN(test_failed_handshake_does_not_update_displayed_frame);

    RUN(test_a_busy_panel_lends_its_completion_to_the_wrong_frame);
    RUN(test_handshake_waits_for_a_refresh_already_in_flight);
    RUN(test_an_idle_panel_triggers_immediately);
    RUN(test_a_wedged_foreign_refresh_times_out_without_triggering);
    RUN(test_a_timed_out_handshake_is_recorded_as_a_failed_render);

    RUN(test_burst_of_pushes_lands_on_newest_frame);
    RUN(test_repeated_pushes_keep_storage_bounded);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
