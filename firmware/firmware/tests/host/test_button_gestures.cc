/**
 * @file test_button_gestures.cc
 * @brief Host tests for the real button_gestures translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The event order used here is the one the driver produces
 * (main/boards/common/button.cc over iot_button): PRESS_DOWN, then either
 * LONG_PRESS_START while held, then PRESS_UP, then SINGLE_CLICK. Tests that
 * omit LONG_PRESS_START are modelling the case where the driver never sends
 * it and the release has to notice the hold by itself.
 */

#include "common/button_gestures.h"

#include <cstdio>
#include <cstring>

using namespace gesture;

// ------------------------------------------------------------ mini harness --

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

#define RUN(fn)                                \
    do {                                       \
        g_current_test = #fn;                  \
        const int before = g_failures;         \
        fn();                                  \
        std::printf("%-46s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

static void CheckSemantic(Semantic actual, Semantic expected, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] expected %s, got %s\n",
                    g_current_test, line, SemanticName(expected), SemanticName(actual));
    }
}

#define CHECK_SEM(actual, expected) CheckSemantic((actual), (expected), __LINE__)

static constexpr int64_t kLongMs = 1000;

// ------------------------------------------------------------- simple taps --

static void test_short_press_is_a_click() {
    Recognizer r;
    CHECK_SEM(r.OnPressDown(Button::kUp, 0), Semantic::kNone);
    CHECK_SEM(r.OnPressUp(Button::kUp, 120), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kUp, 120), Semantic::kUpClick);
}

static void test_each_button_reports_its_own_click() {
    Recognizer r;
    r.OnPressDown(Button::kDown, 0);
    r.OnPressUp(Button::kDown, 50);
    CHECK_SEM(r.OnClick(Button::kDown, 50), Semantic::kDownClick);
    r.OnPressDown(Button::kConfirm, 100);
    r.OnPressUp(Button::kConfirm, 150);
    CHECK_SEM(r.OnClick(Button::kConfirm, 150), Semantic::kConfirmClick);
}

static void test_release_just_below_threshold_is_a_click() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnPressUp(Button::kUp, kLongMs - 1), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kUp, kLongMs - 1), Semantic::kUpClick);
}

static void test_hold_of_exactly_the_threshold_is_long() {
    // The board file used >=, and changing that would move the feel of every
    // button, so the boundary is pinned here.
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnPressUp(Button::kUp, kLongMs), Semantic::kUpLong);
}

// ------------------------------------------------------ long-press handling --

static void test_long_press_from_the_driver_fires_once() {
    Recognizer r;
    r.OnPressDown(Button::kDown, 0);
    CHECK_SEM(r.OnLongPress(Button::kDown, kLongMs), Semantic::kDownLong);
    // A second LONG_PRESS_START for the same hold must not fire again.
    CHECK_SEM(r.OnLongPress(Button::kDown, kLongMs + 500), Semantic::kNone);
    CHECK_SEM(r.OnPressUp(Button::kDown, kLongMs + 800), Semantic::kNone);
}

static void test_long_press_detected_on_release_fires_once() {
    Recognizer r;
    r.OnPressDown(Button::kDown, 0);
    // No LONG_PRESS_START from the driver at all.
    CHECK_SEM(r.OnPressUp(Button::kDown, kLongMs + 400), Semantic::kDownLong);
    CHECK_SEM(r.OnClick(Button::kDown, kLongMs + 400), Semantic::kNone);
}

static void test_click_is_suppressed_after_a_driver_long_press() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kUpLong);
    r.OnPressUp(Button::kUp, kLongMs + 200);
    CHECK_SEM(r.OnClick(Button::kUp, kLongMs + 200), Semantic::kNone);
}

static void test_boot_long_press_also_suppresses_its_click() {
    // The board file suppressed clicks for UP and DOWN but never for BOOT, so
    // a BOOT hold produced both the long press and a click. That was harmless
    // while BOOT-long only toggled AP transfer. It is not harmless now: BOOT
    // long is push-to-talk and BOOT click opens the quick switch, so one hold
    // would arm the microphone and open a menu.
    Recognizer r;
    r.OnPressDown(Button::kConfirm, 0);
    CHECK_SEM(r.OnLongPress(Button::kConfirm, kLongMs), Semantic::kConfirmLong);
    r.OnPressUp(Button::kConfirm, kLongMs + 100);
    CHECK_SEM(r.OnClick(Button::kConfirm, kLongMs + 100), Semantic::kNone);
}

static void test_suppression_lasts_exactly_one_click() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnLongPress(Button::kUp, kLongMs);
    r.OnPressUp(Button::kUp, kLongMs + 10);
    CHECK_SEM(r.OnClick(Button::kUp, kLongMs + 10), Semantic::kNone);
    // The next, separate tap must work normally.
    r.OnPressDown(Button::kUp, 3000);
    r.OnPressUp(Button::kUp, 3100);
    CHECK_SEM(r.OnClick(Button::kUp, 3100), Semantic::kUpClick);
}

static void test_press_down_clears_stale_suppression() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnLongPress(Button::kUp, kLongMs);
    CHECK(r.ClickSuppressed(Button::kUp));
    // A fresh press starts clean even if the click for the previous hold was
    // never delivered.
    r.OnPressDown(Button::kUp, 5000);
    CHECK(!r.ClickSuppressed(Button::kUp));
    r.OnPressUp(Button::kUp, 5100);
    CHECK_SEM(r.OnClick(Button::kUp, 5100), Semantic::kUpClick);
}

static void test_release_without_press_down_reports_nothing() {
    // Wake from deep sleep can deliver a release for a press the recogniser
    // never saw. Measuring a hold from -1 would report a very long one.
    Recognizer r;
    CHECK_SEM(r.OnPressUp(Button::kUp, 999999), Semantic::kNone);
    CHECK(!r.IsHeld(Button::kUp));
}

// ---------------------------------------------------------------- the combo --

static void test_combo_fires_once_when_both_are_held() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 50);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kComboLong);
    // The second key's own long press must not open the access point again.
    CHECK_SEM(r.OnLongPress(Button::kDown, kLongMs + 50), Semantic::kNone);
}

static void test_combo_does_not_double_fire_on_release() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 10);
    CHECK_SEM(r.OnLongPress(Button::kDown, kLongMs), Semantic::kComboLong);
    CHECK_SEM(r.OnPressUp(Button::kUp, kLongMs + 300), Semantic::kNone);
    CHECK_SEM(r.OnPressUp(Button::kDown, kLongMs + 400), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kUp, kLongMs + 300), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kDown, kLongMs + 400), Semantic::kNone);
}

static void test_combo_detected_on_release_when_the_driver_stays_silent() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 20);
    CHECK_SEM(r.OnPressUp(Button::kUp, kLongMs + 100), Semantic::kComboLong);
    CHECK_SEM(r.OnPressUp(Button::kDown, kLongMs + 200), Semantic::kNone);
}

static void test_staggered_release_after_a_combo_adds_nothing() {
    // The regression this pins: the second key's release measured its own hold
    // and reported a single long press right after the combo. With UP-long as
    // Back and DOWN-long as Home, that would have walked away from the Wi-Fi
    // access point the combo had just opened.
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 20);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kComboLong);
    CHECK_SEM(r.OnPressUp(Button::kUp, kLongMs + 100), Semantic::kNone);
    CHECK_SEM(r.OnPressUp(Button::kDown, kLongMs + 900), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kDown, kLongMs + 900), Semantic::kNone);
}

static void test_single_key_long_press_is_not_a_combo() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kUpLong);
    r.OnPressUp(Button::kUp, kLongMs + 10);

    r.OnPressDown(Button::kDown, 4000);
    CHECK_SEM(r.OnLongPress(Button::kDown, 4000 + kLongMs), Semantic::kDownLong);
}

static void test_combo_rearms_for_the_next_gesture() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 10);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kComboLong);
    r.OnPressUp(Button::kUp, kLongMs + 100);
    r.OnPressUp(Button::kDown, kLongMs + 150);
    r.OnClick(Button::kUp, kLongMs + 100);
    r.OnClick(Button::kDown, kLongMs + 150);

    // A completely new two-key hold must be able to fire again.
    r.OnPressDown(Button::kUp, 9000);
    r.OnPressDown(Button::kDown, 9020);
    CHECK_SEM(r.OnLongPress(Button::kDown, 9000 + kLongMs), Semantic::kComboLong);
}

static void test_second_key_arriving_after_the_first_long_press_is_separate() {
    // UP is held long alone, then DOWN joins. The UP long has already fired,
    // so DOWN's own long press is a combo, not a second UP long.
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs), Semantic::kUpLong);
    r.OnPressDown(Button::kDown, kLongMs + 100);
    CHECK_SEM(r.OnLongPress(Button::kDown, kLongMs + 100 + kLongMs), Semantic::kComboLong);
}

static void test_confirm_is_never_part_of_the_combo() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kConfirm, 10);
    CHECK_SEM(r.OnLongPress(Button::kConfirm, kLongMs), Semantic::kConfirmLong);
    CHECK_SEM(r.OnLongPress(Button::kUp, kLongMs + 20), Semantic::kUpLong);
}

// ------------------------------------------------------------ interleavings --

static void test_interleaved_taps_do_not_cross_talk() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnPressDown(Button::kDown, 10);
    r.OnPressUp(Button::kUp, 100);
    CHECK_SEM(r.OnClick(Button::kUp, 100), Semantic::kUpClick);
    r.OnPressUp(Button::kDown, 150);
    CHECK_SEM(r.OnClick(Button::kDown, 150), Semantic::kDownClick);
}

static void test_one_key_held_long_while_the_other_taps() {
    Recognizer r;
    r.OnPressDown(Button::kConfirm, 0);
    r.OnPressDown(Button::kUp, 10);
    r.OnPressUp(Button::kUp, 80);
    CHECK_SEM(r.OnClick(Button::kUp, 80), Semantic::kUpClick);
    CHECK_SEM(r.OnLongPress(Button::kConfirm, kLongMs), Semantic::kConfirmLong);
    CHECK(r.IsHeld(Button::kConfirm));
    r.OnPressUp(Button::kConfirm, kLongMs + 10);
    CHECK(!r.IsHeld(Button::kConfirm));
}

static void test_held_state_tracks_each_button_independently() {
    Recognizer r;
    CHECK(!r.IsHeld(Button::kUp));
    r.OnPressDown(Button::kUp, 0);
    CHECK(r.IsHeld(Button::kUp));
    CHECK(!r.IsHeld(Button::kDown));
    r.OnPressDown(Button::kDown, 5);
    CHECK(r.IsHeld(Button::kDown));
    r.OnPressUp(Button::kUp, 10);
    CHECK(!r.IsHeld(Button::kUp));
    CHECK(r.IsHeld(Button::kDown));
}

static void test_reset_clears_everything() {
    Recognizer r;
    r.OnPressDown(Button::kUp, 0);
    r.OnLongPress(Button::kUp, kLongMs);
    r.Reset();
    CHECK(!r.IsHeld(Button::kUp));
    CHECK(!r.ClickSuppressed(Button::kUp));
    r.OnPressDown(Button::kUp, 100);
    r.OnPressUp(Button::kUp, 150);
    CHECK_SEM(r.OnClick(Button::kUp, 150), Semantic::kUpClick);
}

static void test_threshold_is_configurable() {
    Config config;
    config.long_press_ms = 250;
    Recognizer r(config);
    CHECK(r.config().long_press_ms == 250);
    r.OnPressDown(Button::kUp, 0);
    CHECK_SEM(r.OnPressUp(Button::kUp, 250), Semantic::kUpLong);
}

// ------------------------------------------------ push-to-talk suppression --

static void test_suppress_next_click_swallows_exactly_one_click() {
    // Push-to-talk arms at BOOT press-down and decides at its own 300 ms
    // threshold, long before the 1000 ms that would make this a long press. A
    // 500 ms hold on the Dashboard would otherwise record an utterance and
    // also quick-switch the page: two outcomes for one press.
    Recognizer r;
    r.OnPressDown(Button::kConfirm, 0);
    r.SuppressNextClick(Button::kConfirm);
    CHECK(r.ClickSuppressed(Button::kConfirm));
    CHECK_SEM(r.OnPressUp(Button::kConfirm, 500), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kConfirm, 500), Semantic::kNone);
    CHECK(!r.ClickSuppressed(Button::kConfirm));
    // The next, ordinary press still clicks.
    r.OnPressDown(Button::kConfirm, 1000);
    CHECK_SEM(r.OnPressUp(Button::kConfirm, 1100), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kConfirm, 1100), Semantic::kConfirmClick);
}

static void test_suppression_does_not_leak_to_another_button() {
    Recognizer r;
    r.OnPressDown(Button::kConfirm, 0);
    r.SuppressNextClick(Button::kConfirm);
    r.OnPressDown(Button::kDown, 10);
    CHECK_SEM(r.OnPressUp(Button::kDown, 100), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kDown, 100), Semantic::kDownClick);
}

static void test_suppression_survives_a_hold_that_also_goes_long() {
    // A two-second hold: push-to-talk suppressed the click at 300 ms and the
    // driver then reports its own long press. The long press must still be
    // delivered, and the click must still be swallowed exactly once.
    Recognizer r;
    r.OnPressDown(Button::kConfirm, 0);
    r.SuppressNextClick(Button::kConfirm);
    CHECK_SEM(r.OnLongPress(Button::kConfirm, 1000), Semantic::kConfirmLong);
    CHECK_SEM(r.OnPressUp(Button::kConfirm, 2000), Semantic::kNone);
    CHECK_SEM(r.OnClick(Button::kConfirm, 2000), Semantic::kNone);
    CHECK(!r.ClickSuppressed(Button::kConfirm));
}

static void test_semantic_names_are_total() {
    CHECK(std::strcmp(SemanticName(Semantic::kComboLong), "ComboLong") == 0);
    CHECK(std::strcmp(SemanticName(Semantic::kConfirmLong), "BootLong") == 0);
    CHECK(std::strcmp(SemanticName(Semantic::kNone), "none") == 0);
    CHECK(std::strcmp(ButtonName(Button::kConfirm), "BOOT") == 0);
    CHECK(std::strcmp(ButtonName(Button::kUp), "UP") == 0);
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("button_gestures host tests (real firmware translation unit)\n\n");

    RUN(test_short_press_is_a_click);
    RUN(test_each_button_reports_its_own_click);
    RUN(test_release_just_below_threshold_is_a_click);
    RUN(test_hold_of_exactly_the_threshold_is_long);

    RUN(test_long_press_from_the_driver_fires_once);
    RUN(test_long_press_detected_on_release_fires_once);
    RUN(test_click_is_suppressed_after_a_driver_long_press);
    RUN(test_boot_long_press_also_suppresses_its_click);
    RUN(test_suppression_lasts_exactly_one_click);
    RUN(test_press_down_clears_stale_suppression);
    RUN(test_release_without_press_down_reports_nothing);

    RUN(test_combo_fires_once_when_both_are_held);
    RUN(test_combo_does_not_double_fire_on_release);
    RUN(test_combo_detected_on_release_when_the_driver_stays_silent);
    RUN(test_staggered_release_after_a_combo_adds_nothing);
    RUN(test_single_key_long_press_is_not_a_combo);
    RUN(test_combo_rearms_for_the_next_gesture);
    RUN(test_second_key_arriving_after_the_first_long_press_is_separate);
    RUN(test_confirm_is_never_part_of_the_combo);

    RUN(test_interleaved_taps_do_not_cross_talk);
    RUN(test_one_key_held_long_while_the_other_taps);
    RUN(test_held_state_tracks_each_button_independently);
    RUN(test_reset_clears_everything);
    RUN(test_threshold_is_configurable);
    RUN(test_suppress_next_click_swallows_exactly_one_click);
    RUN(test_suppression_does_not_leak_to_another_button);
    RUN(test_suppression_survives_a_hold_that_also_goes_long);
    RUN(test_semantic_names_are_total);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
