/**
 * @file autonomy_policy.h
 * @brief The one rule that decides whether a locally composed panel is allowed
 *        to replace what is on the glass, and whether this wake spends any
 *        radio at all.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why this is a separate file with no ESP-IDF in it
 * ------------------------------------------------
 * Because the alternative is the rule living inside ServiceWakeCycle, tangled
 * with sockets and timers, where the only way to ask "what happens when the
 * tower is late and the interactive window is open and the cached frame is an
 * hour old" is to build a device and arrange all three. Here it is a pure
 * function of a struct, and the host suite drives the whole truth table in
 * milliseconds.
 *
 * THE RULE, in the order the refusals are applied
 * -----------------------------------------------
 * 1. **A tower frame always wins, immediately.** A PUT is an act of the
 *    operator. It is never queued behind a local render, never compared
 *    against one, and never delayed by the autonomy path.
 *
 * 2. **The kill-switch is absolute.** `autonomy.enabled = false` makes the
 *    device a pure push target again. The stored profile is kept, not erased,
 *    so turning it back on does not require a re-push — but nothing in the
 *    profile has any effect while it is off.
 *
 * 3. **The device must have been asked.** A local render happens only when the
 *    profile actually puts a module in Device mode, or in Auto mode *and* this
 *    wake's tower wait expired with no frame. Auto means "the tower first",
 *    and a device that composed before the wait was over would be racing the
 *    thing it is supposed to defer to.
 *
 * 4. **A fresh tower frame is not overwritten.** If the panel is showing a
 *    tower frame younger than one wake interval plus five minutes of grace,
 *    the local render is not displayed. This is the rule that stops the panel
 *    flickering between two sources: without it, a device whose tower went
 *    away for ninety seconds would redraw the whole screen locally and then
 *    redraw it again from the tower a minute later, at twenty-five seconds and
 *    a meaningful slice of the battery each time.
 *
 * 5. **The interactive window suspends local replacement.** Somebody is
 *    standing in front of the device pressing its button. Repainting the
 *    dashboard under them is not an improvement.
 *
 * What is NOT in here
 * -------------------
 * Deduplication. The A/B store already returns kDuplicate for a payload it is
 * already holding, and that is the right place for it: the check has to happen
 * against the bytes actually stored, not against a hash this module was told
 * about. Re-implementing it here would be a second opinion that could differ
 * from the one that decides whether flash is written.
 */

#ifndef COMMON_AUTONOMY_POLICY_H
#define COMMON_AUTONOMY_POLICY_H

#include <stdint.h>

namespace autonomy {

/// Where a frame came from.
enum class Origin : uint8_t {
    kNone = 0,  ///< nothing valid has ever been displayed
    kTower,     ///< pushed over the API
    kLocal,     ///< composed by this device
    /**
     * @brief Something is there, and this device cannot say where it came from.
     *
     * The honest answer, and a real state rather than a placeholder: a frame is
     * on the glass but the record it was drawn from is no longer the record the
     * store holds, so the origin bit that would answer the question belongs to a
     * different frame. Reported as `unknown` rather than guessed — an origin
     * asserted from the wrong record is worse than no origin, because the
     * arbitration rule below acts on it.
     */
    kUnknown,
};

const char* OriginName(Origin origin);

/**
 * @brief Grace added to the wake interval before a tower frame counts as old.
 *
 * Five minutes, and the number matters. The tower's scheduler aims at the
 * predicted wake and probes around it; a device that treated a frame as stale
 * the instant one interval had elapsed would start composing locally during
 * the very window the tower was trying to reach it in.
 */
constexpr int32_t kTowerFrameGraceS = 5 * 60;

/// Everything the render decision depends on.
struct PolicyInputs {
    /// The config kill-switch. False makes every other field irrelevant.
    bool autonomy_enabled = false;
    /// A valid profile is stored and parsed.
    bool profile_present = false;

    /// The profile puts at least one module in Device mode.
    bool has_device_module = false;
    /// The profile puts at least one module in Auto mode.
    bool has_auto_module = false;

    /// A tower frame arrived during this wake.
    bool tower_frame_arrived = false;
    /// This wake's tower wait has elapsed.
    bool tower_wait_expired = false;

    Origin displayed_origin = Origin::kNone;
    /// Age of what is displayed, in seconds. Meaningless when origin is kNone.
    int64_t displayed_age_s = 0;
    /// False when the clock has never been set, in which case the age above is
    /// not a measurement and must not be treated as one.
    bool displayed_age_known = false;

    int32_t wake_interval_min = 60;
    bool interactive_window_open = false;
};

/// Stable reason tokens. Reported on the status route and logged, so the field
/// report is "we did not draw because the tower frame was fresh" rather than
/// "we did not draw".
extern const char* const kReasonTowerFrame;
extern const char* const kReasonDisabled;
extern const char* const kReasonNoProfile;
extern const char* const kReasonNotAsked;
extern const char* const kReasonWaitingForTower;
extern const char* const kReasonTowerFrameFresh;
extern const char* const kReasonInteractive;
extern const char* const kReasonAllowed;

struct LocalRenderVerdict {
    /// True when the device may compose and, if the bytes changed, display.
    bool allowed = false;
    /// True when a tower frame arrived this wake and simply wins.
    bool tower_frame_wins = false;
    const char* reason = "";
};

/// Apply the rule. Pure; every input is in @p in.
LocalRenderVerdict EvaluateLocalRender(const PolicyInputs& in);

// ------------------------------------------------------------- the fetch --

/**
 * @brief Whether this wake may spend budget on a forecast.
 *
 * Separate from the render decision because they answer different questions at
 * different moments: this one runs before the radio comes up, and the render
 * decision runs after the tower has had its chance.
 */
struct FetchInputs {
    /// The profile has a module that needs the forecast.
    bool wants_weather = false;
    /// A forecast is in the A/B cache and parsed.
    bool has_cache = false;
    int64_t cache_fetched_epoch = 0;
    int64_t now_epoch = 0;
    bool clock_set = false;
    int32_t min_fetch_interval_min = 30;
};

extern const char* const kFetchNotWanted;
extern const char* const kFetchNoCache;
extern const char* const kFetchTooSoon;
extern const char* const kFetchDue;
extern const char* const kFetchClockUnknown;

struct FetchVerdict {
    bool fetch = false;
    const char* reason = "";
};

/**
 * @brief Decide whether to fetch.
 *
 * The interesting case is an unset clock. Without one, "thirty minutes since
 * the last fetch" is not a question this device can answer, and both possible
 * mistakes are real: never fetching leaves the panel on a stale cache forever,
 * and always fetching turns every wake into a radio wake. The answer taken
 * here is to fetch once — a device with no clock and a cache has almost
 * certainly just booted, and one fetch is what gets it a clock via SNTP as
 * well as a forecast.
 */
FetchVerdict ShouldFetchWeather(const FetchInputs& in);

/**
 * @brief Does this wake need the radio at all?
 *
 * The answer is no more often than it looks, and that is the whole energy
 * argument for Device mode: a wake with no fetch due and no tower wait is a
 * wake that composes from the cache and goes back to sleep without ever
 * associating.
 *
 * @param tower_wait_s the profile's tower wait for this wake.
 */
bool NeedsNetwork(const PolicyInputs& policy, const FetchVerdict& fetch,
                  int32_t tower_wait_s);

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_POLICY_H
