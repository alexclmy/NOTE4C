/**
 * @file openmeteo_parse.cc
 * @brief Implementation of the device's forecast reader.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See openmeteo_parse.h for what this is a port of and what it deliberately
 * refuses. No ESP-IDF headers: the host suite compiles this exact file.
 *
 * Portions of the validation discipline here — reject rather than guess on
 * units, refuse duplicate keys, treat a partial document as no document — are
 * adapted from the approach taken in eMini Home 0.4.0's fetch and parse layer,
 * © 2026 Tomasz Fiedoruk, MIT, commit 05ec313f. No code is copied: eMini reads
 * MET Norway, whose schema is not this one. See THIRD_PARTY_NOTICES.md.
 */

#include "openmeteo_parse.h"

#include <stdio.h>
#include <string.h>

#include "json_scan.h"

namespace weather {

const char* const kErrShape = "forecast_shape";
const char* const kErrLengths = "forecast_array_lengths";
const char* const kErrUnits = "forecast_units";
const char* const kErrCoverage = "forecast_coverage";
const char* const kErrTemperature = "forecast_temperature_range";
const char* const kErrTooLarge = "forecast_too_large";
const char* const kErrDuplicate = "forecast_duplicate_key";

Condition ConditionForWmoCode(int code) {
    // Identical to the tower's conditionForWmoCode, case for case. Two
    // renderers naming the same sky differently would be visible on the glass
    // the first time the tower went away.
    if (code == 0) return Condition::kSunny;
    if (code == 1 || code == 2) return Condition::kPartlyCloudy;
    if (code == 3) return Condition::kCloudy;
    if (code == 45 || code == 48) return Condition::kFog;
    if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 ||
        code == 86) {
        return Condition::kSnowy;
    }
    if (code == 95 || code == 96 || code == 99) return Condition::kThunder;
    if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57 ||
        code == 61 || code == 63 || code == 65 || code == 66 || code == 67 ||
        code == 80 || code == 81 || code == 82) {
        return Condition::kRainy;
    }
    return Condition::kUnknown;
}

const char* ConditionName(Condition c) {
    switch (c) {
        case Condition::kUnknown: return "unknown";
        case Condition::kSunny: return "sunny";
        case Condition::kPartlyCloudy: return "partlycloudy";
        case Condition::kCloudy: return "cloudy";
        case Condition::kFog: return "fog";
        case Condition::kRainy: return "rainy";
        case Condition::kSnowy: return "snowy";
        case Condition::kThunder: return "lightning-rainy";
    }
    return "unknown";
}

namespace {

/**
 * @brief Days from 1970-01-01 to a civil date, for any proleptic Gregorian y/m/d.
 *
 * Howard Hinnant's days_from_civil, which is the standard branch-free form of
 * this calculation. Written out rather than reached for through <ctime>
 * because timegm() is not portable, mktime() applies a local timezone this
 * device does not have, and both would make an arithmetic question depend on
 * the C library's idea of where the panel is.
 */
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool AllDigits(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

int ReadNumber(const char* s, size_t n) {
    int value = 0;
    for (size_t i = 0; i < n; ++i) value = value * 10 + (s[i] - '0');
    return value;
}

/// Is @p day a real day of @p month in @p year? A forecast stamped 31 February
/// is not a forecast, and silently normalising it to 3 March would move an
/// hour of weather to a different day.
bool IsRealDate(int year, int month, int day) {
    if (month < 1 || month > 12 || day < 1) return false;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = kDays[month - 1];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) limit = 29;
    }
    return day <= limit;
}

}  // namespace

bool ParseIso8601Utc(const char* text, size_t len, int64_t* out_epoch) {
    // Accepted: YYYY-MM-DDTHH:MM, plus an optional :SS and an optional Z.
    if (text == nullptr || len < 16) return false;
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') {
        return false;
    }
    if (!AllDigits(text, 4) || !AllDigits(text + 5, 2) || !AllDigits(text + 8, 2) ||
        !AllDigits(text + 11, 2) || !AllDigits(text + 14, 2)) {
        return false;
    }

    int second = 0;
    size_t used = 16;
    if (len >= 19 && text[16] == ':') {
        if (!AllDigits(text + 17, 2)) return false;
        second = ReadNumber(text + 17, 2);
        used = 19;
    }
    if (used < len) {
        // The only trailing byte this accepts is an explicit UTC marker; an
        // offset like +02:00 would mean the service ignored timezone=UTC, and
        // guessing which way to shift is how an hour of weather moves.
        if (!(len == used + 1 && text[used] == 'Z')) return false;
    }

    const int year = ReadNumber(text, 4);
    const int month = ReadNumber(text + 5, 2);
    const int day = ReadNumber(text + 8, 2);
    const int hour = ReadNumber(text + 11, 2);
    const int minute = ReadNumber(text + 14, 2);

    if (!IsRealDate(year, month, day)) return false;
    if (hour > 23 || minute > 59 || second > 60) return false;

    const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month),
                                       static_cast<unsigned>(day));
    *out_epoch = days * 86400ll + hour * 3600ll + minute * 60ll + second;
    return true;
}

namespace {

/// One hour, as read before the coverage rules are applied.
struct Row {
    int64_t at = 0;
    int16_t temp_c10 = 0;
    uint8_t wmo = 255;
};

/**
 * @brief Rows held while parsing.
 *
 * The three arrays are parallel and have to be zipped by index, so all of
 * `time` has to be in hand before the first hour can be placed. The request
 * asks for three days, which is 72 entries; 96 leaves headroom without making
 * the buffer the interesting thing about this function.
 *
 * It matters because Arrays is a local — about 3 KB of the fetch phase's
 * stack. That is deliberate: a static would hold the same memory for the 23
 * hours a day when no fetch is running, and this device's whole argument is
 * about what it costs while idle.
 */
constexpr size_t kMaxRows = 96;

struct Arrays {
    /// "YYYY-MM-DDTHH:MM:SSZ" is 20 bytes plus a terminator.
    char stamps[kMaxRows][24];
    uint8_t stamp_len[kMaxRows];
    size_t time_count = 0;

    int16_t temps[kMaxRows];
    bool temp_null[kMaxRows];
    size_t temp_count = 0;

    int codes[kMaxRows];
    size_t code_count = 0;
};

void Fail(ParseError* err, const char* code, const char* detail, size_t offset) {
    if (err->code != nullptr) return;
    err->code = code;
    err->offset = offset;
    snprintf(err->detail, sizeof(err->detail), "%s", detail);
}

/// Read `hourly_units` and refuse any unit we did not ask for.
bool CheckUnits(json::Reader& r, ParseError* err) {
    if (!r.EnterObject()) {
        Fail(err, kErrShape, "hourly_units", r.error_offset());
        return false;
    }
    char key[64];
    while (r.NextKey(key, sizeof(key), nullptr)) {
        if (strcmp(key, "temperature_2m") == 0) {
            char unit[24];
            if (!r.ReadString(unit, sizeof(unit), nullptr)) {
                Fail(err, kErrShape, "hourly_units.temperature_2m", r.error_offset());
                return false;
            }
            // "°C" as the API spells it, UTF-8 degree sign included. Anything
            // else is refused rather than converted: see the header.
            if (strcmp(unit, "\xc2\xb0""C") != 0 && strcmp(unit, "C") != 0) {
                Fail(err, kErrUnits, unit, r.offset());
                return false;
            }
        } else if (!r.SkipValue()) {
            Fail(err, kErrShape, "hourly_units", r.error_offset());
            return false;
        }
    }
    if (!r.ok()) {
        Fail(err, kErrShape, "hourly_units", r.error_offset());
        return false;
    }
    return true;
}

bool ReadHourly(json::Reader& r, Arrays* a, ParseError* err) {
    if (!r.EnterObject()) {
        Fail(err, kErrShape, "hourly", r.error_offset());
        return false;
    }
    uint32_t seen = 0;
    char key[64];

    while (r.NextKey(key, sizeof(key), nullptr)) {
        uint32_t bit = 0;
        if (strcmp(key, "time") == 0) {
            bit = 1u << 0;
            if (!r.EnterArray()) {
                Fail(err, kErrShape, "hourly.time", r.error_offset());
                return false;
            }
            while (r.NextElement()) {
                if (a->time_count >= kMaxRows) {
                    Fail(err, kErrShape, "hourly.time too long", r.offset());
                    return false;
                }
                size_t n = 0;
                if (!r.ReadString(a->stamps[a->time_count],
                                  sizeof(a->stamps[0]), &n)) {
                    Fail(err, kErrShape, "hourly.time", r.error_offset());
                    return false;
                }
                a->stamp_len[a->time_count] = static_cast<uint8_t>(n);
                ++a->time_count;
            }
            if (!r.ok()) {
                Fail(err, kErrShape, "hourly.time", r.error_offset());
                return false;
            }
        } else if (strcmp(key, "temperature_2m") == 0) {
            bit = 1u << 1;
            if (!r.EnterArray()) {
                Fail(err, kErrShape, "hourly.temperature_2m", r.error_offset());
                return false;
            }
            while (r.NextElement()) {
                if (a->temp_count >= kMaxRows) {
                    Fail(err, kErrShape, "hourly.temperature_2m too long", r.offset());
                    return false;
                }
                json::Type type;
                if (!r.PeekType(&type)) {
                    Fail(err, kErrShape, "hourly.temperature_2m", r.error_offset());
                    return false;
                }
                if (type == json::Type::kNull) {
                    if (!r.ReadNull()) {
                        Fail(err, kErrShape, "hourly.temperature_2m", r.error_offset());
                        return false;
                    }
                    a->temp_null[a->temp_count] = true;
                    a->temps[a->temp_count] = 0;
                } else {
                    double value = 0;
                    // Bounded at read time, so a temperature of 5e300 is a
                    // refusal rather than an integer conversion nobody defined.
                    if (!r.ReadDouble(-1000.0, 1000.0, &value)) {
                        Fail(err, kErrTemperature, "hourly.temperature_2m",
                             r.error_offset());
                        return false;
                    }
                    const double tenths = value * 10.0;
                    const int32_t rounded = static_cast<int32_t>(
                        tenths < 0 ? tenths - 0.5 : tenths + 0.5);
                    if (rounded < kMinTempC10 || rounded > kMaxTempC10) {
                        Fail(err, kErrTemperature, "hourly.temperature_2m", r.offset());
                        return false;
                    }
                    a->temp_null[a->temp_count] = false;
                    a->temps[a->temp_count] = static_cast<int16_t>(rounded);
                }
                ++a->temp_count;
            }
            if (!r.ok()) {
                Fail(err, kErrShape, "hourly.temperature_2m", r.error_offset());
                return false;
            }
        } else if (strcmp(key, "weather_code") == 0) {
            bit = 1u << 2;
            if (!r.EnterArray()) {
                Fail(err, kErrShape, "hourly.weather_code", r.error_offset());
                return false;
            }
            while (r.NextElement()) {
                if (a->code_count >= kMaxRows) {
                    Fail(err, kErrShape, "hourly.weather_code too long", r.offset());
                    return false;
                }
                json::Type type;
                if (!r.PeekType(&type)) {
                    Fail(err, kErrShape, "hourly.weather_code", r.error_offset());
                    return false;
                }
                if (type == json::Type::kNull) {
                    if (!r.ReadNull()) {
                        Fail(err, kErrShape, "hourly.weather_code", r.error_offset());
                        return false;
                    }
                    a->codes[a->code_count] = -1;
                } else {
                    int64_t code = 0;
                    if (!r.ReadInt(0, 254, &code)) {
                        Fail(err, kErrShape, "hourly.weather_code", r.error_offset());
                        return false;
                    }
                    a->codes[a->code_count] = static_cast<int>(code);
                }
                ++a->code_count;
            }
            if (!r.ok()) {
                Fail(err, kErrShape, "hourly.weather_code", r.error_offset());
                return false;
            }
        } else if (!r.SkipValue()) {
            // Unknown keys inside `hourly` are skipped, not refused. This is
            // the opposite of the profile's rule, and deliberately so: the
            // profile is a document we wrote and control, while this is a
            // third-party response that may legitimately grow fields. Refusing
            // those would mean the panel goes dark the day Open-Meteo adds one.
            Fail(err, kErrShape, "hourly", r.error_offset());
            return false;
        }

        if (bit != 0) {
            if (seen & bit) {
                Fail(err, kErrDuplicate, key, r.offset());
                return false;
            }
            seen |= bit;
        }
    }
    if (!r.ok()) {
        Fail(err, kErrShape, "hourly", r.error_offset());
        return false;
    }
    if (seen != 0x7u) {
        Fail(err, kErrShape, "hourly is missing one of time/temperature_2m/weather_code",
             r.offset());
        return false;
    }
    return true;
}

}  // namespace

bool ParseForecast(const char* body, size_t len, int64_t now_epoch, Forecast* out,
                   ParseError* err) {
    ParseError local;
    ParseError* e = err != nullptr ? err : &local;
    *e = ParseError{};

    if (out == nullptr || body == nullptr) {
        Fail(e, kErrShape, "no body", 0);
        return false;
    }
    if (len > kMaxResponseBytes) {
        Fail(e, kErrTooLarge, "body", len);
        return false;
    }

    // Arrays is ~6 KB. Deliberately a local: it lives only for this call, on a
    // stack the fetch phase controls, rather than as a static that would hold
    // internal RAM for the 23 hours of every day when no fetch is running.
    Arrays arrays;
    memset(&arrays, 0, sizeof(arrays));

    json::Reader r(body, len);
    if (!r.EnterObject()) {
        Fail(e, kErrShape, "root", r.error_offset());
        return false;
    }
    char key[64];
    bool saw_hourly = false;
    bool saw_units = false;

    while (r.NextKey(key, sizeof(key), nullptr)) {
        if (strcmp(key, "hourly") == 0) {
            if (saw_hourly) {
                Fail(e, kErrDuplicate, "hourly", r.offset());
                return false;
            }
            saw_hourly = true;
            if (!ReadHourly(r, &arrays, e)) return false;
        } else if (strcmp(key, "hourly_units") == 0) {
            if (saw_units) {
                Fail(e, kErrDuplicate, "hourly_units", r.offset());
                return false;
            }
            saw_units = true;
            if (!CheckUnits(r, e)) return false;
        } else if (!r.SkipValue()) {
            Fail(e, kErrShape, "root", r.error_offset());
            return false;
        }
    }
    if (!r.ok() || !r.Finish()) {
        Fail(e, kErrShape, "root", r.error_offset());
        return false;
    }
    if (!saw_hourly) {
        Fail(e, kErrShape, "hourly", r.offset());
        return false;
    }

    if (arrays.time_count != arrays.temp_count ||
        arrays.time_count != arrays.code_count) {
        // Three arrays that disagree about how many hours there are cannot be
        // zipped, and picking the shortest would silently drop the hours the
        // service did send.
        Fail(e, kErrLengths, "time/temperature_2m/weather_code", 0);
        return false;
    }

    // Zip, drop the hours outside the window, and keep them in order. The
    // service sends them sorted; this does not assume it.
    const int64_t window_end = now_epoch + 24 * kHourSeconds;
    Row rows[kMaxHours];
    size_t count = 0;

    for (size_t i = 0; i < arrays.time_count; ++i) {
        if (arrays.temp_null[i]) continue;  // an hour with no temperature
        int64_t at = 0;
        if (!ParseIso8601Utc(arrays.stamps[i], arrays.stamp_len[i], &at)) {
            // A stamp we cannot read is dropped rather than refused: one bad
            // entry in seventy-two should not cost the whole forecast, and the
            // coverage rules below will refuse the result if it mattered.
            continue;
        }
        if (at < now_epoch || at >= window_end) continue;

        // Insertion sort by time, discarding an exact duplicate hour. At 24
        // entries this is cheaper than anything cleverer and has no edge cases.
        if (count >= kMaxHours) continue;
        size_t at_index = count;
        bool duplicate = false;
        for (size_t k = 0; k < count; ++k) {
            if (rows[k].at == at) {
                duplicate = true;
                break;
            }
            if (rows[k].at > at) {
                at_index = k;
                break;
            }
        }
        if (duplicate) continue;
        for (size_t k = count; k > at_index; --k) rows[k] = rows[k - 1];
        rows[at_index].at = at;
        rows[at_index].temp_c10 = arrays.temps[i];
        rows[at_index].wmo = arrays.codes[i] < 0
                                 ? 255u
                                 : static_cast<uint8_t>(arrays.codes[i]);
        ++count;
    }

    // The coverage contract, ported line for line from the tower's normalizer.
    if (count < static_cast<size_t>(kMinHours)) {
        Fail(e, kErrCoverage, "fewer than 23 hours in the next 24", 0);
        return false;
    }
    if (rows[0].at - now_epoch > kHourSeconds) {
        Fail(e, kErrCoverage, "does not start within the hour", 0);
        return false;
    }
    if (rows[count - 1].at - now_epoch < 23 * kHourSeconds) {
        Fail(e, kErrCoverage, "does not reach 23 hours out", 0);
        return false;
    }
    for (size_t i = 1; i < count; ++i) {
        if (rows[i].at - rows[i - 1].at > kHourSeconds) {
            Fail(e, kErrCoverage, "gap wider than an hour", 0);
            return false;
        }
    }

    *out = Forecast{};
    out->fetched_epoch = now_epoch;
    out->first_hour_epoch = rows[0].at;
    out->hour_count = static_cast<uint8_t>(count);
    int16_t low = rows[0].temp_c10;
    int16_t high = rows[0].temp_c10;
    for (size_t i = 0; i < count; ++i) {
        out->temp_c10[i] = rows[i].temp_c10;
        out->wmo[i] = rows[i].wmo;
        if (rows[i].temp_c10 < low) low = rows[i].temp_c10;
        if (rows[i].temp_c10 > high) high = rows[i].temp_c10;
    }
    out->low_c10 = low;
    out->high_c10 = high;
    return true;
}

// -------------------------------------------------------------- the cache --

namespace {

constexpr uint8_t kForecastRecordVersion = 1;

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void PutI64(uint8_t* p, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xff);
}

int64_t GetI64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(u);
}

}  // namespace

size_t EncodeForecast(const Forecast& f, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kEncodedForecastBytes) return 0;
    if (f.hour_count > kMaxHours) return 0;

    size_t at = 0;
    out[at++] = 'W';
    out[at++] = 'T';
    out[at++] = 'H';
    out[at++] = kForecastRecordVersion;
    PutI64(out + at, f.fetched_epoch);
    at += 8;
    PutI64(out + at, f.first_hour_epoch);
    at += 8;
    out[at++] = f.hour_count;
    out[at++] = 0;  // reserved, written as zero so the length stays fixed
    for (size_t i = 0; i < kMaxHours; ++i) {
        const uint16_t t = static_cast<uint16_t>(f.temp_c10[i]);
        out[at++] = static_cast<uint8_t>(t & 0xff);
        out[at++] = static_cast<uint8_t>((t >> 8) & 0xff);
        out[at++] = f.wmo[i];
    }
    PutU32(out + at, (static_cast<uint32_t>(static_cast<uint16_t>(f.low_c10))) |
                         (static_cast<uint32_t>(static_cast<uint16_t>(f.high_c10))
                          << 16));
    at += 4;
    return at;
}

bool DecodeForecast(const uint8_t* data, size_t len, Forecast* out) {
    if (data == nullptr || out == nullptr || len < kEncodedForecastBytes) return false;
    if (data[0] != 'W' || data[1] != 'T' || data[2] != 'H') return false;
    if (data[3] != kForecastRecordVersion) return false;

    Forecast f;
    size_t at = 4;
    f.fetched_epoch = GetI64(data + at);
    at += 8;
    f.first_hour_epoch = GetI64(data + at);
    at += 8;
    f.hour_count = data[at++];
    ++at;  // reserved
    if (f.hour_count > kMaxHours) return false;
    for (size_t i = 0; i < kMaxHours; ++i) {
        const uint16_t t = static_cast<uint16_t>(data[at]) |
                           (static_cast<uint16_t>(data[at + 1]) << 8);
        f.temp_c10[i] = static_cast<int16_t>(t);
        f.wmo[i] = data[at + 2];
        at += 3;
    }
    const uint32_t bounds = GetU32(data + at);
    f.low_c10 = static_cast<int16_t>(bounds & 0xffff);
    f.high_c10 = static_cast<int16_t>((bounds >> 16) & 0xffff);
    *out = f;
    return true;
}

}  // namespace weather
