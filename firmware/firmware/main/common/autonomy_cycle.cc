/**
 * @file autonomy_cycle.cc
 * @brief Implementation of the wake cycle's autonomy sequencing.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See autonomy_cycle.h for why this is a pure function, why a refusal is a
 * stand-down rather than an end to the cycle, and how the sub-phases inside the
 * fetch phase are allocated. No ESP-IDF headers: the host suite compiles this
 * translation unit and drives its whole input space and its whole timeline.
 */

#include "autonomy_cycle.h"

namespace autonomy {

const char* const kReasonBudgetSpent = "budget_spent";
const char* const kReasonComposeIdentical = "compose_identical";
const char* const kReasonComposeFailed = "compose_failed";

const char* CycleStepName(CycleStep step) {
    switch (step) {
        case CycleStep::kWaitForTower: return "wait_for_tower";
        case CycleStep::kFetchWeather: return "fetch_weather";
        case CycleStep::kCompose: return "compose";
        case CycleStep::kStandDown: return "stand_down";
    }
    return "";
}

const char* ComposeOutcomeName(ComposeOutcome result) {
    switch (result) {
        case ComposeOutcome::kNotTried:   return "not_tried";
        case ComposeOutcome::kAccepted:   return "accepted";
        case ComposeOutcome::kDeduped:    return "deduped";
        case ComposeOutcome::kSuperseded: return "superseded";
        case ComposeOutcome::kFailed:     return "failed";
    }
    return "";
}

namespace {

/// Would this wake's forecast be out of date enough to go and get another?
FetchVerdict WeatherVerdict(const CycleInputs& in) {
    FetchInputs f;
    f.wants_weather = in.wants_weather;
    f.has_cache = in.has_cache;
    f.cache_fetched_epoch = in.cache_fetched_epoch;
    f.now_epoch = in.now_epoch;
    f.clock_set = in.clock_set;
    f.min_fetch_interval_min = in.min_fetch_interval_min;
    return ShouldFetchWeather(f);
}

/// A forecast this wake still intends to go and get.
bool FetchStillWanted(const CycleInputs& in) {
    if (in.fetch_attempted || !in.network_up) return false;
    return WeatherVerdict(in).fetch;
}

/**
 * @brief Did this cycle fall short of what it set out to do?
 *
 * True when a forecast was wanted and this wake did not get a fresh one —
 * either because there was no radio, or because the attempt failed and the
 * cache was used instead. False when no forecast was wanted at all: a panel of
 * countdowns and messages drawn without a radio has done everything it meant to
 * and must not be labelled as damaged.
 */
bool FellShort(const CycleInputs& in) {
    if (!in.wants_weather) return false;
    const FetchVerdict wanted = WeatherVerdict(in);
    if (!wanted.fetch) return false;           // nothing was due; nothing missed
    if (in.fetch_attempted) return !in.fetch_ok;
    return true;                               // due, and never even attempted
}

CycleDecision StandDown(bool participated, CycleOutcome outcome,
                        const char* reason, bool degraded) {
    CycleDecision d;
    d.step = CycleStep::kStandDown;
    d.participated = participated;
    d.outcome = outcome;
    d.reason = reason;
    d.degraded = degraded;
    return d;
}

}  // namespace

CycleDecision PlanCycleStep(const CycleInputs& in) {
    // 1. The kill-switch and the profile, before anything else — and both of
    //    them stand autonomy down rather than ending the cycle.
    //
    // This is the ordering that keeps a device with autonomy off behaving
    // exactly as it did before this feature existed: the caller carries on with
    // its ordinary fetch-phase timing, the tower still gets its full ninety
    // seconds to land a push, and a wake that failed to reach the network still
    // reports a network failure rather than "unchanged".
    if (!in.autonomy_enabled) {
        return StandDown(false, CycleOutcome::kUnchanged, kReasonDisabled, false);
    }
    if (!in.profile_present) {
        return StandDown(false, CycleOutcome::kUnchanged, kReasonNoProfile, false);
    }

    // 2. A tower frame wins immediately, and ends the autonomy path. A PUT is
    //    an act of the operator; the render phase is already about to draw it.
    if (in.tower_frame_arrived) {
        return StandDown(true, CycleOutcome::kUpdated, kReasonTowerFrame, false);
    }

    // 3. Already drew — or tried to — this wake. Report what it came to. The
    //    three failure-ish results are deliberately distinct: "identical to
    //    what is displayed" is the cheapest good outcome this device produces,
    //    "a push landed first" is the arbitration rule working, and only the
    //    third is a fault.
    switch (in.compose_result) {
        case ComposeOutcome::kAccepted: {
            const bool short_of_it = FellShort(in);
            return StandDown(true,
                             short_of_it ? CycleOutcome::kDegraded
                                         : CycleOutcome::kUpdated,
                             kReasonAllowed, short_of_it);
        }
        case ComposeOutcome::kDeduped:
            return StandDown(true, CycleOutcome::kUnchanged, kReasonComposeIdentical,
                             FellShort(in));
        case ComposeOutcome::kSuperseded:
            return StandDown(true, CycleOutcome::kUpdated, kReasonTowerFrame, false);
        case ComposeOutcome::kFailed:
            return StandDown(true, CycleOutcome::kFailed, kReasonComposeFailed, true);
        case ComposeOutcome::kNotTried:
            break;
    }

    // 4. Is the tower's turn over? The profile says how long it gets, and the
    //    budget says how long it can have: waiting the full thirty seconds and
    //    then discovering there is no room left to fetch or draw would spend
    //    the whole wake on a rendezvous that produced nothing.
    const bool fetch_wanted = FetchStillWanted(in);
    const uint32_t floor_ms = fetch_wanted ? kMinFetchBudgetMs : kMinComposeBudgetMs;
    const bool tower_wait_expired =
        in.tower_wait_remaining_ms == 0 || in.work_remaining_ms <= floor_ms;

    // 5. Everything the policy module already owns: whether the device was
    //    asked at all, whether a fresh tower frame is being painted over, and
    //    the interactive window. Asked here rather than re-derived so there is
    //    one answer to "may the device draw".
    PolicyInputs policy;
    policy.autonomy_enabled = in.autonomy_enabled;
    policy.profile_present = in.profile_present;
    policy.has_device_module = in.has_device_module;
    policy.has_auto_module = in.has_auto_module;
    policy.tower_frame_arrived = in.tower_frame_arrived;
    policy.tower_wait_expired = tower_wait_expired;
    policy.displayed_origin = in.displayed_origin;
    policy.displayed_age_s = in.displayed_age_s;
    policy.displayed_age_known = in.displayed_age_known;
    policy.wake_interval_min = in.wake_interval_min;
    policy.interactive_window_open = in.interactive_window_open;

    const LocalRenderVerdict verdict = EvaluateLocalRender(policy);
    if (!verdict.allowed) {
        // One refusal is not final: an Auto module whose tower wait is still
        // running gets to keep waiting, which is the whole meaning of Auto.
        if (verdict.reason == kReasonWaitingForTower) {
            CycleDecision d;
            d.step = CycleStep::kWaitForTower;
            d.participated = true;
            d.reason = kReasonWaitingForTower;
            return d;
        }
        CycleDecision d = StandDown(true, CycleOutcome::kUnchanged, verdict.reason,
                                    false);
        d.tower_wait_expired = tower_wait_expired;
        return d;
    }

    // 6. The bounded-power rule. A compose is CPU and a flash write rather than
    //    a network round trip, but it is not free, and one begun with nothing
    //    left would be finishing after the budget had already expired.
    if (in.work_remaining_ms < kMinComposeBudgetMs) {
        CycleDecision d = StandDown(true, CycleOutcome::kUnchanged, kReasonBudgetSpent,
                                    false);
        d.tower_wait_expired = tower_wait_expired;
        return d;
    }

    // 7. A forecast, if one is due, this wake has not already tried, the radio
    //    is up, and the budget covers a whole bounded fetch *and* the compose
    //    that has to follow it. All of them, because each missing one turns the
    //    fetch into time spent for nothing.
    if (fetch_wanted && in.work_remaining_ms >= kMinFetchBudgetMs) {
        CycleDecision d;
        d.step = CycleStep::kFetchWeather;
        d.participated = true;
        d.reason = WeatherVerdict(in).reason;
        d.tower_wait_expired = tower_wait_expired;
        // The whole-operation deadline handed to the client. Never its own
        // ceiling when the budget is tighter than that, so a fetch can never
        // run past the moment the compose has to begin.
        const uint32_t room = in.work_remaining_ms - kMinComposeBudgetMs;
        d.budget_ms = room < kFetchWireTimeoutMs ? room : kFetchWireTimeoutMs;
        return d;
    }

    // 8. Draw. This is the step that makes the feature real, and it is reached
    //    whether or not the forecast arrived: a panel from a four-hour-old
    //    cache, marked as such, is worth more than no panel.
    CycleDecision d;
    d.step = CycleStep::kCompose;
    d.participated = true;
    d.reason = kReasonAllowed;
    d.degraded = FellShort(in);
    d.tower_wait_expired = tower_wait_expired;
    return d;
}

const char* FetchPhaseActName(FetchPhaseAct act) {
    switch (act) {
        case FetchPhaseAct::kBackToNetwork: return "back_to_network";
        case FetchPhaseAct::kAutonomyStep:  return "autonomy_step";
        case FetchPhaseAct::kGoToRender:    return "go_to_render";
        case FetchPhaseAct::kFinish:        return "finish";
    }
    return "";
}

FetchPhaseDecision PlanFetchPhase(const FetchPhaseInputs& in) {
    FetchPhaseDecision d;

    // 1. Lost the association mid-cycle. The bounded retry lives in the network
    //    phase; the budget is unchanged, so this cannot become a loop.
    if (!in.network_up) {
        d.act = FetchPhaseAct::kBackToNetwork;
        d.reason = "network_lost";
        return d;
    }

    // 2. Something is on its way to the glass. A pushed frame, a frame this
    //    device composed, or a refresh already running: all three mean the
    //    render phase owns the rest of this wake, and the cycle's outcome comes
    //    from whether the panel actually drew rather than from who asked it to.
    if (in.tower_frame_seen) {
        d.act = FetchPhaseAct::kGoToRender;
        d.reason = kReasonTowerFrame;
        return d;
    }
    if (in.local_frame_stored || in.panel_busy) {
        d.act = FetchPhaseAct::kGoToRender;
        d.reason = in.local_frame_stored ? kReasonAllowed : "panel_busy";
        return d;
    }

    // 3. The tower's rendezvous, measured against the phase's own cap. This is
    //    the ninety seconds the push path holds open, and autonomy standing
    //    down does not shorten it by a single tick.
    //
    //    It does not apply while autonomy is mid-sequence. A fetch is allowed
    //    to be half the cap on its own, and a phase that stopped the moment the
    //    cap ran out would fetch a forecast and then sleep without drawing it.
    //    The bound on autonomy's own work is the total, through
    //    CycleInputs::work_remaining_ms, and the cycle still cannot outlive it.
    if (in.phase_left_ms == 0 && in.autonomy_settled) {
        d.act = FetchPhaseAct::kFinish;
        // Normally a success: the device reached the network and the panel is
        // already showing what it should. A cycle whose own compose failed is
        // the exception and keeps the failure, so the backoff still applies.
        d.failed = in.autonomy_participated &&
                   in.autonomy_outcome == CycleOutcome::kFailed;
        d.reason = d.failed ? kReasonComposeFailed : "rendezvous_over";
        return d;
    }

    d.act = FetchPhaseAct::kAutonomyStep;
    d.reason = "autonomy_turn";
    return d;
}

}  // namespace autonomy
