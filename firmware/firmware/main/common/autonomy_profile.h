/**
 * @file autonomy_profile.h
 * @brief The autonomy profile: the one document that tells this device how to
 *        compose a panel without the tower, and the closed table it is
 *        validated against before it is ever stored.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * A document, not a protocol
 * --------------------------
 * The device never receives a URL, a secret, a template or an instruction it
 * has to interpret. It receives a typed, versioned, bounded record, validates
 * it against the table below, and refuses anything it does not already know how
 * to draw. Every autonomous capability this panel has is a consequence of a
 * field in here, which is what makes the whole remote surface of autonomy
 * readable in one header on each side of the wire — this file and the tower's
 * src/core/autonomy/profile.ts, which are deliberately the same shape.
 *
 * What is deliberately absent, and why it must stay absent
 * -------------------------------------------------------
 * There is no URL field at any depth. Adding one would be the single change
 * that turns this from a profile into an SSRF surface: the device's one
 * outbound origin is a compile-time constant in the forecast client, not
 * something this document can move. There is no token, no credential, no
 * header and no free text that gets interpreted.
 *
 * Validate before persist, always
 * -------------------------------
 * ParseProfile runs to completion before the PUT route is allowed to write a
 * byte. A profile the device cannot render is refused with the field name that
 * caused it, never stored "for later": a stored document that cannot be drawn
 * is a panel that fails at 3am on battery instead of at the moment somebody was
 * looking at the tower.
 *
 * Why the raw bytes are kept as well as the parse
 * ----------------------------------------------
 * The slot stores the exact bytes that arrived, and GET returns them
 * unchanged. That is what makes the tower's read-back comparison meaningful: it
 * compares what it sent against what is held, byte for byte, rather than
 * against this device's idea of how to re-serialise it. The parse below exists
 * to decide whether to accept and how to draw — never to round-trip.
 *
 * Memory
 * ------
 * A Profile embeds its own text arena and is a little over 18 KB. It belongs in
 * PSRAM, one instance, owned by the wake cycle. It is deliberately not on any
 * stack and deliberately allocates nothing: strings are offsets into the arena,
 * so a profile costs exactly one object whatever it contains.
 *
 * Free of ESP-IDF headers on purpose: the host suite compiles this exact
 * translation unit, which is the only way the claim "every malformed document
 * is refused with a named field" is testable without a board.
 */

#ifndef COMMON_AUTONOMY_PROFILE_H
#define COMMON_AUTONOMY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "record_slot.h"

namespace autonomy {

/// Schema version of the document. Bumped with the wire contract.
constexpr int32_t kProfileVersion = 1;

/**
 * @brief Hard ceiling on the serialised document, in bytes.
 *
 * The same number as the tower's AUTONOMY_PROFILE_MAX_BYTES, and the reason it
 * is a property of the firmware rather than a tower policy: this is the buffer
 * the device parses into. The tower refuses to emit a larger profile so the
 * failure happens in the designer rather than after the bytes crossed the wire,
 * but this is the bound that actually holds.
 */
constexpr size_t kProfileMaxBytes = 16384;

/**
 * @brief The text arena.
 *
 * Unescaping never lengthens a JSON string, so every string in a document that
 * fits the wire bound fits here. The headroom is for the NUL after each one:
 * the worst case is eight list modules of a title and 24 rows apiece, plus the
 * dashboard id and the weather label, which is 202 terminators. 512 leaves that
 * case comfortable rather than exact, and the host suite drives a maximal
 * document through it so the margin is measured rather than assumed.
 */
constexpr size_t kProfileTextBytes = kProfileMaxBytes + 512;

constexpr size_t kMaxModules = 8;
constexpr size_t kMaxListRows = 24;
constexpr size_t kMaxListRowChars = 120;
constexpr size_t kMaxTitleChars = 80;
constexpr size_t kMaxMessageChars = 500;
constexpr size_t kMaxLabelChars = 40;
constexpr size_t kMaxDashboardIdChars = 64;
constexpr size_t kMaxConditions = 4;

/// Wake interval bounds, identical to power_policy.h's kMin/kMaxWakeIntervalMin.
/// Stated again rather than included, because a profile is validated in places
/// that have no business depending on the power contract; the host test asserts
/// the two agree so the duplication cannot drift.
constexpr int32_t kMinWakeIntervalMin = 15;
constexpr int32_t kMaxWakeIntervalMin = 1440;
/// The tower-wait ceiling is the fetch phase cap, not a preference. It matches
/// power_policy.h's WakeBudgetLimits::fetch_ms (90 s): a profile may ask the
/// device to hold the rendezvous door open for at most the whole fetch phase.
constexpr int32_t kMaxTowerWaitS = 90;

/// The three compositions the device's own renderer knows how to draw.
enum class Composition : uint8_t { kEditorial = 0, kFlow, kFocus };
const char* CompositionName(Composition c);

/**
 * @brief Per-source mode, as it reaches the device.
 *
 * Only two, where the tower has three. A module in Tower mode is simply not in
 * the document: the device is told about the panel it can draw, not about the
 * panel it cannot. That keeps "the device knows nothing about this" literally
 * true rather than true-by-flag.
 */
enum class Mode : uint8_t { kDevice = 0, kAuto };
const char* ModeName(Mode m);

enum class ModuleType : uint8_t {
    kWeather = 0,
    kCountdown,
    kMessage,
    kConditionalMessage,
    kList,
    kTimestamp,
};
const char* ModuleTypeName(ModuleType t);

/// How a conditional message's conditions combine.
enum class Match : uint8_t { kAll = 0, kAny };

/**
 * @brief The conditions the device can decide for itself.
 *
 * A closed set, and a deliberately smaller one than the tower's. The tower can
 * evaluate a condition against a calendar because it can read one; this device
 * cannot, and a condition it cannot evaluate never arrives here — the tower's
 * compiler forces that whole module to Tower mode instead. That is why there is
 * no escape hatch in this enum: an unrepresentable condition has to become a
 * visible mode decision on the tower, not a rule this device silently drops.
 */
enum class ConditionKind : uint8_t { kDateRange = 0, kDaysOfWeek, kWeatherState };

/// How fresh the forecast is, as a condition can ask about it.
enum class WeatherState : uint8_t { kOk = 0, kStale, kUnavailable };

/**
 * @brief A pooled string: an offset into the profile's text arena and a length.
 *
 * Offsets rather than pointers so a Profile can be copied, and rather than
 * fixed arrays so eight modules carrying one 500-character message between
 * them cost 500 bytes rather than 4000.
 */
struct Str {
    uint16_t off = 0;
    uint16_t len = 0;
    bool empty() const { return len == 0; }
};

struct Condition {
    ConditionKind kind = ConditionKind::kDateRange;

    // kDateRange. Either end may be open, which is what has_from/has_to say.
    // Civil dates as YYYY-MM-DD, evaluated in the device's own local time; the
    // tower refuses to carry a condition whose timezone is not the panel's, so
    // "local time" means one thing on both sides.
    bool has_from = false;
    bool has_to = false;
    Str from;
    Str to;

    // kDaysOfWeek. Bit 0 is Sunday, as the tower counts.
    uint8_t days_mask = 0;

    // kWeatherState.
    WeatherState state = WeatherState::kOk;
};

struct Module {
    ModuleType type = ModuleType::kWeather;
    Mode mode = Mode::kAuto;

    // kCountdown
    int64_t target_epoch = 0;
    Str label;

    // kMessage, kConditionalMessage
    Str text;
    bool has_expires = false;
    int64_t expires_epoch = 0;

    // kConditionalMessage
    Match match = Match::kAll;
    uint8_t condition_count = 0;
    Condition conditions[kMaxConditions];
    bool has_fallback = false;
    Str fallback_text;

    // kList
    Str title;
    uint8_t row_count = 0;
    Str rows[kMaxListRows];
    int64_t synced_epoch = 0;
};

struct Weather {
    /// Already coarsened to two decimals by the tower, and checked here: a
    /// profile is a file, a file ends up in a backup, and a home address in a
    /// backup is a different thing from a city in a backup.
    double latitude = 0.0;
    double longitude = 0.0;
    Str label;
    int32_t min_fetch_interval_min = 30;
    int32_t stale_after_min = 180;
    int32_t unavailable_after_min = 720;
};

/**
 * @brief One parsed profile, arena and all.
 *
 * Over 18 KB. PSRAM, one instance, owned by the wake cycle.
 *
 * Never build one as a temporary. `*p = Profile{}` reads as a reset and is not
 * one: it constructs 18 KB on the *caller's* stack and then copies. The main
 * task runs on 8 KB, so that expression overruns its stack into the heap
 * underneath and corrupts whatever is there. Use Reset().
 */
struct Profile {
    int32_t profile_version = 0;
    int32_t revision = 0;
    int64_t compiled_at = 0;
    Str dashboard_id;
    int32_t dashboard_doc_version = 0;
    int32_t wake_interval_min = 60;
    int32_t tower_wait_s = 80;
    bool provenance_line = true;
    Composition composition = Composition::kEditorial;

    bool has_weather = false;
    Weather weather;

    uint8_t module_count = 0;
    Module modules[kMaxModules];

    /// The text arena. `text_len` is how much of it is in use.
    uint16_t text_len = 0;
    char text[kProfileTextBytes] = {};

    /// Resolve a pooled string. Always NUL-terminated; returns "" for an empty
    /// Str, so a caller never has to check before printing.
    const char* Get(Str s) const {
        return s.len == 0 ? "" : text + s.off;
    }

    /// True when any module wants the forecast, which is what decides whether
    /// a wake cycle may spend budget on the radio.
    bool WantsWeather() const;

    /// True when every module is one the device can draw with no network at
    /// all. Such a wake can skip Wi-Fi entirely.
    bool IsFullyOffline() const;

    /// Back to the state a freshly constructed Profile is in, in place and
    /// with no temporary. The defaults come from the member initialisers
    /// above, so this cannot drift from what `Profile{}` would have produced.
    void Reset();
};

// ------------------------------------------------------------------ errors --

/**
 * @brief Stable refusal tokens, in the same spirit as device_config's.
 *
 * Tokens rather than sentences: the human wording lives in the tower's
 * CODE_COPY table, and a token whose spelling drifted would silently become an
 * unrecognised code there. Treat these as part of the wire contract.
 */
extern const char* const kErrUnknownField;
extern const char* const kErrMissingField;
extern const char* const kErrBadType;
extern const char* const kErrOutOfRange;
extern const char* const kErrTooLong;
extern const char* const kErrDuplicateField;
extern const char* const kErrTooManyModules;
extern const char* const kErrTooLarge;
extern const char* const kErrUnsupportedVersion;
extern const char* const kErrWeatherMissing;
extern const char* const kErrBadValue;

/**
 * @brief Why a document was refused, and where.
 *
 * `field` is a dotted path — `modules[2].rows[7]` — because "a string was too
 * long" is not something anyone can act on, and the tower shows this verbatim
 * beside the control that produced it.
 */
struct ParseError {
    const char* code = nullptr;
    char field[96] = {};
    size_t offset = 0;
};

/**
 * @brief Parse and validate a profile document.
 *
 * @param json  the exact bytes that arrived, not NUL-terminated necessarily.
 * @param len   their length.
 * @param out   filled on success. On failure it is left in an unspecified but
 *              valid state — never a half-document to be used anyway. The
 *              caller that must not lose the profile it is drawing from
 *              validates into a scratch Profile, which is what the PUT route
 *              does; the live profile is only ever reloaded from the slot.
 * @param err   filled only on failure.
 * @return true when the document is one this build can both store and draw.
 *
 * Unknown fields are refused, not ignored. That is the same choice the config
 * API's closed table makes, and for the same reason: a tower that sent a field
 * this firmware does not have is a tower that believes it configured something,
 * and the honest answer is to say which field rather than to succeed while
 * doing less than was asked.
 */
bool ParseProfile(const char* json, size_t len, Profile* out, ParseError* err);

// ------------------------------------------------------------ profile slot --

/// The profile record's spec. Variable length, bounded by what we will parse.
/// A different magic from the frame, so a profile in a frame slot — or the
/// reverse — fails validation and is ignored rather than half-read.
constexpr record::RecordSpec kProfileSpec = {
    {'N', '4', 'C', 'P', 'R', 'O', 'F', '1'}, 2, kProfileMaxBytes};

/// Bytes of scratch a profile RecordSlot needs.
constexpr size_t kProfileRecordBytes = record::kHeaderBytes + kProfileMaxBytes;

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_PROFILE_H
