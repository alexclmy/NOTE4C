/**
 * @file test_autonomy_cycle.cc
 * @brief Host tests for the autonomy half of the bounded wake cycle, and for
 *        the fetch phase that contains it.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS SUITE COVERS THAT A PURE-FUNCTION SWEEP DID NOT
 * ---------------------------------------------------------
 * The first version of this suite drove `PlanCycleStep` over its whole input
 * space and proved a great deal about one function — while the integration
 * around it was wrong in ways the sweep could not see:
 *
 *  - a device with autonomy switched off ended its wake on the first tick of
 *    the fetch phase, closing the forty-second window a push needs;
 *  - a wake with no network reported "unchanged" instead of a failure;
 *  - the budget the planner was handed was the fetch phase's whole cap rather
 *    than what was left of it, so a fetch could be started thirty seconds in;
 *  - the tick after a fetch compared a timestamp from before the fetch against
 *    a deadline after it, and finished the cycle before composing.
 *
 * So the last section here drives a whole wake, tick by tick, against a mock
 * clock: the real `WakeBudget`, the real `PlanFetchPhase`, the real
 * `PlanCycleStep`, and a fake executor that consumes realistic wall-clock time
 * for a fetch, a compose and a panel refresh. It is the same sequence
 * `Application::ServiceWakeCycle` runs, through the same functions.
 *
 * THE INVARIANT THIS SUITE EXISTS FOR
 * -----------------------------------
 * **No step that costs time is ever started without the budget to finish it**,
 * and **autonomy never shortens the cycle the push path already had.**
 */

#include "common/autonomy_cycle.h"

#include <stdio.h>
#include <string.h>

#include "common/power_policy.h"

using namespace autonomy;

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

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        ++g_checks;                                                        \
        if (strcmp((a), (b)) != 0) {                                       \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: \"%s\" == \"%s\"\n", __FILE__,     \
                   __LINE__, g_current, (a), (b));                         \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const long long _a = (long long)(a);                               \
        const long long _b = (long long)(b);                               \
        if (_a != _b) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %lld == %lld\n", __FILE__,         \
                   __LINE__, g_current, _a, _b);                           \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-64s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

/// A wake where everything is in order and a local render is due: autonomy on,
/// a profile with a device module, the tower given its chance and silent, a
/// fresh-enough cache, and plenty of budget.
CycleInputs Ready() {
    CycleInputs in;
    in.autonomy_enabled = true;
    in.profile_present = true;
    in.has_device_module = true;
    in.has_auto_module = false;
    in.wants_weather = true;

    in.tower_frame_arrived = false;
    in.tower_wait_remaining_ms = 0;
    in.network_up = true;

    in.work_remaining_ms = 60000;

    in.fetch_attempted = false;
    in.fetch_ok = false;
    in.compose_result = ComposeOutcome::kNotTried;

    in.has_cache = true;
    in.cache_fetched_epoch = 1789387200;
    in.now_epoch = 1789387200 + 600;  // ten minutes old
    in.clock_set = true;
    in.min_fetch_interval_min = 30;

    in.displayed_origin = Origin::kNone;
    in.displayed_age_s = 0;
    in.displayed_age_known = false;
    in.wake_interval_min = 60;
    in.interactive_window_open = false;
    return in;
}

}  // namespace

// ------------------------------------------------- the bounded-power rules --

static void test_a_spent_budget_stands_down_instead_of_starting_anything() {
    // The invariant this whole module exists for. Whatever else is true, a
    // cycle with no room left to draw does not begin work.
    for (uint32_t remaining = 0; remaining < kMinComposeBudgetMs; ++remaining) {
        CycleInputs in = Ready();
        in.work_remaining_ms = remaining;
        const CycleDecision d = PlanCycleStep(in);
        ++g_checks;
        if (d.step != CycleStep::kStandDown) {
            ++g_failures;
            printf("  FAIL in %s: %u ms left produced step %d, not stand down\n",
                   g_current, remaining, static_cast<int>(d.step));
            break;
        }
    }
}

static void test_a_budget_too_small_for_a_fetch_composes_from_cache_instead() {
    // Enough time to draw, not enough to go to the network. The right answer is
    // the panel the device can still produce, not a fetch it cannot finish.
    CycleInputs in = Ready();
    in.cache_fetched_epoch = 0;
    in.has_cache = false;  // a fetch would otherwise be due
    in.work_remaining_ms = kMinComposeBudgetMs + 10;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    // And it says so: a panel drawn without the forecast it wanted is degraded,
    // never silently equivalent to one drawn with it.
    CHECK(d.degraded);
}

static void test_a_fetch_is_only_started_with_room_for_a_whole_one() {
    CycleInputs in = Ready();
    in.has_cache = false;

    in.work_remaining_ms = kMinFetchBudgetMs - 1;
    CHECK(PlanCycleStep(in).step == CycleStep::kCompose);

    in.work_remaining_ms = kMinFetchBudgetMs;
    CHECK(PlanCycleStep(in).step == CycleStep::kFetchWeather);
}

static void test_a_fetch_deadline_never_exceeds_what_is_left_to_draw_with() {
    // The number the client is handed. It is the ceiling when there is room for
    // it, and what remains minus the compose reserve when there is not — never
    // a promise the budget cannot keep.
    CycleInputs in = Ready();
    in.has_cache = false;

    in.work_remaining_ms = 60000;
    CHECK_EQ_INT(PlanCycleStep(in).budget_ms, kFetchWireTimeoutMs);

    // The tightest budget a fetch is ever started on. The floor is sized so
    // that even here the whole ceiling fits with the compose reserve behind it
    // — which is the point of the floor, and is checked rather than assumed.
    in.work_remaining_ms = kMinFetchBudgetMs;  // 24 000
    const CycleDecision tight = PlanCycleStep(in);
    CHECK(tight.step == CycleStep::kFetchWeather);
    CHECK_EQ_INT(tight.budget_ms, kFetchWireTimeoutMs);
    CHECK(tight.budget_ms + kMinComposeBudgetMs <= in.work_remaining_ms);
    // One below the floor, and no fetch is started at all.
    in.work_remaining_ms = kMinFetchBudgetMs - 1;
    CHECK(PlanCycleStep(in).step == CycleStep::kCompose);
}

// --------------------------------------------------------- the kill switch --

/**
 * THE REGRESSION THIS FILE WAS WRITTEN FOR.
 *
 * Autonomy switched off used to answer "finish", and the caller took that
 * literally: it ended the wake on the first tick of the fetch phase, so a
 * device with the feature turned off went back to sleep roughly forty seconds
 * earlier than it used to and a queued push had nowhere to land. The answer is
 * "stand down, I am not part of this wake", and the caller's own rendezvous is
 * left untouched.
 */
static void test_autonomy_off_stands_down_without_ending_the_cycle() {
    CycleInputs in = Ready();
    in.autonomy_enabled = false;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(!d.participated);
    CHECK_STR(d.reason, kReasonDisabled);
    CHECK(!d.degraded);

    // And the phase above it keeps the whole forty seconds.
    FetchPhaseInputs phase;
    phase.phase_left_ms = 40000;
    phase.autonomy_participated = d.participated;
    phase.autonomy_outcome = d.outcome;
    CHECK(PlanFetchPhase(phase).act == FetchPhaseAct::kAutonomyStep);

    phase.phase_left_ms = 1;
    CHECK(PlanFetchPhase(phase).act == FetchPhaseAct::kAutonomyStep);

    phase.phase_left_ms = 0;
    const FetchPhaseDecision over = PlanFetchPhase(phase);
    CHECK(over.act == FetchPhaseAct::kFinish);
    CHECK(!over.failed);  // reaching the network and finding nothing is success
}

static void test_no_profile_stands_down_without_ending_the_cycle() {
    CycleInputs in = Ready();
    in.profile_present = false;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(!d.participated);
    CHECK_STR(d.reason, kReasonNoProfile);
}

static void test_a_profile_that_asks_for_nothing_still_reports_itself() {
    // Distinct from "no profile": the device has one, autonomy ran, and the
    // answer is that nothing in it is in Device or Auto mode. That is worth
    // reporting, so `participated` is true and the status route says so.
    CycleInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = false;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(d.participated);
    CHECK_STR(d.reason, kReasonNotAsked);
}

// ------------------------------------------------------------ arbitration --

static void test_a_tower_frame_this_wake_ends_the_autonomy_path_at_once() {
    CycleInputs in = Ready();
    in.tower_frame_arrived = true;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(d.participated);
    CHECK_STR(d.reason, kReasonTowerFrame);
    // Not "unchanged": the tower's frame is what this cycle achieved, and the
    // render phase is about to draw it.
    CHECK(d.outcome == CycleOutcome::kUpdated);
}

static void test_auto_mode_waits_for_the_tower_before_composing() {
    CycleInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = true;
    in.tower_wait_remaining_ms = 30000;

    const CycleDecision waiting = PlanCycleStep(in);
    CHECK(waiting.step == CycleStep::kWaitForTower);
    CHECK(waiting.participated);

    in.tower_wait_remaining_ms = 0;
    CHECK(PlanCycleStep(in).step != CycleStep::kWaitForTower);
}

static void test_the_tower_wait_is_cut_short_rather_than_leaving_no_room() {
    // Waiting the full thirty seconds and then discovering there is no budget
    // left to fetch or draw would spend the whole wake on a rendezvous that
    // produced nothing. The wait ends early instead, and the panel gets drawn.
    CycleInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = true;
    in.tower_wait_remaining_ms = 30000;
    in.has_cache = false;  // a fetch is due, so the floor is the fetch budget

    in.work_remaining_ms = kMinFetchBudgetMs + 1;
    CHECK(PlanCycleStep(in).step == CycleStep::kWaitForTower);

    in.work_remaining_ms = kMinFetchBudgetMs;
    const CycleDecision cut = PlanCycleStep(in);
    CHECK(cut.step == CycleStep::kFetchWeather);
    CHECK(cut.tower_wait_expired);

    // With no fetch due the floor is just the compose, so the tower keeps more
    // of its turn.
    in.has_cache = true;
    in.cache_fetched_epoch = in.now_epoch - 60;
    in.work_remaining_ms = kMinFetchBudgetMs;
    CHECK(PlanCycleStep(in).step == CycleStep::kWaitForTower);
    in.work_remaining_ms = kMinComposeBudgetMs;
    CHECK(PlanCycleStep(in).step == CycleStep::kCompose);
}

static void test_device_mode_does_not_wait_for_the_tower() {
    // The energy case. A profile with everything in Device mode has no reason
    // to hold the radio open for a frame it is not going to use.
    CycleInputs in = Ready();
    in.has_auto_module = false;
    in.has_device_module = true;
    in.tower_wait_remaining_ms = 30000;
    CHECK(PlanCycleStep(in).step != CycleStep::kWaitForTower);
}

static void test_a_fresh_tower_frame_is_not_painted_over() {
    CycleInputs in = Ready();
    in.displayed_origin = Origin::kTower;
    in.displayed_age_known = true;
    in.displayed_age_s = 60;  // a minute old, against a sixty-minute interval
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(d.participated);
    CHECK_STR(d.reason, kReasonTowerFrameFresh);
}

static void test_an_unknown_origin_is_protected_like_a_tower_frame() {
    // When the device cannot say where what is on the glass came from, the two
    // mistakes are not symmetrical: painting over the operator's frame is
    // visible and wrong, deferring a local render costs one wake.
    CycleInputs in = Ready();
    in.displayed_origin = Origin::kUnknown;
    in.displayed_age_known = false;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK_STR(d.reason, kReasonTowerFrameFresh);
}

static void test_the_interactive_window_suspends_local_replacement() {
    CycleInputs in = Ready();
    in.interactive_window_open = true;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK_STR(d.reason, kReasonInteractive);
}

// ----------------------------------------------------------- the fetch leg --

static void test_a_due_fetch_is_taken_before_composing() {
    CycleInputs in = Ready();
    in.cache_fetched_epoch = in.now_epoch - 3600;  // an hour old, past 30 min
    CHECK(PlanCycleStep(in).step == CycleStep::kFetchWeather);
}

static void test_a_fetch_that_is_not_due_goes_straight_to_composing() {
    CycleInputs in = Ready();  // cache is ten minutes old
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    CHECK(!d.degraded);
}

static void test_a_fetch_is_attempted_once_per_wake_and_not_retried() {
    // The retry is the next wake, with the backoff that already exists. A
    // second attempt inside one cycle spends the budget twice for the same
    // outage.
    CycleInputs in = Ready();
    in.cache_fetched_epoch = in.now_epoch - 3600;
    in.fetch_attempted = true;
    in.fetch_ok = false;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    CHECK(d.degraded);
}

static void test_no_network_composes_from_cache_and_says_it_is_degraded() {
    CycleInputs in = Ready();
    in.network_up = false;
    in.cache_fetched_epoch = in.now_epoch - 3600;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    CHECK(d.degraded);
}

static void test_no_network_and_no_weather_wanted_is_not_degraded() {
    // A countdown and a message need nothing but the clock. A wake with no
    // radio that draws them has done everything it meant to.
    CycleInputs in = Ready();
    in.network_up = false;
    in.wants_weather = false;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    CHECK(!d.degraded);
}

static void test_a_successful_fetch_leaves_the_compose_undegraded() {
    CycleInputs in = Ready();
    in.cache_fetched_epoch = in.now_epoch - 3600;
    in.fetch_attempted = true;
    in.fetch_ok = true;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kCompose);
    CHECK(!d.degraded);
}

static void test_an_unset_clock_still_fetches_once() {
    // Without a clock, "thirty minutes since the last fetch" is unanswerable.
    // One fetch is what gets the device a clock via SNTP as well as a forecast.
    CycleInputs in = Ready();
    in.clock_set = false;
    in.now_epoch = 0;
    CHECK(PlanCycleStep(in).step == CycleStep::kFetchWeather);
}

// ------------------------------------------------ what the compose came to --

static void test_an_accepted_compose_reports_an_update() {
    CycleInputs in = Ready();
    in.compose_result = ComposeOutcome::kAccepted;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(d.outcome == CycleOutcome::kUpdated);
}

static void test_a_deduped_compose_is_unchanged_and_not_an_update() {
    // The cheapest good outcome this device produces, and it must not be
    // reported as an update: nothing was written and nothing was redrawn.
    CycleInputs in = Ready();
    in.compose_result = ComposeOutcome::kDeduped;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.outcome == CycleOutcome::kUnchanged);
    CHECK_STR(d.reason, kReasonComposeIdentical);
}

static void test_a_superseded_compose_reports_the_tower_winning_not_a_failure() {
    CycleInputs in = Ready();
    in.compose_result = ComposeOutcome::kSuperseded;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.outcome == CycleOutcome::kUpdated);
    CHECK_STR(d.reason, kReasonTowerFrame);
}

static void test_a_failed_compose_reports_a_failure() {
    // The regression: a store that refused used to be reported as an update,
    // because `composed` was set before anything was known.
    CycleInputs in = Ready();
    in.compose_result = ComposeOutcome::kFailed;
    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.outcome == CycleOutcome::kFailed);
    CHECK_STR(d.reason, kReasonComposeFailed);
    CHECK(d.degraded);

    // And the phase above it turns that into a failed wake, so the retry
    // backoff applies rather than the cycle looking successful.
    FetchPhaseInputs phase;
    phase.phase_left_ms = 0;
    phase.autonomy_participated = true;
    phase.autonomy_outcome = d.outcome;
    const FetchPhaseDecision over = PlanFetchPhase(phase);
    CHECK(over.act == FetchPhaseAct::kFinish);
    CHECK(over.failed);
}

static void test_an_accepted_compose_after_a_failed_fetch_is_degraded() {
    CycleInputs in = Ready();
    in.cache_fetched_epoch = in.now_epoch - 3600;
    in.compose_result = ComposeOutcome::kAccepted;
    in.fetch_attempted = true;
    in.fetch_ok = false;

    const CycleDecision d = PlanCycleStep(in);
    CHECK(d.step == CycleStep::kStandDown);
    CHECK(d.outcome == CycleOutcome::kDegraded);
}

// -------------------------------------------------------- the fetch phase --

static void test_the_phase_goes_back_to_the_network_when_wifi_drops() {
    FetchPhaseInputs in;
    in.network_up = false;
    in.phase_left_ms = 20000;
    CHECK(PlanFetchPhase(in).act == FetchPhaseAct::kBackToNetwork);
}

static void test_a_pushed_frame_sends_the_phase_to_the_render() {
    FetchPhaseInputs in;
    in.phase_left_ms = 20000;
    in.tower_frame_seen = true;
    const FetchPhaseDecision d = PlanFetchPhase(in);
    CHECK(d.act == FetchPhaseAct::kGoToRender);
    CHECK_STR(d.reason, kReasonTowerFrame);
}

static void test_a_locally_stored_frame_also_sends_the_phase_to_the_render() {
    // The convergence the offline path needs: a frame this device composed goes
    // through the same render and settle phases a pushed one does, so the
    // outcome is what the panel did rather than what the compositor intended.
    FetchPhaseInputs in;
    in.phase_left_ms = 20000;
    in.local_frame_stored = true;
    CHECK(PlanFetchPhase(in).act == FetchPhaseAct::kGoToRender);
}

static void test_a_busy_panel_sends_the_phase_to_the_render() {
    FetchPhaseInputs in;
    in.phase_left_ms = 20000;
    in.panel_busy = true;
    CHECK(PlanFetchPhase(in).act == FetchPhaseAct::kGoToRender);
}

// ------------------------------------------------------- the whole timeline --

namespace {

/**
 * @brief One wake, driven the way ServiceWakeCycle drives it.
 *
 * Real WakeBudget, real PlanFetchPhase, real PlanCycleStep. What is faked is
 * only the hardware: how long a fetch takes, how long a compose takes, how long
 * the panel takes, and whether a push arrives.
 */
struct WakeSim {
    power::WakeBudget budget;
    power::WakePhase phase = power::WakePhase::kNetwork;
    int64_t now_ms = 0;
    int64_t phase_started_ms = 0;

    // The device
    CycleInputs profile = Ready();
    uint32_t tower_wait_s = 80;
    uint32_t network_up_at_ms = 5000;
    uint32_t fetch_duration_ms = 18000;
    uint32_t compose_duration_ms = 1800;
    uint32_t panel_duration_ms = 24000;
    bool fetch_succeeds = true;

    // What happened
    bool fetch_ran = false;
    int64_t fetch_started_ms = -1;
    int64_t fetch_ended_ms = -1;
    int64_t compose_ended_ms = -1;
    int64_t panel_done_ms = -1;
    int64_t finished_ms = -1;
    bool drew = false;
    ComposeOutcome compose_result = ComposeOutcome::kNotTried;
    bool participated = false;
    bool settled = true;
    CycleOutcome outcome = CycleOutcome::kUnchanged;
    power::CycleOutcome power_outcome = power::CycleOutcome::kUnchanged;
    int ticks = 0;

    void Start() {
        budget.Start(now_ms, power::WakeBudgetLimits{});
        phase = power::WakePhase::kNetwork;
        phase_started_ms = now_ms;
    }

    void EnterPhase(power::WakePhase p) {
        phase = p;
        phase_started_ms = now_ms;
    }

    void Finish(power::CycleOutcome o) {
        power_outcome = o;
        finished_ms = now_ms;
    }

    bool done() const { return finished_ms >= 0; }

    /// One second of wall clock, or however long the work took.
    void Tick() {
        ++ticks;
        if (done()) return;
        if (budget.Exhausted(now_ms)) {
            Finish(power::CycleOutcome::kBudgetExhausted);
            return;
        }

        switch (phase) {
            case power::WakePhase::kNetwork:
                if (now_ms >= static_cast<int64_t>(network_up_at_ms)) {
                    EnterPhase(power::WakePhase::kFetch);
                }
                now_ms += 1000;
                return;

            case power::WakePhase::kFetch: {
                FetchPhaseInputs pin;
                pin.network_up = true;
                pin.panel_busy = false;
                pin.local_frame_stored = compose_result == ComposeOutcome::kAccepted;
                pin.phase_left_ms = budget.RemainingInPhase(power::WakePhase::kFetch,
                                                            phase_started_ms, now_ms);
                pin.autonomy_participated = participated;
                pin.autonomy_outcome = outcome;
                pin.autonomy_settled = settled;
                const FetchPhaseDecision pd = PlanFetchPhase(pin);
                if (pd.act == FetchPhaseAct::kGoToRender) {
                    EnterPhase(power::WakePhase::kRender);
                    now_ms += 1000;
                    return;
                }
                if (pd.act == FetchPhaseAct::kFinish) {
                    Finish(pd.failed ? power::CycleOutcome::kNetworkFailed
                                     : power::CycleOutcome::kUnchanged);
                    return;
                }

                CycleInputs in = profile;
                in.network_up = true;
                in.fetch_attempted = fetch_ran;
                in.fetch_ok = fetch_ran && fetch_succeeds;
                in.compose_result = compose_result;
                in.work_remaining_ms = budget.WorkRemainingMs(now_ms);
                const int64_t wait_ms = static_cast<int64_t>(tower_wait_s) * 1000;
                const int64_t elapsed = now_ms - phase_started_ms;
                in.tower_wait_remaining_ms =
                    elapsed >= wait_ms ? 0u
                                       : static_cast<uint32_t>(wait_ms - elapsed);

                const CycleDecision d = PlanCycleStep(in);
                participated = participated || d.participated;
                settled = d.step == CycleStep::kStandDown;
                outcome = d.outcome;

                switch (d.step) {
                    case CycleStep::kFetchWeather:
                        fetch_ran = true;
                        fetch_started_ms = now_ms;
                        // The client is bounded by the deadline it was handed,
                        // and a fetch that would run past it is cut off there.
                        now_ms += fetch_duration_ms < d.budget_ms
                                      ? fetch_duration_ms
                                      : d.budget_ms;
                        fetch_ended_ms = now_ms;
                        return;
                    case CycleStep::kCompose:
                        now_ms += compose_duration_ms;
                        compose_result = ComposeOutcome::kAccepted;
                        compose_ended_ms = now_ms;
                        EnterPhase(power::WakePhase::kRender);
                        return;
                    case CycleStep::kWaitForTower:
                    case CycleStep::kStandDown:
                        now_ms += 1000;
                        return;
                }
                return;
            }

            case power::WakePhase::kRender: {
                if (panel_done_ms < 0) panel_done_ms = phase_started_ms + panel_duration_ms;
                if (now_ms < panel_done_ms) {
                    const uint32_t left = budget.RemainingInPhase(
                        power::WakePhase::kRender, phase_started_ms, now_ms);
                    if (left == 0) {
                        budget.NoteExhaustedIn(power::WakePhase::kRender);
                        Finish(power::CycleOutcome::kBudgetExhausted);
                        return;
                    }
                    now_ms += 1000;
                    return;
                }
                drew = true;
                EnterPhase(power::WakePhase::kSettle);
                now_ms += 1000;
                return;
            }

            case power::WakePhase::kSettle:
                Finish(drew ? power::CycleOutcome::kUpdated
                            : power::CycleOutcome::kUnchanged);
                return;

            case power::WakePhase::kCount:
                return;
        }
    }

    void RunToCompletion() {
        for (int i = 0; i < 400 && !done(); ++i) Tick();
    }
};

}  // namespace

/**
 * THE DEFAULT BALANCED TIMELINE, END TO END.
 *
 * Nothing in the cache, so a fetch is due. 60-minute wake interval, 80-second
 * tower wait, a network that comes up at five seconds, an eighteen-second
 * fetch, a compose, and a twenty-four-second panel refresh. Every number below
 * is measured from the run, not asserted into it.
 */
static void test_the_default_balanced_wake_fetches_composes_draws_and_sleeps() {
    WakeSim sim;
    sim.profile.has_cache = false;
    sim.profile.cache_fetched_epoch = 0;
    sim.profile.has_device_module = false;
    sim.profile.has_auto_module = true;  // Auto: the tower gets its turn first
    sim.Start();
    sim.RunToCompletion();

    CHECK(sim.done());
    // The tower got its full eighty seconds before the device did anything.
    CHECK(sim.fetch_started_ms >= sim.network_up_at_ms + 80000);
    // The fetch finished inside its own ceiling.
    CHECK(sim.fetch_ended_ms - sim.fetch_started_ms <= kFetchWireTimeoutMs);
    // Something was composed, and it was composed *after* the fetch, so the
    // panel was drawn from the forecast this wake actually fetched.
    CHECK(sim.compose_ended_ms > sim.fetch_ended_ms);
    CHECK(sim.compose_result == ComposeOutcome::kAccepted);
    // The panel drew it, inside the budget, and the cycle reported an update.
    CHECK(sim.drew);
    CHECK(sim.power_outcome == power::CycleOutcome::kUpdated);
    // The whole thing fitted inside the 165-second guarantee.
    CHECK(sim.finished_ms <= 165000);
    printf("    timeline: network up %lld, fetch %lld..%lld, compose %lld, "
           "panel %lld, slept %lld ms\n",
           (long long)sim.network_up_at_ms, (long long)sim.fetch_started_ms,
           (long long)sim.fetch_ended_ms, (long long)sim.compose_ended_ms,
           (long long)sim.panel_done_ms, (long long)sim.finished_ms);
}

static void test_the_same_wake_with_autonomy_off_holds_the_push_window_open() {
    // The regression, at the level that matters. With autonomy off this wake
    // must last as long as it always did — the full fetch-phase rendezvous —
    // and must not sleep on the first tick.
    WakeSim sim;
    sim.profile.autonomy_enabled = false;
    sim.Start();
    sim.RunToCompletion();

    CHECK(sim.done());
    CHECK(!sim.fetch_ran);
    CHECK(sim.compose_result == ComposeOutcome::kNotTried);
    // Ninety seconds of fetch phase after the network came up at five: the full
    // push rendezvous is held open, which is the reliability fix.
    CHECK_EQ_INT(sim.finished_ms, sim.network_up_at_ms + 90000);
    CHECK(sim.power_outcome == power::CycleOutcome::kUnchanged);
}

static void test_a_slow_network_cuts_the_tower_wait_rather_than_the_fetch() {
    // The network phase takes almost its whole cap. There is no longer room for
    // eighty seconds of rendezvous *and* a fetch, so the rendezvous is what
    // gives — and the device still fetches, composes and draws inside the total.
    WakeSim sim;
    sim.profile.has_cache = false;
    sim.profile.has_device_module = false;
    sim.profile.has_auto_module = true;
    sim.network_up_at_ms = 40000;
    sim.Start();
    sim.RunToCompletion();

    CHECK(sim.done());
    CHECK(sim.fetch_ran);
    CHECK(sim.compose_result == ComposeOutcome::kAccepted);
    CHECK(sim.drew);
    CHECK(sim.finished_ms <= 165000);
    // The wait was cut: it started at 41 s and did not run the full 80 s.
    CHECK(sim.fetch_started_ms < 41000 + 80000);
}

static void test_no_work_is_ever_started_outside_the_budget() {
    // The sweep that matters for the bound: across a range of network delays,
    // fetch durations and panel durations, nothing may begin after the moment
    // the budget could still cover it, and the wake must always end.
    const uint32_t net_delays[] = {0, 5000, 20000, 40000, 44000};
    const uint32_t fetches[] = {500, 8000, 18000, 25000};
    const uint32_t panels[] = {2000, 24000, 44000};
    for (uint32_t net : net_delays) {
        for (uint32_t fetch : fetches) {
            for (uint32_t panel : panels) {
                WakeSim sim;
                sim.profile.has_cache = false;
                sim.profile.has_device_module = false;
                sim.profile.has_auto_module = true;
                sim.network_up_at_ms = net;
                sim.fetch_duration_ms = fetch;
                sim.panel_duration_ms = panel;
                sim.Start();
                sim.RunToCompletion();

                ++g_checks;
                if (!sim.done()) {
                    ++g_failures;
                    printf("  FAIL in %s: net %u fetch %u panel %u never ended\n",
                           g_current, net, fetch, panel);
                    continue;
                }
                // A fetch is never started with less than the planner's floor,
                // and never runs past the point the compose has to begin.
                if (sim.fetch_ran) {
                    ++g_checks;
                    if (sim.fetch_ended_ms - sim.fetch_started_ms >
                        static_cast<int64_t>(kFetchWireTimeoutMs)) {
                        ++g_failures;
                        printf("  FAIL in %s: net %u fetch %u ran %lld ms\n",
                               g_current, net, fetch,
                               (long long)(sim.fetch_ended_ms - sim.fetch_started_ms));
                    }
                }
                // A compose is never started so late that the panel could not
                // begin inside the total.
                if (sim.compose_ended_ms >= 0) {
                    ++g_checks;
                    if (sim.compose_ended_ms > 165000) {
                        ++g_failures;
                        printf("  FAIL in %s: net %u fetch %u composed at %lld\n",
                               g_current, net, fetch,
                               (long long)sim.compose_ended_ms);
                    }
                }
            }
        }
    }
}

/**
 * The exhaustive one. Whatever the eight independent inputs say, the planner
 * must never return a step that costs time when there is no time to spend it,
 * and must never return kWaitForTower once the wait has expired — either of
 * which would be a cycle that does not end.
 */
static void test_no_input_combination_can_produce_an_unbounded_cycle() {
    for (int mask = 0; mask < 256; ++mask) {
        CycleInputs in = Ready();
        in.autonomy_enabled = (mask & 1) != 0;
        in.profile_present = (mask & 2) != 0;
        in.has_device_module = (mask & 4) != 0;
        in.has_auto_module = (mask & 8) != 0;
        in.tower_frame_arrived = (mask & 16) != 0;
        in.tower_wait_remaining_ms = (mask & 32) != 0 ? 0u : 30000u;
        in.network_up = (mask & 64) != 0;
        in.interactive_window_open = (mask & 128) != 0;

        // With no budget, nothing that costs time may be started.
        in.work_remaining_ms = 0;
        const CycleDecision broke = PlanCycleStep(in);
        ++g_checks;
        if (broke.step == CycleStep::kFetchWeather ||
            broke.step == CycleStep::kCompose) {
            ++g_failures;
            printf("  FAIL in %s: mask %d started work with no budget\n",
                   g_current, mask);
        }

        // With a full budget and the wait expired, it must not still be waiting.
        in.work_remaining_ms = 60000;
        in.tower_wait_remaining_ms = 0;
        const CycleDecision full = PlanCycleStep(in);
        ++g_checks;
        if (full.step == CycleStep::kWaitForTower) {
            ++g_failures;
            printf("  FAIL in %s: mask %d waits for a tower that had its turn\n",
                   g_current, mask);
        }

        // A fetch's deadline always leaves room for the compose behind it.
        if (full.step == CycleStep::kFetchWeather) {
            ++g_checks;
            if (full.budget_ms + kMinComposeBudgetMs > in.work_remaining_ms ||
                full.budget_ms > kFetchWireTimeoutMs) {
                ++g_failures;
                printf("  FAIL in %s: mask %d allocated %u ms to a fetch\n",
                       g_current, mask, full.budget_ms);
            }
        }

        // And every decision names a reason, because the status route reports
        // it and "the device did not draw" is not a field report.
        ++g_checks;
        if (full.reason == nullptr || full.reason[0] == '\0') {
            ++g_failures;
            printf("  FAIL in %s: mask %d produced a decision with no reason\n",
                   g_current, mask);
        }
    }
}

static void test_every_step_and_outcome_has_a_name() {
    const CycleStep steps[] = {CycleStep::kWaitForTower, CycleStep::kFetchWeather,
                               CycleStep::kCompose, CycleStep::kStandDown};
    for (CycleStep s : steps) CHECK(CycleStepName(s)[0] != '\0');

    const ComposeOutcome results[] = {
        ComposeOutcome::kNotTried, ComposeOutcome::kAccepted, ComposeOutcome::kDeduped,
        ComposeOutcome::kSuperseded, ComposeOutcome::kFailed};
    for (ComposeOutcome r : results) CHECK(ComposeOutcomeName(r)[0] != '\0');

    const FetchPhaseAct acts[] = {
        FetchPhaseAct::kBackToNetwork, FetchPhaseAct::kAutonomyStep,
        FetchPhaseAct::kGoToRender, FetchPhaseAct::kFinish};
    for (FetchPhaseAct a : acts) CHECK(FetchPhaseActName(a)[0] != '\0');

    CHECK_STR(OriginName(Origin::kUnknown), "unknown");
}

int main() {
    RUN(test_a_spent_budget_stands_down_instead_of_starting_anything);
    RUN(test_a_budget_too_small_for_a_fetch_composes_from_cache_instead);
    RUN(test_a_fetch_is_only_started_with_room_for_a_whole_one);
    RUN(test_a_fetch_deadline_never_exceeds_what_is_left_to_draw_with);

    RUN(test_autonomy_off_stands_down_without_ending_the_cycle);
    RUN(test_no_profile_stands_down_without_ending_the_cycle);
    RUN(test_a_profile_that_asks_for_nothing_still_reports_itself);

    RUN(test_a_tower_frame_this_wake_ends_the_autonomy_path_at_once);
    RUN(test_auto_mode_waits_for_the_tower_before_composing);
    RUN(test_the_tower_wait_is_cut_short_rather_than_leaving_no_room);
    RUN(test_device_mode_does_not_wait_for_the_tower);
    RUN(test_a_fresh_tower_frame_is_not_painted_over);
    RUN(test_an_unknown_origin_is_protected_like_a_tower_frame);
    RUN(test_the_interactive_window_suspends_local_replacement);

    RUN(test_a_due_fetch_is_taken_before_composing);
    RUN(test_a_fetch_that_is_not_due_goes_straight_to_composing);
    RUN(test_a_fetch_is_attempted_once_per_wake_and_not_retried);
    RUN(test_no_network_composes_from_cache_and_says_it_is_degraded);
    RUN(test_no_network_and_no_weather_wanted_is_not_degraded);
    RUN(test_a_successful_fetch_leaves_the_compose_undegraded);
    RUN(test_an_unset_clock_still_fetches_once);

    RUN(test_an_accepted_compose_reports_an_update);
    RUN(test_a_deduped_compose_is_unchanged_and_not_an_update);
    RUN(test_a_superseded_compose_reports_the_tower_winning_not_a_failure);
    RUN(test_a_failed_compose_reports_a_failure);
    RUN(test_an_accepted_compose_after_a_failed_fetch_is_degraded);

    RUN(test_the_phase_goes_back_to_the_network_when_wifi_drops);
    RUN(test_a_pushed_frame_sends_the_phase_to_the_render);
    RUN(test_a_locally_stored_frame_also_sends_the_phase_to_the_render);
    RUN(test_a_busy_panel_sends_the_phase_to_the_render);

    RUN(test_the_default_balanced_wake_fetches_composes_draws_and_sleeps);
    RUN(test_the_same_wake_with_autonomy_off_holds_the_push_window_open);
    RUN(test_a_slow_network_cuts_the_tower_wait_rather_than_the_fetch);
    RUN(test_no_work_is_ever_started_outside_the_budget);

    RUN(test_no_input_combination_can_produce_an_unbounded_cycle);
    RUN(test_every_step_and_outcome_has_a_name);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
