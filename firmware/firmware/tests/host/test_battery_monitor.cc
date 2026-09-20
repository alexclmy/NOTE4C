/**
 * @file test_battery_monitor.cc
 * @brief The battery ADC's ownership rules, driven by real concurrent threads.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS SUITE IS FOR
 * ----------------------
 * The device crashed with a LoadProhibited at EXCVADDR 0x5, inside
 * `adc_oneshot_read -> adc_lock_release -> _lock_release -> uxListRemove`,
 * whenever the embedded status page was polled. The cause was ownership, not
 * arithmetic: the ADC unit handle lived in an unsynchronised function-local
 * `static`, so two tasks could register the unit concurrently and one could
 * release a lock belonging to a handle the other had already replaced.
 *
 * You cannot assert "the handle was not replaced under me" against a scripted
 * single-threaded fake, so this suite does not try. `FakeAdc` below reproduces
 * the part of the ESP-IDF oneshot driver's contract that matters — a unit is
 * registered at most once, a second registration is refused, and reading
 * through a handle that has been released or replaced is use-after-free — and
 * the concurrency tests drive it from real threads under ASan and UBSan.
 * `FakeAdc::faults` counts every contract violation, and every concurrency
 * test asserts it is still zero afterwards.
 *
 * The three defects each have a named regression here:
 *   - double registration under concurrency
 *       -> test_two_tasks_racing_the_first_read_open_the_unit_once
 *       -> test_a_storm_of_concurrent_readers_never_reopens_or_faults
 *   - an uncalibrated chip reopening the unit on every single read
 *       -> test_a_chip_without_calibration_is_opened_once_and_never_again
 *   - forty conversions per status request
 *       -> test_the_cache_collapses_a_status_request_into_one_refresh
 */

#include "common/battery_monitor.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace battery;

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

#define CHECK_EQ_I64(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const int64_t va = (int64_t)(a);                                   \
        const int64_t vb = (int64_t)(b);                                   \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %lld == %lld\n", __FILE__,         \
                   __LINE__, g_current, (long long)va, (long long)vb);     \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-68s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

// ------------------------------------------------------ the driver's rules --

/**
 * @brief A stand-in for the ESP-IDF oneshot ADC unit, with its actual rules.
 *
 * The behaviours reproduced, and why each one is here:
 *
 *  - `Register` refuses a second registration while one is live, exactly as
 *    `adc_oneshot_new_unit` does for a unit already claimed. The old board
 *    code called it again on every read of an uncalibrated chip, and the
 *    refusal went into `ESP_ERROR_CHECK`, which aborts.
 *
 *  - Each registration produces a *heap-allocated* `Unit` carrying a `lock`
 *    field. `Read` touches that lock on the way in and on the way out, which
 *    is what `adc_oneshot_read` does via `adc_lock_acquire/release`. Reading
 *    through a released `Unit` is a genuine use-after-free, so ASan reports
 *    the real bug rather than this fake's opinion of it.
 *
 *  - `concurrent_readers` is tracked with a plain (non-atomic) counter guarded
 *    by nothing. If the Monitor ever let two threads into a hook at once, TSan
 *    would see the race; since these suites run ASan+UBSan rather than TSan,
 *    the counter is also checked for a value above one, which a real overlap
 *    reaches quickly across thousands of iterations.
 */
struct FakeAdc {
    struct Unit {
        uint32_t lock = 0xA5A5A5A5u;  ///< poisoned by the destructor
        bool live = true;
        ~Unit() {
            lock = 0xDEADBEEFu;
            live = false;
        }
    };

    std::atomic<int> faults{0};
    std::atomic<int> registrations{0};
    std::atomic<int> deletions{0};
    std::atomic<int> reads{0};
    std::atomic<int> max_concurrent_readers{0};
    int concurrent_readers = 0;  ///< deliberately not atomic; see above

    Unit* unit = nullptr;
    bool unit_claimed = false;  ///< the driver's "this unit is already in use"
    bool allow_register = true;
    bool allow_calibration = true;
    bool reads_fail = false;
    uint16_t next_mv = 3900;

    /// `adc_oneshot_new_unit`. Refuses while a unit is already claimed.
    bool Register() {
        if (unit_claimed) {
            // The exact condition the old code walked into on every read of an
            // uncalibrated chip. Counting it as a fault is the point.
            faults.fetch_add(1);
            return false;
        }
        ++registrations;
        if (!allow_register) {
            return false;
        }
        unit_claimed = true;
        unit = new Unit();
        return true;
    }

    /// `adc_oneshot_del_unit`.
    void Unregister() {
        if (!unit_claimed) {
            faults.fetch_add(1);
            return;
        }
        ++deletions;
        unit_claimed = false;
        delete unit;
        unit = nullptr;
    }

    /// `adc_oneshot_read`, lock acquire and release included.
    bool Read(uint16_t* mv_out) {
        ++reads;
        Unit* u = unit;
        if (u == nullptr || !u->live || u->lock != 0xA5A5A5A5u) {
            // Reading through a handle that was replaced or released. This is
            // the crash: `_lock_release` on memory that is no longer a lock.
            faults.fetch_add(1);
            return false;
        }

        const int entered = ++concurrent_readers;
        if (entered > 1) {
            faults.fetch_add(1);
        }
        int observed = max_concurrent_readers.load();
        while (entered > observed &&
               !max_concurrent_readers.compare_exchange_weak(observed, entered)) {
        }

        // Where the driver would convert. The yield widens any window in which
        // two readers could overlap, so a missing lock shows up rather than
        // being hidden by how fast the fake is.
        std::this_thread::yield();

        const bool ok = !reads_fail;
        if (ok && mv_out != nullptr) {
            *mv_out = next_mv;
        }

        if (u->lock != 0xA5A5A5A5u || !u->live) {
            faults.fetch_add(1);
        }
        --concurrent_readers;
        return ok;
    }
};

/// A clock the tests move by hand, so "the cache expired" is an input rather
/// than something to sleep for.
struct FakeClock {
    std::atomic<int64_t> now{1000};
    int64_t Now() const { return now.load(); }
};

Hooks MakeHooks(FakeAdc& adc, FakeClock& clock) {
    Hooks hooks;
    hooks.open = [&adc]() -> OpenResult {
        OpenResult r;
        r.unit_ready = adc.Register();
        r.calibrated = r.unit_ready && adc.allow_calibration;
        return r;
    };
    hooks.read_mv = [&adc](uint16_t* mv) { return adc.Read(mv); };
    hooks.close = [&adc]() { adc.Unregister(); };
    hooks.now_ms = [&clock]() { return clock.Now(); };
    return hooks;
}

// ------------------------------------------------------------ the curve -----

static void test_the_percentage_curve_clamps_at_both_ends() {
    CHECK_EQ_I64(PercentFromMillivolts(0), 0);
    CHECK_EQ_I64(PercentFromMillivolts(-5), 0);
    // Well below the fit's zero crossing: clamped rather than negative.
    CHECK_EQ_I64(PercentFromMillivolts(3000), 0);
    // Well above: clamped rather than over a hundred.
    CHECK_EQ_I64(PercentFromMillivolts(4400), 100);
    // Mid-range is monotonic and strictly inside the clamps.
    const uint8_t low = PercentFromMillivolts(3600);
    const uint8_t mid = PercentFromMillivolts(3800);
    const uint8_t high = PercentFromMillivolts(4000);
    CHECK(low < mid);
    CHECK(mid < high);
    CHECK(high <= 100);
}

static void test_the_curve_matches_the_board_arithmetic_it_replaced() {
    // The expression the board carried inline, evaluated here for the same
    // inputs, so lifting it out of the board did not change any reported
    // percentage. Deliberately spelled out rather than shared with the
    // implementation: a shared helper would pass even if both were wrong.
    for (int mv = 3400; mv <= 4300; mv += 7) {
        int expected =
            (-1 * mv * mv + 9016 * mv - 19189000) / 10000;
        expected = expected > 100 ? 100 : (expected < 0 ? 0 : expected);
        CHECK_EQ_I64(PercentFromMillivolts(mv), expected);
    }
}

// ------------------------------------------------------ the single open -----

static void test_the_first_read_opens_the_unit_and_later_ones_do_not() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    CHECK_EQ_I64(m.open_calls(), 0);  // Begin must not touch the converter
    CHECK_EQ_I64(adc.registrations.load(), 0);

    Sample s = m.Get();
    CHECK(s.valid);
    CHECK(s.calibrated);
    CHECK_EQ_I64(s.millivolts, 3900);
    CHECK_EQ_I64(m.open_calls(), 1);

    for (int i = 0; i < 50; ++i) {
        clock.now += 10000;  // past the interval every time
        (void)m.Get();
    }
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_chip_without_calibration_is_opened_once_and_never_again() {
    // Regression for the defect that needed no concurrency at all: the old
    // code only latched `initialized` inside the calibration branch, so an
    // uncalibrated chip re-registered the ADC unit on every single read and
    // aborted on the driver's refusal.
    FakeAdc adc;
    FakeClock clock;
    adc.allow_calibration = false;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    for (int i = 0; i < 200; ++i) {
        clock.now += 10000;
        const Sample s = m.Get();
        CHECK(!s.valid);
        CHECK(!s.calibrated);
        CHECK_EQ_I64(s.millivolts, 0);
        CHECK_EQ_I64(s.percent, 0);
    }
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
    // And not one conversion was issued: counts are not volts.
    CHECK_EQ_I64(adc.reads.load(), 0);
    CHECK_EQ_I64(m.read_calls(), 0);
}

static void test_a_unit_that_will_not_open_is_not_retried() {
    FakeAdc adc;
    FakeClock clock;
    adc.allow_register = false;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    for (int i = 0; i < 100; ++i) {
        clock.now += 10000;
        const Sample s = m.Get();
        CHECK(!s.valid);
        CHECK(!s.calibrated);
    }
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_monitor_with_no_hooks_at_all_reports_nothing_and_does_not_crash() {
    Monitor m;  // never begun
    const Sample s = m.Get();
    CHECK(!s.valid);
    CHECK(!s.calibrated);
    CHECK_EQ_I64(m.open_calls(), 0);
    CHECK_EQ_I64(m.read_calls(), 0);
    m.Close();  // must be safe on something that never opened
    CHECK_EQ_I64(m.close_calls(), 0);
}

static void test_a_second_begin_does_not_replace_the_hooks_that_own_the_handles() {
    FakeAdc first;
    FakeAdc second;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(first, clock));
    m.Begin(MakeHooks(second, clock));  // ignored

    (void)m.Get();
    m.Close();

    CHECK_EQ_I64(first.registrations.load(), 1);
    CHECK_EQ_I64(first.deletions.load(), 1);
    // The second fake was never touched, so nothing was opened by one set of
    // hooks and closed by another.
    CHECK_EQ_I64(second.registrations.load(), 0);
    CHECK_EQ_I64(second.deletions.load(), 0);
    CHECK_EQ_I64(first.faults.load(), 0);
    CHECK_EQ_I64(second.faults.load(), 0);
}

// ------------------------------------------------------------- the cache ----

static void test_a_reading_inside_the_interval_does_not_touch_the_converter() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    const Sample first = m.Get();
    CHECK(first.valid);
    CHECK_EQ_I64(adc.reads.load(), kDefaultSamples);

    adc.next_mv = 3300;  // a value only a fresh conversion could report
    clock.now += kDefaultMinIntervalMs - 1;
    const Sample cached = m.Get();
    CHECK_EQ_I64(cached.millivolts, 3900);
    CHECK_EQ_I64(adc.reads.load(), kDefaultSamples);

    clock.now += 1;  // now exactly at the interval
    const Sample fresh = m.Get();
    CHECK_EQ_I64(fresh.millivolts, 3300);
    CHECK_EQ_I64(adc.reads.load(), 2 * kDefaultSamples);
}

static void test_the_cache_collapses_a_status_request_into_one_refresh() {
    // The status route reads the battery four times per request:
    // PowerSnapshot() takes a percentage and then millivolts, and
    // AutonomySnapshot() calls PowerSnapshot() again. That was forty
    // conversions per poll, driven from the HTTP task.
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    const Sample a = m.Get();  // GetBatteryLevel
    const Sample b = m.Get();  // ZectrixReadBatteryMillivolts
    const Sample c = m.Get();  // ...and both again via AutonomySnapshot
    const Sample d = m.Get();

    CHECK_EQ_I64(adc.reads.load(), kDefaultSamples);
    // All four also agree, which the two independent averages they used to
    // come from could not guarantee: a percentage and a voltage from
    // different sample sets could contradict each other on the wire.
    CHECK_EQ_I64(a.millivolts, b.millivolts);
    CHECK_EQ_I64(b.millivolts, c.millivolts);
    CHECK_EQ_I64(c.millivolts, d.millivolts);
    CHECK_EQ_I64(a.percent, d.percent);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_the_factory_test_bypasses_the_cache_but_not_the_lock() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    (void)m.Get();
    CHECK_EQ_I64(adc.reads.load(), kDefaultSamples);

    adc.next_mv = 3700;
    const Sample fresh = m.GetFresh();  // same millisecond, still converts
    CHECK_EQ_I64(fresh.millivolts, 3700);
    CHECK_EQ_I64(adc.reads.load(), 2 * kDefaultSamples);
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_clock_that_jumps_backwards_refreshes_rather_than_freezing() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    (void)m.Get();
    adc.next_mv = 3500;
    clock.now = 10;  // e.g. a counter reset behind the monotonic clock
    const Sample s = m.Get();
    CHECK_EQ_I64(s.millivolts, 3500);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_peek_never_touches_the_converter() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    const Sample before = m.Peek();
    CHECK(!before.valid);
    CHECK_EQ_I64(m.open_calls(), 0);
    CHECK_EQ_I64(adc.reads.load(), 0);

    (void)m.Get();
    const Sample after = m.Peek();
    CHECK(after.valid);
    CHECK_EQ_I64(after.millivolts, 3900);
    CHECK_EQ_I64(adc.reads.load(), kDefaultSamples);
}

// ------------------------------------------------------- failed readings ----

static void test_a_failed_reading_is_reported_not_escalated() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    adc.reads_fail = true;
    const Sample bad = m.Get();
    CHECK(!bad.valid);
    CHECK(bad.calibrated);  // the chip is fine; this attempt was not
    CHECK_EQ_I64(bad.percent, 0);
    // The handle was not torn down and not reopened.
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.deletions.load(), 0);

    adc.reads_fail = false;
    clock.now += 10000;
    const Sample good = m.Get();
    CHECK(good.valid);
    CHECK_EQ_I64(good.millivolts, 3900);
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_zero_reading_is_unknown_rather_than_an_empty_battery() {
    FakeAdc adc;
    FakeClock clock;
    adc.next_mv = 0;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    const Sample s = m.Get();
    CHECK(!s.valid);
    CHECK_EQ_I64(s.percent, 0);
    CHECK_EQ_I64(s.millivolts, 0);
}

static void test_the_average_ignores_failed_conversions_instead_of_counting_them_as_zero() {
    // The old loop summed ten readings and divided by ten regardless of how
    // many had failed, so one bad conversion reported a battery ten percent
    // flatter than it was.
    FakeAdc adc;
    FakeClock clock;
    Hooks hooks = MakeHooks(adc, clock);

    int call = 0;
    hooks.read_mv = [&adc, &call](uint16_t* mv) {
        ++call;
        if (call % 2 == 0) {
            (void)adc.Read(nullptr);  // still exercises the driver contract
            return false;
        }
        return adc.Read(mv);
    };

    Monitor m;
    m.Begin(std::move(hooks));
    const Sample s = m.Get();
    CHECK(s.valid);
    CHECK_EQ_I64(s.millivolts, 3900);  // the average of the five that worked
    CHECK_EQ_I64(adc.faults.load(), 0);
}

// ----------------------------------------------------------- the teardown ---

static void test_close_releases_once_and_is_idempotent() {
    FakeAdc adc;
    FakeClock clock;
    {
        Monitor m;
        m.Begin(MakeHooks(adc, clock));
        (void)m.Get();
        m.Close();
        m.Close();
        m.Close();
        CHECK_EQ_I64(m.close_calls(), 1);
    }  // the destructor must not close a second time either
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.deletions.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_the_destructor_alone_releases_the_unit() {
    FakeAdc adc;
    FakeClock clock;
    {
        Monitor m;
        m.Begin(MakeHooks(adc, clock));
        (void)m.Get();
    }
    CHECK_EQ_I64(adc.deletions.load(), 1);
    CHECK(!adc.unit_claimed);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_failed_open_is_never_closed() {
    FakeAdc adc;
    FakeClock clock;
    adc.allow_register = false;
    {
        Monitor m;
        m.Begin(MakeHooks(adc, clock));
        (void)m.Get();
    }
    // Nothing was claimed, so nothing may be released: Unregister on an
    // unclaimed unit is a fault in the fake, as it is in the driver.
    CHECK_EQ_I64(adc.deletions.load(), 0);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_an_open_that_hands_the_unit_back_itself_is_not_closed_again() {
    // The board's real partial-failure path: `adc_oneshot_new_unit` succeeds,
    // `adc_oneshot_config_channel` does not, and the hook deletes the unit
    // before returning `unit_ready == false`. Nothing is held afterwards, so
    // Close must not delete it a second time.
    FakeAdc adc;
    FakeClock clock;
    Hooks hooks = MakeHooks(adc, clock);
    hooks.open = [&adc]() -> OpenResult {
        OpenResult r;
        if (adc.Register()) {
            adc.Unregister();  // the channel config failed; give it straight back
        }
        return r;  // unit_ready stays false
    };

    {
        Monitor m;
        m.Begin(std::move(hooks));
        (void)m.Get();
        CHECK_EQ_I64(m.close_calls(), 0);
    }
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.deletions.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_reads_after_close_report_nothing_and_touch_no_handle() {
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));
    (void)m.Get();
    const int reads_before = adc.reads.load();
    m.Close();

    for (int i = 0; i < 20; ++i) {
        clock.now += 10000;
        const Sample s = m.Get();
        CHECK(!s.valid);
        const Sample f = m.GetFresh();
        CHECK(!f.valid);
    }
    CHECK_EQ_I64(adc.reads.load(), reads_before);
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

// --------------------------------------------------------- the concurrency --

/// Release N threads at once, so they contend rather than queue.
struct Gate {
    std::mutex m;
    std::atomic<bool> open{false};
    void Wait() {
        while (!open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void Open() { open.store(true, std::memory_order_release); }
};

static void test_two_tasks_racing_the_first_read_open_the_unit_once() {
    // The crash, reduced: the HTTP task and the UI task both reaching the
    // first battery read. The old code had an unsynchronised `if
    // (!initialized)` here, so both could register the unit and one could then
    // release a lock belonging to a handle the other had replaced.
    for (int attempt = 0; attempt < 200; ++attempt) {
        FakeAdc adc;
        FakeClock clock;
        Monitor m;
        m.Begin(MakeHooks(adc, clock));

        Gate gate;
        std::atomic<int> valid_count{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&]() {
                gate.Wait();
                if (m.Get().valid) {
                    valid_count.fetch_add(1);
                }
            });
        }
        gate.Open();
        for (auto& th : threads) th.join();

        CHECK_EQ_I64(m.open_calls(), 1);
        CHECK_EQ_I64(adc.registrations.load(), 1);
        CHECK_EQ_I64(valid_count.load(), 2);
        CHECK_EQ_I64(adc.faults.load(), 0);
        CHECK_EQ_I64(adc.max_concurrent_readers.load(), 1);
    }
}

static void test_a_storm_of_concurrent_readers_never_reopens_or_faults() {
    // Eight tasks polling the status route flat out, with the clock advancing
    // under them so the cache expires mid-flight and refreshes race.
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    Gate gate;
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            gate.Wait();
            while (!stop.load(std::memory_order_relaxed)) {
                (void)m.Get();
                (void)m.Peek();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // A ninth thread standing in for the wall clock, so refreshes and cache
    // hits interleave unpredictably rather than in a fixed pattern.
    std::thread ticker([&]() {
        gate.Wait();
        while (!stop.load(std::memory_order_relaxed)) {
            clock.now.fetch_add(500);
            std::this_thread::yield();
        }
    });

    gate.Open();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    stop.store(true);
    for (auto& th : threads) th.join();
    ticker.join();

    CHECK(reads.load() > 100);  // the storm actually ran
    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.registrations.load(), 1);
    CHECK_EQ_I64(adc.max_concurrent_readers.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

static void test_a_close_racing_readers_frees_the_unit_exactly_once() {
    // Teardown under load: the case where the crash's shape — a release of a
    // handle another task is reading through — would show up as a genuine
    // use-after-free for ASan, not merely as a counter mismatch.
    for (int attempt = 0; attempt < 50; ++attempt) {
        FakeAdc adc;
        FakeClock clock;
        Monitor m;
        m.Begin(MakeHooks(adc, clock));

        Gate gate;
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                gate.Wait();
                while (!stop.load(std::memory_order_relaxed)) {
                    clock.now.fetch_add(3000);  // force a real refresh
                    (void)m.Get();
                }
            });
        }
        std::thread closer([&]() {
            gate.Wait();
            for (int i = 0; i < 50; ++i) std::this_thread::yield();
            m.Close();
            m.Close();
        });

        gate.Open();
        closer.join();
        for (int i = 0; i < 200; ++i) std::this_thread::yield();
        stop.store(true);
        for (auto& th : threads) th.join();

        CHECK_EQ_I64(adc.deletions.load(), 1);
        CHECK_EQ_I64(m.close_calls(), 1);
        CHECK_EQ_I64(adc.faults.load(), 0);
        CHECK_EQ_I64(adc.max_concurrent_readers.load(), 1);
    }
}

static void test_concurrent_fresh_and_cached_readers_stay_serialised() {
    // The factory test's uncached path running while the status route polls:
    // GetFresh skips the cache, so it converts every time and is the reader
    // most likely to overlap another.
    FakeAdc adc;
    FakeClock clock;
    Monitor m;
    m.Begin(MakeHooks(adc, clock));

    Gate gate;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        const bool fresh = (t % 2) == 0;
        threads.emplace_back([&, fresh]() {
            gate.Wait();
            while (!stop.load(std::memory_order_relaxed)) {
                const Sample s = fresh ? m.GetFresh() : m.Get();
                if (s.valid && s.millivolts != 3900) {
                    adc.faults.fetch_add(1);  // a torn sample
                }
            }
        });
    }
    gate.Open();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    stop.store(true);
    for (auto& th : threads) th.join();

    CHECK_EQ_I64(m.open_calls(), 1);
    CHECK_EQ_I64(adc.max_concurrent_readers.load(), 1);
    CHECK_EQ_I64(adc.faults.load(), 0);
}

}  // namespace

int main() {
    printf("test_battery_monitor\n\n");

    RUN(test_the_percentage_curve_clamps_at_both_ends);
    RUN(test_the_curve_matches_the_board_arithmetic_it_replaced);

    RUN(test_the_first_read_opens_the_unit_and_later_ones_do_not);
    RUN(test_a_chip_without_calibration_is_opened_once_and_never_again);
    RUN(test_a_unit_that_will_not_open_is_not_retried);
    RUN(test_a_monitor_with_no_hooks_at_all_reports_nothing_and_does_not_crash);
    RUN(test_a_second_begin_does_not_replace_the_hooks_that_own_the_handles);

    RUN(test_a_reading_inside_the_interval_does_not_touch_the_converter);
    RUN(test_the_cache_collapses_a_status_request_into_one_refresh);
    RUN(test_the_factory_test_bypasses_the_cache_but_not_the_lock);
    RUN(test_a_clock_that_jumps_backwards_refreshes_rather_than_freezing);
    RUN(test_peek_never_touches_the_converter);

    RUN(test_a_failed_reading_is_reported_not_escalated);
    RUN(test_a_zero_reading_is_unknown_rather_than_an_empty_battery);
    RUN(test_the_average_ignores_failed_conversions_instead_of_counting_them_as_zero);

    RUN(test_close_releases_once_and_is_idempotent);
    RUN(test_the_destructor_alone_releases_the_unit);
    RUN(test_a_failed_open_is_never_closed);
    RUN(test_an_open_that_hands_the_unit_back_itself_is_not_closed_again);
    RUN(test_reads_after_close_report_nothing_and_touch_no_handle);

    RUN(test_two_tasks_racing_the_first_read_open_the_unit_once);
    RUN(test_a_storm_of_concurrent_readers_never_reopens_or_faults);
    RUN(test_a_close_racing_readers_frees_the_unit_exactly_once);
    RUN(test_concurrent_fresh_and_cached_readers_stay_serialised);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
