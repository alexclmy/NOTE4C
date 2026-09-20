/**
 * @file test_power_policy.cc
 * @brief Host tests for the hybrid low-power contract.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These compile main/common/power_policy.cc directly, so what is tested is the
 * translation unit the firmware links, not a model of it.
 *
 * The two claims worth testing hardest, because both are the kind that a
 * reviewer can only take on trust otherwise:
 *
 *  1. **The budget is a bound, not a hope.** A wake cycle cannot outlive
 *     total_ms no matter how the phases are sequenced or how many times the
 *     network is retried. The exhaustive walk below drives every ordering it
 *     can and asserts the invariant after each step.
 *
 *  2. **An uncalibrated battery renders as null.** The rendered bytes are
 *     searched for digits in the battery fields, so a future edit that
 *     substitutes a plausible-looking number fails here rather than on a
 *     dashboard.
 */

#include "common/power_policy.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace power;

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
            printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n",         \
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

constexpr int64_t kMinute = 60 * 1000;

/// Does @p json contain @p key followed by exactly `null`?
bool FieldIsNull(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":null";
    return json.find(needle) != std::string::npos;
}

/// Extract the raw token after "key": up to the next , or } — so a test can
/// assert on what is actually on the wire rather than on a re-parse.
std::string FieldToken(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    const size_t start = at + needle.size();
    size_t end = start;
    while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
    return json.substr(start, end - start);
}

}  // namespace

// ------------------------------------------------------------------ modes --

static void test_mode_names_round_trip() {
    const Mode modes[] = {Mode::kAutoSaver, Mode::kInteractive, Mode::kAlwaysOn};
    for (Mode m : modes) {
        Mode back = Mode::kAlwaysOn;
        CHECK(ParseMode(ModeName(m), &back));
        CHECK(back == m);
    }
}

static void test_unknown_mode_names_are_refused() {
    Mode out = Mode::kAutoSaver;
    CHECK(!ParseMode("", &out));
    CHECK(!ParseMode("AUTO_SAVER", &out));      // case matters on the wire
    CHECK(!ParseMode("auto", &out));
    CHECK(!ParseMode("sleep", &out));
    CHECK(!ParseMode(nullptr, &out));
    // A refused parse must not have written anything.
    CHECK(out == Mode::kAutoSaver);
}

static void test_interactive_minutes_is_a_closed_list() {
    CHECK(IsValidInteractiveMinutes(5));
    CHECK(IsValidInteractiveMinutes(15));
    CHECK(IsValidInteractiveMinutes(30));
    CHECK(IsValidInteractiveMinutes(60));
    // Everything else, including plausible-looking neighbours.
    CHECK(!IsValidInteractiveMinutes(0));
    CHECK(!IsValidInteractiveMinutes(1));
    CHECK(!IsValidInteractiveMinutes(10));
    CHECK(!IsValidInteractiveMinutes(45));
    CHECK(!IsValidInteractiveMinutes(120));
    CHECK(!IsValidInteractiveMinutes(-5));
}

static void test_wake_interval_bounds() {
    CHECK(IsValidWakeInterval(kMinWakeIntervalMin));
    CHECK(IsValidWakeInterval(60));
    CHECK(IsValidWakeInterval(kMaxWakeIntervalMin));
    // Below the floor the mode stops saving anything, so it is refused rather
    // than quietly accepted.
    CHECK(!IsValidWakeInterval(kMinWakeIntervalMin - 1));
    CHECK(!IsValidWakeInterval(0));
    CHECK(!IsValidWakeInterval(-1));
    CHECK(!IsValidWakeInterval(kMaxWakeIntervalMin + 1));
}

// ------------------------------------------------------ wake classification --
//
// The mapping used to live in an anonymous namespace inside application.cc,
// wrapped around esp_sleep_get_wakeup_causes(), where no host could reach it.
// The rule it encodes is the one that decides whether a boot buys a
// fifteen-minute interactive window, so it is worth more than a comment.

static void test_the_button_wins_a_wake_it_shares_with_the_timer() {
    // A person pressing BOOT in the same millisecond the hourly timer fires
    // wants a usable device, not one that goes straight back to sleep in their
    // hand. ext0 is checked first, deliberately.
    DeepSleepCauses both;
    both.ext0 = true;
    both.timer = true;
    CHECK(ClassifyWake(true, both, ResetClass::kOther) == WakeReason::kButton);

    DeepSleepCauses ext0_only;
    ext0_only.ext0 = true;
    CHECK(ClassifyWake(true, ext0_only, ResetClass::kOther) == WakeReason::kButton);
}

static void test_a_timer_wake_is_a_timer_wake() {
    DeepSleepCauses timer;
    timer.timer = true;
    CHECK(ClassifyWake(true, timer, ResetClass::kHuman) == WakeReason::kTimer);
    // Even a human reset class cannot turn a timer wake into a person: the
    // reset reason is only consulted when deep sleep reported no cause at all.
    CHECK(!IsUserInitiated(ClassifyWake(true, timer, ResetClass::kHuman)));
}

static void test_the_alarm_line_is_reported_rather_than_folded_into_button() {
    DeepSleepCauses ext1;
    ext1.ext1 = true;
    CHECK(ClassifyWake(true, ext1, ResetClass::kOther) == WakeReason::kRtcAlarm);
}

static void test_a_deep_sleep_wake_with_no_known_cause_is_other() {
    CHECK(ClassifyWake(true, DeepSleepCauses{}, ResetClass::kHuman) ==
          WakeReason::kOther);
}

static void test_only_the_two_human_resets_open_a_window() {
    // Power-on and the external reset line are the two a person can cause.
    CHECK(ClassifyWake(false, DeepSleepCauses{}, ResetClass::kHuman) ==
          WakeReason::kPowerOn);
    CHECK(IsUserInitiated(ClassifyWake(false, DeepSleepCauses{}, ResetClass::kHuman)));
}

static void test_a_reset_loop_does_not_buy_fifteen_minute_windows() {
    // A USB/JTAG reset, a watchdog, a brownout or a panic restart: nobody is
    // standing there. Reporting these as kPowerOn meant a device stuck in a
    // brownout loop came back awake for fifteen minutes on every crash, which
    // on a flat cell is exactly the wrong thing to do.
    const WakeReason r = ClassifyWake(false, DeepSleepCauses{}, ResetClass::kOther);
    CHECK(r == WakeReason::kOther);
    CHECK(!IsUserInitiated(r));

    // And the causes struct is ignored when this was not a deep-sleep wake:
    // stale cause bits must not promote a crash into a button press.
    DeepSleepCauses stale;
    stale.ext0 = true;
    stale.timer = true;
    CHECK(ClassifyWake(false, stale, ResetClass::kOther) == WakeReason::kOther);
}

// ------------------------------------------------------ serving power save --

/**
 * THE ASYMMETRY THAT MADE THE LAN SERVER UNREACHABLE.
 *
 * The AP provisioning path has always called esp_wifi_set_ps(WIFI_PS_NONE)
 * before serving. The LAN path never did, so the station served HTTP from
 * modem sleep with a ten-beacon listen interval — associated, addressed, and
 * unable to complete an inbound TCP handshake. This is the rule, in the one
 * place a host can read it back.
 */
static void test_a_device_that_is_serving_http_does_not_doze() {
    CHECK(ServingWifiPowerSave(true) == WifiPowerSave::kNone);
}

static void test_a_device_that_is_not_serving_keeps_modem_sleep() {
    // Deep sleep, not modem sleep, is this product's power story. Outside a
    // serving window the driver default is the right answer and is restored.
    CHECK(ServingWifiPowerSave(false) == WifiPowerSave::kMinModem);
}

// ---------------------------------------------------------------- battery --

static void test_a_good_reading_is_usable() {
    const BatteryReading r = EvaluateBattery(true, true, 3900, 57);
    CHECK(r.present);
    CHECK(r.calibrated);
    CHECK(r.plausible);
    CHECK(r.usable());
    CHECK_EQ_INT(r.millivolts, 3900);
    CHECK_EQ_INT(r.percent, 57);
}

static void test_an_uncalibrated_reading_is_never_usable() {
    // The chip had no factory calibration. There are counts but no volts, so
    // there is no percentage either, however reasonable the number looks.
    const BatteryReading r = EvaluateBattery(true, false, 3900, 57);
    CHECK(r.present);
    CHECK(!r.calibrated);
    CHECK(!r.usable());
}

static void test_an_implausible_voltage_is_never_usable() {
    // A floating divider with no cell fitted, and a reading far above one cell.
    CHECK(!EvaluateBattery(true, true, 1200, 0).usable());
    CHECK(!EvaluateBattery(true, true, 5000, 100).usable());
    // The edges of the window are inside it.
    CHECK(EvaluateBattery(true, true, kBatteryMinPlausibleMv, 0).usable());
    CHECK(EvaluateBattery(true, true, kBatteryMaxPlausibleMv, 100).usable());
    CHECK(!EvaluateBattery(true, true, kBatteryMinPlausibleMv - 1, 0).usable());
    CHECK(!EvaluateBattery(true, true, kBatteryMaxPlausibleMv + 1, 100).usable());
}

static void test_a_failed_read_is_never_usable() {
    CHECK(!EvaluateBattery(false, true, 3900, 57).usable());
    // Zero millivolts means the driver returned nothing, not a flat cell.
    CHECK(!EvaluateBattery(true, true, 0, 0).present);
}

static void test_a_percentage_over_100_is_clamped() {
    CHECK_EQ_INT(EvaluateBattery(true, true, 4200, 200).percent, 100);
}

// ----------------------------------------------------------- wake budget --

static void test_an_unstarted_budget_grants_nothing() {
    WakeBudget b;
    CHECK(!b.started());
    CHECK_EQ_INT(b.TotalRemainingMs(0), 0);
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kNetwork, 0), 0);
    // Not exhausted either: it never started, which is a different state from
    // having run out, and the caller treats them differently.
    CHECK(!b.Exhausted(0));
}

static void test_a_phase_never_gets_more_than_the_total_has_left() {
    WakeBudgetLimits limits;
    limits.total_ms = 10000;
    limits.network_ms = 45000;  // deliberately larger than the whole budget
    WakeBudget b;
    b.Start(1000, limits);

    // At the start, the phase cap is larger than the total, so the total wins.
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kNetwork, 1000), 10000);
    // Nine seconds in, one second is left, and that is all any phase may have.
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kNetwork, 10000), 1000);
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kRender, 10000), 1000);
}

static void test_a_phase_cap_bounds_a_phase_that_could_afford_more() {
    WakeBudgetLimits limits;
    limits.total_ms = 120000;
    limits.settle_ms = 8000;
    WakeBudget b;
    b.Start(0, limits);
    // Plenty of total left, so the phase's own cap is the binding constraint.
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kSettle, 0), 8000);
}

/**
 * THE MISREADING THAT COST A WHOLE FETCH PHASE.
 *
 * RemainingFor() answers "how big is this phase's slice", and the wake cycle
 * was using it to answer "how much of this phase is left". Thirty seconds into
 * a forty-second fetch phase it reported forty thousand, and a fetch started on
 * the strength of that ran ten seconds past the phase's own cap.
 *
 * RemainingInPhase() is the one that subtracts the time the phase has spent,
 * and these two tests are the difference, side by side.
 */
static void test_remaining_for_is_the_phase_cap_and_not_what_is_left_of_it() {
    WakeBudgetLimits limits;   // defaults: 165 s total, 90 s fetch
    WakeBudget b;
    b.Start(0, limits);
    // Thirty seconds into a ninety-second phase that began at zero. The phase
    // cap is the answer to "how big is this slice", not "how much is left".
    CHECK_EQ_INT(b.RemainingFor(WakePhase::kFetch, 30000), 90000);
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 30000), 60000);
}

static void test_remaining_in_phase_is_bounded_by_the_total_as_well() {
    WakeBudgetLimits limits;
    limits.total_ms = 12000;
    limits.fetch_ms = 40000;
    WakeBudget b;
    b.Start(0, limits);
    // The phase has 38 s of its own cap left, and the total has 2 s.
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 2000), 10000);
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 11000), 1000);
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 12000), 0);
    // A phase that has run past its own cap gets nothing, rather than wrapping.
    // The default fetch cap is now 90 s.
    b.Start(0, WakeBudgetLimits{});
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 90000), 0);
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 0, 150000), 0);
    // A clock that went backwards reads as no time spent, not as a huge budget.
    CHECK_EQ_INT(b.RemainingInPhase(WakePhase::kFetch, 5000, 0), 90000);
    // And an unstarted budget grants nothing at all.
    WakeBudget fresh;
    CHECK_EQ_INT(fresh.RemainingInPhase(WakePhase::kFetch, 0, 0), 0);
}

/**
 * THE RENDEZVOUS RELIABILITY BOUND.
 *
 * The tower's scheduler pulses every 30 s. For a pushed frame to reliably land
 * inside a timer wake, the fetch-phase rendezvous — the awake window the door
 * is held open for a push — must be comfortably longer than one pulse. It was
 * 40 s and delivery was a coin flip; it is now 90 s. The total must also cover
 * that rendezvous plus the render+settle reserve, and no single phase may
 * exceed the total (the whole point of the budget).
 */
static void test_the_rendezvous_window_outlasts_the_tower_pulse() {
    const WakeBudgetLimits d;  // production defaults
    // The awake tower-wait window is the fetch phase cap.
    CHECK_EQ_INT(d.fetch_ms, 90000);
    // Comfortably longer than the 30 s tower pulse, and in the 75-90 s target.
    CHECK(d.fetch_ms >= 75000);
    CHECK(d.fetch_ms > 30000 * 2);
    // The total accommodates rendezvous + render + settle without clipping.
    CHECK_EQ_INT(d.total_ms, 165000);
    CHECK(d.total_ms >= d.fetch_ms + d.render_ms + d.settle_ms);
    // No single phase may exceed the total — the budget's core invariant.
    for (int i = 0; i < static_cast<int>(WakePhase::kCount); ++i) {
        const WakePhase p = static_cast<WakePhase>(i);
        CHECK(d.PhaseCap(p) <= d.total_ms);
    }
    // WorkRemainingMs stays above the compose floor long enough for an 80 s
    // rendezvous to be honoured when the network came up quickly (~5 s).
    WakeBudget b;
    b.Start(0, d);
    CHECK(b.WorkRemainingMs(5000 + 80000) >= 3000);
}

/**
 * The render is part of the 120 seconds, so content work is allocated against
 * the total minus what the panel may still need. A frame composed at 118 s is a
 * frame the panel starts drawing after the cycle should have ended.
 */
static void test_work_remaining_reserves_the_render_and_the_settle() {
    WakeBudgetLimits limits;    // 165 s total, 45 s render, 8 s settle
    WakeBudget b;
    b.Start(0, limits);
    CHECK_EQ_INT(b.WorkRemainingMs(0), 165000 - 45000 - 8000);  // 112000
    CHECK_EQ_INT(b.WorkRemainingMs(30000), 112000 - 30000);
    // It reaches zero well before the total does, which is the whole point.
    CHECK_EQ_INT(b.WorkRemainingMs(112000), 0);
    CHECK_EQ_INT(b.WorkRemainingMs(130000), 0);
    CHECK(!b.Exhausted(130000));
    WakeBudget fresh;
    CHECK_EQ_INT(fresh.WorkRemainingMs(0), 0);
}

static void test_the_budget_is_exhausted_exactly_at_the_total() {
    WakeBudgetLimits limits;
    limits.total_ms = 5000;
    WakeBudget b;
    b.Start(100, limits);
    CHECK(!b.Exhausted(100 + 4999));
    CHECK(b.Exhausted(100 + 5000));
    CHECK(b.Exhausted(100 + 500000));
    CHECK_EQ_INT(b.TotalRemainingMs(100 + 5000), 0);
}

static void test_a_backwards_clock_does_not_extend_the_budget() {
    // A monotonic clock should not go backwards, but if it does the safe
    // reading is "no time has passed", not "an enormous budget remains".
    WakeBudgetLimits limits;
    limits.total_ms = 5000;
    WakeBudget b;
    b.Start(10000, limits);
    CHECK_EQ_INT(b.TotalRemainingMs(9000), 5000);
    CHECK(!b.Exhausted(9000));
}

/**
 * The headline invariant. Walk every phase ordering the glue could produce,
 * spending each phase's full grant, and assert that the wall clock from Start()
 * to exhaustion never exceeds the total.
 */
static void test_no_sequence_of_phases_can_outlive_the_total() {
    WakeBudgetLimits limits;
    limits.total_ms = 165000;
    limits.network_ms = 45000;
    limits.fetch_ms = 90000;
    limits.render_ms = 45000;
    limits.settle_ms = 8000;

    const WakePhase order[] = {WakePhase::kNetwork, WakePhase::kFetch,
                               WakePhase::kRender, WakePhase::kSettle};

    // Every ordering of the four phases, each repeated enough times to blow
    // through the total if the caps ever added up instead of sharing.
    std::vector<int> idx = {0, 1, 2, 3};
    int permutations = 0;
    do {
        ++permutations;
        WakeBudget b;
        const int64_t start = 5000;
        b.Start(start, limits);
        int64_t now = start;
        for (int round = 0; round < 6; ++round) {
            for (int i : idx) {
                const WakePhase phase = order[i];
                const uint32_t grant = b.RemainingFor(phase, now);
                // The invariant, checked before every single grant.
                CHECK(now - start + static_cast<int64_t>(grant) <=
                      static_cast<int64_t>(limits.total_ms));
                if (grant == 0) {
                    b.NoteExhaustedIn(phase);
                    continue;
                }
                now += grant;  // the phase blocks for everything it was given
            }
        }
        CHECK(b.Exhausted(now));
        CHECK(now - start <= static_cast<int64_t>(limits.total_ms));
    } while (std::next_permutation(idx.begin(), idx.end()));
    CHECK_EQ_INT(permutations, 24);
}

static void test_the_phase_that_ran_out_is_the_one_reported() {
    WakeBudget b;
    WakeBudgetLimits limits;
    b.Start(0, limits);
    CHECK(!b.has_exhausted_phase());
    b.NoteExhaustedIn(WakePhase::kFetch);
    CHECK(b.has_exhausted_phase());
    CHECK(b.exhausted_phase() == WakePhase::kFetch);
    // A later phase asking does not overwrite the first, honest answer.
    b.NoteExhaustedIn(WakePhase::kSettle);
    CHECK(b.exhausted_phase() == WakePhase::kFetch);
}

static void test_reset_returns_the_budget_to_unstarted() {
    WakeBudget b;
    b.Start(0, WakeBudgetLimits{});
    b.NoteExhaustedIn(WakePhase::kRender);
    b.Reset();
    CHECK(!b.started());
    CHECK(!b.has_exhausted_phase());
}

static void test_phase_names_are_total() {
    CHECK_STR(WakePhaseName(WakePhase::kNetwork), "network");
    CHECK_STR(WakePhaseName(WakePhase::kFetch), "fetch");
    CHECK_STR(WakePhaseName(WakePhase::kRender), "render");
    CHECK_STR(WakePhaseName(WakePhase::kSettle), "settle");
}

// ------------------------------------------------------ network recovery --

static void test_the_first_attempt_waits_for_nothing() {
    NetworkRecovery r;
    uint32_t delay = 999;
    CHECK(r.ShouldRetry(120000, &delay));
    CHECK_EQ_INT(delay, 0);
}

static void test_backoff_doubles_and_then_stops_doubling() {
    NetworkRecovery r;
    CHECK_EQ_INT(r.BackoffMs(), 0);
    r.NoteAttempt();
    CHECK_EQ_INT(r.BackoffMs(), NetworkRecovery::kBaseBackoffMs);
    r.NoteAttempt();
    CHECK_EQ_INT(r.BackoffMs(), NetworkRecovery::kBaseBackoffMs * 2);
    r.NoteAttempt();
    CHECK_EQ_INT(r.BackoffMs(), NetworkRecovery::kBaseBackoffMs * 4);
    // Capped, and the cap holds however many attempts are recorded.
    for (int i = 0; i < 20; ++i) r.NoteAttempt();
    CHECK(r.BackoffMs() <= NetworkRecovery::kMaxBackoffMs);
}

static void test_retries_stop_at_the_attempt_ceiling() {
    NetworkRecovery r;
    uint32_t delay = 0;
    for (uint32_t i = 0; i < NetworkRecovery::kMaxAttempts; ++i) {
        CHECK(r.ShouldRetry(10u * 60u * 1000u, &delay));
        r.NoteAttempt();
    }
    // Budget is enormous and it still stops: the ceiling is its own bound.
    CHECK(!r.ShouldRetry(10u * 60u * 1000u, &delay));
}

static void test_the_budget_overrules_a_permitted_retry() {
    NetworkRecovery r;
    r.NoteAttempt();  // next backoff is kBaseBackoffMs
    uint32_t delay = 0;
    const uint32_t needed =
        NetworkRecovery::kBaseBackoffMs + NetworkRecovery::kMinUsefulAttemptMs;
    CHECK(r.ShouldRetry(needed, &delay));
    // One millisecond short of being able to back off and still try usefully.
    CHECK(!r.ShouldRetry(needed - 1, &delay));
    CHECK(!r.ShouldRetry(0, &delay));
}

static void test_a_refused_retry_does_not_write_a_delay() {
    NetworkRecovery r;
    uint32_t delay = 0xABCDEF;
    CHECK(!r.ShouldRetry(0, &delay));
    CHECK_EQ_INT(delay, 0xABCDEF);
}

/**
 * The combination that matters: retrying inside a real budget can never make
 * the cycle outlive the budget, because every backoff is checked against it.
 */
static void test_retrying_never_outlives_the_budget() {
    WakeBudgetLimits limits;
    limits.total_ms = 60000;
    WakeBudget b;
    b.Start(0, limits);
    NetworkRecovery r;

    int64_t now = 0;
    uint32_t delay = 0;
    int loops = 0;
    while (r.ShouldRetry(b.TotalRemainingMs(now), &delay) && loops < 100) {
        ++loops;
        now += delay;
        CHECK(now <= static_cast<int64_t>(limits.total_ms));
        const uint32_t grant = b.RemainingFor(WakePhase::kNetwork, now);
        now += grant;
        CHECK(now <= static_cast<int64_t>(limits.total_ms));
        r.NoteAttempt();
    }
    CHECK(loops > 0);              // it did actually retry
    CHECK(loops <= (int)NetworkRecovery::kMaxAttempts);
    CHECK(now <= static_cast<int64_t>(limits.total_ms));
}

// ------------------------------------------------------------ retry delay --

static void test_a_successful_cycle_waits_the_full_interval() {
    CHECK_EQ_INT(RetryDelayMs(0, 60), 60u * 60u * 1000u);
    CHECK_EQ_INT(RetryDelayMs(0, 15), 15u * 60u * 1000u);
}

static void test_the_first_failure_retries_sooner_than_the_interval() {
    const uint32_t d = RetryDelayMs(1, 60);
    CHECK_EQ_INT(d, kFirstRetryMs);
    CHECK(d < 60u * 60u * 1000u);
}

static void test_repeated_failures_back_off_but_never_past_the_interval() {
    uint32_t previous = 0;
    for (uint32_t f = 1; f <= 12; ++f) {
        const uint32_t d = RetryDelayMs(f, 60);
        CHECK(d >= previous);               // monotonic
        CHECK(d <= 60u * 60u * 1000u);      // never longer than the interval
        previous = d;
    }
    // A device whose network is genuinely gone settles on the normal cadence
    // rather than waking every ten minutes all day.
    CHECK_EQ_INT(RetryDelayMs(12, 60), 60u * 60u * 1000u);
}

static void test_a_short_interval_clamps_the_retry_immediately() {
    // With a 15 minute interval the first retry would be 10 minutes, which is
    // shorter, so it is used; but it can never exceed the interval.
    CHECK(RetryDelayMs(1, 15) <= 15u * 60u * 1000u);
    CHECK(RetryDelayMs(5, 15) <= 15u * 60u * 1000u);
}

static void test_an_out_of_range_interval_falls_back_to_the_default() {
    CHECK_EQ_INT(RetryDelayMs(0, 0),
                 static_cast<uint32_t>(kDefaultWakeIntervalMin) * 60u * 1000u);
    CHECK_EQ_INT(RetryDelayMs(0, 99999),
                 static_cast<uint32_t>(kDefaultWakeIntervalMin) * 60u * 1000u);
}

// ---------------------------------------------------------- wake planning --

static void test_auto_saver_sleeps_for_the_interval() {
    WakeInputs in;
    in.mode = Mode::kAutoSaver;
    in.wake_interval_min = 60;
    in.last_outcome = CycleOutcome::kUpdated;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK_EQ_INT(p.wake_in_ms, 60u * 60u * 1000u);
    CHECK(p.timer_armed);
    CHECK(p.button_wakes);
    CHECK_STR(p.reason, "scheduled_wake");
}

static void test_an_unchanged_frame_still_counts_as_a_good_cycle() {
    // Nothing was drawn, but the device did its job: it asked, and the answer
    // was "no change". Treating that as a failure would retry all day.
    WakeInputs in;
    in.last_outcome = CycleOutcome::kUnchanged;
    const WakePlan p = PlanNextWake(in);
    CHECK_STR(p.reason, "scheduled_wake");
    CHECK_EQ_INT(p.wake_in_ms,
                 static_cast<uint32_t>(kDefaultWakeIntervalMin) * 60u * 1000u);
}

static void test_always_on_never_sleeps() {
    WakeInputs in;
    in.mode = Mode::kAlwaysOn;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kStayAwake);
    CHECK(!p.timer_armed);
    CHECK_STR(p.reason, "always_on");
}

static void test_an_open_interactive_window_stays_awake() {
    WakeInputs in;
    in.mode = Mode::kInteractive;
    in.interactive_remaining_ms = 5000;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kStayAwake);
    CHECK_STR(p.reason, "interactive_window_open");
}

static void test_an_expired_interactive_window_sleeps_like_the_saver() {
    WakeInputs in;
    in.mode = Mode::kInteractive;
    in.interactive_remaining_ms = 0;   // it ran out
    in.wake_interval_min = 60;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK_EQ_INT(p.wake_in_ms, 60u * 60u * 1000u);
}

static void test_a_refresh_in_flight_beats_every_mode() {
    // Sleeping through a panel refresh leaves a half-drawn image that e-paper
    // holds until the next one, so this outranks the mode.
    WakeInputs in;
    in.mode = Mode::kAutoSaver;
    in.refresh_in_flight = true;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kStayAwake);
    CHECK_STR(p.reason, "refresh_in_flight");
}

static void test_the_provisioning_portal_beats_the_saver() {
    WakeInputs in;
    in.provisioning_portal_open = true;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kStayAwake);
    CHECK_STR(p.reason, "provisioning_portal_open");
}

static void test_a_running_slideshow_beats_the_saver() {
    WakeInputs in;
    in.slideshow_active = true;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kStayAwake);
    CHECK_STR(p.reason, "slideshow_active");
}

static void test_charging_changes_the_reason_but_not_the_schedule() {
    WakeInputs in;
    in.wake_interval_min = 60;
    in.charging = true;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK_EQ_INT(p.wake_in_ms, 60u * 60u * 1000u);
    CHECK_STR(p.reason, "scheduled_wake_charging");
}

static void test_a_failed_cycle_schedules_a_shorter_retry() {
    WakeInputs in;
    in.wake_interval_min = 60;
    in.last_outcome = CycleOutcome::kNetworkFailed;
    in.consecutive_failures = 1;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK(p.wake_in_ms < 60u * 60u * 1000u);
    CHECK_STR(p.reason, "retry_after_network_failure");
}

static void test_an_exhausted_budget_says_so_rather_than_blaming_the_network() {
    WakeInputs in;
    in.last_outcome = CycleOutcome::kBudgetExhausted;
    in.consecutive_failures = 1;
    const WakePlan p = PlanNextWake(in);
    CHECK_STR(p.reason, "retry_after_budget_exhausted");
}

/**
 * A refresh that started and did not reach the glass is a failed wake.
 *
 * It used to be reported as kUnchanged, which CycleSucceeded() calls a success:
 * the failure counter was reset and the next attempt was scheduled a whole wake
 * interval away. A frame pushed by the operator and refused by the painter then
 * sat in flash for an hour while the panel showed something else.
 */
static void test_a_refused_refresh_is_a_failure_and_earns_a_retry() {
    CHECK(!CycleSucceeded(CycleOutcome::kRenderFailed));
    CHECK_STR(CycleOutcomeName(CycleOutcome::kRenderFailed), "render_failed");

    WakeInputs in;
    in.wake_interval_min = 60;
    in.last_outcome = CycleOutcome::kRenderFailed;
    in.consecutive_failures = 1;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK(p.timer_armed);
    CHECK(p.wake_in_ms > 0);
    CHECK(p.wake_in_ms < 60u * 60u * 1000u);
    CHECK_EQ_INT(p.wake_in_ms, kFirstRetryMs);
    // Named for what happened. "retry_after_network_failure" on a device whose
    // network was fine would send the operator looking at the router.
    CHECK_STR(p.reason, "retry_after_render_failure");
}

/// The three failure outcomes must stay distinguishable on the wire: each one
/// sends whoever reads the status route somewhere different.
static void test_the_three_failure_reasons_are_not_the_same_string() {
    WakeInputs in;
    in.consecutive_failures = 1;

    in.last_outcome = CycleOutcome::kNetworkFailed;
    const std::string net = PlanNextWake(in).reason;
    in.last_outcome = CycleOutcome::kBudgetExhausted;
    const std::string budget = PlanNextWake(in).reason;
    in.last_outcome = CycleOutcome::kRenderFailed;
    const std::string render = PlanNextWake(in).reason;

    CHECK(net != budget);
    CHECK(budget != render);
    CHECK(net != render);
}

static void test_a_failure_always_schedules_a_wake_even_at_zero_count() {
    // The glue may report the failure before it has incremented the counter.
    // That must still produce a timer, not a device that never comes back.
    WakeInputs in;
    in.last_outcome = CycleOutcome::kNetworkFailed;
    in.consecutive_failures = 0;
    const WakePlan p = PlanNextWake(in);
    CHECK(p.action == WakeAction::kSleep);
    CHECK(p.wake_in_ms > 0);
    CHECK(p.timer_armed);
}

static void test_every_sleep_plan_arms_a_timer_and_keeps_the_button() {
    // The honest pair: a sleeping device is unreachable over Wi-Fi, so the
    // button is always a wake source and the timer is the only other one.
    const CycleOutcome outcomes[] = {
        CycleOutcome::kUpdated, CycleOutcome::kUnchanged,
        CycleOutcome::kNetworkFailed, CycleOutcome::kBudgetExhausted,
        CycleOutcome::kRenderFailed};
    for (CycleOutcome o : outcomes) {
        for (uint32_t f = 0; f < 5; ++f) {
            WakeInputs in;
            in.last_outcome = o;
            in.consecutive_failures = f;
            const WakePlan p = PlanNextWake(in);
            if (p.action != WakeAction::kSleep) continue;
            CHECK(p.timer_armed);
            CHECK(p.button_wakes);
            CHECK(p.wake_in_ms > 0);
        }
    }
}

// ------------------------------------------------------ interactive window --

static void test_a_window_counts_down_and_closes() {
    InteractiveWindow w;
    w.Open(0, 15);
    CHECK(w.IsOpen(0));
    CHECK_EQ_INT(w.RemainingMs(0), 15 * kMinute);
    CHECK_EQ_INT(w.RemainingMs(5 * kMinute), 10 * kMinute);
    CHECK(!w.IsOpen(15 * kMinute));
    CHECK_EQ_INT(w.RemainingMs(15 * kMinute), 0);
    CHECK(!w.IsOpen(99 * kMinute));
}

static void test_a_window_with_an_invalid_length_never_opens() {
    InteractiveWindow w;
    w.Open(0, 7);
    CHECK(!w.IsOpen(0));
    CHECK_EQ_INT(w.RemainingMs(0), 0);
}

static void test_reopening_replaces_the_previous_window() {
    InteractiveWindow w;
    w.Open(0, 5);
    w.Open(4 * kMinute, 30);
    CHECK_EQ_INT(w.RemainingMs(4 * kMinute), 30 * kMinute);
    CHECK(w.IsOpen(20 * kMinute));
}

static void test_closing_a_window_takes_effect_now() {
    InteractiveWindow w;
    w.Open(0, 60);
    w.Close();
    CHECK(!w.IsOpen(0));
}

// --------------------------------------------------------------- the state --

static void test_a_timer_wake_comes_back_in_the_saver() {
    // Nobody is standing there. Opening a window would defeat the wake.
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.effective_mode(0) == Mode::kAutoSaver);
    CHECK(s.desired_mode() == Mode::kAutoSaver);
    CHECK(s.ack_state(0) == ModeAckState::kAcknowledged);
    CHECK_EQ_INT(s.interactive_remaining_ms(0), 0);
}

static void test_a_button_wake_opens_an_interactive_window() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kButton);
    CHECK(s.effective_mode(0) == Mode::kInteractive);
    CHECK_EQ_INT(s.interactive_remaining_ms(0), 15 * kMinute);
    // And it ends on its own.
    CHECK(s.effective_mode(15 * kMinute) == Mode::kAutoSaver);
}

static void test_a_power_on_opens_a_window_too() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 30, 0, WakeReason::kPowerOn);
    CHECK(s.effective_mode(0) == Mode::kInteractive);
    CHECK_EQ_INT(s.interactive_remaining_ms(0), 30 * kMinute);
}

static void test_always_on_survives_a_reboot_and_opens_no_window() {
    PowerState s;
    s.Init(Mode::kAlwaysOn, 60, 15, 0, WakeReason::kButton);
    CHECK(s.effective_mode(0) == Mode::kAlwaysOn);
    CHECK(s.effective_mode(99 * kMinute) == Mode::kAlwaysOn);
}

static void test_interactive_is_never_restored_from_storage() {
    // A window that survived a reboot would be a window nobody opened.
    PowerState s;
    s.Init(Mode::kInteractive, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.effective_mode(0) == Mode::kAutoSaver);
    CHECK(s.PersistableMode() == Mode::kAutoSaver);
}

static void test_interactive_is_never_written_to_storage() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.Request(Mode::kInteractive, 30, 0));
    CHECK(s.effective_mode(0) == Mode::kInteractive);
    // What goes to NVS is the mode to come back in, not the window.
    CHECK(s.PersistableMode() == Mode::kAutoSaver);
}

static void test_requesting_interactive_while_awake_is_acknowledged_at_once() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.Request(Mode::kInteractive, 60, 1000));
    CHECK(s.effective_mode(1000) == Mode::kInteractive);
    CHECK(s.desired_mode() == Mode::kInteractive);
    CHECK(s.ack_state(1000) == ModeAckState::kAcknowledged);
    CHECK_EQ_INT(s.interactive_remaining_ms(1000), 60 * kMinute);
}

static void test_an_invalid_window_length_is_refused_and_changes_nothing() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(!s.Request(Mode::kInteractive, 7, 0));
    CHECK(s.effective_mode(0) == Mode::kAutoSaver);
    CHECK(s.desired_mode() == Mode::kAutoSaver);
}

static void test_requesting_interactive_with_zero_uses_the_stored_length() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 30, 0, WakeReason::kTimer);
    CHECK(s.Request(Mode::kInteractive, 0, 0));
    CHECK_EQ_INT(s.interactive_remaining_ms(0), 30 * kMinute);
}

static void test_going_back_to_the_saver_closes_the_window_immediately() {
    // "Back to power saving now" has to mean now, or the tower's button does
    // nothing visible for up to an hour.
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 60, 0, WakeReason::kButton);
    CHECK(s.effective_mode(0) == Mode::kInteractive);
    CHECK(s.Request(Mode::kAutoSaver, 0, 1000));
    CHECK(s.effective_mode(1000) == Mode::kAutoSaver);
    CHECK_EQ_INT(s.interactive_remaining_ms(1000), 0);
}

static void test_always_on_overrides_an_open_window() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kButton);
    CHECK(s.Request(Mode::kAlwaysOn, 0, 1000));
    CHECK(s.effective_mode(1000) == Mode::kAlwaysOn);
    CHECK(s.effective_mode(999 * kMinute) == Mode::kAlwaysOn);
    CHECK(s.PersistableMode() == Mode::kAlwaysOn);
}

static void test_tick_reports_the_expiry_exactly_once() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 5, 0, WakeReason::kButton);
    CHECK(!s.Tick(1000));                 // still open
    CHECK(!s.Tick(4 * kMinute));          // still open
    CHECK(s.Tick(5 * kMinute));           // this is the moment it closed
    CHECK(!s.Tick(5 * kMinute + 1));      // and only once
    CHECK(!s.Tick(60 * kMinute));
}

static void test_after_expiry_the_desired_mode_follows_the_base() {
    // Otherwise the status route reports a permanent pending_wake for a
    // request that was served and has since run out.
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 5, 0, WakeReason::kButton);
    CHECK(s.desired_mode() == Mode::kInteractive);
    s.Tick(5 * kMinute);
    CHECK(s.desired_mode() == Mode::kAutoSaver);
    CHECK(s.ack_state(5 * kMinute) == ModeAckState::kAcknowledged);
}

static void test_the_wake_interval_is_bounds_checked() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.SetWakeInterval(120));
    CHECK_EQ_INT(s.wake_interval_min(), 120);
    CHECK(!s.SetWakeInterval(1));
    CHECK_EQ_INT(s.wake_interval_min(), 120);   // unchanged by the refusal
}

static void test_an_out_of_range_persisted_interval_falls_back() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 3, 15, 0, WakeReason::kTimer);
    CHECK_EQ_INT(s.wake_interval_min(), kDefaultWakeIntervalMin);
}

static void test_an_out_of_range_persisted_window_falls_back() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 7, 0, WakeReason::kTimer);
    CHECK_EQ_INT(s.interactive_minutes(), kDefaultInteractiveMinutes);
}

static void test_the_wake_reason_is_remembered_for_the_status_route() {
    PowerState s;
    s.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(s.wake_reason() == WakeReason::kTimer);
    CHECK_STR(WakeReasonName(s.wake_reason()), "timer");
}

// ------------------------------------------------------------------ JSON --

// ------------------------------------------ the window length, on its own --

/**
 * Changing how long the *next* interactive window will be is not a request to
 * be thrown out of the one you are standing in.
 *
 * Before SetInteractiveMinutes existed, the config field power.interactive_min
 * was routed through Request(), which closes the window. So a user who had a
 * thirty-minute window open and adjusted the length to sixty had their window
 * closed on the spot and the device went back to power saving under them.
 */
static void test_changing_the_window_length_leaves_an_open_window_alone() {
    PowerState state;
    state.Init(Mode::kAutoSaver, 60, 30, 0, WakeReason::kTimer);
    CHECK(state.Request(Mode::kInteractive, 30, 0));
    CHECK_EQ_INT(state.interactive_remaining_ms(60000), 29u * 60u * 1000u);

    CHECK(state.SetInteractiveMinutes(60));
    // The length the *next* window will use changed...
    CHECK_EQ_INT(state.interactive_minutes(), 60);
    // ...and the open one kept both its deadline and its existence.
    CHECK(state.effective_mode(60000) == Mode::kInteractive);
    CHECK_EQ_INT(state.interactive_remaining_ms(60000), 29u * 60u * 1000u);
}

static void test_the_new_window_length_applies_to_the_next_window() {
    PowerState state;
    state.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(state.SetInteractiveMinutes(60));
    // Zero means "the stored length", which is now the one just set.
    CHECK(state.Request(Mode::kInteractive, 0, 0));
    CHECK_EQ_INT(state.interactive_remaining_ms(0), 60u * 60u * 1000u);
}

static void test_an_invalid_window_length_is_refused_without_side_effects() {
    PowerState state;
    state.Init(Mode::kAutoSaver, 60, 15, 0, WakeReason::kTimer);
    CHECK(state.Request(Mode::kInteractive, 15, 0));
    CHECK(!state.SetInteractiveMinutes(7));
    CHECK_EQ_INT(state.interactive_minutes(), 15);
    // And emphatically did not close the window on the way to refusing.
    CHECK(state.effective_mode(1000) == Mode::kInteractive);
}

// ------------------------------------------------ the honest status fields --

/**
 * A device that has not finished a cycle has no last outcome, and saying
 * "updated" would tell the tower the last update succeeded on a device that
 * has never completed one.
 */
static void test_an_unfinished_cycle_reports_a_null_outcome() {
    PowerStatus st;
    st.last_outcome_known = false;
    st.last_outcome = CycleOutcome::kUpdated;  // the struct default, unreported

    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    const std::string json(buf);
    CHECK_STR(FieldToken(json, "last_outcome").c_str(), "null");
    // Belt and braces: the word must not appear anywhere in the object.
    CHECK(json.find("updated") == std::string::npos);
}

static void test_a_finished_cycle_reports_the_outcome_it_had() {
    PowerStatus st;
    st.last_outcome_known = true;
    st.last_outcome = CycleOutcome::kNetworkFailed;

    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    const std::string json(buf);
    CHECK_STR(FieldToken(json, "last_outcome").c_str(), "\"network_failed\"");
}

/**
 * power_policy.h promises the status route reports *which phase* ran out of
 * time, on the grounds that "we gave up waiting for DHCP" is a far better
 * field report than "we gave up". Until this field existed that promise had
 * nowhere to land.
 */
static void test_the_exhausted_phase_reaches_the_status_json() {
    PowerStatus st;
    st.budget_exhausted_phase = WakePhase::kNetwork;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    CHECK_STR(FieldToken(std::string(buf), "budget_exhausted_phase").c_str(),
              "\"network\"");
}

static void test_a_cycle_that_did_not_run_out_reports_no_phase() {
    PowerStatus st;
    st.budget_exhausted_phase = WakePhase::kCount;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    CHECK_STR(FieldToken(std::string(buf), "budget_exhausted_phase").c_str(),
              "null");
}

/**
 * The buffer constant is a promise to every caller that reserves it. Render
 * the largest object the struct can produce and check it fits with room left,
 * so a future field cannot silently push a real status into the truncation
 * path where RenderPowerJson returns 0 and the route emits null.
 */
static void test_the_rendered_power_json_fits_the_advertised_buffer() {
    PowerStatus st;
    st.effective = Mode::kInteractive;
    st.desired = Mode::kInteractive;
    st.ack = ModeAckState::kPendingWake;
    st.awake = true;
    st.sleep_intent = true;
    st.interactive_remaining_s = 4294967295u;
    st.wake_interval_min = 1440;
    st.timer_armed = true;
    st.next_wake_in_s = 4294967295u;
    st.next_wake_epoch = 4294967295u;
    st.last_wake_reason = WakeReason::kRtcAlarm;
    st.last_outcome_known = true;
    st.last_outcome = CycleOutcome::kBudgetExhausted;
    st.budget_exhausted_phase = WakePhase::kNetwork;
    st.consecutive_failures = 4294967295u;
    st.battery = EvaluateBattery(true, true, 4400, 100);
    st.charge_state = "no_battery";
    st.charging = true;

    char buf[kPowerJsonMax];
    const size_t n = RenderPowerJson(st, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(n < kPowerJsonMax);
    // Whole object, not a fragment.
    CHECK(buf[n - 1] == '}');
}

/// The new outcome has to reach the tower, or the device knows why the frame is
/// not on the glass and nobody else does.
static void test_a_refused_refresh_reaches_the_status_route() {
    PowerStatus st;
    st.last_outcome_known = true;
    st.last_outcome = CycleOutcome::kRenderFailed;

    char buf[kPowerJsonMax];
    const size_t n = RenderPowerJson(st, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(buf[n - 1] == '}');
    CHECK_STR(FieldToken(std::string(buf), "last_outcome").c_str(),
              "\"render_failed\"");
}

static void test_the_power_json_carries_the_honest_fields() {
    PowerStatus st;
    st.effective = Mode::kInteractive;
    st.desired = Mode::kInteractive;
    st.ack = ModeAckState::kAcknowledged;
    st.awake = true;
    st.sleep_intent = false;
    st.interactive_remaining_s = 842;
    st.wake_interval_min = 60;
    st.timer_armed = false;
    st.battery = EvaluateBattery(true, true, 3912, 57);
    st.charge_state = "charging";
    st.charging = true;

    char buf[kPowerJsonMax];
    const size_t n = RenderPowerJson(st, buf, sizeof(buf));
    CHECK(n > 0);
    const std::string json(buf);

    CHECK_STR(FieldToken(json, "mode").c_str(), "\"interactive\"");
    CHECK_STR(FieldToken(json, "desired_mode").c_str(), "\"interactive\"");
    CHECK_STR(FieldToken(json, "ack").c_str(), "\"acknowledged\"");
    CHECK_STR(FieldToken(json, "awake").c_str(), "true");
    CHECK_STR(FieldToken(json, "sleep_intent").c_str(), "false");
    CHECK_STR(FieldToken(json, "interactive_remaining_s").c_str(), "842");
    CHECK_STR(FieldToken(json, "mv").c_str(), "3912");
    CHECK_STR(FieldToken(json, "percent").c_str(), "57");
    CHECK_STR(FieldToken(json, "state").c_str(), "\"charging\"");
}

/**
 * The claim that matters most on this route: a battery the device cannot
 * honestly measure is reported as null, and there is no path that puts a
 * number in its place.
 */
static void test_an_unusable_battery_renders_null_and_not_a_number() {
    const BatteryReading bad[] = {
        EvaluateBattery(true, false, 3900, 57),   // no calibration
        EvaluateBattery(false, true, 3900, 57),   // read failed
        EvaluateBattery(true, true, 1000, 0),     // implausible low
        EvaluateBattery(true, true, 6000, 100),   // implausible high
    };
    for (const BatteryReading& r : bad) {
        PowerStatus st;
        st.battery = r;
        char buf[kPowerJsonMax];
        CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
        const std::string json(buf);
        CHECK(FieldIsNull(json, "mv"));
        CHECK(FieldIsNull(json, "percent"));
        // Belt and braces: the numbers must not appear anywhere as a value.
        CHECK(json.find("\"mv\":3900") == std::string::npos);
        CHECK(json.find("\"percent\":57") == std::string::npos);
    }
}

static void test_an_unset_clock_renders_a_null_next_wake_epoch() {
    // A wall-clock time derived from an unset clock is a fabricated timestamp,
    // and the tower would display it as a real one.
    PowerStatus st;
    st.next_wake_epoch = 0;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    CHECK(FieldIsNull(std::string(buf), "next_wake_epoch"));
}

static void test_a_set_clock_renders_the_epoch() {
    PowerStatus st;
    st.next_wake_epoch = 1757880000u;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    CHECK_STR(FieldToken(std::string(buf), "next_wake_epoch").c_str(),
              "1757880000");
}

static void test_a_sleeping_device_reports_sleep_intent() {
    PowerStatus st;
    st.effective = Mode::kAutoSaver;
    st.awake = false;
    st.sleep_intent = true;
    st.timer_armed = true;
    st.next_wake_in_s = 3600;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    const std::string json(buf);
    CHECK_STR(FieldToken(json, "sleep_intent").c_str(), "true");
    CHECK_STR(FieldToken(json, "timer_armed").c_str(), "true");
    CHECK_STR(FieldToken(json, "next_wake_in_s").c_str(), "3600");
}

static void test_a_pending_mode_is_visible_as_pending() {
    PowerStatus st;
    st.effective = Mode::kAutoSaver;
    st.desired = Mode::kInteractive;
    st.ack = ModeAckState::kPendingWake;
    char buf[kPowerJsonMax];
    CHECK(RenderPowerJson(st, buf, sizeof(buf)) > 0);
    const std::string json(buf);
    CHECK_STR(FieldToken(json, "mode").c_str(), "\"auto_saver\"");
    CHECK_STR(FieldToken(json, "desired_mode").c_str(), "\"interactive\"");
    CHECK_STR(FieldToken(json, "ack").c_str(), "\"pending_wake\"");
}

static void test_a_too_small_buffer_yields_nothing_not_a_fragment() {
    PowerStatus st;
    for (size_t n = 1; n < 200; ++n) {
        std::vector<char> buf(n, '\xEE');
        const size_t written = RenderPowerJson(st, buf.data(), n);
        if (written == 0) {
            // Empty, rather than a truncated JSON fragment a parser would
            // choke on halfway through.
            CHECK_EQ_INT(buf[0], '\0');
        } else {
            CHECK(written < n);
            CHECK_EQ_INT(buf[written], '\0');
        }
    }
}

static void test_render_refuses_a_null_buffer() {
    PowerStatus st;
    CHECK_EQ_INT(RenderPowerJson(st, nullptr, 100), 0);
    char buf[4];
    CHECK_EQ_INT(RenderPowerJson(st, buf, 0), 0);
}

static void test_every_mode_and_outcome_renders_a_non_empty_name() {
    // A name that came back "" would put `"mode":""` on the wire and the tower
    // would fail to parse it, which is a worse failure than it looks.
    const Mode modes[] = {Mode::kAutoSaver, Mode::kInteractive, Mode::kAlwaysOn};
    for (Mode m : modes) CHECK(strlen(ModeName(m)) > 0);
    const CycleOutcome outcomes[] = {
        CycleOutcome::kUpdated, CycleOutcome::kUnchanged,
        CycleOutcome::kNetworkFailed, CycleOutcome::kBudgetExhausted,
        CycleOutcome::kRenderFailed};
    for (CycleOutcome o : outcomes) CHECK(strlen(CycleOutcomeName(o)) > 0);
    const WakeReason reasons[] = {WakeReason::kPowerOn, WakeReason::kTimer,
                                  WakeReason::kButton, WakeReason::kRtcAlarm,
                                  WakeReason::kOther};
    for (WakeReason r : reasons) CHECK(strlen(WakeReasonName(r)) > 0);
}

// ------------------------------------------------------------------ main --

// ----------------------------------------------------- CycleAdvanceGate --
//
// WHY THIS EXISTS AT ALL
// ----------------------
// ServiceWakeCycle is called from two tasks. The Run() loop ticks it once a
// second; the wake-budget backstop calls the same function from the esp_timer
// task, which is the entire reason the backstop works when the Run() loop is
// starved by a long panel refresh.
//
// The gap that closed: "starved" and "busy" look identical from the backstop.
// A Run() loop blocked inside a bounded forecast fetch is *inside* the cycle
// holding no lock at all, and the backstop was free to enter behind it and run
// the compose, the store and the status record at the same time — against the
// very fields application.h documents as guarded by power_mutex_, on the paths
// that read them without taking it.
//
// These tests pin the two properties the fix depends on: exactly one holder,
// and refusal rather than waiting.

static void test_the_cycle_gate_admits_exactly_one_advance() {
    power::CycleAdvanceGate gate;
    CHECK(!gate.busy());
    CHECK(gate.TryEnter());
    CHECK(gate.busy());
    // The backstop, arriving behind a fetch that is still running. It must be
    // told no, and it must be told immediately.
    CHECK(!gate.TryEnter());
    gate.Leave();
    CHECK(!gate.busy());
    CHECK(gate.TryEnter());
    gate.Leave();
}

/**
 * ServiceWakeCycle has more than a dozen early returns. A release done by hand
 * would eventually miss one, and a gate left claimed is worse than the race it
 * replaced: every later tick refuses, and the cycle never reaches sleep.
 */
static void test_the_cycle_claim_releases_on_every_path_out() {
    power::CycleAdvanceGate gate;
    {
        power::CycleAdvanceClaim claim(gate);
        CHECK(claim.entered());
        power::CycleAdvanceClaim second(gate);
        CHECK(!second.entered());
        // A refused claim must not release a gate it never held.
    }
    CHECK(!gate.busy());
    {
        power::CycleAdvanceClaim claim(gate);
        CHECK(claim.entered());
    }
    CHECK(!gate.busy());
}

/**
 * The race itself, run for real under the sanitizers this suite builds with.
 * Two tasks in the firmware; eight threads here, because a bug that needs a
 * precise interleaving should be given every chance to happen.
 *
 * The invariant is the one the firmware depends on: never two advances inside
 * the cycle at the same instant. A plain bool checked and then set passes the
 * single-threaded tests above and fails this one.
 */
static void test_two_tasks_cannot_advance_the_cycle_at_once() {
    power::CycleAdvanceGate gate;
    std::atomic<int> inside{0};
    std::atomic<int> admitted{0};
    std::atomic<bool> overlap{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            for (int n = 0; n < 4000; ++n) {
                power::CycleAdvanceClaim claim(gate);
                if (!claim.entered()) continue;
                admitted.fetch_add(1, std::memory_order_relaxed);
                if (inside.fetch_add(1, std::memory_order_acq_rel) != 0) {
                    overlap.store(true, std::memory_order_release);
                }
                inside.fetch_sub(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK(!overlap.load(std::memory_order_acquire));
    // Refusal is the expected common case under contention, not an error, so
    // the only claim made about the count is that work actually happened.
    CHECK(admitted.load(std::memory_order_relaxed) > 0);
    CHECK(!gate.busy());
}

int main() {
    printf("\n== power policy ==\n");

    RUN(test_mode_names_round_trip);
    RUN(test_unknown_mode_names_are_refused);
    RUN(test_interactive_minutes_is_a_closed_list);
    RUN(test_wake_interval_bounds);

    RUN(test_the_button_wins_a_wake_it_shares_with_the_timer);
    RUN(test_a_timer_wake_is_a_timer_wake);
    RUN(test_the_alarm_line_is_reported_rather_than_folded_into_button);
    RUN(test_a_deep_sleep_wake_with_no_known_cause_is_other);
    RUN(test_only_the_two_human_resets_open_a_window);
    RUN(test_a_reset_loop_does_not_buy_fifteen_minute_windows);

    RUN(test_a_device_that_is_serving_http_does_not_doze);
    RUN(test_a_device_that_is_not_serving_keeps_modem_sleep);

    RUN(test_a_good_reading_is_usable);
    RUN(test_an_uncalibrated_reading_is_never_usable);
    RUN(test_an_implausible_voltage_is_never_usable);
    RUN(test_a_failed_read_is_never_usable);
    RUN(test_a_percentage_over_100_is_clamped);

    RUN(test_an_unstarted_budget_grants_nothing);
    RUN(test_a_phase_never_gets_more_than_the_total_has_left);
    RUN(test_a_phase_cap_bounds_a_phase_that_could_afford_more);
    RUN(test_remaining_for_is_the_phase_cap_and_not_what_is_left_of_it);
    RUN(test_remaining_in_phase_is_bounded_by_the_total_as_well);
    RUN(test_work_remaining_reserves_the_render_and_the_settle);
    RUN(test_the_rendezvous_window_outlasts_the_tower_pulse);
    RUN(test_the_budget_is_exhausted_exactly_at_the_total);
    RUN(test_a_backwards_clock_does_not_extend_the_budget);
    RUN(test_no_sequence_of_phases_can_outlive_the_total);
    RUN(test_the_phase_that_ran_out_is_the_one_reported);
    RUN(test_reset_returns_the_budget_to_unstarted);
    RUN(test_phase_names_are_total);

    RUN(test_the_first_attempt_waits_for_nothing);
    RUN(test_backoff_doubles_and_then_stops_doubling);
    RUN(test_retries_stop_at_the_attempt_ceiling);
    RUN(test_the_budget_overrules_a_permitted_retry);
    RUN(test_a_refused_retry_does_not_write_a_delay);
    RUN(test_retrying_never_outlives_the_budget);

    RUN(test_a_successful_cycle_waits_the_full_interval);
    RUN(test_the_first_failure_retries_sooner_than_the_interval);
    RUN(test_repeated_failures_back_off_but_never_past_the_interval);
    RUN(test_a_short_interval_clamps_the_retry_immediately);
    RUN(test_an_out_of_range_interval_falls_back_to_the_default);

    RUN(test_auto_saver_sleeps_for_the_interval);
    RUN(test_an_unchanged_frame_still_counts_as_a_good_cycle);
    RUN(test_always_on_never_sleeps);
    RUN(test_an_open_interactive_window_stays_awake);
    RUN(test_an_expired_interactive_window_sleeps_like_the_saver);
    RUN(test_a_refresh_in_flight_beats_every_mode);
    RUN(test_the_provisioning_portal_beats_the_saver);
    RUN(test_a_running_slideshow_beats_the_saver);
    RUN(test_charging_changes_the_reason_but_not_the_schedule);
    RUN(test_a_failed_cycle_schedules_a_shorter_retry);
    RUN(test_an_exhausted_budget_says_so_rather_than_blaming_the_network);
    RUN(test_a_refused_refresh_is_a_failure_and_earns_a_retry);
    RUN(test_the_three_failure_reasons_are_not_the_same_string);
    RUN(test_a_failure_always_schedules_a_wake_even_at_zero_count);
    RUN(test_every_sleep_plan_arms_a_timer_and_keeps_the_button);

    RUN(test_a_window_counts_down_and_closes);
    RUN(test_a_window_with_an_invalid_length_never_opens);
    RUN(test_reopening_replaces_the_previous_window);
    RUN(test_closing_a_window_takes_effect_now);

    RUN(test_a_timer_wake_comes_back_in_the_saver);
    RUN(test_a_button_wake_opens_an_interactive_window);
    RUN(test_a_power_on_opens_a_window_too);
    RUN(test_always_on_survives_a_reboot_and_opens_no_window);
    RUN(test_interactive_is_never_restored_from_storage);
    RUN(test_interactive_is_never_written_to_storage);
    RUN(test_requesting_interactive_while_awake_is_acknowledged_at_once);
    RUN(test_an_invalid_window_length_is_refused_and_changes_nothing);
    RUN(test_requesting_interactive_with_zero_uses_the_stored_length);
    RUN(test_going_back_to_the_saver_closes_the_window_immediately);
    RUN(test_always_on_overrides_an_open_window);
    RUN(test_tick_reports_the_expiry_exactly_once);
    RUN(test_after_expiry_the_desired_mode_follows_the_base);
    RUN(test_the_wake_interval_is_bounds_checked);
    RUN(test_an_out_of_range_persisted_interval_falls_back);
    RUN(test_an_out_of_range_persisted_window_falls_back);
    RUN(test_the_wake_reason_is_remembered_for_the_status_route);

    RUN(test_changing_the_window_length_leaves_an_open_window_alone);
    RUN(test_the_new_window_length_applies_to_the_next_window);
    RUN(test_an_invalid_window_length_is_refused_without_side_effects);

    RUN(test_an_unfinished_cycle_reports_a_null_outcome);
    RUN(test_a_finished_cycle_reports_the_outcome_it_had);
    RUN(test_the_exhausted_phase_reaches_the_status_json);
    RUN(test_a_cycle_that_did_not_run_out_reports_no_phase);
    RUN(test_the_rendered_power_json_fits_the_advertised_buffer);

    RUN(test_a_refused_refresh_reaches_the_status_route);
    RUN(test_the_power_json_carries_the_honest_fields);
    RUN(test_an_unusable_battery_renders_null_and_not_a_number);
    RUN(test_an_unset_clock_renders_a_null_next_wake_epoch);
    RUN(test_a_set_clock_renders_the_epoch);
    RUN(test_a_sleeping_device_reports_sleep_intent);
    RUN(test_a_pending_mode_is_visible_as_pending);
    RUN(test_a_too_small_buffer_yields_nothing_not_a_fragment);
    RUN(test_render_refuses_a_null_buffer);
    RUN(test_every_mode_and_outcome_renders_a_non_empty_name);

    RUN(test_the_cycle_gate_admits_exactly_one_advance);
    RUN(test_the_cycle_claim_releases_on_every_path_out);
    RUN(test_two_tasks_cannot_advance_the_cycle_at_once);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
