/**
 * @file power_policy.cc
 * @brief Implementation of the hybrid low-power contract. See power_policy.h.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "power_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace power {
namespace {

/// Append to a bounded buffer, reporting truncation rather than hiding it.
/// Same shape as the helper in device_config.cc: a caller that would overflow
/// gets false and an untouched tail, never a half-written escape.
bool Append(char* out, size_t out_len, size_t* used, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

bool Append(char* out, size_t out_len, size_t* used, const char* fmt, ...) {
    if (*used >= out_len) return false;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out + *used, out_len - *used, fmt, args);
    va_end(args);
    if (n < 0) return false;
    if (static_cast<size_t>(n) >= out_len - *used) return false;
    *used += static_cast<size_t>(n);
    return true;
}

}  // namespace

// ------------------------------------------------------------------ modes --

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::kAutoSaver:   return "auto_saver";
        case Mode::kInteractive: return "interactive";
        case Mode::kAlwaysOn:    return "always_on";
    }
    return "";
}

bool ParseMode(const char* name, Mode* out) {
    if (name == nullptr || out == nullptr) return false;
    if (strcmp(name, "auto_saver") == 0)   { *out = Mode::kAutoSaver;   return true; }
    if (strcmp(name, "interactive") == 0)  { *out = Mode::kInteractive; return true; }
    if (strcmp(name, "always_on") == 0)    { *out = Mode::kAlwaysOn;    return true; }
    return false;
}

bool IsValidInteractiveMinutes(int32_t minutes) {
    for (size_t i = 0; i < kInteractiveMinutesCount; ++i) {
        if (kInteractiveMinutesChoices[i] == minutes) return true;
    }
    return false;
}

bool IsValidWakeInterval(int32_t minutes) {
    return minutes >= kMinWakeIntervalMin && minutes <= kMaxWakeIntervalMin;
}

// ------------------------------------------------------------ wake reason --

const char* WakeReasonName(WakeReason reason) {
    switch (reason) {
        case WakeReason::kPowerOn:  return "power_on";
        case WakeReason::kTimer:    return "timer";
        case WakeReason::kButton:   return "button";
        case WakeReason::kRtcAlarm: return "rtc_alarm";
        case WakeReason::kOther:    return "other";
    }
    return "other";
}

bool IsUserInitiated(WakeReason reason) {
    // Power-on counts: somebody held the power key, or plugged it in. Either
    // way a person is present and expects the device to respond, so the same
    // interactive window opens as for a button wake.
    return reason == WakeReason::kButton || reason == WakeReason::kPowerOn;
}

WakeReason ClassifyWake(bool from_deep_sleep, DeepSleepCauses causes,
                        ResetClass reset) {
    if (!from_deep_sleep) {
        // Not a wake from deep sleep at all. But "not a deep-sleep wake" is not
        // the same as "a person did this", and the difference decides whether
        // this boot buys a fifteen-minute interactive window. Only the two
        // resets a human can cause are treated as such; see the header.
        return reset == ResetClass::kHuman ? WakeReason::kPowerOn
                                           : WakeReason::kOther;
    }
    // The button first when both are set: a person pressing it wants the device
    // to stay up and be usable.
    if (causes.ext0) return WakeReason::kButton;
    if (causes.timer) return WakeReason::kTimer;
    // Not armed by this firmware today. Reported honestly rather than folded
    // into "button", so a build that does arm the PCF8563 alarm line does not
    // have to remember to come back and fix this.
    if (causes.ext1) return WakeReason::kRtcAlarm;
    return WakeReason::kOther;
}

// ------------------------------------------------------- serving power save --

WifiPowerSave ServingWifiPowerSave(bool serving_http) {
    return serving_http ? WifiPowerSave::kNone : WifiPowerSave::kMinModem;
}

// -------------------------------------------------------- mode acknowledge --

const char* ModeAckStateName(ModeAckState state) {
    switch (state) {
        case ModeAckState::kAcknowledged: return "acknowledged";
        case ModeAckState::kPendingWake:  return "pending_wake";
    }
    return "acknowledged";
}

// --------------------------------------------------------------- battery --

BatteryReading EvaluateBattery(bool read_ok, bool calibration_ok,
                               uint16_t millivolts, uint8_t percent) {
    BatteryReading r;
    r.present = read_ok && millivolts > 0;
    r.calibrated = calibration_ok;
    r.plausible = r.present && millivolts >= kBatteryMinPlausibleMv &&
                  millivolts <= kBatteryMaxPlausibleMv;
    // The numbers are carried even when they will not be shown, because a
    // diagnostics page that wants to explain *why* nothing is shown needs the
    // raw value. usable() is the gate; this struct is not itself a claim.
    r.millivolts = millivolts;
    r.percent = percent > 100 ? 100 : percent;
    return r;
}

// ------------------------------------------------------------ wake budget --

const char* WakePhaseName(WakePhase phase) {
    switch (phase) {
        case WakePhase::kNetwork: return "network";
        case WakePhase::kFetch:   return "fetch";
        case WakePhase::kRender:  return "render";
        case WakePhase::kSettle:  return "settle";
        case WakePhase::kCount:   break;
    }
    return "";
}

uint32_t WakeBudgetLimits::PhaseCap(WakePhase phase) const {
    switch (phase) {
        case WakePhase::kNetwork: return network_ms;
        case WakePhase::kFetch:   return fetch_ms;
        case WakePhase::kRender:  return render_ms;
        case WakePhase::kSettle:  return settle_ms;
        case WakePhase::kCount:   break;
    }
    return 0;
}

void WakeBudget::Start(int64_t now_ms, const WakeBudgetLimits& limits) {
    started_ = true;
    start_ms_ = now_ms;
    limits_ = limits;
    has_exhausted_phase_ = false;
    exhausted_phase_ = WakePhase::kNetwork;
}

uint32_t WakeBudget::TotalRemainingMs(int64_t now_ms) const {
    if (!started_) return 0;
    const int64_t elapsed = now_ms - start_ms_;
    // A clock that went backwards is treated as no time having passed rather
    // than as an enormous remaining budget. The former loses a little battery;
    // the latter is the unbounded wake this class exists to prevent.
    if (elapsed <= 0) return limits_.total_ms;
    if (elapsed >= static_cast<int64_t>(limits_.total_ms)) return 0;
    return limits_.total_ms - static_cast<uint32_t>(elapsed);
}

uint32_t WakeBudget::RemainingFor(WakePhase phase, int64_t now_ms) const {
    if (!started_ || phase == WakePhase::kCount) return 0;
    const uint32_t total_left = TotalRemainingMs(now_ms);
    const uint32_t cap = limits_.PhaseCap(phase);
    // The whole invariant, in one line: a phase never gets time the total does
    // not have, so the worst case for the cycle is the total and not the sum
    // of the caps.
    return cap < total_left ? cap : total_left;
}

uint32_t WakeBudget::RemainingInPhase(WakePhase phase, int64_t phase_started_ms,
                                      int64_t now_ms) const {
    if (!started_ || phase == WakePhase::kCount) return 0;
    const uint32_t cap = limits_.PhaseCap(phase);
    const int64_t in_phase = now_ms - phase_started_ms;
    // A phase that has not started yet, or a clock that went backwards, is
    // treated as no time spent. Same reasoning as TotalRemainingMs: the other
    // reading would hand out a budget nobody has.
    uint32_t left_in_phase = cap;
    if (in_phase > 0) {
        left_in_phase = in_phase >= static_cast<int64_t>(cap)
                            ? 0u
                            : cap - static_cast<uint32_t>(in_phase);
    }
    const uint32_t total_left = TotalRemainingMs(now_ms);
    return left_in_phase < total_left ? left_in_phase : total_left;
}

uint32_t WakeBudget::WorkRemainingMs(int64_t now_ms) const {
    if (!started_) return 0;
    const uint32_t total_left = TotalRemainingMs(now_ms);
    const uint32_t reserve = limits_.render_ms + limits_.settle_ms;
    return total_left > reserve ? total_left - reserve : 0u;
}

bool WakeBudget::Exhausted(int64_t now_ms) const {
    return started_ && TotalRemainingMs(now_ms) == 0;
}

void WakeBudget::NoteExhaustedIn(WakePhase phase) {
    if (phase == WakePhase::kCount) return;
    // First one wins: the phase that actually ran out is the useful field
    // report, not whichever one the caller happened to ask about last.
    if (has_exhausted_phase_) return;
    has_exhausted_phase_ = true;
    exhausted_phase_ = phase;
}

void WakeBudget::Reset() {
    started_ = false;
    start_ms_ = 0;
    limits_ = WakeBudgetLimits{};
    has_exhausted_phase_ = false;
    exhausted_phase_ = WakePhase::kNetwork;
}

// -------------------------------------------------------- network recovery --

void NetworkRecovery::Reset() {
    attempts_ = 0;
}

void NetworkRecovery::NoteAttempt() {
    if (attempts_ < kMaxAttempts) ++attempts_;
}

uint32_t NetworkRecovery::BackoffMs() const {
    if (attempts_ == 0) return 0;  // the first attempt waits for nothing
    uint32_t delay = kBaseBackoffMs;
    for (uint32_t i = 1; i < attempts_; ++i) {
        if (delay >= kMaxBackoffMs) break;
        delay *= 2;
    }
    return delay > kMaxBackoffMs ? kMaxBackoffMs : delay;
}

bool NetworkRecovery::ShouldRetry(uint32_t remaining_ms, uint32_t* delay_out) const {
    if (delay_out == nullptr) return false;
    if (attempts_ >= kMaxAttempts) return false;
    const uint32_t delay = BackoffMs();
    // The budget has the last word, not the attempt counter. An attempt that
    // cannot finish before the total runs out spends the rest of the cycle to
    // achieve nothing, and the device sleeps anyway with a stale panel.
    const uint32_t needed = delay + kMinUsefulAttemptMs;
    if (remaining_ms < needed) return false;
    *delay_out = delay;
    return true;
}

// ----------------------------------------------------------- wake planning --

const char* CycleOutcomeName(CycleOutcome outcome) {
    switch (outcome) {
        case CycleOutcome::kUpdated:         return "updated";
        case CycleOutcome::kUnchanged:       return "unchanged";
        case CycleOutcome::kNetworkFailed:   return "network_failed";
        case CycleOutcome::kBudgetExhausted: return "budget_exhausted";
        case CycleOutcome::kRenderFailed:    return "render_failed";
    }
    return "";
}

bool CycleSucceeded(CycleOutcome outcome) {
    return outcome == CycleOutcome::kUpdated || outcome == CycleOutcome::kUnchanged;
}

uint32_t RetryDelayMs(uint32_t consecutive_failures, int32_t wake_interval_min) {
    if (!IsValidWakeInterval(wake_interval_min)) {
        wake_interval_min = kDefaultWakeIntervalMin;
    }
    const uint32_t normal_ms =
        static_cast<uint32_t>(wake_interval_min) * 60u * 1000u;
    if (consecutive_failures == 0) return normal_ms;

    uint32_t delay = kFirstRetryMs;
    for (uint32_t i = 1; i < consecutive_failures; ++i) {
        if (delay >= normal_ms) break;
        delay *= 2;
    }
    // Never longer than the interval the user asked for: past that point the
    // "retry" is simply the next scheduled wake, and calling it a retry would
    // overstate what the device is doing about the outage.
    return delay > normal_ms ? normal_ms : delay;
}

WakePlan PlanNextWake(const WakeInputs& in) {
    WakePlan plan;
    plan.button_wakes = true;

    // ---- physical activity beats policy ----------------------------------
    //
    // Each of these produces a visibly broken device if slept through, which
    // is worse than the battery cost of staying up for another few seconds.
    if (in.refresh_in_flight) {
        plan.action = WakeAction::kStayAwake;
        plan.reason = "refresh_in_flight";
        return plan;
    }
    if (in.provisioning_portal_open) {
        plan.action = WakeAction::kStayAwake;
        plan.reason = "provisioning_portal_open";
        return plan;
    }
    if (in.slideshow_active) {
        plan.action = WakeAction::kStayAwake;
        plan.reason = "slideshow_active";
        return plan;
    }

    // ---- modes -----------------------------------------------------------
    if (in.mode == Mode::kAlwaysOn) {
        plan.action = WakeAction::kStayAwake;
        plan.reason = "always_on";
        return plan;
    }
    if (in.mode == Mode::kInteractive && in.interactive_remaining_ms > 0) {
        plan.action = WakeAction::kStayAwake;
        plan.reason = "interactive_window_open";
        return plan;
    }

    // ---- auto saver ------------------------------------------------------
    plan.action = WakeAction::kSleep;
    plan.timer_armed = true;

    if (!CycleSucceeded(in.last_outcome)) {
        plan.wake_in_ms = RetryDelayMs(in.consecutive_failures == 0
                                           ? 1u
                                           : in.consecutive_failures,
                                       in.wake_interval_min);
        switch (in.last_outcome) {
            case CycleOutcome::kBudgetExhausted:
                plan.reason = "retry_after_budget_exhausted";
                break;
            case CycleOutcome::kRenderFailed:
                plan.reason = "retry_after_render_failure";
                break;
            default:
                plan.reason = "retry_after_network_failure";
                break;
        }
        return plan;
    }

    int32_t interval = in.wake_interval_min;
    if (!IsValidWakeInterval(interval)) interval = kDefaultWakeIntervalMin;
    plan.wake_in_ms = static_cast<uint32_t>(interval) * 60u * 1000u;
    // Charging changes nothing about the schedule. It is tempting to stay
    // awake on a charger, but the update cadence the user chose is the update
    // cadence they get, and a device that behaves differently on USB is a
    // device whose behaviour cannot be tested on a bench.
    plan.reason = in.charging ? "scheduled_wake_charging" : "scheduled_wake";
    return plan;
}

// ------------------------------------------------------ interactive window --

void InteractiveWindow::Open(int64_t now_ms, int32_t minutes) {
    if (!IsValidInteractiveMinutes(minutes)) return;
    open_ = true;
    opened_ms_ = now_ms;
    minutes_ = minutes;
}

void InteractiveWindow::Close() {
    open_ = false;
    opened_ms_ = 0;
    minutes_ = 0;
}

bool InteractiveWindow::IsOpen(int64_t now_ms) const {
    return RemainingMs(now_ms) > 0;
}

uint32_t InteractiveWindow::RemainingMs(int64_t now_ms) const {
    if (!open_ || minutes_ <= 0) return 0;
    const int64_t duration = static_cast<int64_t>(minutes_) * 60 * 1000;
    const int64_t elapsed = now_ms - opened_ms_;
    if (elapsed < 0) return static_cast<uint32_t>(duration);
    if (elapsed >= duration) return 0;
    return static_cast<uint32_t>(duration - elapsed);
}

void InteractiveWindow::Reset() {
    Close();
}

// --------------------------------------------------------------- the state --

void PowerState::Init(Mode persisted_mode, int32_t wake_interval_min,
                      int32_t interactive_minutes, int64_t now_ms,
                      WakeReason reason) {
    // kInteractive is never restored from NVS, and PersistableMode() is why it
    // is never written there either. Belt and braces: a value that got into
    // NVS some other way still must not resurrect a window nobody opened.
    base_ = persisted_mode == Mode::kInteractive ? Mode::kAutoSaver : persisted_mode;
    desired_ = base_;
    wake_interval_min_ = IsValidWakeInterval(wake_interval_min)
                             ? wake_interval_min
                             : kDefaultWakeIntervalMin;
    interactive_minutes_ = IsValidInteractiveMinutes(interactive_minutes)
                               ? interactive_minutes
                               : kDefaultInteractiveMinutes;
    wake_reason_ = reason;
    window_.Reset();
    window_was_open_ = false;

    // A person pressed the button or powered it on, so they are standing in
    // front of it. Coming straight back up in auto-saver and sleeping again a
    // few seconds later is the behaviour that makes a device feel broken, so a
    // user-initiated wake opens the window. A timer wake does not: nobody is
    // there, and staying awake would defeat the wake it just performed.
    if (IsUserInitiated(reason) && base_ != Mode::kAlwaysOn) {
        window_.Open(now_ms, interactive_minutes_);
        desired_ = Mode::kInteractive;
        window_was_open_ = true;
    }
}

Mode PowerState::effective_mode(int64_t now_ms) const {
    if (base_ == Mode::kAlwaysOn) return Mode::kAlwaysOn;
    if (window_.IsOpen(now_ms)) return Mode::kInteractive;
    return Mode::kAutoSaver;
}

ModeAckState PowerState::ack_state(int64_t now_ms) const {
    return effective_mode(now_ms) == desired_ ? ModeAckState::kAcknowledged
                                              : ModeAckState::kPendingWake;
}

uint32_t PowerState::interactive_remaining_ms(int64_t now_ms) const {
    return window_.RemainingMs(now_ms);
}

bool PowerState::Request(Mode mode, int32_t minutes, int64_t now_ms) {
    if (mode == Mode::kInteractive) {
        if (minutes == 0) minutes = interactive_minutes_;
        if (!IsValidInteractiveMinutes(minutes)) return false;
        interactive_minutes_ = minutes;
        base_ = Mode::kAutoSaver;
        window_.Open(now_ms, minutes);
        window_was_open_ = true;
        desired_ = Mode::kInteractive;
        return true;
    }

    // Leaving interactive closes the window rather than letting it run out
    // underneath the new mode. "Back to power saving now" has to mean now, or
    // the button in the tower does nothing visible for up to an hour.
    window_.Close();
    window_was_open_ = false;
    base_ = mode;
    desired_ = mode;
    return true;
}

bool PowerState::SetWakeInterval(int32_t minutes) {
    if (!IsValidWakeInterval(minutes)) return false;
    wake_interval_min_ = minutes;
    return true;
}

bool PowerState::SetInteractiveMinutes(int32_t minutes) {
    if (!IsValidInteractiveMinutes(minutes)) return false;
    // Deliberately does not touch window_. An open window keeps the deadline
    // it was opened with; the new length applies to the next one. Re-opening
    // the window here would let "make the window half an hour" either cut a
    // longer window short or silently extend one, and neither is what the
    // sentence says.
    interactive_minutes_ = minutes;
    return true;
}

bool PowerState::Tick(int64_t now_ms) {
    const bool open_now = window_.IsOpen(now_ms);
    if (window_was_open_ && !open_now) {
        window_was_open_ = false;
        window_.Close();
        // Falling out of the window is a return to the base mode, and the
        // desired mode follows it. If it did not, the status route would
        // report a permanent pending_wake for a request that has been served
        // and has since expired.
        desired_ = base_;
        return true;
    }
    window_was_open_ = open_now;
    return false;
}

Mode PowerState::PersistableMode() const {
    return base_ == Mode::kInteractive ? Mode::kAutoSaver : base_;
}

// ------------------------------------------------------------ JSON output --

size_t RenderPowerJson(const PowerStatus& s, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    bool ok = true;

    ok = ok && Append(out, out_len, &used,
                      "{\"contract\":%d,"
                      "\"mode\":\"%s\",\"desired_mode\":\"%s\",\"ack\":\"%s\","
                      "\"awake\":%s,\"sleep_intent\":%s,"
                      "\"interactive_remaining_s\":%u,"
                      "\"wake_interval_min\":%d,"
                      "\"timer_armed\":%s,\"next_wake_in_s\":%u,",
                      kPowerContractVersion,
                      ModeName(s.effective), ModeName(s.desired),
                      ModeAckStateName(s.ack),
                      s.awake ? "true" : "false",
                      s.sleep_intent ? "true" : "false",
                      static_cast<unsigned>(s.interactive_remaining_s),
                      static_cast<int>(s.wake_interval_min),
                      s.timer_armed ? "true" : "false",
                      static_cast<unsigned>(s.next_wake_in_s));

    // An unset clock renders null rather than 1970, because a wall-clock time
    // computed from an unset clock is a fabricated timestamp and the tower
    // would display it as a real one.
    if (s.next_wake_epoch > 0) {
        ok = ok && Append(out, out_len, &used, "\"next_wake_epoch\":%u,",
                          static_cast<unsigned>(s.next_wake_epoch));
    } else {
        ok = ok && Append(out, out_len, &used, "\"next_wake_epoch\":null,");
    }

    ok = ok && Append(out, out_len, &used, "\"last_wake_reason\":\"%s\",",
                      WakeReasonName(s.last_wake_reason));

    // Null, not "updated", when no cycle has finished. A freshly booted device
    // has no last outcome, and naming one would tell the tower the last update
    // succeeded on a device that has never completed an update.
    if (s.last_outcome_known) {
        ok = ok && Append(out, out_len, &used, "\"last_outcome\":\"%s\",",
                          CycleOutcomeName(s.last_outcome));
    } else {
        ok = ok && Append(out, out_len, &used, "\"last_outcome\":null,");
    }

    // Which phase ran out of budget, when one did. Null is the common case and
    // means the cycle ended for some other reason, not that the phase is
    // unknown.
    if (s.budget_exhausted_phase != WakePhase::kCount) {
        ok = ok && Append(out, out_len, &used, "\"budget_exhausted_phase\":\"%s\",",
                          WakePhaseName(s.budget_exhausted_phase));
    } else {
        ok = ok && Append(out, out_len, &used, "\"budget_exhausted_phase\":null,");
    }

    ok = ok && Append(out, out_len, &used, "\"consecutive_failures\":%u,",
                      static_cast<unsigned>(s.consecutive_failures));

    // The battery claim, and the whole reason this renders here rather than in
    // the route: when the reading is not fit to show, the numbers are null and
    // there is no code path that puts a figure in their place.
    ok = ok && Append(out, out_len, &used,
                      "\"battery\":{\"present\":%s,\"calibrated\":%s,"
                      "\"plausible\":%s,",
                      s.battery.present ? "true" : "false",
                      s.battery.calibrated ? "true" : "false",
                      s.battery.plausible ? "true" : "false");
    if (s.battery.usable()) {
        ok = ok && Append(out, out_len, &used,
                          "\"mv\":%u,\"percent\":%u},",
                          static_cast<unsigned>(s.battery.millivolts),
                          static_cast<unsigned>(s.battery.percent));
    } else {
        ok = ok && Append(out, out_len, &used, "\"mv\":null,\"percent\":null},");
    }

    ok = ok && Append(out, out_len, &used,
                      "\"charge\":{\"state\":\"%s\",\"charging\":%s}}",
                      s.charge_state == nullptr ? "unknown" : s.charge_state,
                      s.charging ? "true" : "false");

    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

}  // namespace power
