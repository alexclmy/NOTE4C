/**
 * @file test_weather_cache.cc
 * @brief Host tests for the durable forecast cache.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The cache is the difference between "the device draws a panel when the
 * network is there" and "the device draws a panel". A wake with no Wi-Fi, or
 * with Open-Meteo down, composes from this record or composes nothing.
 *
 * So the properties tested here are the ones that decide whether a panel
 * survives a bad day:
 *
 *  - a forecast written now is readable after a reboot,
 *  - a write cut at any byte offset leaves the previous forecast intact, never
 *    a mixture of two,
 *  - both slots corrupt reads as "no forecast" and never as a forecast of
 *    zeroes, because a panel showing 0.0 °C for every hour of tomorrow is worse
 *    than a panel saying it does not know,
 *  - a frame record dropped into a forecast slot is refused by magic, which is
 *    what makes it safe for three record types to share one filesystem.
 */

#include "common/weather_cache.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

using namespace weather;

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

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const long long va = (long long)(a);                               \
        const long long vb = (long long)(b);                               \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n",        \
                   __FILE__, __LINE__, g_current, #a, #b, va, vb);         \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-62s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

/// In-memory slots with a write-cut injector, the same shape the frame and
/// profile suites use.
class FakeIo : public record::SlotIo {
public:
    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        if (slot < 0 || slot >= record::kSlotCount) return -1;
        const std::vector<uint8_t>& s = slots_[slot];
        const size_t n = s.size() < max ? s.size() : max;
        memcpy(buf, s.data(), n);
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (slot < 0 || slot >= record::kSlotCount) return false;
        if (cut_after_ >= 0) {
            // A power cut: as many bytes as asked for land, the rest do not,
            // and the call reports the failure the filesystem would not.
            const size_t kept = static_cast<size_t>(cut_after_) < len
                                    ? static_cast<size_t>(cut_after_)
                                    : len;
            slots_[slot].assign(data, data + kept);
            return false;
        }
        slots_[slot].assign(data, data + len);
        return true;
    }

    void CutAfter(int bytes) { cut_after_ = bytes; }
    void NoCut() { cut_after_ = -1; }
    void Scribble(int slot, size_t at, uint8_t value) {
        if (at < slots_[slot].size()) slots_[slot][at] = value;
    }
    size_t SlotSize(int slot) const { return slots_[slot].size(); }

private:
    std::vector<uint8_t> slots_[record::kSlotCount];
    int cut_after_ = -1;
};

struct Fixture {
    FakeIo io;
    std::vector<uint8_t> scratch;
    WeatherCache cache;

    Fixture() : scratch(kWeatherRecordBytes), cache(&io, scratch.data(), scratch.size()) {}
};

/// A forecast that is recognisably itself, so a mixture of two is visible.
Forecast Sample(int64_t fetched, int16_t base) {
    Forecast f;
    f.fetched_epoch = fetched;
    f.first_hour_epoch = fetched;
    f.hour_count = kMaxHours;
    for (size_t i = 0; i < kMaxHours; ++i) {
        f.temp_c10[i] = static_cast<int16_t>(base + static_cast<int16_t>(i));
        f.wmo[i] = static_cast<uint8_t>(i);
    }
    f.low_c10 = base;
    f.high_c10 = static_cast<int16_t>(base + kMaxHours - 1);
    return f;
}

bool Same(const Forecast& a, const Forecast& b) {
    if (a.fetched_epoch != b.fetched_epoch) return false;
    if (a.first_hour_epoch != b.first_hour_epoch) return false;
    if (a.hour_count != b.hour_count) return false;
    if (a.low_c10 != b.low_c10 || a.high_c10 != b.high_c10) return false;
    for (size_t i = 0; i < kMaxHours; ++i) {
        if (a.temp_c10[i] != b.temp_c10[i]) return false;
        if (a.wmo[i] != b.wmo[i]) return false;
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------- the basics --

static void test_a_forecast_written_now_is_readable_now() {
    Fixture f;
    const Forecast in = Sample(1789387200, 150);
    CHECK(f.cache.Store(in) == record::StoreResult::kOk);
    CHECK(f.cache.has_forecast());

    Forecast out;
    CHECK(f.cache.Read(&out));
    CHECK(Same(in, out));
}

static void test_an_empty_cache_reports_nothing_rather_than_zeroes() {
    // The distinction the whole panel rests on: "I have no forecast" and "every
    // hour tomorrow is 0.0 degrees" must never be the same answer.
    Fixture f;
    CHECK(f.cache.Load() == false);
    CHECK(!f.cache.has_forecast());
    Forecast out;
    CHECK(!f.cache.Read(&out));
}

static void test_a_forecast_survives_a_reload_the_way_a_reboot_would() {
    Fixture f;
    const Forecast in = Sample(1789387200, -85);
    CHECK(f.cache.Store(in) == record::StoreResult::kOk);

    std::vector<uint8_t> scratch(kWeatherRecordBytes);
    WeatherCache reloaded(&f.io, scratch.data(), scratch.size());
    CHECK(reloaded.Load());
    CHECK(reloaded.has_forecast());
    Forecast out;
    CHECK(reloaded.Read(&out));
    CHECK(Same(in, out));
    // Negative temperatures round-trip: the wire is two's complement and a
    // Montreal January is the normal case, not an edge case.
    CHECK_EQ_INT(out.temp_c10[0], -85);
}

static void test_storing_the_same_forecast_twice_writes_nothing() {
    // The wear argument. A device that refetched an identical forecast every
    // thirty minutes and rewrote flash for it would be spending the panel's
    // life on a no-op.
    Fixture f;
    const Forecast in = Sample(1789387200, 150);
    CHECK(f.cache.Store(in) == record::StoreResult::kOk);
    CHECK(f.cache.Store(in) == record::StoreResult::kDuplicate);
}

static void test_a_newer_forecast_replaces_an_older_one() {
    Fixture f;
    CHECK(f.cache.Store(Sample(1789387200, 150)) == record::StoreResult::kOk);
    const Forecast second = Sample(1789390800, 200);
    CHECK(f.cache.Store(second) == record::StoreResult::kOk);

    Forecast out;
    CHECK(f.cache.Read(&out));
    CHECK(Same(second, out));
    CHECK_EQ_INT(out.fetched_epoch, 1789390800);
}

// ------------------------------------------------------------ the bad days --

static void test_a_power_cut_at_any_offset_never_tears_a_forecast() {
    // The test that earns the A/B design. A write is cut at every byte offset
    // of the record, and what loads afterwards must be exactly one of the two
    // complete forecasts — never a header from one and hours from the other.
    const Forecast first = Sample(1789387200, 150);
    const Forecast second = Sample(1789390800, -200);

    const size_t record = kWeatherRecordBytes;
    int cuts = 0;
    for (size_t offset = 0; offset <= record; ++offset) {
        Fixture f;
        CHECK(f.cache.Store(first) == record::StoreResult::kOk);

        f.io.CutAfter(static_cast<int>(offset));
        f.cache.Store(second);
        f.io.NoCut();

        std::vector<uint8_t> scratch(kWeatherRecordBytes);
        WeatherCache reloaded(&f.io, scratch.data(), scratch.size());
        reloaded.Load();

        ++cuts;
        if (!reloaded.has_forecast()) {
            // Acceptable only if the cut destroyed the slot that was being
            // written *and* the other one — which this sequence cannot do,
            // because the first forecast is in the other slot and untouched.
            ++g_failures;
            printf("  FAIL in %s: no forecast after a cut at offset %zu\n",
                   g_current, offset);
            continue;
        }
        Forecast out;
        reloaded.Read(&out);
        ++g_checks;
        if (!Same(out, first) && !Same(out, second)) {
            ++g_failures;
            printf("  FAIL in %s: torn forecast after a cut at offset %zu\n",
                   g_current, offset);
        }
    }
    CHECK(cuts == static_cast<int>(record) + 1);
}

static void test_both_slots_corrupt_reads_as_no_forecast() {
    Fixture f;
    CHECK(f.cache.Store(Sample(1789387200, 150)) == record::StoreResult::kOk);
    CHECK(f.cache.Store(Sample(1789390800, 200)) == record::StoreResult::kOk);

    // Flip a payload byte in each slot, past the header, so both fail their own
    // digest rather than their magic.
    for (int slot = 0; slot < record::kSlotCount; ++slot) {
        CHECK(f.io.SlotSize(slot) > record::kHeaderBytes);
        f.io.Scribble(slot, record::kHeaderBytes + 3, 0xa5);
    }

    std::vector<uint8_t> scratch(kWeatherRecordBytes);
    WeatherCache reloaded(&f.io, scratch.data(), scratch.size());
    CHECK(!reloaded.Load());
    CHECK(!reloaded.has_forecast());
    Forecast out;
    CHECK(!reloaded.Read(&out));
}

static void test_a_frame_record_is_not_mistaken_for_a_forecast() {
    // Three record types share one SPIFFS partition. The magic is what keeps a
    // frame from being read as a forecast of garbage.
    Fixture f;
    CHECK(f.cache.Store(Sample(1789387200, 150)) == record::StoreResult::kOk);

    for (int slot = 0; slot < record::kSlotCount; ++slot) {
        f.io.Scribble(slot, 0, 'N');
        f.io.Scribble(slot, 1, '4');
        f.io.Scribble(slot, 2, 'C');
        f.io.Scribble(slot, 3, 'D');
        f.io.Scribble(slot, 4, 'A');
        f.io.Scribble(slot, 5, 'S');
        f.io.Scribble(slot, 6, 'H');
        f.io.Scribble(slot, 7, '1');
    }

    std::vector<uint8_t> scratch(kWeatherRecordBytes);
    WeatherCache reloaded(&f.io, scratch.data(), scratch.size());
    CHECK(!reloaded.Load());
    CHECK(!reloaded.has_forecast());
}

static void test_a_forecast_with_too_few_hours_is_refused_before_it_is_stored() {
    // The coverage contract, applied at the door. A forecast that cannot cover
    // the day is refused rather than cached, because a cached short forecast
    // would be drawn later by a wake that had no way to tell it was short.
    Fixture f;
    Forecast thin = Sample(1789387200, 150);
    thin.hour_count = 4;
    CHECK(f.cache.Store(thin) == record::StoreResult::kBadLength);
    CHECK(!f.cache.has_forecast());
}

static void test_the_stored_age_is_measured_from_the_fetch_not_the_write() {
    // Freshness is a property of the observation, not of the flash write. A
    // device that rewrote its cache would otherwise make a three-hour-old
    // forecast look new.
    Fixture f;
    CHECK(f.cache.Store(Sample(1789387200, 150)) == record::StoreResult::kOk);
    Forecast out;
    CHECK(f.cache.Read(&out));
    CHECK_EQ_INT(out.fetched_epoch, 1789387200);
    CHECK_EQ_INT(f.cache.fetched_epoch(), 1789387200);
}

int main() {
    RUN(test_a_forecast_written_now_is_readable_now);
    RUN(test_an_empty_cache_reports_nothing_rather_than_zeroes);
    RUN(test_a_forecast_survives_a_reload_the_way_a_reboot_would);
    RUN(test_storing_the_same_forecast_twice_writes_nothing);
    RUN(test_a_newer_forecast_replaces_an_older_one);

    RUN(test_a_power_cut_at_any_offset_never_tears_a_forecast);
    RUN(test_both_slots_corrupt_reads_as_no_forecast);
    RUN(test_a_frame_record_is_not_mistaken_for_a_forecast);
    RUN(test_a_forecast_with_too_few_hours_is_refused_before_it_is_stored);
    RUN(test_the_stored_age_is_measured_from_the_fetch_not_the_write);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
