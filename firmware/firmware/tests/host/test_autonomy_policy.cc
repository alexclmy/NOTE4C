/**
 * @file test_autonomy_policy.cc
 * @brief Host tests for the local-render and fetch rules.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The truth table, driven exhaustively. This is the file that exists so nobody
 * has to build a device and arrange for the tower to be ninety seconds late
 * while somebody holds the button in order to find out what happens.
 *
 * Two claims are tested harder than the rest:
 *
 *  1. **A tower frame always wins.** Asserted across every combination of the
 *     other eight inputs, because "always" is either true for all 256 of them
 *     or it is not the word for it.
 *
 *  2. **A fresh tower frame is never overwritten.** This is the rule that stops
 *     the panel flickering between two sources, and the cost of getting it
 *     wrong is measured in twenty-five-second refreshes and battery.
 */

#include "common/autonomy_policy.h"

#include <stdio.h>
#include <string.h>

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

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

/// A device doing ordinary autonomy: enabled, profiled, asked, nothing in the
/// way. Each test names only what it changes from this.
PolicyInputs Ready() {
    PolicyInputs in;
    in.autonomy_enabled = true;
    in.profile_present = true;
    in.has_device_module = true;
    in.has_auto_module = false;
    in.tower_frame_arrived = false;
    in.tower_wait_expired = true;
    in.displayed_origin = Origin::kLocal;
    in.displayed_age_s = 7200;
    in.displayed_age_known = true;
    in.wake_interval_min = 60;
    in.interactive_window_open = false;
    return in;
}

}  // namespace

// ------------------------------------------------------ the tower's primacy --

static void test_a_tower_frame_wins_whatever_else_is_true() {
    // "Always" is either true for every combination or it is the wrong word.
    // Eight independent booleans, all 256 combinations.
    int checked = 0;
    for (int bits = 0; bits < 256; ++bits) {
        PolicyInputs in;
        in.tower_frame_arrived = true;
        in.autonomy_enabled = (bits & 1) != 0;
        in.profile_present = (bits & 2) != 0;
        in.has_device_module = (bits & 4) != 0;
        in.has_auto_module = (bits & 8) != 0;
        in.tower_wait_expired = (bits & 16) != 0;
        in.interactive_window_open = (bits & 32) != 0;
        in.displayed_age_known = (bits & 64) != 0;
        in.displayed_origin = (bits & 128) != 0 ? Origin::kTower : Origin::kLocal;
        in.displayed_age_s = 0;

        const LocalRenderVerdict v = EvaluateLocalRender(in);
        if (!v.tower_frame_wins || v.allowed ||
            strcmp(v.reason, kReasonTowerFrame) != 0) {
            ++g_failures;
            printf("  FAIL in %s: bits=%d did not yield the tower frame\n", g_current,
                   bits);
        }
        ++g_checks;
        ++checked;
    }
    CHECK(checked == 256);
}

// ------------------------------------------------------------ the refusals --

static void test_the_kill_switch_stops_everything() {
    PolicyInputs in = Ready();
    in.autonomy_enabled = false;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK(!v.tower_frame_wins);
    CHECK_STR(v.reason, kReasonDisabled);
}

static void test_no_profile_means_no_local_render() {
    PolicyInputs in = Ready();
    in.profile_present = false;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonNoProfile);
}

static void test_a_profile_that_asks_for_nothing_renders_nothing() {
    PolicyInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = false;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonNotAsked);
}

static void test_auto_mode_waits_for_the_tower_before_composing() {
    // Auto means "the tower first". Composing before the wait was over would be
    // racing the thing this mode exists to defer to.
    PolicyInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = true;
    in.tower_wait_expired = false;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonWaitingForTower);
}

static void test_auto_mode_composes_once_the_wait_expires() {
    PolicyInputs in = Ready();
    in.has_device_module = false;
    in.has_auto_module = true;
    in.tower_wait_expired = true;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(v.allowed);
    CHECK_STR(v.reason, kReasonAllowed);
}

static void test_device_mode_does_not_wait_for_the_tower() {
    // Device mode is an instruction to compose, not a fallback.
    PolicyInputs in = Ready();
    in.has_device_module = true;
    in.has_auto_module = false;
    in.tower_wait_expired = false;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(v.allowed);
}

static void test_the_interactive_window_suspends_local_replacement() {
    // Somebody is standing at the device pressing its button. Repainting the
    // dashboard under them is not an improvement.
    PolicyInputs in = Ready();
    in.interactive_window_open = true;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonInteractive);
}

// ------------------------------------------------- the anti-flicker rule --

static void test_a_fresh_tower_frame_is_not_overwritten() {
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kTower;
    in.wake_interval_min = 60;
    // One interval plus the grace is 3900 s. Anything younger is fresh.
    in.displayed_age_s = 3899;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonTowerFrameFresh);
}

static void test_a_tower_frame_past_the_interval_and_grace_may_be_replaced() {
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kTower;
    in.wake_interval_min = 60;
    in.displayed_age_s = 3900;  // exactly at the threshold
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(v.allowed);
}

static void test_the_threshold_follows_the_wake_interval() {
    // A device that wakes every fifteen minutes must not wait an hour before it
    // is allowed to redraw, and one that wakes daily must not redraw hourly.
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kTower;

    in.wake_interval_min = 15;
    in.displayed_age_s = 15 * 60 + kTowerFrameGraceS - 1;
    CHECK(!EvaluateLocalRender(in).allowed);
    in.displayed_age_s = 15 * 60 + kTowerFrameGraceS;
    CHECK(EvaluateLocalRender(in).allowed);

    in.wake_interval_min = 1440;
    in.displayed_age_s = 1440 * 60 + kTowerFrameGraceS - 1;
    CHECK(!EvaluateLocalRender(in).allowed);
    in.displayed_age_s = 1440 * 60 + kTowerFrameGraceS;
    CHECK(EvaluateLocalRender(in).allowed);
}

static void test_a_local_frame_is_never_protected_from_its_successor() {
    // Only tower frames get the freshness protection. A local frame replacing
    // an older local frame is the normal hourly update, and blocking it would
    // freeze the panel permanently after the first local render.
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kLocal;
    in.displayed_age_s = 0;
    CHECK(EvaluateLocalRender(in).allowed);
}

static void test_an_empty_panel_may_always_be_drawn_on() {
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kNone;
    in.displayed_age_s = 0;
    CHECK(EvaluateLocalRender(in).allowed);
}

static void test_an_unmeasurable_age_is_treated_as_fresh() {
    // Without a clock the age is not a measurement. Treating an unknown age as
    // "old" would let a just-booted device paint over a frame the tower pushed
    // seconds earlier; treating it as fresh costs at most one wake.
    PolicyInputs in = Ready();
    in.displayed_origin = Origin::kTower;
    in.displayed_age_known = false;
    in.displayed_age_s = 999999;
    const LocalRenderVerdict v = EvaluateLocalRender(in);
    CHECK(!v.allowed);
    CHECK_STR(v.reason, kReasonTowerFrameFresh);
}

static void test_every_refusal_carries_a_distinct_reason() {
    // The field report is the point: "we did not draw because the tower frame
    // was fresh" is worth a great deal more than "we did not draw".
    const char* reasons[] = {kReasonTowerFrame,      kReasonDisabled,
                             kReasonNoProfile,       kReasonNotAsked,
                             kReasonWaitingForTower, kReasonTowerFrameFresh,
                             kReasonInteractive,     kReasonAllowed};
    const int n = static_cast<int>(sizeof(reasons) / sizeof(reasons[0]));
    for (int i = 0; i < n; ++i) {
        CHECK(reasons[i][0] != '\0');
        for (int j = i + 1; j < n; ++j) {
            CHECK(strcmp(reasons[i], reasons[j]) != 0);
        }
    }
}

// ------------------------------------------------------------- the fetch --

static void test_no_weather_module_means_no_fetch() {
    FetchInputs in;
    in.wants_weather = false;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(!v.fetch);
    CHECK_STR(v.reason, kFetchNotWanted);
}

static void test_an_empty_cache_is_always_worth_filling() {
    // The interval exists to stop refreshing too often, not to stop starting.
    FetchInputs in;
    in.wants_weather = true;
    in.has_cache = false;
    in.clock_set = true;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(v.fetch);
    CHECK_STR(v.reason, kFetchNoCache);
}

static void test_a_recent_fetch_is_not_repeated() {
    FetchInputs in;
    in.wants_weather = true;
    in.has_cache = true;
    in.clock_set = true;
    in.min_fetch_interval_min = 30;
    in.now_epoch = 10000;
    in.cache_fetched_epoch = 10000 - 29 * 60;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(!v.fetch);
    CHECK_STR(v.reason, kFetchTooSoon);
}

static void test_a_fetch_at_exactly_the_interval_is_due() {
    FetchInputs in;
    in.wants_weather = true;
    in.has_cache = true;
    in.clock_set = true;
    in.min_fetch_interval_min = 30;
    in.now_epoch = 10000;
    in.cache_fetched_epoch = 10000 - 30 * 60;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(v.fetch);
    CHECK_STR(v.reason, kFetchDue);
}

static void test_an_unset_clock_fetches_once() {
    FetchInputs in;
    in.wants_weather = true;
    in.has_cache = true;
    in.clock_set = false;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(v.fetch);
    CHECK_STR(v.reason, kFetchClockUnknown);
}

static void test_a_clock_that_moved_backwards_refetches() {
    // SNTP correcting a drifted RTC makes the cache look like it came from the
    // future. That is not a reason to trust it for another half hour.
    FetchInputs in;
    in.wants_weather = true;
    in.has_cache = true;
    in.clock_set = true;
    in.min_fetch_interval_min = 30;
    in.now_epoch = 10000;
    in.cache_fetched_epoch = 20000;
    const FetchVerdict v = ShouldFetchWeather(in);
    CHECK(v.fetch);
    CHECK_STR(v.reason, kFetchDue);
}

// ------------------------------------------------------------ the radio --

static void test_a_wake_with_nothing_to_fetch_and_no_wait_skips_the_radio() {
    // This is the case that makes Device mode worth having: everything the wake
    // needs is already on the device, so it never associates at all.
    PolicyInputs policy = Ready();
    FetchVerdict fetch;
    fetch.fetch = false;
    CHECK(!NeedsNetwork(policy, fetch, 0));
}

static void test_a_tower_wait_brings_the_radio_up() {
    PolicyInputs policy = Ready();
    FetchVerdict fetch;
    fetch.fetch = false;
    CHECK(NeedsNetwork(policy, fetch, 30));
}

static void test_a_due_fetch_brings_the_radio_up() {
    PolicyInputs policy = Ready();
    FetchVerdict fetch;
    fetch.fetch = true;
    CHECK(NeedsNetwork(policy, fetch, 0));
}

static void test_a_device_without_autonomy_behaves_exactly_as_before() {
    // No profile, or the kill-switch off, means an ordinary push target: the
    // radio comes up every wake, as it did before this feature existed.
    PolicyInputs policy = Ready();
    FetchVerdict fetch;
    fetch.fetch = false;

    policy.autonomy_enabled = false;
    CHECK(NeedsNetwork(policy, fetch, 0));

    policy.autonomy_enabled = true;
    policy.profile_present = false;
    CHECK(NeedsNetwork(policy, fetch, 0));
}

static void test_origin_names_are_stable() {
    CHECK_STR(OriginName(Origin::kNone), "none");
    CHECK_STR(OriginName(Origin::kTower), "tower");
    CHECK_STR(OriginName(Origin::kLocal), "local");
}

int main() {
    RUN(test_a_tower_frame_wins_whatever_else_is_true);

    RUN(test_the_kill_switch_stops_everything);
    RUN(test_no_profile_means_no_local_render);
    RUN(test_a_profile_that_asks_for_nothing_renders_nothing);
    RUN(test_auto_mode_waits_for_the_tower_before_composing);
    RUN(test_auto_mode_composes_once_the_wait_expires);
    RUN(test_device_mode_does_not_wait_for_the_tower);
    RUN(test_the_interactive_window_suspends_local_replacement);

    RUN(test_a_fresh_tower_frame_is_not_overwritten);
    RUN(test_a_tower_frame_past_the_interval_and_grace_may_be_replaced);
    RUN(test_the_threshold_follows_the_wake_interval);
    RUN(test_a_local_frame_is_never_protected_from_its_successor);
    RUN(test_an_empty_panel_may_always_be_drawn_on);
    RUN(test_an_unmeasurable_age_is_treated_as_fresh);
    RUN(test_every_refusal_carries_a_distinct_reason);

    RUN(test_no_weather_module_means_no_fetch);
    RUN(test_an_empty_cache_is_always_worth_filling);
    RUN(test_a_recent_fetch_is_not_repeated);
    RUN(test_a_fetch_at_exactly_the_interval_is_due);
    RUN(test_an_unset_clock_fetches_once);
    RUN(test_a_clock_that_moved_backwards_refetches);

    RUN(test_a_wake_with_nothing_to_fetch_and_no_wait_skips_the_radio);
    RUN(test_a_tower_wait_brings_the_radio_up);
    RUN(test_a_due_fetch_brings_the_radio_up);
    RUN(test_a_device_without_autonomy_behaves_exactly_as_before);
    RUN(test_origin_names_are_stable);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
