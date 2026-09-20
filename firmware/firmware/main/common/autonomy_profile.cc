/**
 * @file autonomy_profile.cc
 * @brief Implementation of the autonomy profile's closed validation table.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See autonomy_profile.h for the contract. No ESP-IDF headers: the host suite
 * compiles this exact translation unit, which is what makes "every malformed
 * document is refused with a named field" a tested claim rather than a hope.
 *
 * Why the module parse runs twice over the same bytes
 * --------------------------------------------------
 * A module's schema depends on its `type`, and JSON objects have no guaranteed
 * key order. The tower's canonical serialiser does put `type` first, and it
 * would have been a line of code to require that — but then this device would
 * refuse a document that says exactly the right thing in a different order,
 * and the first person to hit it would be debugging a 400 from a profile that
 * looks correct in every way. So each module object is located once, scanned
 * for its `type`, and then parsed properly with that type known. The cost is a
 * second pass over at most 16 KB, in a path that runs once per push and once
 * per wake.
 */

#include "autonomy_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <new>
#include <type_traits>

#include "json_scan.h"

namespace autonomy {

const char* const kErrUnknownField = "unknown_field";
const char* const kErrMissingField = "missing_field";
const char* const kErrBadType = "bad_type";
const char* const kErrOutOfRange = "out_of_range";
const char* const kErrTooLong = "too_long";
const char* const kErrDuplicateField = "duplicate_field";
const char* const kErrTooManyModules = "too_many_modules";
const char* const kErrTooLarge = "profile_too_large";
const char* const kErrUnsupportedVersion = "unsupported_profile_version";
const char* const kErrWeatherMissing = "weather_block_missing";
const char* const kErrBadValue = "bad_value";

const char* CompositionName(Composition c) {
    switch (c) {
        case Composition::kEditorial: return "editorial";
        case Composition::kFlow: return "flow";
        case Composition::kFocus: return "focus";
    }
    return "";
}

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::kDevice: return "device";
        case Mode::kAuto: return "auto";
    }
    return "";
}

const char* ModuleTypeName(ModuleType t) {
    switch (t) {
        case ModuleType::kWeather: return "weather";
        case ModuleType::kCountdown: return "countdown";
        case ModuleType::kMessage: return "message";
        case ModuleType::kConditionalMessage: return "conditional_message";
        case ModuleType::kList: return "list";
        case ModuleType::kTimestamp: return "timestamp";
    }
    return "";
}

bool Profile::WantsWeather() const {
    for (uint8_t i = 0; i < module_count; ++i) {
        if (modules[i].type == ModuleType::kWeather) return true;
        // A condition that asks about forecast freshness needs the forecast to
        // have been attempted, or "stale" would be indistinguishable from
        // "never fetched" and the message would be wrong rather than absent.
        if (modules[i].type == ModuleType::kConditionalMessage) {
            for (uint8_t c = 0; c < modules[i].condition_count; ++c) {
                if (modules[i].conditions[c].kind == ConditionKind::kWeatherState) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Profile::IsFullyOffline() const { return !WantsWeather(); }

void Profile::Reset() {
    // Destroy-and-reconstruct rather than a field-by-field clear, because a
    // hand-written clear is a second copy of the defaults that drifts the first
    // time someone adds a member. Placement new runs the same member
    // initialisers straight into this object: no 18 KB temporary, and nothing
    // to keep in sync.
    //
    // Profile is trivially destructible and has no const or reference members,
    // so this is a plain re-initialisation of storage the caller still owns and
    // `this` stays valid across it.
    static_assert(std::is_trivially_destructible<Profile>::value,
                  "Reset() skips the destructor; it must have nothing to do");
    new (this) Profile();
}

namespace {

/// Bit positions for the root object's closed field table.
enum RootField : uint32_t {
    kRootProfileVersion = 1u << 0,
    kRootRevision = 1u << 1,
    kRootCompiledAt = 1u << 2,
    kRootDashboardId = 1u << 3,
    kRootDocVersion = 1u << 4,
    kRootWakeInterval = 1u << 5,
    kRootTowerWait = 1u << 6,
    kRootProvenance = 1u << 7,
    kRootComposition = 1u << 8,
    kRootWeather = 1u << 9,
    kRootModules = 1u << 10,
};

constexpr uint32_t kRootRequired =
    kRootProfileVersion | kRootRevision | kRootCompiledAt | kRootDashboardId |
    kRootDocVersion | kRootWakeInterval | kRootTowerWait | kRootProvenance |
    kRootComposition | kRootModules;

/// Longest key this schema has, plus room for a refusal to name a longer one.
constexpr size_t kKeyBufBytes = 64;

/// Marks a field name whose front was dropped to fit. Three characters, so it
/// never costs more room than it explains.
constexpr char kFieldElision[] = "...";

/**
 * @brief Build "path.suffix" into a bounded buffer, dropping the front if it
 *        does not fit.
 *
 * The front, deliberately, and this is the whole reason the helper exists
 * rather than a bare snprintf. A key may be 63 characters and a path is
 * already nested, so their sum can exceed ParseError::field; something has to
 * go. Truncating the tail — which is what snprintf does — throws away the part
 * that identifies the control the owner has to go and fix, and keeps the part
 * they could have guessed. So the tail is kept and an ellipsis says the name is
 * not the full path, because a path that was never in the document is worse
 * than an obviously partial one.
 *
 * Doing the arithmetic here rather than leaving it to snprintf also means GCC
 * can see the bound, which -Wformat-truncation could not at the call sites.
 *
 * @param cap size of @p out, including the terminator. Must be > 4.
 */
void JoinField(char* out, size_t cap, const char* path, const char* suffix_fmt,
               ...) __attribute__((format(printf, 4, 5)));

void JoinField(char* out, size_t cap, const char* path, const char* suffix_fmt,
               ...) {
    char suffix[kKeyBufBytes + 16];
    va_list args;
    va_start(args, suffix_fmt);
    vsnprintf(suffix, sizeof(suffix), suffix_fmt, args);
    va_end(args);

    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);

    if (path_len + suffix_len + 1 <= cap) {
        memcpy(out, path, path_len);
        memcpy(out + path_len, suffix, suffix_len + 1);
        return;
    }

    // Keep the tail. If even the suffix alone does not fit, it is itself cut
    // from the front, so the last characters of the key still show.
    const size_t elide = sizeof(kFieldElision) - 1;
    const size_t room = cap - 1 - elide;
    memcpy(out, kFieldElision, elide);
    if (suffix_len >= room) {
        memcpy(out + elide, suffix + (suffix_len - room), room);
        out[elide + room] = '\0';
        return;
    }
    const size_t path_room = room - suffix_len;
    memcpy(out + elide, path + (path_len - path_room), path_room);
    memcpy(out + elide + path_room, suffix, suffix_len + 1);
}

/**
 * @brief Count UTF-16 code units in well-formed UTF-8, and check it is.
 *
 * UTF-16 units rather than bytes or code points because that is what the
 * tower's zod `.max()` counts, and the two sides refusing different documents
 * for the same stated bound would be worse than either bound on its own.
 *
 * @return false when the bytes are not well-formed UTF-8. The arena feeds the
 *         text engine directly, so a malformed sequence is refused here rather
 *         than drawn as whatever the renderer makes of it.
 */
bool Utf16Length(const char* s, size_t len, size_t* out) {
    size_t units = 0;
    size_t i = 0;
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1fu;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0fu;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            return false;  // continuation byte or 5-byte form as a lead
        }
        if (extra > 0 && i + extra >= len) return false;  // truncated sequence
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3fu);
        }
        // Overlong forms and surrogates encode a code point that already has a
        // shorter spelling, or none at all. Either way the bytes did not come
        // from an encoder we should trust.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10ffff) return false;
        if (cp >= 0xd800 && cp <= 0xdfff) return false;

        units += (cp >= 0x10000) ? 2 : 1;
        i += extra + 1;
    }
    *out = units;
    return true;
}

/**
 * @brief Is every byte something the panel can draw?
 *
 * Mirrors the tower's `panelText` exactly: control characters are refused
 * rather than filtered, because a string that arrived with a NUL in it did not
 * come from the designer's text box, and quietly repairing it would hide that.
 * Angle brackets are refused for the same reason — the device has no markup, so
 * a tag here is either a mistake or an attempt, and both deserve the same
 * answer.
 */
bool IsPanelSafe(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '<' || c == '>') return false;
    }
    return true;
}

/// YYYY-MM-DD, and a plausible civil date rather than merely ten characters.
bool IsCivilDate(const char* s, size_t len) {
    if (len != 10) return false;
    for (size_t i = 0; i < 10; ++i) {
        const bool want_dash = (i == 4 || i == 7);
        const bool is_dash = s[i] == '-';
        const bool is_digit = s[i] >= '0' && s[i] <= '9';
        if (want_dash != is_dash) return false;
        if (!want_dash && !is_digit) return false;
    }
    const int month = (s[5] - '0') * 10 + (s[6] - '0');
    const int day = (s[8] - '0') * 10 + (s[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

/**
 * @brief The whole schema walk, holding the reader, the output and the error.
 */
class ProfileParser {
public:
    ProfileParser(const char* json, size_t len, Profile* out, ParseError* err)
        : json_(json), len_(len), out_(out), err_(err) {}

    bool Run();

private:
    void Fail(const char* code, const char* path, size_t offset);
    /// Translate whatever the reader latched into a field-level refusal.
    void FailFromReader(const json::Reader& r, const char* path);

    bool Intern(json::Reader& r, size_t max_units, const char* path, Str* out);
    bool ReadModeField(json::Reader& r, const char* path, Mode* out);

    bool Root(json::Reader& r);
    bool ParseWeather(json::Reader& r);
    bool ParseModules(json::Reader& r);
    bool ParseModule(size_t start, size_t end, size_t index);
    bool ParseCondition(size_t start, size_t end, size_t module_index,
                        size_t condition_index, Condition* out);

    /// Locate the object starting at the reader's cursor and return its span.
    bool SpanOfObject(json::Reader& r, const char* path, size_t* start, size_t* end);
    /// Scan an object's span for a single string field, without validating the
    /// rest. Used to learn a discriminant before the real parse.
    bool PeekDiscriminant(size_t start, size_t end, const char* key,
                          const char* path, char* out, size_t cap);

    const char* json_;
    size_t len_;
    Profile* out_;
    ParseError* err_;
    bool failed_ = false;
};

void ProfileParser::Fail(const char* code, const char* path, size_t offset) {
    if (failed_) return;  // first refusal wins; it explains the rest
    failed_ = true;
    err_->code = code;
    err_->offset = offset;
    snprintf(err_->field, sizeof(err_->field), "%s", path);
}

void ProfileParser::FailFromReader(const json::Reader& r, const char* path) {
    const char* e = r.error();
    const char* code = kErrBadType;
    if (e == json::kErrNumberRange) {
        code = kErrOutOfRange;
    } else if (e == json::kErrStringTooLong) {
        code = kErrTooLong;
    } else if (e == json::kErrSyntax || e == json::kErrDepth ||
               e == json::kErrTrailing || e == json::kErrControlChar ||
               e == json::kErrEscape) {
        // Not a field problem at all: the bytes are not the document they
        // claimed to be. Report the reader's own token so the tower can say so.
        code = e;
    }
    Fail(code, path, r.error_offset());
}

bool ProfileParser::Intern(json::Reader& r, size_t max_units, const char* path,
                           Str* out) {
    char* dst = out_->text + out_->text_len;
    const size_t cap = kProfileTextBytes - out_->text_len;
    size_t n = 0;
    if (cap == 0 || !r.ReadString(dst, cap, &n)) {
        FailFromReader(r, path);
        return false;
    }
    size_t units = 0;
    if (!Utf16Length(dst, n, &units)) {
        Fail(kErrBadValue, path, r.offset());
        return false;
    }
    if (units > max_units) {
        Fail(kErrTooLong, path, r.offset());
        return false;
    }
    if (!IsPanelSafe(dst, n)) {
        Fail(kErrBadValue, path, r.offset());
        return false;
    }
    out->off = static_cast<uint16_t>(out_->text_len);
    out->len = static_cast<uint16_t>(n);
    // Keep the terminator ReadString wrote, so Get() can hand out a C string.
    out_->text_len = static_cast<uint16_t>(out_->text_len + n + 1);
    return true;
}

bool ProfileParser::ReadModeField(json::Reader& r, const char* path, Mode* out) {
    char buf[16];
    if (!r.ReadString(buf, sizeof(buf), nullptr)) {
        FailFromReader(r, path);
        return false;
    }
    if (strcmp(buf, "device") == 0) {
        *out = Mode::kDevice;
        return true;
    }
    if (strcmp(buf, "auto") == 0) {
        *out = Mode::kAuto;
        return true;
    }
    // "tower" is deliberately not accepted. A tower-mode module is one the
    // device is not told about at all; carrying one would mean the document
    // describes a panel the device is not composing.
    Fail(kErrBadValue, path, r.offset());
    return false;
}

bool ProfileParser::SpanOfObject(json::Reader& r, const char* path, size_t* start,
                                 size_t* end) {
    json::Type type;
    if (!r.PeekType(&type)) {
        FailFromReader(r, path);
        return false;
    }
    if (type != json::Type::kObject) {
        Fail(kErrBadType, path, r.offset());
        return false;
    }
    // PeekType has skipped leading whitespace, so the cursor is on the brace.
    *start = r.offset();
    if (!r.SkipValue()) {
        FailFromReader(r, path);
        return false;
    }
    *end = r.offset();
    return true;
}

bool ProfileParser::PeekDiscriminant(size_t start, size_t end, const char* key,
                                     const char* path, char* out, size_t cap) {
    json::Reader sub(json_ + start, end - start);
    if (!sub.EnterObject()) {
        FailFromReader(sub, path);
        return false;
    }
    char k[kKeyBufBytes];
    bool found = false;
    while (sub.NextKey(k, sizeof(k), nullptr)) {
        if (strcmp(k, key) == 0) {
            if (found) {
                Fail(kErrDuplicateField, path, start);
                return false;
            }
            if (!sub.ReadString(out, cap, nullptr)) {
                FailFromReader(sub, path);
                return false;
            }
            found = true;
        } else if (!sub.SkipValue()) {
            FailFromReader(sub, path);
            return false;
        }
    }
    if (!sub.ok()) {
        FailFromReader(sub, path);
        return false;
    }
    if (!found) {
        char field[sizeof(err_->field)];
        JoinField(field, sizeof(field), path, ".%s", key);
        Fail(kErrMissingField, field, start);
        return false;
    }
    return true;
}

bool ProfileParser::ParseWeather(json::Reader& r) {
    enum : uint32_t {
        kLat = 1u << 0,
        kLon = 1u << 1,
        kLabel = 1u << 2,
        kMinFetch = 1u << 3,
        kStale = 1u << 4,
        kUnavailable = 1u << 5,
    };
    constexpr uint32_t kRequired =
        kLat | kLon | kLabel | kMinFetch | kStale | kUnavailable;

    if (!r.EnterObject()) {
        FailFromReader(r, "weather");
        return false;
    }
    uint32_t seen = 0;
    char key[kKeyBufBytes];
    char path[sizeof(err_->field)];
    Weather& w = out_->weather;

    while (r.NextKey(key, sizeof(key), nullptr)) {
        snprintf(path, sizeof(path), "weather.%s", key);
        uint32_t bit = 0;
        bool ok = true;
        if (strcmp(key, "latitude") == 0) {
            bit = kLat;
            ok = r.ReadDouble(-90.0, 90.0, &w.latitude);
        } else if (strcmp(key, "longitude") == 0) {
            bit = kLon;
            ok = r.ReadDouble(-180.0, 180.0, &w.longitude);
        } else if (strcmp(key, "label") == 0) {
            bit = kLabel;
            if (seen & bit) { Fail(kErrDuplicateField, path, r.offset()); return false; }
            if (!Intern(r, kMaxLabelChars, path, &w.label)) return false;
            seen |= bit;
            continue;
        } else if (strcmp(key, "min_fetch_interval_min") == 0) {
            bit = kMinFetch;
            int64_t v = 0;
            ok = r.ReadInt(5, 1440, &v);
            if (ok) w.min_fetch_interval_min = static_cast<int32_t>(v);
        } else if (strcmp(key, "stale_after_min") == 0) {
            bit = kStale;
            int64_t v = 0;
            ok = r.ReadInt(30, 2880, &v);
            if (ok) w.stale_after_min = static_cast<int32_t>(v);
        } else if (strcmp(key, "unavailable_after_min") == 0) {
            bit = kUnavailable;
            int64_t v = 0;
            ok = r.ReadInt(60, 10080, &v);
            if (ok) w.unavailable_after_min = static_cast<int32_t>(v);
        } else {
            Fail(kErrUnknownField, path, r.offset());
            return false;
        }
        if (seen & bit) {
            Fail(kErrDuplicateField, path, r.offset());
            return false;
        }
        if (!ok) {
            FailFromReader(r, path);
            return false;
        }
        seen |= bit;
    }
    if (!r.ok()) {
        FailFromReader(r, "weather");
        return false;
    }
    if ((seen & kRequired) != kRequired) {
        Fail(kErrMissingField, "weather", r.offset());
        return false;
    }

    // Two decimals, checked rather than trusted. The tower rounds before it
    // writes; this refuses anything finer so a hand-edited document cannot
    // smuggle a street address past the coarsening.
    const double lat100 = w.latitude * 100.0;
    const double lon100 = w.longitude * 100.0;
    const int64_t lat_round = static_cast<int64_t>(lat100 < 0 ? lat100 - 0.5 : lat100 + 0.5);
    const int64_t lon_round = static_cast<int64_t>(lon100 < 0 ? lon100 - 0.5 : lon100 + 0.5);
    const double lat_delta = lat100 - static_cast<double>(lat_round);
    const double lon_delta = lon100 - static_cast<double>(lon_round);
    if (lat_delta > 1e-6 || lat_delta < -1e-6) {
        Fail(kErrBadValue, "weather.latitude", r.offset());
        return false;
    }
    if (lon_delta > 1e-6 || lon_delta < -1e-6) {
        Fail(kErrBadValue, "weather.longitude", r.offset());
        return false;
    }

    if (w.stale_after_min >= w.unavailable_after_min) {
        // A value cannot become unavailable before it becomes stale. Refused
        // rather than reordered: the two thresholds mean different things to
        // the panel and guessing which one was intended would show the wrong
        // one.
        Fail(kErrBadValue, "weather.unavailable_after_min", r.offset());
        return false;
    }
    return true;
}

bool ProfileParser::ParseCondition(size_t start, size_t end, size_t module_index,
                                   size_t condition_index, Condition* out) {
    char path[sizeof(err_->field)];
    snprintf(path, sizeof(path), "modules[%zu].when[%zu]", module_index,
             condition_index);

    char kind[32];
    if (!PeekDiscriminant(start, end, "kind", path, kind, sizeof(kind))) return false;

    if (strcmp(kind, "date_range") == 0) {
        out->kind = ConditionKind::kDateRange;
    } else if (strcmp(kind, "days_of_week") == 0) {
        out->kind = ConditionKind::kDaysOfWeek;
    } else if (strcmp(kind, "weather_state") == 0) {
        out->kind = ConditionKind::kWeatherState;
    } else {
        char field[sizeof(err_->field)];
        JoinField(field, sizeof(field), path, ".kind");
        Fail(kErrBadValue, field, start);
        return false;
    }

    enum : uint32_t {
        kKind = 1u << 0,
        kFrom = 1u << 1,
        kTo = 1u << 2,
        kDays = 1u << 3,
        kState = 1u << 4,
    };
    uint32_t required = kKind;
    switch (out->kind) {
        case ConditionKind::kDateRange: required |= kFrom | kTo; break;
        case ConditionKind::kDaysOfWeek: required |= kDays; break;
        case ConditionKind::kWeatherState: required |= kState; break;
    }

    json::Reader r(json_ + start, end - start);
    if (!r.EnterObject()) {
        FailFromReader(r, path);
        return false;
    }
    uint32_t seen = 0;
    char key[kKeyBufBytes];
    char field[sizeof(err_->field)];

    while (r.NextKey(key, sizeof(key), nullptr)) {
        JoinField(field, sizeof(field), path, ".%s", key);
        uint32_t bit = 0;
        if (strcmp(key, "kind") == 0) {
            bit = kKind;
            if (!r.SkipValue()) {
                FailFromReader(r, field);
                return false;
            }
        } else if (strcmp(key, "from") == 0 || strcmp(key, "to") == 0) {
            const bool is_from = key[0] == 'f';
            bit = is_from ? kFrom : kTo;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            json::Type type;
            if (!r.PeekType(&type)) {
                FailFromReader(r, field);
                return false;
            }
            if (type == json::Type::kNull) {
                if (!r.ReadNull()) {
                    FailFromReader(r, field);
                    return false;
                }
            } else {
                Str* target = is_from ? &out->from : &out->to;
                if (!Intern(r, 10, field, target)) return false;
                if (!IsCivilDate(out_->Get(*target), target->len)) {
                    Fail(kErrBadValue, field, start + r.offset());
                    return false;
                }
                if (is_from) {
                    out->has_from = true;
                } else {
                    out->has_to = true;
                }
            }
        } else if (strcmp(key, "days") == 0) {
            bit = kDays;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!r.EnterArray()) {
                FailFromReader(r, field);
                return false;
            }
            int count = 0;
            while (r.NextElement()) {
                int64_t day = 0;
                if (!r.ReadInt(0, 6, &day)) {
                    FailFromReader(r, field);
                    return false;
                }
                out->days_mask = static_cast<uint8_t>(out->days_mask | (1u << day));
                if (++count > 7) {
                    Fail(kErrOutOfRange, field, start + r.offset());
                    return false;
                }
            }
            if (!r.ok()) {
                FailFromReader(r, field);
                return false;
            }
            if (count == 0) {
                Fail(kErrOutOfRange, field, start + r.offset());
                return false;
            }
        } else if (strcmp(key, "state") == 0) {
            bit = kState;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            char state[24];
            if (!r.ReadString(state, sizeof(state), nullptr)) {
                FailFromReader(r, field);
                return false;
            }
            if (strcmp(state, "ok") == 0) {
                out->state = WeatherState::kOk;
            } else if (strcmp(state, "stale") == 0) {
                out->state = WeatherState::kStale;
            } else if (strcmp(state, "unavailable") == 0) {
                out->state = WeatherState::kUnavailable;
            } else {
                Fail(kErrBadValue, field, start + r.offset());
                return false;
            }
        } else {
            Fail(kErrUnknownField, field, start + r.offset());
            return false;
        }
        if (seen & bit) {
            Fail(kErrDuplicateField, field, start + r.offset());
            return false;
        }
        seen |= bit;
    }
    if (!r.ok()) {
        FailFromReader(r, path);
        return false;
    }
    if ((seen & required) != required) {
        Fail(kErrMissingField, path, start);
        return false;
    }

    // An all-open date range matches everything, which means the owner asked
    // for a condition and got an unconditional message. The tower refuses to
    // compile one; this refuses to draw one.
    if (out->kind == ConditionKind::kDateRange && !out->has_from && !out->has_to) {
        Fail(kErrBadValue, path, start);
        return false;
    }
    return true;
}

bool ProfileParser::ParseModule(size_t start, size_t end, size_t index) {
    char path[sizeof(err_->field)];
    snprintf(path, sizeof(path), "modules[%zu]", index);

    char type_name[40];
    if (!PeekDiscriminant(start, end, "type", path, type_name, sizeof(type_name))) {
        return false;
    }

    Module& m = out_->modules[index];
    if (strcmp(type_name, "weather") == 0) {
        m.type = ModuleType::kWeather;
    } else if (strcmp(type_name, "countdown") == 0) {
        m.type = ModuleType::kCountdown;
    } else if (strcmp(type_name, "message") == 0) {
        m.type = ModuleType::kMessage;
    } else if (strcmp(type_name, "conditional_message") == 0) {
        m.type = ModuleType::kConditionalMessage;
    } else if (strcmp(type_name, "list") == 0) {
        m.type = ModuleType::kList;
    } else if (strcmp(type_name, "timestamp") == 0) {
        m.type = ModuleType::kTimestamp;
    } else {
        char field[sizeof(err_->field)];
        JoinField(field, sizeof(field), path, ".type");
        // Not "unknown field": the field is known, the value names a module
        // this build cannot draw. The tower is meant to have filtered it out,
        // so this is also how a firmware downgrade announces itself.
        Fail(kErrBadValue, field, start);
        return false;
    }

    enum : uint32_t {
        kType = 1u << 0,
        kMode = 1u << 1,
        kTarget = 1u << 2,
        kLabel = 1u << 3,
        kText = 1u << 4,
        kExpires = 1u << 5,
        kMatch = 1u << 6,
        kWhen = 1u << 7,
        kFallback = 1u << 8,
        kTitle = 1u << 9,
        kRows = 1u << 10,
        kSynced = 1u << 11,
    };
    uint32_t required = kType | kMode;
    switch (m.type) {
        case ModuleType::kWeather:
        case ModuleType::kTimestamp:
            break;
        case ModuleType::kCountdown:
            required |= kTarget | kLabel;
            break;
        case ModuleType::kMessage:
            required |= kText | kExpires;
            break;
        case ModuleType::kConditionalMessage:
            required |= kMatch | kWhen | kText | kFallback;
            break;
        case ModuleType::kList:
            required |= kTitle | kRows | kSynced;
            break;
    }

    json::Reader r(json_ + start, end - start);
    if (!r.EnterObject()) {
        FailFromReader(r, path);
        return false;
    }
    uint32_t seen = 0;
    char key[kKeyBufBytes];
    char field[sizeof(err_->field)];

    while (r.NextKey(key, sizeof(key), nullptr)) {
        JoinField(field, sizeof(field), path, ".%s", key);
        uint32_t bit = 0;

        if (strcmp(key, "type") == 0) {
            bit = kType;
            if (!r.SkipValue()) {
                FailFromReader(r, field);
                return false;
            }
        } else if (strcmp(key, "mode") == 0) {
            bit = kMode;
            if (!ReadModeField(r, field, &m.mode)) return false;
        } else if (strcmp(key, "target_epoch") == 0) {
            bit = kTarget;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!r.ReadInt(0, 4102444800ll, &m.target_epoch)) {
                FailFromReader(r, field);
                return false;
            }
        } else if (strcmp(key, "label") == 0) {
            bit = kLabel;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!Intern(r, kMaxTitleChars, field, &m.label)) return false;
        } else if (strcmp(key, "text") == 0) {
            bit = kText;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!Intern(r, kMaxMessageChars, field, &m.text)) return false;
        } else if (strcmp(key, "expires_epoch") == 0) {
            bit = kExpires;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            json::Type type;
            if (!r.PeekType(&type)) {
                FailFromReader(r, field);
                return false;
            }
            if (type == json::Type::kNull) {
                if (!r.ReadNull()) {
                    FailFromReader(r, field);
                    return false;
                }
            } else {
                if (!r.ReadInt(0, 4102444800ll, &m.expires_epoch)) {
                    FailFromReader(r, field);
                    return false;
                }
                m.has_expires = true;
            }
        } else if (strcmp(key, "match") == 0) {
            bit = kMatch;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            char match[16];
            if (!r.ReadString(match, sizeof(match), nullptr)) {
                FailFromReader(r, field);
                return false;
            }
            if (strcmp(match, "all") == 0) {
                m.match = Match::kAll;
            } else if (strcmp(match, "any") == 0) {
                m.match = Match::kAny;
            } else {
                Fail(kErrBadValue, field, start + r.offset());
                return false;
            }
        } else if (strcmp(key, "when") == 0) {
            bit = kWhen;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!r.EnterArray()) {
                FailFromReader(r, field);
                return false;
            }
            while (r.NextElement()) {
                if (m.condition_count >= kMaxConditions) {
                    Fail(kErrOutOfRange, field, start + r.offset());
                    return false;
                }
                size_t cstart = 0;
                size_t cend = 0;
                if (!SpanOfObject(r, field, &cstart, &cend)) return false;
                if (!ParseCondition(start + cstart, start + cend, index,
                                    m.condition_count,
                                    &m.conditions[m.condition_count])) {
                    return false;
                }
                ++m.condition_count;
            }
            if (!r.ok()) {
                FailFromReader(r, field);
                return false;
            }
            if (m.condition_count == 0) {
                Fail(kErrOutOfRange, field, start + r.offset());
                return false;
            }
        } else if (strcmp(key, "fallback_text") == 0) {
            bit = kFallback;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            json::Type type;
            if (!r.PeekType(&type)) {
                FailFromReader(r, field);
                return false;
            }
            if (type == json::Type::kNull) {
                if (!r.ReadNull()) {
                    FailFromReader(r, field);
                    return false;
                }
            } else {
                if (!Intern(r, kMaxMessageChars, field, &m.fallback_text)) return false;
                m.has_fallback = true;
            }
        } else if (strcmp(key, "title") == 0) {
            bit = kTitle;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!Intern(r, kMaxTitleChars, field, &m.title)) return false;
        } else if (strcmp(key, "rows") == 0) {
            bit = kRows;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!r.EnterArray()) {
                FailFromReader(r, field);
                return false;
            }
            while (r.NextElement()) {
                if (m.row_count >= kMaxListRows) {
                    Fail(kErrOutOfRange, field, start + r.offset());
                    return false;
                }
                char row_path[sizeof(err_->field)];
                JoinField(row_path, sizeof(row_path), path, ".rows[%u]",
                          static_cast<unsigned>(m.row_count));
                if (!Intern(r, kMaxListRowChars, row_path, &m.rows[m.row_count])) {
                    return false;
                }
                ++m.row_count;
            }
            if (!r.ok()) {
                FailFromReader(r, field);
                return false;
            }
        } else if (strcmp(key, "synced_epoch") == 0) {
            bit = kSynced;
            if ((required & bit) == 0) {
                Fail(kErrUnknownField, field, start + r.offset());
                return false;
            }
            if (!r.ReadInt(0, 4102444800ll, &m.synced_epoch)) {
                FailFromReader(r, field);
                return false;
            }
        } else {
            Fail(kErrUnknownField, field, start + r.offset());
            return false;
        }

        if (seen & bit) {
            Fail(kErrDuplicateField, field, start + r.offset());
            return false;
        }
        seen |= bit;
    }
    if (!r.ok()) {
        FailFromReader(r, path);
        return false;
    }
    if ((seen & required) != required) {
        Fail(kErrMissingField, path, start);
        return false;
    }
    return true;
}

bool ProfileParser::ParseModules(json::Reader& r) {
    if (!r.EnterArray()) {
        FailFromReader(r, "modules");
        return false;
    }
    while (r.NextElement()) {
        if (out_->module_count >= kMaxModules) {
            Fail(kErrTooManyModules, "modules", r.offset());
            return false;
        }
        size_t start = 0;
        size_t end = 0;
        if (!SpanOfObject(r, "modules", &start, &end)) return false;
        if (!ParseModule(start, end, out_->module_count)) return false;
        ++out_->module_count;
    }
    if (!r.ok()) {
        FailFromReader(r, "modules");
        return false;
    }
    return true;
}

bool ProfileParser::Root(json::Reader& r) {
    if (!r.EnterObject()) {
        FailFromReader(r, "");
        return false;
    }
    uint32_t seen = 0;
    char key[kKeyBufBytes];

    while (r.NextKey(key, sizeof(key), nullptr)) {
        uint32_t bit = 0;
        bool ok = true;

        if (strcmp(key, "profile_version") == 0) {
            bit = kRootProfileVersion;
            int64_t v = 0;
            if (!r.ReadInt(0, 1000000, &v)) {
                FailFromReader(r, key);
                return false;
            }
            if (v != kProfileVersion) {
                // A future profile is refused with its own token, so the tower
                // can say "this device's firmware is older than this profile"
                // rather than "field profile_version is wrong".
                Fail(kErrUnsupportedVersion, key, r.offset());
                return false;
            }
            out_->profile_version = static_cast<int32_t>(v);
        } else if (strcmp(key, "revision") == 0) {
            bit = kRootRevision;
            int64_t v = 0;
            ok = r.ReadInt(1, 2147483647ll, &v);
            if (ok) out_->revision = static_cast<int32_t>(v);
        } else if (strcmp(key, "compiled_at") == 0) {
            bit = kRootCompiledAt;
            ok = r.ReadInt(0, 4102444800ll, &out_->compiled_at);
        } else if (strcmp(key, "dashboard_id") == 0) {
            bit = kRootDashboardId;
            if (seen & bit) { Fail(kErrDuplicateField, key, r.offset()); return false; }
            if (!Intern(r, kMaxDashboardIdChars, key, &out_->dashboard_id)) return false;
            if (out_->dashboard_id.len == 0) {
                Fail(kErrBadValue, key, r.offset());
                return false;
            }
            seen |= bit;
            continue;
        } else if (strcmp(key, "dashboard_doc_version") == 0) {
            bit = kRootDocVersion;
            int64_t v = 0;
            ok = r.ReadInt(0, 2147483647ll, &v);
            if (ok) out_->dashboard_doc_version = static_cast<int32_t>(v);
        } else if (strcmp(key, "wake_interval_min") == 0) {
            bit = kRootWakeInterval;
            int64_t v = 0;
            ok = r.ReadInt(kMinWakeIntervalMin, kMaxWakeIntervalMin, &v);
            if (ok) out_->wake_interval_min = static_cast<int32_t>(v);
        } else if (strcmp(key, "tower_wait_s") == 0) {
            bit = kRootTowerWait;
            int64_t v = 0;
            ok = r.ReadInt(0, kMaxTowerWaitS, &v);
            if (ok) out_->tower_wait_s = static_cast<int32_t>(v);
        } else if (strcmp(key, "provenance_line") == 0) {
            bit = kRootProvenance;
            ok = r.ReadBool(&out_->provenance_line);
        } else if (strcmp(key, "composition") == 0) {
            bit = kRootComposition;
            char name[24];
            if (!r.ReadString(name, sizeof(name), nullptr)) {
                FailFromReader(r, key);
                return false;
            }
            if (strcmp(name, "editorial") == 0) {
                out_->composition = Composition::kEditorial;
            } else if (strcmp(name, "flow") == 0) {
                out_->composition = Composition::kFlow;
            } else if (strcmp(name, "focus") == 0) {
                out_->composition = Composition::kFocus;
            } else {
                Fail(kErrBadValue, key, r.offset());
                return false;
            }
        } else if (strcmp(key, "weather") == 0) {
            bit = kRootWeather;
            if (seen & bit) { Fail(kErrDuplicateField, key, r.offset()); return false; }
            if (!ParseWeather(r)) return false;
            out_->has_weather = true;
            seen |= bit;
            continue;
        } else if (strcmp(key, "modules") == 0) {
            bit = kRootModules;
            if (seen & bit) { Fail(kErrDuplicateField, key, r.offset()); return false; }
            if (!ParseModules(r)) return false;
            seen |= bit;
            continue;
        } else {
            Fail(kErrUnknownField, key, r.offset());
            return false;
        }

        if (seen & bit) {
            Fail(kErrDuplicateField, key, r.offset());
            return false;
        }
        if (!ok) {
            FailFromReader(r, key);
            return false;
        }
        seen |= bit;
    }
    if (!r.ok()) {
        FailFromReader(r, "");
        return false;
    }
    if ((seen & kRootRequired) != kRootRequired) {
        // Name the first one missing, because "a required field is missing" is
        // not something anybody can act on.
        struct Named { uint32_t bit; const char* name; };
        static const Named kNames[] = {
            {kRootProfileVersion, "profile_version"},
            {kRootRevision, "revision"},
            {kRootCompiledAt, "compiled_at"},
            {kRootDashboardId, "dashboard_id"},
            {kRootDocVersion, "dashboard_doc_version"},
            {kRootWakeInterval, "wake_interval_min"},
            {kRootTowerWait, "tower_wait_s"},
            {kRootProvenance, "provenance_line"},
            {kRootComposition, "composition"},
            {kRootModules, "modules"},
        };
        for (const Named& n : kNames) {
            if ((seen & n.bit) == 0) {
                Fail(kErrMissingField, n.name, r.offset());
                return false;
            }
        }
        Fail(kErrMissingField, "", r.offset());
        return false;
    }

    if (out_->WantsWeather() && !out_->has_weather) {
        Fail(kErrWeatherMissing, "weather", r.offset());
        return false;
    }
    return true;
}

bool ProfileParser::Run() {
    json::Reader r(json_, len_);
    if (!Root(r)) return false;
    if (!r.Finish()) {
        FailFromReader(r, "");
        return false;
    }
    return true;
}

}  // namespace

bool ParseProfile(const char* json, size_t len, Profile* out, ParseError* err) {
    ParseError local;
    ParseError* e = err != nullptr ? err : &local;
    *e = ParseError{};

    if (out == nullptr) {
        e->code = kErrBadValue;
        return false;
    }
    if (json == nullptr) {
        e->code = json::kErrSyntax;
        return false;
    }
    if (len > kProfileMaxBytes) {
        // Checked before a byte is parsed, so an oversized document costs a
        // length comparison rather than a parse.
        e->code = kErrTooLarge;
        e->offset = len;
        return false;
    }

    // Parsed straight into the caller's Profile, which is why the contract
    // says @p out is unspecified on failure rather than untouched.
    //
    // The tempting alternative — parse into a scratch copy and publish on
    // success — would cost a second 18 KB Profile, and on this device the
    // obvious place to put it is a function-local static, which is 18 KB of
    // internal RAM that is never freed and is not reentrant. The caller that
    // actually needs the old profile preserved is the PUT route, and it has a
    // better answer available: validate into its own scratch Profile, and only
    // then write the raw bytes to the slot. The live profile is reloaded from
    // the slot, so a refused PUT never touches it.
    out->Reset();
    ProfileParser parser(json, len, out, e);
    return parser.Run();
}

}  // namespace autonomy
