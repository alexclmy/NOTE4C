/**
 * @file power_policy.h
 * @brief The hybrid low-power contract: modes, wake planning, and the bounded
 *        budget that guarantees a wake cycle ends in sleep.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Like device_config.h and dashboard_service.h, this file deliberately
 * contains no ESP-IDF header, so the host tests exercise these exact
 * translation units rather than a parallel reimplementation of the rules. The
 * device glue that owns esp_sleep, NVS and the Wi-Fi driver lives in
 * application.cc and calls into here for every decision.
 *
 * What this owns
 * --------------
 * The *whole* answer to "should this device be awake, and if not, when does it
 * come back". That is: the three modes and their wire names, the interactive
 * window and its expiry, the plan for the next wake, the bounded budget that
 * makes "we will go back to sleep" a property of the arithmetic rather than a
 * hope, and the rule for when a battery reading may be shown to a human.
 *
 * Why a budget rather than a timeout
 * ----------------------------------
 * A wake cycle is a sequence of things that can each hang: associating with an
 * access point, a DHCP lease, a TCP connect to the tower, a 15 kB frame, and a
 * panel refresh that waits on a BUSY pin. Giving each one its own timeout does
 * not bound the total, because the timeouts add up. A device whose worst case
 * is the sum of six timeouts is a device that can stay awake for minutes on a
 * battery that was budgeted for seconds.
 *
 * WakeBudget inverts that: there is one total, every phase asks it how much
 * time is left, and no phase can be granted time the total does not have. The
 * worst case is the total, by construction. That is the difference between a
 * device that returns to sleep and a device that usually returns to sleep.
 *
 * What this deliberately refuses to do
 * ------------------------------------
 * It will not claim a remote command can wake a sleeping device. A device in
 * deep sleep has no radio: the Wi-Fi PHY is off and there is nothing listening
 * on any socket. `WakePlan` therefore has no "wake now" input that could come
 * from the network, and the only immediate wake source in this file is the
 * physical button. The tower's way to change a sleeping device's mode is an
 * intent it holds until the device next calls in, and the honest word for the
 * delay that introduces is in ModeAckState below.
 */

#ifndef COMMON_POWER_POLICY_H
#define COMMON_POWER_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace power {

/// Bumped alongside the `power.hybrid.v1` capability string.
constexpr int kPowerContractVersion = 1;

// ------------------------------------------------------------------ modes --

/**
 * @brief What the device is trying to be between refreshes.
 *
 * Three, because the device has three behaviours and flattening any pair of
 * them would make the tower tell the user something false.
 */
enum class Mode {
    /// Wake on a timer, do one bounded update cycle, sleep again. The default,
    /// and the only mode that gets the advertised battery life.
    kAutoSaver = 0,
    /// Stay awake and serve the API until the window expires, then fall back
    /// to kAutoSaver on its own. Never persists across the expiry.
    kInteractive,
    /// Stay awake indefinitely. Costs roughly two orders of magnitude more
    /// current than kAutoSaver; the tower is required to say so.
    kAlwaysOn,
};

/// Wire name, for the config field and the status route. "" for an invalid mode.
const char* ModeName(Mode mode);

/// Resolve a wire name. false when it is not one of the three.
bool ParseMode(const char* name, Mode* out);

/**
 * @brief The interactive window durations the contract accepts.
 *
 * A closed list rather than a range, for the same reason gallery.slide_min is
 * a closed list: these are the four buttons in the tower, and a value that is
 * not one of them arrived from something other than the UI.
 */
constexpr int32_t kInteractiveMinutesChoices[] = {5, 15, 30, 60};
constexpr size_t kInteractiveMinutesCount = 4;
bool IsValidInteractiveMinutes(int32_t minutes);

/// Default interactive window when the tower does not name one.
constexpr int32_t kDefaultInteractiveMinutes = 15;

/**
 * @brief Bounds on the auto-saver wake interval.
 *
 * The product asks for hourly. The floor is not a preference: at five-minute
 * wakes the radio and the panel dominate the average current and the saver
 * mode stops being one, so a caller that asks for it is told no rather than
 * quietly sold a mode that does not save anything.
 */
constexpr int32_t kMinWakeIntervalMin = 15;
constexpr int32_t kMaxWakeIntervalMin = 1440;
constexpr int32_t kDefaultWakeIntervalMin = 60;
bool IsValidWakeInterval(int32_t minutes);

// ------------------------------------------------------------ wake reason --

/// Why this boot happened. Maps from esp_sleep_get_wakeup_cause() in the glue.
enum class WakeReason {
    kPowerOn = 0,   ///< cold boot or reset; not a wake from deep sleep
    kTimer,         ///< the scheduled auto-saver wake
    kButton,        ///< the physical button, the only immediate wake there is
    kRtcAlarm,      ///< the PCF8563 alarm line, when it is the wake source
    kOther,         ///< a cause deep sleep reported that is none of the above
};

const char* WakeReasonName(WakeReason reason);

/// True when this wake was caused by a person touching the device. Those wakes
/// open an interactive window; timer wakes deliberately do not.
bool IsUserInitiated(WakeReason reason);

/**
 * @brief Which resets a person can be assumed to have caused.
 *
 * kHuman is ESP_RST_POWERON and ESP_RST_EXT and nothing else. Everything the
 * chip can do to itself — watchdog, brownout, panic, software restart, a USB or
 * JTAG reset — is kOther, and the glue maps it that way.
 */
enum class ResetClass {
    kHuman = 0,
    kOther,
};

/// The deep-sleep wake causes, as booleans rather than as a chip bitmask. All
/// false means deep sleep reported no cause this firmware arms.
struct DeepSleepCauses {
    bool ext0 = false;   ///< the BOOT button, the only thing ext0 is armed on
    bool timer = false;  ///< the scheduled auto-saver wake
    bool ext1 = false;   ///< the RTC alarm line, if a build ever arms it
};

/**
 * @brief Decide why this boot happened.
 *
 * @param from_deep_sleep true when the chip reported any deep-sleep wake cause.
 * @param causes          which ones. Ignored entirely when @p from_deep_sleep
 *                        is false, so a stale bit cannot promote a crash into
 *                        a button press.
 * @param reset           how to read the reset reason, for the case where deep
 *                        sleep reported nothing.
 *
 * Portable so a host can read the rule back; the chip calls that produce these
 * three inputs stay in application.cc. Two orderings are load-bearing:
 *
 *  - **ext0 beats the timer.** Both sources are armed for every sleep and they
 *    can genuinely coincide — somebody presses the button in the same
 *    millisecond the hourly timer fires. Treating that as a plain timer wake
 *    would put the device back to sleep in their hand. Preferring the person
 *    costs one wasted interactive window at worst.
 *
 *  - **Only human resets are kPowerOn.** A user-initiated wake opens a
 *    fifteen-minute interactive window, so reporting every reset as kPowerOn
 *    meant a device stuck in a brownout or watchdog loop came back awake for
 *    fifteen minutes on each crash. On a flat battery that is exactly the
 *    worst thing to do: spend the last of the charge holding the radio on,
 *    crash, repeat. Those resets are reported as kOther rather than
 *    mislabelled, and the device sleeps on schedule instead.
 */
WakeReason ClassifyWake(bool from_deep_sleep, DeepSleepCauses causes,
                        ResetClass reset);

// ------------------------------------------------------- serving power save --

/**
 * @brief Wi-Fi modem sleep, as the two settings this firmware ever wants.
 *
 * Not the driver's enum: this header has no ESP-IDF in it, and the glue maps
 * these onto WIFI_PS_NONE and WIFI_PS_MIN_MODEM.
 */
enum class WifiPowerSave {
    kNone = 0,   ///< the radio stays listening; WIFI_PS_NONE
    kMinModem,   ///< the driver default; the station dozes between beacons
};

/**
 * @brief What modem sleep should be while (or while not) serving HTTP.
 *
 * @param serving_http true when an HTTP server is up and expected to answer.
 *
 * WHY THIS EXISTS
 * ---------------
 * The AP provisioning path has always disabled power save before serving. The
 * LAN path never did, so a station that had associated at -36 dBm, taken a DHCP
 * lease and logged `httpd_start` success still could not complete an inbound
 * TCP handshake: it was in modem sleep, advertising a ten-beacon listen
 * interval (~1 s), and consumer access points routinely fail to deliver frames
 * buffered for a dozing station inside a client's SYN-retransmit window. The
 * asymmetry was the bug; this is the rule both paths now read.
 *
 * WHAT THIS IS NOT
 * ----------------
 * It is not a retreat from low power. Deep sleep — not modem sleep — is this
 * product's power story, and the awake windows it applies to are seconds to
 * minutes long. Keeping the radio listening for those, and handing it back to
 * the driver default the moment the server stops, costs far less than a device
 * whose API cannot be reached.
 */
WifiPowerSave ServingWifiPowerSave(bool serving_http);

// -------------------------------------------------------- mode acknowledge --

/**
 * @brief Whether the mode the device is in is the mode somebody asked for.
 *
 * This exists because the tower can want something a sleeping device has not
 * heard yet, and the difference has to be visible rather than smoothed over. A
 * UI that showed the desired mode as though it were the live one would be
 * claiming a sleeping device had obeyed a command it never received.
 */
enum class ModeAckState {
    kAcknowledged = 0,  ///< the device is in the mode that was last requested
    kPendingWake,       ///< a request is recorded and will apply at the next wake
};

const char* ModeAckStateName(ModeAckState state);

// --------------------------------------------------------------- battery --

/**
 * @brief A battery reading, and whether it is fit to show a human.
 *
 * `calibrated` is not decoration. The board reads VBAT through a divider on
 * ADC1 channel 3 and converts with a curve-fitting scheme that only exists if
 * this particular chip was factory-calibrated; when the scheme cannot be
 * created the driver has no volts, only counts. A percentage derived from
 * uncalibrated counts is a number with a percent sign after it, and putting it
 * on a dashboard is worse than showing nothing, because nothing is obviously
 * nothing.
 *
 * So: millivolts and percent are only meaningful when `usable()` is true, and
 * the renderer is expected to emit JSON null for both when it is not.
 */
struct BatteryReading {
    bool present = false;      ///< a reading was attempted and returned something
    bool calibrated = false;   ///< the ADC calibration scheme was available
    bool plausible = false;    ///< the value is inside a single-cell LiPo range
    uint16_t millivolts = 0;
    uint8_t percent = 0;

    /// The one gate the status route and the UI both use.
    bool usable() const { return present && calibrated && plausible; }
};

/**
 * @brief The voltage window a single-cell LiPo can actually be in.
 *
 * Outside it, the reading is measuring something other than the cell: a
 * missing battery floating the divider, a bench supply, or a divider ratio
 * that does not match this board. Reporting a percentage for any of those is
 * how a dashboard ends up confidently wrong.
 */
constexpr uint16_t kBatteryMinPlausibleMv = 2800;
constexpr uint16_t kBatteryMaxPlausibleMv = 4400;

/**
 * @brief Build a reading from what the ADC layer managed to produce.
 *
 * @param read_ok        the driver returned a value at all
 * @param calibration_ok a curve-fitting calibration scheme exists on this chip
 * @param millivolts     the converted cell voltage, already past the divider
 * @param percent        the vendor curve's answer, clamped 0..100
 *
 * This does not recompute the percentage. The vendor curve is what the device
 * menu and the status bar have always shown, and having two answers on one
 * screen would be worse than having one imperfect one. What this adds is the
 * gate that decides whether either number is shown at all.
 */
BatteryReading EvaluateBattery(bool read_ok, bool calibration_ok,
                               uint16_t millivolts, uint8_t percent);

// ------------------------------------------------------------ wake budget --

/**
 * @brief The phases of one auto-saver wake cycle, in order.
 *
 * Named rather than anonymous because the status route reports which one ran
 * out of time, and "we gave up" is a much less useful field report than "we
 * gave up waiting for DHCP".
 */
enum class WakePhase {
    kNetwork = 0,  ///< associate, DHCP, and reach the tower
    kFetch,        ///< pull the update through the existing dashboard protocol
    kRender,       ///< blit and wait for the panel, only when the frame changed
    kSettle,       ///< flush, persist, and stop the radio cleanly
    kCount,
};

constexpr size_t kWakePhaseCount = static_cast<size_t>(WakePhase::kCount);

const char* WakePhaseName(WakePhase phase);

/**
 * @brief Default per-phase caps and the total, in milliseconds.
 *
 * The per-phase caps deliberately sum to more than the total. That is the
 * point: a phase that finishes early donates its unused time to the ones after
 * it, while the total stays the guarantee. Sizing every phase so the sum fit
 * the total would waste most of the budget in the common case where the
 * network is fine and the frame has not changed.
 */
struct WakeBudgetLimits {
    uint32_t total_ms = 165000;    ///< two and three-quarter minutes, the whole
                                   ///< cycle. Sized so a ~90 s tower rendezvous
                                   ///< still leaves room for the render+settle
                                   ///< reserve; see fetch_ms.
    uint32_t network_ms = 45000;   ///< association and DHCP are the slow part
    uint32_t fetch_ms = 90000;     ///< the tower rendezvous: how long the door
                                   ///< is held open for a pushed frame. The
                                   ///< tower's scheduler pulses every 30 s, so
                                   ///< this must be comfortably longer than one
                                   ///< pulse for a push to reliably land inside
                                   ///< the awake window. Was 40 s; a 30 s pulse
                                   ///< against a 40 s window made delivery a
                                   ///< coin flip.
    uint32_t render_ms = 45000;    ///< a full 4-colour refresh is ~20-26 s
    uint32_t settle_ms = 8000;

    uint32_t PhaseCap(WakePhase phase) const;
};

/**
 * @brief One wake cycle's time, held as a single falling total.
 *
 * The invariant, and the only reason this class exists:
 *
 *     For any sequence of phases, the elapsed time from Start() to the moment
 *     Exhausted() first returns true is at most limits.total_ms.
 *
 * Every phase asks RemainingFor() and gets `min(phase cap, what is left of the
 * total)`. Nothing can be granted time the total does not have, so the worst
 * case is the total rather than the sum of the timeouts. A cycle that hits the
 * wall stops where it is and sleeps; it does not carry on into the next phase
 * hoping to make it up.
 *
 * Monotonic milliseconds are supplied by the caller rather than read here, so
 * the host tests can drive a cycle through its whole life in no time at all.
 */
class WakeBudget {
public:
    void Start(int64_t now_ms, const WakeBudgetLimits& limits);

    bool started() const { return started_; }
    int64_t start_ms() const { return start_ms_; }
    const WakeBudgetLimits& limits() const { return limits_; }

    /// Milliseconds of the *total* still unspent. 0 once the cycle is over.
    uint32_t TotalRemainingMs(int64_t now_ms) const;

    /**
     * @brief The size of @p phase's slice, bounded by the total.
     *
     * @return `min(phase cap, total remaining)`.
     *
     * **This is the phase's whole cap, not what is left of it.** It does not
     * know when the phase started and therefore cannot subtract the time the
     * phase has already spent. A caller that wants "how much of this phase is
     * still unspent" wants RemainingInPhase(); using this one for that reads
     * 40 000 thirty seconds into a forty-second phase, and a fetch started on
     * the strength of it would finish ten seconds past the phase's own cap.
     * That misreading was a real defect, which is why this comment is here and
     * why RemainingInPhase() exists.
     */
    uint32_t RemainingFor(WakePhase phase, int64_t now_ms) const;

    /**
     * @brief What is left of @p phase's own cap, given when it started.
     *
     * @return `min(cap - elapsed in phase, total remaining)`, floored at zero.
     *
     * This is the one to ask before starting anything that blocks.
     */
    uint32_t RemainingInPhase(WakePhase phase, int64_t phase_started_ms,
                              int64_t now_ms) const;

    /**
     * @brief Milliseconds until the last moment new *content* work may begin.
     *
     * The total is the guarantee, and the render is part of it: a frame stored
     * at 118 s is a frame the panel starts drawing after the cycle should have
     * ended. So content work — a forecast fetch, a local compose — is allocated
     * against the total minus what the panel may still need, which is the
     * render cap plus the settle cap.
     *
     * @return `total remaining - (render cap + settle cap)`, floored at zero.
     *         Zero means "draw nothing new; converge and sleep".
     */
    uint32_t WorkRemainingMs(int64_t now_ms) const;

    /// True once the total is spent. The caller's cue to stop and sleep.
    bool Exhausted(int64_t now_ms) const;

    /// Record which phase the cycle was in when it ran out, for the status route.
    void NoteExhaustedIn(WakePhase phase);
    bool has_exhausted_phase() const { return has_exhausted_phase_; }
    WakePhase exhausted_phase() const { return exhausted_phase_; }

    void Reset();

private:
    bool started_ = false;
    int64_t start_ms_ = 0;
    WakeBudgetLimits limits_;
    bool has_exhausted_phase_ = false;
    WakePhase exhausted_phase_ = WakePhase::kNetwork;
};

// -------------------------------------------------------- network recovery --

/**
 * @brief Bounded retry for the one part of a wake cycle worth retrying.
 *
 * A Wi-Fi association that fails once often succeeds a few seconds later: the
 * access point was busy, the channel was congested, the DHCP server was slow.
 * Retrying is right. Retrying without a bound is how a device with a dead
 * router stays awake until the battery is flat, which is the failure this
 * whole feature exists to prevent.
 *
 * So every delay is checked against the budget before it is taken, and the
 * last word belongs to the budget rather than to the attempt counter. If the
 * time left will not cover the backoff *and* a worthwhile attempt after it,
 * the answer is to stop and sleep, not to squeeze in an attempt that cannot
 * finish.
 */
class NetworkRecovery {
public:
    /// Attempts per wake cycle, including the first. Four fits the budget.
    static constexpr uint32_t kMaxAttempts = 4;
    /// First backoff; doubles each time, capped at kMaxBackoffMs.
    static constexpr uint32_t kBaseBackoffMs = 2000;
    static constexpr uint32_t kMaxBackoffMs = 16000;
    /**
     * The shortest attempt worth making. Starting an association with less
     * than this left is spending the remaining budget on something that cannot
     * complete, and then sleeping anyway with nothing to show for it.
     */
    static constexpr uint32_t kMinUsefulAttemptMs = 5000;

    void Reset();

    uint32_t attempts() const { return attempts_; }

    /// Record that an attempt is about to be made.
    void NoteAttempt();

    /// Backoff before attempt number @p attempts(), ignoring the budget.
    uint32_t BackoffMs() const;

    /**
     * @brief May another attempt be made, and how long to wait first?
     *
     * @param remaining_ms what WakeBudget says is left of the total.
     * @param delay_out    set to the backoff to observe before trying.
     * @return false when the caller must give up and sleep. @p delay_out is
     *         untouched in that case.
     */
    bool ShouldRetry(uint32_t remaining_ms, uint32_t* delay_out) const;

private:
    uint32_t attempts_ = 0;
};

// ----------------------------------------------------------- wake planning --

/// What the last wake cycle achieved, which decides when the next one is.
enum class CycleOutcome {
    kUpdated = 0,      ///< fetched, and the panel was refreshed
    kUnchanged,        ///< fetched, and the frame already matched: nothing drawn
    kNetworkFailed,    ///< never reached the tower
    kBudgetExhausted,  ///< reached it, ran out of time part way through
    /**
     * @brief A frame was stored, a refresh was asked for, and the panel did not
     *        take it.
     *
     * Distinct from kUnchanged, and the distinction is the whole point. A
     * refusal by the painter — another page holding the screen, a BUSY-pin
     * timeout, a handshake that never saw its completion signal — leaves the
     * store holding a frame the glass has never shown. Recording that as
     * "unchanged" told PlanNextWake the cycle had succeeded, which reset the
     * failure counter and scheduled the next attempt a whole wake interval
     * later. The frame the operator pushed then sat in flash for an hour with
     * nothing on the status route to say why.
     *
     * So it is a failure, it earns RetryDelayMs, and it says what happened.
     */
    kRenderFailed,
};

const char* CycleOutcomeName(CycleOutcome outcome);

/// True when the cycle got what it came for, however little it had to do.
bool CycleSucceeded(CycleOutcome outcome);

/**
 * @brief Everything the next-wake decision depends on.
 *
 * Gathered into one struct so the decision is a pure function of it, and the
 * host tests can put the device in states that are awkward to reach on a desk:
 * a flat battery on a charger at the moment an interactive window expires.
 */
struct WakeInputs {
    Mode mode = Mode::kAutoSaver;
    int32_t wake_interval_min = kDefaultWakeIntervalMin;
    /// Milliseconds left of the interactive window; 0 when it is not open.
    uint32_t interactive_remaining_ms = 0;
    CycleOutcome last_outcome = CycleOutcome::kUpdated;
    /// Consecutive failed cycles before this one. Drives the retry backoff.
    uint32_t consecutive_failures = 0;
    /// USB or a charger is attached, so the battery argument does not apply.
    bool charging = false;
    /// A panel refresh is in flight. Sleeping through one leaves a half-drawn
    /// screen that e-paper will hold until the next refresh.
    bool refresh_in_flight = false;
    /// The AP provisioning portal is up. A person is standing there setting up
    /// Wi-Fi, and a device that slept mid-portal would strand them.
    bool provisioning_portal_open = false;
    /// A gallery slideshow is running, which is mutually exclusive with sleep:
    /// a device that slept between slides would show one photo.
    bool slideshow_active = false;
};

/// What the glue should do when the policy is next consulted.
enum class WakeAction {
    kSleep = 0,   ///< enter deep sleep and come back at wake_in_ms
    kStayAwake,   ///< do not sleep; ask again later
};

/**
 * @brief The decision, and enough of the reasoning to report it honestly.
 */
struct WakePlan {
    WakeAction action = WakeAction::kSleep;
    /// Milliseconds until the timer wake. Only meaningful when action is kSleep.
    uint32_t wake_in_ms = 0;
    /// Why. A short stable token, reported on the status route and logged.
    const char* reason = "";
    /// True when the button is the only thing that will bring the device back,
    /// which is the case for every sleep this policy schedules except that it
    /// also arms the timer. False would mean a remote wake existed; none does.
    bool button_wakes = true;
    /// True when a timer wake is armed as well as the button.
    bool timer_armed = false;
};

/**
 * @brief Decide whether to sleep now and when to come back.
 *
 * The order of the refusals is deliberate. Physical activity beats policy: a
 * half-drawn panel, an open provisioning portal and a running slideshow each
 * keep the device awake regardless of mode, because sleeping through them
 * produces a visibly broken device rather than a flat battery later.
 *
 * Note what is *not* here: the LAN API server being up is not a reason to stay
 * awake in kAutoSaver. It used to be, and that is precisely why a device with
 * the tower connected never slept at all. In the hybrid design the API is
 * expected to be reachable only during a window, and the tower is built to
 * expect that.
 */
WakePlan PlanNextWake(const WakeInputs& in);

/**
 * @brief How long to wait before retrying after a failed cycle.
 *
 * Shorter than the normal interval, because an hour is a long time to keep a
 * stale panel when the router was merely rebooting. Doubling, because a device
 * whose network is genuinely gone must not spend the day waking every ten
 * minutes to fail. Capped at the normal interval, because past that point the
 * retry is just the next scheduled wake.
 */
uint32_t RetryDelayMs(uint32_t consecutive_failures, int32_t wake_interval_min);

/// First retry delay after a single failure.
constexpr uint32_t kFirstRetryMs = 10u * 60u * 1000u;

// ------------------------------------------------------ interactive window --

/**
 * @brief The temporary awake window, and the automatic fall back out of it.
 *
 * The expiry is the feature. A mode that had to be turned off again would be
 * one the user forgets to turn off, and "I left it in interactive mode for a
 * week" is the same outcome as having no low-power mode at all. So interactive
 * is always a window, it always ends, and it never survives into the next boot.
 */
class InteractiveWindow {
public:
    /// Open (or replace) the window. @p minutes must be one of the four.
    void Open(int64_t now_ms, int32_t minutes);
    /// Close it now. The device falls back to kAutoSaver.
    void Close();

    bool IsOpen(int64_t now_ms) const;
    uint32_t RemainingMs(int64_t now_ms) const;
    int32_t minutes() const { return minutes_; }

    void Reset();

private:
    bool open_ = false;
    int64_t opened_ms_ = 0;
    int32_t minutes_ = 0;
};

// --------------------------------------------------------------- the state --

/**
 * @brief The one live answer to "what power state is this device in".
 *
 * Holds the desired mode, the acknowledged mode and the window, and keeps them
 * consistent as requests arrive from three places that do not know about each
 * other: the physical button, the device's own Settings menu, and the config
 * API. The separation between desired and effective is the whole point; see
 * ModeAckState.
 */
class PowerState {
public:
    /// Adopt what was persisted, at boot, before anything else touches it.
    void Init(Mode persisted_mode, int32_t wake_interval_min,
              int32_t interactive_minutes, int64_t now_ms, WakeReason reason);

    Mode effective_mode(int64_t now_ms) const;
    Mode desired_mode() const { return desired_; }
    ModeAckState ack_state(int64_t now_ms) const;

    WakeReason wake_reason() const { return wake_reason_; }
    int32_t wake_interval_min() const { return wake_interval_min_; }
    int32_t interactive_minutes() const { return interactive_minutes_; }
    uint32_t interactive_remaining_ms(int64_t now_ms) const;

    /**
     * @brief Ask for a mode.
     *
     * @param minutes the interactive window length, ignored for other modes.
     * @return false when @p minutes is not one of the four accepted values, in
     *         which case nothing changed.
     *
     * A request that arrives while the device is awake is acknowledged
     * immediately, because the device is right here to obey it. There is no
     * path by which this is called on a sleeping device, which is exactly why
     * the tower has to hold its intent instead.
     */
    bool Request(Mode mode, int32_t minutes, int64_t now_ms);

    /// Set the auto-saver interval. false when it is out of bounds.
    bool SetWakeInterval(int32_t minutes);

    /**
     * @brief Change the window length without touching a window that is open.
     *
     * Separate from Request() because "the next interactive window should be
     * thirty minutes" and "be interactive now" are different sentences, and
     * the config API can say the first on its own. Routing the first through
     * Request() would close a window the user is standing in the middle of,
     * which is the opposite of what changing its *length* asks for.
     *
     * @return false when @p minutes is not one of the four accepted values.
     */
    bool SetInteractiveMinutes(int32_t minutes);

    /**
     * @brief Fold the expiry into the state.
     *
     * @return true when this call is the moment the window closed, so the
     *         caller can log it and re-arm the sleep timer exactly once.
     */
    bool Tick(int64_t now_ms);

    /// The mode to write to NVS. kInteractive is never persisted: a window that
    /// survived a reboot would be a window nobody opened.
    Mode PersistableMode() const;

private:
    Mode desired_ = Mode::kAutoSaver;
    Mode base_ = Mode::kAutoSaver;
    int32_t wake_interval_min_ = kDefaultWakeIntervalMin;
    int32_t interactive_minutes_ = kDefaultInteractiveMinutes;
    InteractiveWindow window_;
    WakeReason wake_reason_ = WakeReason::kPowerOn;
    bool window_was_open_ = false;
};

// ------------------------------------------------------------ JSON output --

/**
 * @brief Render the `power` object for GET /api/v1/dashboard/status.
 *
 * @return bytes written, or 0 when @p out was too small, in which case @p out
 *         is left empty rather than holding a truncated JSON fragment.
 *
 * Rendered here rather than in the route for the same reason the config JSON
 * is: the claim "an uncalibrated battery is reported as null and never as a
 * number" is testable on a host only if the bytes are produced by a portable
 * function. test_power_policy.cc reads the rendered output back.
 */
struct PowerStatus {
    Mode effective = Mode::kAutoSaver;
    Mode desired = Mode::kAutoSaver;
    ModeAckState ack = ModeAckState::kAcknowledged;
    bool awake = true;
    /// True when the device intends to sleep as soon as it is allowed to.
    bool sleep_intent = false;
    uint32_t interactive_remaining_s = 0;
    /// Seconds until the armed timer wake; 0 with timer_armed false means none.
    uint32_t next_wake_in_s = 0;
    bool timer_armed = false;
    /// Unix seconds of the next wake, or 0 when the clock is not set. The route
    /// emits null for 0: a wall-clock time derived from an unset clock is a
    /// fabricated timestamp.
    uint32_t next_wake_epoch = 0;
    int32_t wake_interval_min = kDefaultWakeIntervalMin;
    WakeReason last_wake_reason = WakeReason::kPowerOn;
    /**
     * The last cycle's result, and whether there *is* one.
     *
     * Split in two because a device that has just booted has not completed a
     * cycle, and every value of CycleOutcome is a claim about one that
     * finished. Defaulting the field to kUpdated and reporting it would tell
     * the tower the last update succeeded on a device that has never updated
     * at all — which is exactly the reassuring-but-false answer the whole
     * status route is written to avoid. When this is false the renderer emits
     * JSON null and the tower says "no cycle has finished yet".
     */
    bool last_outcome_known = false;
    CycleOutcome last_outcome = CycleOutcome::kUpdated;
    uint32_t consecutive_failures = 0;
    /**
     * Which phase the budget ran out in, or kCount when it has not.
     *
     * The field report power_policy.h promises: "we gave up waiting for DHCP"
     * is worth a great deal more than "we gave up". Rendered as null when
     * there is nothing to report.
     */
    WakePhase budget_exhausted_phase = WakePhase::kCount;
    BatteryReading battery;
    /// From ChargeStatus. "unknown" when the board did not report.
    const char* charge_state = "unknown";
    bool charging = false;
};

/**
 * @brief One task advances the wake cycle at a time, claimed atomically.
 *
 * Two tasks call into the cycle. The Run() loop ticks it once a second, and the
 * wake-budget backstop calls the same function from the esp_timer task — which
 * is the whole point of the backstop, because the case it exists for is a Run()
 * loop starved by a long panel refresh.
 *
 * "Starved" and "busy" are not the same thing, and the backstop could not tell
 * them apart. A Run() loop blocked inside a bounded forecast fetch is *inside*
 * the cycle, holding no lock, with the backstop free to enter behind it and run
 * the compose, the store and the status record concurrently — against the
 * fields application.h documents as guarded by power_mutex_, on the paths that
 * read them without it.
 *
 * So entry is a compare-exchange, and the backstop's answer to "somebody is
 * already in there" is to re-arm and leave rather than to wait. Waiting would
 * be worse than the race: it would park the esp_timer task behind a fetch for
 * as long as the fetch runs, and that task is the one that has to end the cycle
 * if the fetch never returns.
 *
 * This is deliberately not power_mutex_. That mutex guards short field reads
 * and is taken and released many times within one advance; this guards the
 * advance itself, which spans blocking work and must never be waited on.
 *
 * Refusal is always safe. Every caller is a tick: the cycle is driven by
 * repetition, so a skipped advance costs a second, and the backstop re-arms.
 */
class CycleAdvanceGate {
public:
    /// Claim the right to advance. False means another task is inside the
    /// cycle and this caller must return rather than wait.
    bool TryEnter() {
        bool expected = false;
        return busy_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }

    void Leave() { busy_.store(false, std::memory_order_release); }

    bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> busy_{false};
};

/**
 * @brief A scoped claim on a CycleAdvanceGate. `entered()` says whether it got
 *        one.
 *
 * ServiceWakeCycle has more than a dozen early returns, one per phase and per
 * refusal. Releasing by hand would mean getting every one of them right, and
 * the one that was missed would wedge the device: the gate would stay claimed,
 * every later tick would refuse, and the cycle would never reach sleep.
 */
class CycleAdvanceClaim {
public:
    explicit CycleAdvanceClaim(CycleAdvanceGate& gate)
        : gate_(gate), entered_(gate.TryEnter()) {}
    ~CycleAdvanceClaim() { if (entered_) gate_.Leave(); }

    CycleAdvanceClaim(const CycleAdvanceClaim&) = delete;
    CycleAdvanceClaim& operator=(const CycleAdvanceClaim&) = delete;

    bool entered() const { return entered_; }

private:
    CycleAdvanceGate& gate_;
    bool entered_;
};

size_t RenderPowerJson(const PowerStatus& status, char* out, size_t out_len);

/// Bytes a caller should reserve for RenderPowerJson.
/// Raised from 512 when last_outcome gained a null form and the exhausted
/// phase was added, so the headroom over the largest renderable object stayed
/// what it was rather than quietly shrinking by the size of the new fields.
/// test_the_rendered_power_json_fits_the_advertised_buffer pins that.
constexpr size_t kPowerJsonMax = 640;

}  // namespace power

#endif  // COMMON_POWER_POLICY_H
