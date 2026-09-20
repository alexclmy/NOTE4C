/**
 * @file button_gestures.cc
 * @brief Implementation of the portable gesture recogniser.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "button_gestures.h"

namespace gesture {

namespace {

bool IsNavKey(Button button) {
    return button == Button::kUp || button == Button::kDown;
}

Button OtherNavKey(Button button) {
    return button == Button::kUp ? Button::kDown : Button::kUp;
}

}  // namespace

Recognizer::State& Recognizer::At(Button button) {
    return state_[static_cast<int>(button)];
}

const Recognizer::State& Recognizer::At(Button button) const {
    return state_[static_cast<int>(button)];
}

Semantic Recognizer::OnPressDown(Button button, int64_t now_ms) {
    if (IsNavKey(button) && !At(OtherNavKey(button)).held) {
        // First key of a new gesture: the previous combo, if any, is over.
        combo_handled_ = false;
    }
    State& s = At(button);
    s.held = true;
    s.press_down_ms = now_ms;
    s.long_handled = false;
    s.suppress_click = false;
    return Semantic::kNone;
}

Semantic Recognizer::ReportLong(Button button) {
    if (IsNavKey(button) && At(OtherNavKey(button)).held) {
        if (combo_handled_) {
            // The other key already reported it. Reporting again would open
            // the Wi-Fi access point twice.
            return Semantic::kNone;
        }
        combo_handled_ = true;
        // Both keys are now spoken for. Without this, releasing them one at a
        // time made the second release measure its own long hold and report a
        // single long press on top of the combo: the board file did exactly
        // that, which used to be invisible because a stray UP-long only left
        // Settings. It is not invisible now, when UP-long is Back and
        // DOWN-long is Home, and it would fire straight after opening the
        // Wi-Fi access point.
        At(Button::kUp).long_handled = true;
        At(Button::kUp).suppress_click = true;
        At(Button::kDown).long_handled = true;
        At(Button::kDown).suppress_click = true;
        return Semantic::kComboLong;
    }
    switch (button) {
        case Button::kUp:      return Semantic::kUpLong;
        case Button::kDown:    return Semantic::kDownLong;
        case Button::kConfirm: return Semantic::kConfirmLong;
        case Button::kCount:   break;
    }
    return Semantic::kNone;
}

Semantic Recognizer::OnLongPress(Button button, int64_t now_ms) {
    (void)now_ms;
    State& s = At(button);
    if (s.long_handled) {
        return Semantic::kNone;
    }
    s.long_handled = true;
    // Whatever happens next, the click that the driver emits on release
    // belongs to this hold and must not also count as a tap.
    s.suppress_click = true;
    return ReportLong(button);
}

Semantic Recognizer::OnPressUp(Button button, int64_t now_ms) {
    State& s = At(button);
    const int64_t started_at = s.press_down_ms;
    s.press_down_ms = -1;
    Semantic result = Semantic::kNone;

    // The driver does not always deliver BUTTON_LONG_PRESS_START (a press that
    // straddles a wake, for instance), so the release measures the hold too.
    const bool was_long = started_at >= 0 &&
                          (now_ms - started_at) >= static_cast<int64_t>(config_.long_press_ms);
    if (was_long && !s.long_handled) {
        s.long_handled = true;
        s.suppress_click = true;
        result = ReportLong(button);
    }
    s.held = false;
    return result;
}

Semantic Recognizer::OnClick(Button button, int64_t now_ms) {
    (void)now_ms;
    State& s = At(button);
    if (s.suppress_click) {
        s.suppress_click = false;
        return Semantic::kNone;
    }
    switch (button) {
        case Button::kUp:      return Semantic::kUpClick;
        case Button::kDown:    return Semantic::kDownClick;
        case Button::kConfirm: return Semantic::kConfirmClick;
        case Button::kCount:   break;
    }
    return Semantic::kNone;
}

bool Recognizer::IsHeld(Button button) const {
    return At(button).held;
}

bool Recognizer::ClickSuppressed(Button button) const {
    return At(button).suppress_click;
}

void Recognizer::SuppressNextClick(Button button) {
    At(button).suppress_click = true;
}

void Recognizer::Reset() {
    for (int i = 0; i < static_cast<int>(Button::kCount); ++i) {
        state_[i] = State{};
    }
    combo_handled_ = false;
}

const char* ButtonName(Button button) {
    switch (button) {
        case Button::kUp:      return "UP";
        case Button::kDown:    return "DOWN";
        case Button::kConfirm: return "BOOT";
        case Button::kCount:   break;
    }
    return "unknown";
}

const char* SemanticName(Semantic semantic) {
    switch (semantic) {
        case Semantic::kNone:         return "none";
        case Semantic::kUpClick:      return "UpClick";
        case Semantic::kDownClick:    return "DownClick";
        case Semantic::kConfirmClick: return "BootClick";
        case Semantic::kUpLong:       return "UpLong";
        case Semantic::kDownLong:     return "DownLong";
        case Semantic::kConfirmLong:  return "BootLong";
        case Semantic::kComboLong:    return "ComboLong";
    }
    return "unknown";
}

}  // namespace gesture
