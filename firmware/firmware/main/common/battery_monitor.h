/**
 * @file battery_monitor.h
 * @brief One owner for the battery ADC: opened once, locked, cached, and
 *        closed exactly once.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Like power_policy.h and device_config.h, this file contains no ESP-IDF
 * header, so tests/host/run.sh exercises this exact translation unit rather
 * than a parallel reimplementation. The device glue — `adc_oneshot_new_unit`,
 * the curve-fitting calibration scheme, and the millivolt conversion — is
 * injected as `Hooks` by the board and lives in
 * boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc.
 *
 * WHY THIS EXISTS
 * ---------------
 * The board used to keep the ADC unit handle, the calibration handle and an
 * `initialized` flag in three function-local `static`s inside the read
 * function, with no lock and no owner. Three separate defects followed from
 * that shape, and together they reset the device whenever the status page was
 * polled:
 *
 *  1. `static bool initialized = false` is constant-initialised, so it carries
 *     no C++ guard variable and the `if (!initialized)` around the driver
 *     calls was an unsynchronised check. The HTTP task (status route) and the
 *     UI task (status bar) both reach the read. Both could enter the block;
 *     the second `adc_oneshot_new_unit` then either failed — aborting inside
 *     `ESP_ERROR_CHECK` — or overwrote the handle while the first task was
 *     still inside `adc_oneshot_read`, whose exit path releases the lock that
 *     lives in the unit it was handed. That is a `_lock_release` on freed
 *     memory, which is a `uxListRemove` on a garbage list: the observed
 *     LoadProhibited at EXCVADDR 0x5, and the corrupted scheduler state behind
 *     the double exception on the reboot after it.
 *
 *  2. `initialized` was only ever set true *inside* the branch where the
 *     calibration scheme was created. A chip with no factory calibration — or
 *     one transient failure — therefore re-ran `adc_oneshot_new_unit` on
 *     every single call, on a unit the driver already held, leaking the
 *     previous handle and aborting on the driver's refusal. No concurrency was
 *     needed for that one.
 *
 *  3. Every reader took ten conversions, and the status route reads the
 *     battery four times per request (`PowerSnapshot` directly, then again
 *     through `AutonomySnapshot`, each of those once for the percentage and
 *     once for the millivolts). One poll was forty conversions driven from the
 *     HTTP task, concurrently with the UI task and with the panel busy.
 *
 * WHAT THIS GUARANTEES
 * --------------------
 *  - `open` runs **at most once** for the life of a Monitor, whatever it
 *    returns. A failed open is a permanent, remembered fact, never a retry.
 *  - Every call into `open`, `read_mv` and `close` is serialised on one mutex,
 *    so no two tasks are ever inside the driver at the same time and no task
 *    observes a half-built handle.
 *  - A failed *reading* is just a failed reading: it does not invalidate the
 *    handle, does not reopen anything, and does not abort. The caller gets
 *    `valid == false` and reports null, which is what the status contract
 *    already says an unknown battery looks like.
 *  - `close` runs at most once, never while a read is in flight, and after it
 *    no hook is ever called again. Reads after close return the last sample
 *    with `valid == false` rather than touching a released handle.
 *  - `Get()` refuses to touch the ADC more often than `min_interval_ms`. The
 *    cache is not the fix for the crash — the single open and the mutex are —
 *    but it is what keeps a polling client from driving the converter during a
 *    panel refresh, and it makes the status route's four reads into one.
 *
 * WHAT THIS DELIBERATELY REFUSES TO DO
 * ------------------------------------
 * It will not invent a percentage from uncalibrated counts. If the chip has no
 * factory curve the monitor reports `valid == false, calibrated == false` and
 * never issues a conversion at all, because a number derived from raw counts
 * would be indistinguishable, on the wire, from a real measurement.
 */

#ifndef COMMON_BATTERY_MONITOR_H
#define COMMON_BATTERY_MONITOR_H

#include <stdint.h>

#include <functional>
#include <mutex>

namespace battery {

/// Conversions averaged into one refresh. Ten, as the board has always used:
/// the cell node is noisy enough that a single conversion wanders by tens of
/// millivolts. Unlike before, ten conversions happen once per refresh interval
/// rather than once per caller.
constexpr int kDefaultSamples = 10;

/// How stale a reading may be before `Get()` takes a new one, in ms. A battery
/// does not move meaningfully in two seconds, and neither the status bar nor a
/// polling tower can tell the difference — but the ADC very much can.
constexpr int64_t kDefaultMinIntervalMs = 2000;

/**
 * @brief What one battery reading says, including whether it says anything.
 *
 * `valid` and `calibrated` are separate on purpose. `calibrated == false`
 * means this chip can never produce volts; `valid == false` with
 * `calibrated == true` means this particular attempt failed and a later one
 * may succeed. The status route renders null for both, but the difference is
 * the difference between a broken chip and a busy one.
 */
struct Sample {
    bool valid = false;        ///< a conversion succeeded and produced volts
    bool calibrated = false;   ///< the chip carries a factory calibration curve
    uint16_t millivolts = 0;   ///< at the cell, already un-divided
    uint8_t percent = 0;       ///< derived from millivolts; 0 when !valid
    int64_t taken_at_ms = 0;   ///< when the ADC was last actually touched
};

/**
 * @brief What claiming the ADC produced.
 *
 * Two flags rather than one because the unit and the calibration fail
 * independently, and conflating them is exactly how the old code turned a
 * missing calibration curve into an infinite re-initialisation loop.
 */
struct OpenResult {
    bool unit_ready = false;  ///< the oneshot unit and channel are configured
    bool calibrated = false;  ///< a calibration scheme exists for that channel
};

/**
 * @brief The device calls the Monitor makes. All of them are called under the
 *        Monitor's lock, and none of them may call back into the Monitor.
 */
struct Hooks {
    /// Claim the ADC unit, configure the channel, and try for calibration.
    /// Called AT MOST ONCE, whatever it returns. Must not abort on failure.
    std::function<OpenResult()> open;

    /// One conversion, in millivolts at the cell. Returning false means this
    /// reading failed; it must leave the handle usable for the next one.
    std::function<bool(uint16_t* mv_out)> read_mv;

    /// Release whatever `open` claimed. Called at most once, only if `open`
    /// reported `unit_ready`, and never concurrently with a read.
    std::function<void()> close;

    /// Monotonic milliseconds. Only used to age the cache.
    std::function<int64_t()> now_ms;
};

struct Config {
    int samples = kDefaultSamples;
    int64_t min_interval_ms = kDefaultMinIntervalMs;
};

/**
 * @brief The battery percentage the board has always reported, as arithmetic.
 *
 * The quadratic fit for this cell, lifted out of the board so the host tests
 * pin the curve and its clamps. Returns 0 for anything at or below zero
 * millivolts rather than extrapolating off the end of the fit.
 */
uint8_t PercentFromMillivolts(int millivolts);

/**
 * @brief Sole owner of the battery ADC: one open, one lock, one close.
 *
 * Non-copyable and non-movable: the whole point is that exactly one of these
 * holds the driver handles, and a copy would be a second owner closing the
 * same unit.
 */
class Monitor {
public:
    Monitor() = default;
    ~Monitor();

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    Monitor(Monitor&&) = delete;
    Monitor& operator=(Monitor&&) = delete;

    /// Install the device calls. Does NOT open the ADC — the first `Get()`
    /// does, once. Calling this twice keeps the first set of hooks and is
    /// ignored, so a second board construction cannot silently take ownership
    /// of handles the first one still holds.
    void Begin(Hooks hooks, Config config = Config{});

    /// The current reading: the cached one if it is younger than
    /// `min_interval_ms`, otherwise a fresh set of conversions.
    Sample Get();

    /// A fresh reading, ignoring the cache but not the lock or the single
    /// open. For the factory test, which is measuring the hardware rather than
    /// reporting on it, and which does not run concurrently with a poll.
    Sample GetFresh();

    /// The last reading taken, without ever touching the ADC. Safe to call
    /// from any task at any time, including after `Close()`.
    Sample Peek() const;

    /// Release the ADC. Idempotent, and safe to call when the open failed or
    /// never happened. After this every `Get()` reports an invalid sample.
    void Close();

    // -- introspection, for the host tests and for field diagnostics --

    /// How many times `open` has actually been invoked. Must never exceed 1.
    int open_calls() const;
    /// How many conversions have been requested since construction.
    int read_calls() const;
    /// How many times `close` has actually been invoked. Must never exceed 1.
    int close_calls() const;

private:
    Sample Refresh(bool force);

    mutable std::mutex mutex_;
    Hooks hooks_;
    Config config_;
    bool begun_ = false;
    bool opened_ = false;   ///< `open` has been invoked (success or not)
    bool closed_ = false;
    OpenResult open_result_;
    bool have_sample_ = false;
    Sample last_;
    int open_calls_ = 0;
    int read_calls_ = 0;
    int close_calls_ = 0;
};

}  // namespace battery

#endif  // COMMON_BATTERY_MONITOR_H
