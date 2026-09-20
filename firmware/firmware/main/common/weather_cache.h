/**
 * @file weather_cache.h
 * @brief The durable A/B store for the last forecast the device fetched.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why the forecast is persisted at all
 * ------------------------------------
 * Because otherwise "the device can draw a panel without the tower" is only
 * true on days when nothing else is wrong. A wake that finds no Wi-Fi, or finds
 * Open-Meteo returning 503, has to draw *something*, and the only honest
 * something is the last forecast it actually observed, labelled with its age.
 * Holding that in RAM would lose it on every deep sleep — which is to say, on
 * every cycle — so it lives in flash next to the frame and the profile.
 *
 * It is also what makes Device mode cheap. A wake with a cached forecast that
 * is not yet due for a refetch never brings the radio up at all: it composes
 * from these bytes and goes back to sleep. That is the single largest energy
 * saving in the feature, and it is this record that enables it.
 *
 * Why it is the same store as the frame and the profile
 * ----------------------------------------------------
 * RecordSlot, parameterised by a spec. The A/B discipline, the header CRC, the
 * payload digest, the write-inactive-then-read-back — all of it is already
 * written and already driven at every write-cut offset by the frame suite. A
 * second implementation for a third record type would be a second set of bugs.
 *
 * The magic keeps the three apart on one filesystem: a frame dropped into a
 * forecast slot fails validation and reads as "no forecast", which is the
 * answer that makes the panel say it does not know rather than draw a day of
 * garbage.
 *
 * What this deliberately does not do
 * ----------------------------------
 * It does not decide when to fetch — autonomy::ShouldFetchWeather does, and it
 * is host tested on its own. It does not decide whether a cached forecast is
 * too old to draw — autonomy::FreshnessOf does, from the profile's own
 * thresholds. This file stores bytes and hands them back, and knows nothing
 * about what they mean.
 */

#ifndef COMMON_WEATHER_CACHE_H
#define COMMON_WEATHER_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "openmeteo_parse.h"
#include "record_slot.h"

namespace weather {

/**
 * @brief The forecast record's spec.
 *
 * Fixed length, like the frame and unlike the profile: the encoding always
 * writes all twenty-four hour slots whether or not they are all populated, so
 * any other length is a corrupt record rather than a shorter forecast.
 */
constexpr record::RecordSpec kWeatherSpec = {
    {'N', '4', 'C', 'W', 'T', 'H', 'R', '1'},
    kEncodedForecastBytes,
    kEncodedForecastBytes};

/// Scratch a WeatherCache needs, and what one slot occupies once written.
constexpr size_t kWeatherRecordBytes = record::kHeaderBytes + kEncodedForecastBytes;

/**
 * @brief The last forecast this device fetched, across reboots.
 *
 * Not internally locked. The wake cycle owns it and serialises access, the
 * same ownership the frame store has.
 */
class WeatherCache : public record::RecordSlot {
public:
    /**
     * @param io          the two forecast slots.
     * @param scratch     at least kWeatherRecordBytes, caller-owned and reused.
     * @param scratch_len size of @p scratch.
     */
    WeatherCache(record::SlotIo* io, uint8_t* scratch, size_t scratch_len)
        : record::RecordSlot(kWeatherSpec, io, scratch, scratch_len) {}

    /// True when a valid forecast is stored. False both for an empty cache and
    /// for two corrupt slots: from the panel's point of view those are the same
    /// situation, and neither may produce a forecast of zeroes.
    bool has_forecast() const { return has_record(); }

    /**
     * @brief Copy the stored forecast out.
     * @return false when there is none, or when the bytes no longer decode.
     */
    bool Read(Forecast* out) const;

    /**
     * @brief Persist a forecast, if it is one worth keeping.
     *
     * @return kBadLength when @p f does not meet the coverage contract. Refused
     *         at the door rather than cached, because a later wake reading a
     *         short forecast back out has no way to tell it was ever short.
     *         kDuplicate when the bytes match what is already stored, in which
     *         case nothing is written.
     *
     * `fetched_epoch` is carried as the record's source epoch as well as inside
     * the payload, so the age of the cache is readable without decoding it.
     */
    record::StoreResult Store(const Forecast& f);

    /// When the stored forecast was observed, or 0 when there is none. This is
    /// the fetch time, not the write time: freshness belongs to the observation.
    int64_t fetched_epoch() const;
};

}  // namespace weather

#endif  // COMMON_WEATHER_CACHE_H
