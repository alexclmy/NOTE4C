/**
 * @file openmeteo_parse.h
 * @brief Reading a forecast the device fetched for itself, and refusing every
 *        forecast that is not complete enough to draw.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * What this is a port of
 * ----------------------
 * The tower's src/server/sources/openMeteo.ts, and specifically its coverage
 * contract, which is the part worth carrying rather than the part that was easy
 * to carry. That contract says a forecast is only usable when it has at least
 * 23 hourly entries inside the next 24 hours, starts within the hour, reaches
 * 23 hours out, and has no gap wider than an hour.
 *
 * The reason it is a refusal rather than a best effort is the medium. E-paper
 * holds whatever was last drawn, indefinitely and without a spinner. A panel
 * showing four hours of forecast looks exactly as authoritative as one showing
 * twenty-four, and there is nothing on the glass to say which one you are
 * looking at. So a partial forecast is worse than none, and the device says
 * "unavailable" rather than drawing what it happens to have.
 *
 * What is deliberately NOT ported
 * -------------------------------
 * The MET Norway parser eMini uses, because we do not use MET Norway; the
 * ECCC fallback, because a second source is a tower concern and the device has
 * one origin by design; and eMini's 730-line fetcher, whose ideas are taken
 * (reject rather than guess on units, refuse duplicate keys, jitter the next
 * fetch) without its machinery, which exists to serve a general URL this device
 * will never have.
 *
 * Why units are checked rather than converted
 * ------------------------------------------
 * If the response says the temperatures are in Fahrenheit, this refuses the
 * document. It does not convert. A converter is a piece of code that turns a
 * disagreement about what was requested into a plausible number, and the
 * failure it hides — someone changing the request URL — is exactly the one
 * worth seeing. The device asks for Celsius; a reply in anything else means the
 * device and the service are not talking about the same thing.
 *
 * Free of ESP-IDF headers: the host suite compiles this exact translation unit,
 * fixtures and all.
 */

#ifndef COMMON_OPENMETEO_PARSE_H
#define COMMON_OPENMETEO_PARSE_H

#include <stddef.h>
#include <stdint.h>

namespace weather {

/// The most hourly entries a cached forecast keeps. One day, which is what the
/// panel draws; the response carries more and the rest is dropped at parse
/// time rather than stored and ignored.
constexpr size_t kMaxHours = 24;

/// The coverage contract, in one place. Ported from the tower's normalizer.
constexpr int kMinHours = 23;
constexpr int64_t kHourSeconds = 3600;

/// Plausible air temperatures, in tenths of a degree Celsius. Outside this the
/// number is measuring something other than the weather, and drawing it would
/// be a confident lie rather than a rounding error.
constexpr int16_t kMinTempC10 = -900;
constexpr int16_t kMaxTempC10 = 600;

/// Largest response body the device will read. eMini's budget discipline,
/// applied to one known endpoint: three days of hourly data is a few kilobytes,
/// and anything an order of magnitude past that is not a forecast.
constexpr size_t kMaxResponseBytes = 32768;

/**
 * @brief The normalised forecast, as it is cached and drawn.
 *
 * Temperatures in tenths of a degree rather than floats: the cache record is
 * compared byte for byte to decide whether the panel changed, and two floats
 * that print the same and differ in their last bit would cost a needless
 * 25-second refresh. Integers make "did this change" exact.
 */
struct Forecast {
    /// When the device fetched this. The panel prints it, and the staleness
    /// thresholds are measured from it.
    int64_t fetched_epoch = 0;
    /// UTC epoch of hours[0].
    int64_t first_hour_epoch = 0;
    uint8_t hour_count = 0;
    int16_t temp_c10[kMaxHours] = {};
    /// Raw WMO code per hour, 255 when the service sent null.
    uint8_t wmo[kMaxHours] = {};
    int16_t low_c10 = 0;
    int16_t high_c10 = 0;

    bool valid() const { return hour_count >= kMinHours; }
};

/// The conditions the renderer draws, mapped from WMO codes exactly as the
/// tower's conditionForWmoCode does, so a locally composed panel and a pushed
/// one name the same sky the same way.
enum class Condition : uint8_t {
    kUnknown = 0,
    kSunny,
    kPartlyCloudy,
    kCloudy,
    kFog,
    kRainy,
    kSnowy,
    kThunder,
};

Condition ConditionForWmoCode(int code);
const char* ConditionName(Condition c);

/// Stable refusal tokens. As with the profile, these cross into the tower's
/// copy table, so their spelling is part of the contract.
extern const char* const kErrShape;       ///< not the document Open-Meteo sends
extern const char* const kErrLengths;     ///< the three arrays disagree
extern const char* const kErrUnits;       ///< a unit we did not ask for
extern const char* const kErrCoverage;    ///< too few hours, or a gap, or too late
extern const char* const kErrTemperature; ///< a value outside the plausible range
extern const char* const kErrTooLarge;    ///< body past kMaxResponseBytes
extern const char* const kErrDuplicate;   ///< a duplicate key

struct ParseError {
    const char* code = nullptr;
    char detail[64] = {};
    size_t offset = 0;
};

/**
 * @brief Parse and validate a forecast response.
 *
 * @param body      the response bytes.
 * @param len       their length.
 * @param now_epoch UTC seconds, from a clock the caller believes is set.
 * @param out       filled on success.
 * @param err       filled on failure.
 *
 * @return true only when the result is complete enough to draw for a day.
 *
 * `now_epoch` is passed in rather than read here so the host suite can drive a
 * forecast that is eleven hours old without waiting eleven hours, and so the
 * whole function stays pure.
 */
bool ParseForecast(const char* body, size_t len, int64_t now_epoch, Forecast* out,
                   ParseError* err);

/**
 * @brief Convert "YYYY-MM-DDTHH:MM" (and the :SS and Z forms) to a UTC epoch.
 *
 * The request pins timezone=UTC, so the stamps come back naive and are UTC.
 * Exposed because it is worth testing on its own: a date routine that is wrong
 * one day in four years is a bug nobody finds by looking.
 *
 * @return false when the stamp is not a stamp.
 */
bool ParseIso8601Utc(const char* text, size_t len, int64_t* out_epoch);

// -------------------------------------------------------------- the cache --

/**
 * @brief Serialise a Forecast to the bytes the A/B cache record holds.
 *
 * Fixed layout, little-endian, versioned. Written out field by field rather
 * than memcpy'd from the struct so the record does not depend on this
 * compiler's padding, which is the same reason the slot header is written that
 * way.
 *
 * @return bytes written, or 0 when @p cap was too small.
 */
size_t EncodeForecast(const Forecast& f, uint8_t* out, size_t cap);

/// Read back what EncodeForecast wrote. False when the bytes are not a record
/// this build understands.
bool DecodeForecast(const uint8_t* data, size_t len, Forecast* out);

/// Bytes EncodeForecast needs. Well inside the 4 KB the cache record allows.
constexpr size_t kEncodedForecastBytes = 4 + 8 + 8 + 1 + 1 + (kMaxHours * 3) + 4;

}  // namespace weather

#endif  // COMMON_OPENMETEO_PARSE_H
