/**
 * @file autonomy_policy.cc
 * @brief Implementation of the local-render and fetch rules.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See autonomy_policy.h for the rule and the order of its refusals. No ESP-IDF
 * headers: the host suite drives the whole truth table through this exact file.
 */

#include "autonomy_policy.h"

namespace autonomy {

const char* const kReasonTowerFrame = "tower_frame";
const char* const kReasonDisabled = "autonomy_disabled";
const char* const kReasonNoProfile = "no_profile";
const char* const kReasonNotAsked = "no_device_module";
const char* const kReasonWaitingForTower = "waiting_for_tower";
const char* const kReasonTowerFrameFresh = "tower_frame_fresh";
const char* const kReasonInteractive = "interactive_window";
const char* const kReasonAllowed = "allowed";

const char* const kFetchNotWanted = "no_weather_module";
const char* const kFetchNoCache = "no_cache";
const char* const kFetchTooSoon = "too_soon";
const char* const kFetchDue = "due";
const char* const kFetchClockUnknown = "clock_unknown";

const char* OriginName(Origin origin) {
    switch (origin) {
        case Origin::kNone: return "none";
        case Origin::kTower: return "tower";
        case Origin::kLocal: return "local";
        case Origin::kUnknown: return "unknown";
    }
    return "none";
}

LocalRenderVerdict EvaluateLocalRender(const PolicyInputs& in) {
    LocalRenderVerdict v;

    // 1. A tower frame always wins, immediately. Checked first and
    //    unconditionally — before the kill-switch, before the profile — because
    //    a PUT is an act of the operator and none of the autonomy state has any
    //    bearing on whether it is displayed.
    if (in.tower_frame_arrived) {
        v.allowed = false;
        v.tower_frame_wins = true;
        v.reason = kReasonTowerFrame;
        return v;
    }

    // 2. The kill-switch. The profile is kept, but nothing in it applies.
    if (!in.autonomy_enabled) {
        v.reason = kReasonDisabled;
        return v;
    }
    if (!in.profile_present) {
        v.reason = kReasonNoProfile;
        return v;
    }

    // 3. The device must have been asked. Device mode is an instruction to
    //    compose; Auto mode is an instruction to compose only once the tower
    //    has had, and missed, its chance.
    if (!in.has_device_module && !in.has_auto_module) {
        v.reason = kReasonNotAsked;
        return v;
    }
    if (!in.has_device_module && !in.tower_wait_expired) {
        v.reason = kReasonWaitingForTower;
        return v;
    }

    // 5. (checked before 4, because a person at the device outranks arithmetic
    //    about frame ages) The interactive window suspends replacement.
    if (in.interactive_window_open) {
        v.reason = kReasonInteractive;
        return v;
    }

    // 4. A fresh tower frame is not overwritten. Only tower frames get this
    //    protection: a local frame replacing an older local frame is the normal
    //    hourly update and must not be blocked by its own predecessor.
    //
    // kUnknown is treated as a tower frame here, deliberately. When the device
    // cannot say where what is on the glass came from, the two mistakes are not
    // symmetrical: painting over the operator's frame is visible and wrong,
    // while deferring a local render costs one wake and is recoverable.
    if (in.displayed_origin == Origin::kTower ||
        in.displayed_origin == Origin::kUnknown) {
        if (!in.displayed_age_known) {
            // Without a clock the age is not a measurement. Treating an unknown
            // age as "old" would let a device that has just booted paint over a
            // frame the tower pushed seconds earlier; treating it as "fresh" at
            // worst delays a local render by one wake, which is recoverable.
            v.reason = kReasonTowerFrameFresh;
            return v;
        }
        const int64_t threshold =
            static_cast<int64_t>(in.wake_interval_min) * 60 + kTowerFrameGraceS;
        if (in.displayed_age_s < threshold) {
            v.reason = kReasonTowerFrameFresh;
            return v;
        }
    }

    v.allowed = true;
    v.reason = kReasonAllowed;
    return v;
}

FetchVerdict ShouldFetchWeather(const FetchInputs& in) {
    FetchVerdict v;
    if (!in.wants_weather) {
        v.reason = kFetchNotWanted;
        return v;
    }
    if (!in.has_cache) {
        // Nothing to draw from. This is the one case where a fetch happens
        // regardless of interval, because the interval exists to stop
        // *refreshing* too often, not to stop starting.
        v.fetch = true;
        v.reason = kFetchNoCache;
        return v;
    }
    if (!in.clock_set) {
        // See the header: with no clock the interval is unanswerable, and a
        // device in this state has almost certainly just booted. One fetch also
        // gets it a clock.
        v.fetch = true;
        v.reason = kFetchClockUnknown;
        return v;
    }
    const int64_t age_s = in.now_epoch - in.cache_fetched_epoch;
    const int64_t interval_s = static_cast<int64_t>(in.min_fetch_interval_min) * 60;
    if (age_s < 0) {
        // The clock moved backwards — SNTP correcting a drifted RTC. The cache
        // is not from the future; the timestamp comparison is simply not usable,
        // so refetch rather than trust it.
        v.fetch = true;
        v.reason = kFetchDue;
        return v;
    }
    if (age_s < interval_s) {
        v.reason = kFetchTooSoon;
        return v;
    }
    v.fetch = true;
    v.reason = kFetchDue;
    return v;
}

bool NeedsNetwork(const PolicyInputs& policy, const FetchVerdict& fetch,
                  int32_t tower_wait_s) {
    // A device that is not doing autonomy at all is an ordinary push target and
    // behaves exactly as it did before this feature existed: it brings up the
    // radio every wake.
    if (!policy.autonomy_enabled || !policy.profile_present) return true;
    if (fetch.fetch) return true;
    if (tower_wait_s > 0) return true;
    // Everything this wake needs is already on the device. This is the case
    // that makes Device mode worth having.
    return false;
}

}  // namespace autonomy
