/**
 * @file battery_monitor.cc
 * @brief Implementation of the single-owner, single-open, locked battery ADC.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See battery_monitor.h for the crash this shape exists to prevent.
 */

#include "common/battery_monitor.h"

namespace battery {

uint8_t PercentFromMillivolts(int millivolts) {
    if (millivolts <= 0) {
        return 0;
    }
    // The board's quadratic fit for this cell, unchanged. Computed in int64 so
    // the -mv^2 term cannot overflow on the way to a value that then clamps
    // into 0..100 anyway.
    const int64_t mv = millivolts;
    const int64_t fit = (-1 * mv * mv + 9016 * mv - 19189000) / 10000;
    if (fit <= 0) {
        return 0;
    }
    if (fit >= 100) {
        return 100;
    }
    return static_cast<uint8_t>(fit);
}

Monitor::~Monitor() {
    Close();
}

void Monitor::Begin(Hooks hooks, Config config) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (begun_) {
        // A second Begin would install hooks for handles the first set still
        // owns, and the close that eventually ran would release the wrong
        // ones. Keeping the first set is the only answer that cannot leak.
        return;
    }
    hooks_ = std::move(hooks);
    config_ = config;
    if (config_.samples < 1) {
        config_.samples = 1;
    }
    if (config_.min_interval_ms < 0) {
        config_.min_interval_ms = 0;
    }
    begun_ = true;
}

Sample Monitor::Get() {
    std::lock_guard<std::mutex> guard(mutex_);
    return Refresh(false);
}

Sample Monitor::GetFresh() {
    std::lock_guard<std::mutex> guard(mutex_);
    return Refresh(true);
}

Sample Monitor::Peek() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return last_;
}

void Monitor::Close() {
    std::function<void()> close_hook;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        // Only release what was actually claimed — `unit_ready`, not merely
        // "open was attempted". An open that failed, or that handed the unit
        // back itself after a channel-config failure, holds nothing, and
        // releasing an unclaimed unit is a second fault on top of the first.
        if (opened_ && open_result_.unit_ready) {
            close_hook = hooks_.close;
            ++close_calls_;
        }
        // Nothing may be reported as live once the handles are going away.
        last_.valid = false;
        last_.percent = 0;
        last_.millivolts = 0;
    }
    // Outside the lock: the driver's own teardown takes its own locks, and a
    // hook that blocked there while holding ours would stall every reader for
    // the duration. Nothing else can be inside a hook at this point, because
    // `closed_` was set under the lock before we let go of it.
    if (close_hook) {
        close_hook();
    }
}

int Monitor::open_calls() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return open_calls_;
}

int Monitor::read_calls() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return read_calls_;
}

int Monitor::close_calls() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return close_calls_;
}

/// Caller holds `mutex_`.
Sample Monitor::Refresh(bool force) {
    if (closed_ || !begun_) {
        return last_;
    }

    // The single open. `opened_` is set BEFORE the hook runs and is never
    // cleared, so a hook that throws, fails, or reports no calibration still
    // consumes the one attempt. This is the defect that made an uncalibrated
    // chip re-register the ADC unit on every read.
    if (!opened_) {
        opened_ = true;
        ++open_calls_;
        open_result_ = hooks_.open ? hooks_.open() : OpenResult{};
        last_.calibrated = open_result_.calibrated;
    }

    // A unit that would not open, or a chip with no factory curve, is a
    // permanent answer. Report it without issuing a conversion and without
    // ever trying to open again — an uncalibrated read is counts, not volts,
    // and there is no honest percentage to be had from it.
    if (!open_result_.unit_ready || !open_result_.calibrated) {
        last_.valid = false;
        last_.calibrated = open_result_.calibrated;
        last_.millivolts = 0;
        last_.percent = 0;
        have_sample_ = true;
        return last_;
    }

    const int64_t now = hooks_.now_ms ? hooks_.now_ms() : 0;
    if (!force && have_sample_ &&
        now - last_.taken_at_ms < config_.min_interval_ms &&
        now >= last_.taken_at_ms) {
        return last_;
    }

    if (!hooks_.read_mv) {
        last_.valid = false;
        last_.millivolts = 0;
        last_.percent = 0;
        last_.taken_at_ms = now;
        have_sample_ = true;
        return last_;
    }

    // Average over the conversions that actually succeeded. The old code
    // summed failures in as zero and divided by ten regardless, so one bad
    // conversion in ten silently reported a battery ten percent flatter than
    // it was.
    int64_t sum = 0;
    int ok_count = 0;
    for (int i = 0; i < config_.samples; ++i) {
        uint16_t mv = 0;
        ++read_calls_;
        if (hooks_.read_mv(&mv)) {
            sum += mv;
            ++ok_count;
        }
    }

    last_.taken_at_ms = now;
    have_sample_ = true;
    last_.calibrated = true;

    if (ok_count == 0) {
        // Every conversion failed. That is a transient hardware answer, not a
        // reason to tear anything down: the handle stays, the next refresh
        // tries again, and the caller is told it learned nothing.
        last_.valid = false;
        last_.millivolts = 0;
        last_.percent = 0;
        return last_;
    }

    const int64_t average = sum / ok_count;
    if (average <= 0) {
        last_.valid = false;
        last_.millivolts = 0;
        last_.percent = 0;
        return last_;
    }

    last_.valid = true;
    last_.millivolts = average > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(average);
    last_.percent = PercentFromMillivolts(static_cast<int>(last_.millivolts));
    return last_;
}

}  // namespace battery
