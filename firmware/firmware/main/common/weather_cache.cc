/**
 * @file weather_cache.cc
 * @brief Implementation of the durable forecast cache.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See weather_cache.h for why this record exists. No ESP-IDF headers: the host
 * suite compiles this translation unit and drives a write cut at every byte
 * offset of it.
 */

#include "weather_cache.h"

#include <string.h>

namespace weather {

bool WeatherCache::Read(Forecast* out) const {
    if (out == nullptr || !has_record()) return false;

    uint8_t buf[kEncodedForecastBytes];
    size_t len = 0;
    if (!ReadPayload(buf, sizeof(buf), &len)) return false;

    // Decoded rather than memcpy'd into the struct. The record passed its own
    // digest, which says the bytes are the bytes that were written; it does not
    // say they are a forecast this build understands. A record written by a
    // newer firmware and then rolled back reads as "no forecast" here, which is
    // the answer that makes the panel say it does not know.
    return DecodeForecast(buf, len, out);
}

record::StoreResult WeatherCache::Store(const Forecast& f) {
    // The coverage contract, at the door. A forecast too short to cover the day
    // is refused rather than cached: nothing downstream re-checks, and a wake
    // reading it back would draw a day it cannot actually see.
    if (!f.valid()) return record::StoreResult::kBadLength;

    uint8_t buf[kEncodedForecastBytes];
    const size_t written = EncodeForecast(f, buf, sizeof(buf));
    if (written != kEncodedForecastBytes) return record::StoreResult::kBadLength;

    // fetched_epoch in the header as well as the payload, so the age of the
    // cache is readable from the slot status without decoding it — which is
    // what the status route wants and what a fetch decision wants.
    //
    // Narrowed to 32 bits because that is the header field's width. It is a
    // 2038 problem shared with every other record on this device, and the
    // payload keeps the full 64-bit value, so the narrowing costs nothing that
    // is not already recoverable.
    const uint32_t epoch = f.fetched_epoch > 0
                               ? static_cast<uint32_t>(f.fetched_epoch)
                               : 0u;
    return record::RecordSlot::Store(buf, written, epoch);
}

int64_t WeatherCache::fetched_epoch() const {
    if (!has_record()) return 0;
    return static_cast<int64_t>(active_source_epoch());
}

}  // namespace weather
