/**
 * @file button_gestures.h
 * @brief Portable press/hold/release/combo recognition for the three buttons.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This logic used to live in nine file-static atomics inside
 * main/boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc:351-444. It was
 * correct-looking and untestable: the rules it encodes (a click that arrives
 * after a long press must be swallowed, a long press must fire exactly once
 * whether the driver reports it or the release does, the two-key combo must
 * not fire twice) are exactly the kind that break silently.
 *
 * Here it takes injected timestamps and no globals, so
 * tests/host/test_button_gestures.cc can drive every interleaving. The board
 * file becomes glue: it forwards driver callbacks in and dispatches the
 * semantic event out.
 *
 * Driver contract this depends on (iot_button, main/boards/common/button.cc):
 * BUTTON_PRESS_UP is delivered before BUTTON_SINGLE_CLICK for the same press.
 * That ordering is what lets a release-detected long press swallow the click
 * that follows it.
 */

#ifndef COMMON_BUTTON_GESTURES_H
#define COMMON_BUTTON_GESTURES_H

#include <stdint.h>

namespace gesture {

enum class Button {
    kUp = 0,
    kDown,
    kConfirm,  ///< The BOOT key.
    kCount,
};

/// What the board should act on. kNone means the raw event was consumed
/// (a suppressed click, a duplicate long press, a second combo report).
enum class Semantic {
    kNone = 0,
    kUpClick,
    kDownClick,
    kConfirmClick,
    kUpLong,
    kDownLong,
    kConfirmLong,
    kComboLong,  ///< UP and DOWN held together past the threshold.
};

struct Config {
    /// Matches kNavLongPressMs in the board file. A hold of exactly this many
    /// milliseconds counts as long, as it did before.
    uint32_t long_press_ms = 1000;
};

/**
 * @brief Turns raw driver callbacks into one semantic event, or none.
 *
 * Not thread-safe by construction; the board file calls it from the button
 * task only, which is the same single-threaded context the atomics were
 * pretending to protect.
 */
class Recognizer {
public:
    explicit Recognizer(Config config = Config{}) : config_(config) {}

    Semantic OnPressDown(Button button, int64_t now_ms);
    Semantic OnPressUp(Button button, int64_t now_ms);
    Semantic OnLongPress(Button button, int64_t now_ms);
    Semantic OnClick(Button button, int64_t now_ms);

    bool IsHeld(Button button) const;
    /// True when the next click on @p button will be swallowed.
    bool ClickSuppressed(Button button) const;
    /**
     * @brief Swallow the click this press is about to produce.
     *
     * Called by the board when a hold has already been consumed by something
     * other than this recogniser: push-to-talk arms at BOOT press-down and
     * decides at its own 300 ms threshold, well before the 1000 ms that would
     * make this a long press. Without this, a 500 ms hold on the Dashboard
     * would record an utterance and also quick-switch the page, which is two
     * outcomes for one press.
     *
     * Must be called before the driver's click callback arrives; the button
     * driver delivers BUTTON_PRESS_UP first, which is where the board calls it.
     */
    void SuppressNextClick(Button button);
    void Reset();

    const Config& config() const { return config_; }

private:
    struct State {
        bool held = false;
        /// Milliseconds at press-down, or -1 when no press is in flight.
        int64_t press_down_ms = -1;
        /// The long press for this hold has already been reported.
        bool long_handled = false;
        /// Swallow the next click: it belongs to a hold, not to a tap.
        bool suppress_click = false;
    };

    State& At(Button button);
    const State& At(Button button) const;
    /// Report the long press for @p button, as a combo when the other
    /// navigation key is also held. Assumes the caller has already marked the
    /// hold as handled.
    Semantic ReportLong(Button button);

    Config config_;
    State state_[static_cast<int>(Button::kCount)];
    /// The combo reports once per pair of presses, not once per key.
    bool combo_handled_ = false;
};

const char* ButtonName(Button button);
const char* SemanticName(Semantic semantic);

}  // namespace gesture

#endif  // COMMON_BUTTON_GESTURES_H
