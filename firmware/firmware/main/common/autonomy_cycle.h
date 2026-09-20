/**
 * @file autonomy_cycle.h
 * @brief What the fetch phase of a wake should do next, as a pure function.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why the sequencing is here and not in ServiceWakeCycle
 * -----------------------------------------------------
 * ServiceWakeCycle is the one path on this device that can flatten a battery in
 * the field. Its guarantee — every exit ends in deep sleep, inside a 120-second
 * budget — is the kind that cannot be checked by reading, because the cases
 * that break it are "the fetch was still running when the budget ran out" and
 * "Wi-Fi returned between two ticks".
 *
 * So the decisions are here, as a function of a struct, and the host suite
 * drives all of them — including the whole default timeline, tick by tick,
 * against a mock clock. What is left in application.cc is execution: bring the
 * radio up, call the client, call the compositor, store the frame.
 *
 * Autonomy is a guest in this cycle, and stands down rather than ending it
 * -----------------------------------------------------------------------
 * The wake cycle existed before autonomy did, and its shape is the contract the
 * tower relies on: the device comes up, associates, and then holds the fetch
 * phase open for up to forty seconds so a queued push has somewhere to land.
 *
 * Autonomy must not shorten that. A device with autonomy switched off, with no
 * profile, or with a profile that asks for nothing, has to behave *exactly* as
 * it did before this feature existed — which means this planner answers
 * @c kStandDown, and the caller carries on with its ordinary phase timing. It
 * does not answer "finish". An earlier revision did, and the result was a
 * device that went back to sleep on the first tick of the fetch phase and
 * never received another push.
 *
 * `participated` is the flag that keeps the status route honest about this: a
 * stand-down with `participated == false` means autonomy took no part in this
 * wake and has nothing to report, which is a different thing from "autonomy ran
 * and found nothing to do".
 *
 * Bounded sub-phase allocation
 * ----------------------------
 * The rule this module adds to autonomy_policy — which knows only "may the
 * device draw" and "is a forecast due" — is that **no step that costs time is
 * started without the budget to finish it.**
 *
 * The budget it measures against is `WakeBudget::WorkRemainingMs()`: the total
 * still unspent, minus the render and settle caps. The render is part of the
 * 165 seconds, so a frame composed at 160 s is a frame the panel begins drawing
 * after the cycle should already have ended. Allocating content work against
 * the total-minus-render is what makes the compose land early enough to be
 * displayed inside the same wake.
 *
 * Deliberately *not* measured against the fetch phase's own ninety-second cap.
 * That cap sizes the tower's rendezvous — how long the device holds the door
 * open for a push — and it was widened from 40 s to 90 s precisely so a pushed
 * frame reliably lands inside the awake window: the tower's scheduler pulses
 * every 30 s, and a 40 s window made delivery a coin flip. With the default
 * profile the sub-phases inside it are:
 *
 *     tower wait  80 s   (profile; cut short only if the fetch could not fit)
 *     fetch     ≤ 20 s   (the client's own ceiling, clamped to what is left)
 *     compose     ~2 s   (CPU and one flash write; 3 s reserved)
 *
 * which is a hundred and two seconds of allocation inside a cycle whose
 * remaining content window, after the network phase and the 53 s render+settle
 * reserve, is a hundred and twelve seconds from the start of the wake. The
 * numbers are checked end to end by the default-timeline test rather than
 * asserted here.
 *
 * What it does not do
 * -------------------
 * It does not decide whether the composed bytes are worth writing. The A/B
 * store already answers that by returning kDuplicate for a payload it holds,
 * and that check has to happen against the bytes actually stored rather than
 * against a hash this module was told about. It is told the *result* instead,
 * through ComposeOutcome, so "accepted", "identical" and "a push beat us to it"
 * are three outcomes rather than one boolean.
 */

#ifndef COMMON_AUTONOMY_CYCLE_H
#define COMMON_AUTONOMY_CYCLE_H

#include <stdint.h>

#include "autonomy_policy.h"
#include "autonomy_status.h"

namespace autonomy {

/**
 * @brief The forecast client's own whole-operation ceiling, in milliseconds.
 *
 * Repeated here rather than included, because this translation unit is portable
 * and openmeteo_client.h drags the parser in with it. application.cc
 * static_asserts that the two agree, so they cannot drift.
 */
constexpr uint32_t kFetchWireTimeoutMs = 20000;

/**
 * @brief Time the planner insists on before it will start a bounded fetch.
 *
 * The client's own ceiling plus the compose reserve plus a second of slack for
 * the association check and the store. Under this, the honest answer is to
 * compose from whatever cache there is and let the next wake be the retry —
 * which is what the existing failure backoff is for.
 */
constexpr uint32_t kMinComposeBudgetMs = 3000;
constexpr uint32_t kMinFetchBudgetMs = kFetchWireTimeoutMs + kMinComposeBudgetMs + 1000;

/// What the fetch phase should do on this tick.
enum class CycleStep : uint8_t {
    /// Keep watching for a pushed frame. Only ever returned while the profile's
    /// tower wait is still running.
    kWaitForTower,
    /// Spend budget on one bounded forecast fetch, inside @c budget_ms.
    kFetchWeather,
    /// Build a frame from the profile, the cache and the clock.
    kCompose,
    /// Autonomy has nothing more to do this wake. **The caller continues its
    /// ordinary phase timing** — it does not end the cycle on this account.
    kStandDown,
};

const char* CycleStepName(CycleStep step);

/// What the compose attempt came to. Deliberately not a boolean: "stored",
/// "identical to what is displayed" and "a push landed first" are three
/// different facts and the status route reports them differently.
enum class ComposeOutcome : uint8_t {
    kNotTried = 0,
    kAccepted,    ///< new bytes stored; a refresh was requested
    kDeduped,     ///< byte-identical to the stored frame; no write, no refresh
    kSuperseded,  ///< the stored frame moved under us: the tower won
    kFailed,      ///< the compositor or the store refused
};

const char* ComposeOutcomeName(ComposeOutcome result);

/// Everything the sequencing depends on. Closed, so the sweep is meaningful.
struct CycleInputs {
    // ---- the profile and the switch ----
    bool autonomy_enabled = false;
    bool profile_present = false;
    bool has_device_module = false;
    bool has_auto_module = false;
    /// A module in the profile needs the forecast.
    bool wants_weather = false;

    // ---- this wake so far ----
    bool tower_frame_arrived = false;
    /// Milliseconds still to run on the profile's tower wait for this wake.
    /// Zero once it has elapsed. Device-only profiles pass zero from the start.
    uint32_t tower_wait_remaining_ms = 0;
    bool network_up = false;
    /**
     * @brief Milliseconds until no new content work may be started.
     *
     * `WakeBudget::WorkRemainingMs()`: the total still unspent, less the render
     * and settle caps. See the header comment on sub-phase allocation.
     */
    uint32_t work_remaining_ms = 0;

    // ---- what has already been done this wake ----
    bool fetch_attempted = false;
    bool fetch_ok = false;
    ComposeOutcome compose_result = ComposeOutcome::kNotTried;

    // ---- the cache and the clock ----
    bool has_cache = false;
    int64_t cache_fetched_epoch = 0;
    int64_t now_epoch = 0;
    bool clock_set = false;
    int32_t min_fetch_interval_min = 30;

    // ---- arbitration ----
    Origin displayed_origin = Origin::kNone;
    int64_t displayed_age_s = 0;
    bool displayed_age_known = false;
    int32_t wake_interval_min = 60;
    bool interactive_window_open = false;
};

struct CycleDecision {
    CycleStep step = CycleStep::kStandDown;
    /**
     * @brief Did autonomy take any part in this wake?
     *
     * False when it is switched off or there is no profile. The status route
     * reports nothing at all in that case rather than a manufactured
     * "unchanged", and — more importantly — the wake cycle keeps the timing and
     * the failure semantics it had before this feature existed.
     */
    bool participated = false;
    /// What the content cycle achieved. Meaningful when @c participated.
    CycleOutcome outcome = CycleOutcome::kUnchanged;
    /// True when this cycle could not do everything it meant to: no radio, or a
    /// forecast fetch that failed and left the cache in use. Carried into the
    /// composed panel so the glass says so too.
    bool degraded = false;
    /// For kFetchWeather: the hard whole-operation deadline for that fetch, in
    /// milliseconds. Never more than the client's own ceiling and never more
    /// than what the budget can cover. Zero for every other step.
    uint32_t budget_ms = 0;
    /// True once the tower's turn is over for this wake, either because the
    /// profile's wait elapsed or because waiting any longer would leave no room
    /// to draw. Reported so the caller can log why it stopped waiting.
    bool tower_wait_expired = false;
    /// A stable token, reported on the status route. Never empty.
    const char* reason = "";
};

/// Stable reason tokens this module adds to the ones in autonomy_policy.h.
extern const char* const kReasonBudgetSpent;
extern const char* const kReasonComposeIdentical;
extern const char* const kReasonComposeFailed;

/// Decide the next step. Pure; every input is in @p in.
CycleDecision PlanCycleStep(const CycleInputs& in);

// -------------------------------------------------- the fetch phase itself --

/**
 * @brief What the fetch phase should do with this tick, above the autonomy step.
 *
 * The phase transitions used to be a ladder of ifs inside ServiceWakeCycle,
 * where the only way to ask "what happens when a push lands during a compose,
 * forty-one seconds into a cycle whose network phase took five" was to build a
 * device and arrange all three. They are here instead, so the host suite can
 * drive a whole wake — network, rendezvous, fetch, compose, render, settle —
 * against a mock clock, through the same function the device calls.
 */
enum class FetchPhaseAct : uint8_t {
    /// Wi-Fi went away mid-cycle. Back to the network phase, where the bounded
    /// retry lives.
    kBackToNetwork,
    /// Give autonomy this tick.
    kAutonomyStep,
    /// Something is on its way to the panel. The render phase owns the rest.
    kGoToRender,
    /// The rendezvous is over. End the cycle.
    kFinish,
};

const char* FetchPhaseActName(FetchPhaseAct act);

struct FetchPhaseInputs {
    bool network_up = true;
    /// The store's sequence moved, and the frame it moved to is not one this
    /// device composed. Only a push can do that.
    bool tower_frame_seen = false;
    /// The panel is drawing, or has a frame queued.
    bool panel_busy = false;
    /// This wake composed a frame and the store accepted it.
    bool local_frame_stored = false;
    /**
     * @brief What is left of the fetch phase's *own* cap.
     *
     * `WakeBudget::RemainingInPhase(kFetch, phase_started, now)` — which
     * subtracts the time the phase has already spent, unlike RemainingFor().
     * Forty seconds at the top of the phase, and this is what sizes the
     * tower's rendezvous. Autonomy's own work is allocated against a different
     * number; see CycleInputs::work_remaining_ms.
     */
    uint32_t phase_left_ms = 0;
    /// The last autonomy decision this wake, or the defaults when autonomy took
    /// no part in it.
    bool autonomy_participated = false;
    CycleOutcome autonomy_outcome = CycleOutcome::kUnchanged;
    /**
     * @brief Autonomy has finished with this wake, one way or another.
     *
     * True before it has been asked anything, and true again once it stands
     * down. False in between — while it is waiting for the tower, or has
     * fetched and not yet composed.
     *
     * This is what stops the rendezvous cap from cutting a sequence in half. A
     * fetch can take twenty seconds, which is half the forty-second phase cap,
     * and a phase that ended the moment the cap ran out would fetch a forecast
     * and then go to sleep without drawing it — which is exactly what an
     * earlier revision did. The cap sizes the tower's turn; once the device has
     * started work of its own, the bound on that work is the one in
     * CycleInputs::work_remaining_ms, and it is a bound on the *total*.
     */
    bool autonomy_settled = true;
};

struct FetchPhaseDecision {
    FetchPhaseAct act = FetchPhaseAct::kAutonomyStep;
    /// For kFinish: true when this wake must be recorded as a failure and earn
    /// the retry backoff. False is the ordinary "nothing new to draw", which is
    /// a success — the device reached the network and the panel is right.
    bool failed = false;
    const char* reason = "";
};

FetchPhaseDecision PlanFetchPhase(const FetchPhaseInputs& in);

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_CYCLE_H
