/**
 * @file autonomy_compose.h
 * @brief The panel the device draws for itself, when the tower is not there.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Portions adapted from eMini Home 0.4.0, (c) 2026 Tomasz Fiedoruk, MIT,
 * commit 05ec313f86b1ecfb6f8ecc692fb1f9ef00e78544 — specifically the ordered
 * Bayer dither kernel and the descending auto-fit discipline. See
 * THIRD_PARTY_NOTICES.md for the full notice and for what was deliberately not
 * taken.
 *
 * Deliberately a smaller renderer than the tower's
 * -----------------------------------------------
 * The tower's renderer is the premium one and stays that way. This draws
 * weather, a countdown, a message, a conditional message, a list and a
 * timestamp, in three compositions, and nothing else. There is no Octopus tile,
 * no calendar, no Home Assistant sensor: those need readers this device does
 * not have, and a local renderer that attempted them would be inventing
 * content rather than composing it.
 *
 * Why every number in here is an integer
 * --------------------------------------
 * This file has a twin: src/core/autonomyPreview/compose.ts in the tower, which
 * exists so the designer can show what the device would draw. The two are
 * compared byte for byte against shared 30000-byte golden fixtures, and that
 * test is only worth running if the two can actually agree.
 *
 * Floating point is where they would stop agreeing. JavaScript has one number
 * type and C++ has several; `(a / b) | 0` truncates toward zero and
 * `Math.floor` does not; a double that rounds one way here rounds the other way
 * there for exactly one input in a million, and that input turns up on a
 * Tuesday in February on somebody's wall.
 *
 * So there is no floating point in the layout at all. Temperatures are tenths
 * of a degree as int16. Divisions go through FloorDiv, which is written the
 * same way on both sides because C++ integer division truncating toward zero
 * and JavaScript's Math.floor rounding toward negative infinity disagree for
 * every negative numerator — which is to say, for every sub-zero temperature.
 *
 * Memory
 * ------
 * Allocates nothing. The caller supplies a 120000-byte canvas (one byte per
 * pixel, PSRAM) and a 30000-byte packed output. Reentrant, no statics, no
 * ESP-IDF headers, so the host suite compiles this exact translation unit and
 * emits the goldens the tower's mirror is tested against.
 */

#ifndef COMMON_AUTONOMY_COMPOSE_H
#define COMMON_AUTONOMY_COMPOSE_H

#include <stddef.h>
#include <stdint.h>

#include "autonomy_profile.h"
#include "openmeteo_parse.h"

namespace autonomy {

constexpr int kPanelWidth = 400;
constexpr int kPanelHeight = 300;
constexpr size_t kCanvasBytes = static_cast<size_t>(kPanelWidth) * kPanelHeight;
constexpr size_t kPackedBytes = kCanvasBytes / 4;

/// The device palette. This ordering is law and comes from the panel itself.
/// Identical to the tower's src/core/palette.ts.
constexpr uint8_t kBlack = 0;
constexpr uint8_t kWhite = 1;
constexpr uint8_t kYellow = 2;
constexpr uint8_t kRed = 3;

/// How fresh the forecast is, as the panel is entitled to describe it.
enum class Freshness : uint8_t {
    kNone = 0,      ///< never fetched, or the cache was unreadable
    kOk,            ///< inside stale_after_min
    kStale,         ///< past stale_after_min: drawn, with its age beside it
    kUnavailable,   ///< past unavailable_after_min: not drawn at all
};

/**
 * @brief Everything the compositor is allowed to know.
 *
 * A closed input struct rather than a pile of globals, because this is the
 * function the goldens pin: a fixture is exactly one of these plus the frame it
 * must produce.
 */
struct ComposeInput {
    /// Never null. The document that says what to draw.
    const Profile* profile = nullptr;

    /// The cached forecast, or null when there is none to draw.
    const weather::Forecast* forecast = nullptr;

    /**
     * @brief UTC seconds, or 0 when SNTP has never succeeded.
     *
     * `clock_set` is separate rather than inferred from a zero, because a
     * device whose clock is unset must render "heure inconnue" rather than a
     * countdown to 1970. A fabricated timestamp on frozen ink is the exact
     * failure this product refuses.
     */
    int64_t now_epoch = 0;
    bool clock_set = false;

    /**
     * @brief Seconds to add to UTC to get the panel's civil time.
     *
     * Passed in rather than derived, and that is a deliberate architectural
     * choice rather than laziness. Conditions in the profile are civil-date
     * conditions — "on weekdays", "after 1 December" — and the tower already
     * refuses to compile one whose timezone is not the panel's, so both sides
     * agree on *which* timezone. What they must not have to agree on is the
     * rules for it: implementing EST5EDT's transitions twice, once in C++ and
     * once in TypeScript, would be two implementations of a thing that changes
     * by legislation, and they would diverge on exactly the two Sundays a year
     * when anyone would notice.
     *
     * So the device resolves its own offset from the TZ it already has, the
     * mirror is told the offset, and the golden fixtures pin it. The
     * compositor does arithmetic, not timezone policy.
     *
     * Resolve it with UtcOffsetSeconds() below rather than by hand.
     */
    int32_t utc_offset_s = 0;

    /**
     * @brief True when this cycle could not do everything it wanted.
     *
     * Wi-Fi never associated, or the forecast fetch failed and the cache was
     * used instead. The panel says so rather than looking identical to a cycle
     * that went perfectly.
     */
    bool degraded = false;
};

/// What a compose produced, for the status route and the tests.
struct ComposeResult {
    Freshness freshness = Freshness::kNone;
    /// Modules that actually drew something. A message past its expiry, or a
    /// conditional whose conditions did not hold, contributes nothing and is
    /// not counted.
    uint8_t modules_drawn = 0;
    /// True when nothing at all was drawable and the panel says so in words.
    bool empty = false;
};

/**
 * @brief Compose the panel.
 *
 * @param in         what to draw.
 * @param canvas     kCanvasBytes of scratch, one byte per pixel. Clobbered.
 * @param frame_out  kPackedBytes, the 2bpp frame the slot stores.
 * @param result     optional; what was drawn and how fresh it was.
 * @return false only when an argument is missing or a buffer is the wrong size.
 *
 * Never fails for want of content: a profile with nothing currently drawable
 * produces a panel that says so, because a blank panel and a broken panel look
 * identical on e-paper.
 */
bool Compose(const ComposeInput& in, uint8_t* canvas, uint8_t* frame_out,
             ComposeResult* result = nullptr);

/**
 * @brief Pack a byte-per-pixel canvas into the device's 2bpp frame.
 *
 * MSB first, four pixels per byte, palette order. Identical to the tower's
 * pack() in src/core/frame.ts, which is the format the PUT route already
 * accepts — an autonomous frame and a pushed frame are the same kind of object.
 */
void PackFrame(const uint8_t* canvas, uint8_t* frame_out);

/// Floor division that agrees with JavaScript's Math.floor(a / b) for negative
/// numerators, which C++'s truncating `/` does not. Exposed because the mirror
/// has the same function and a test pins that they agree.
int32_t FloorDiv(int32_t a, int32_t b);

/// How fresh a cached forecast is, given the profile's thresholds.
Freshness FreshnessOf(const Profile& profile, const weather::Forecast* forecast,
                      int64_t now_epoch, bool clock_set);

/**
 * @brief The local UTC offset at @p epoch, in seconds, daylight saving included.
 *
 * @param epoch UTC seconds. Must be a real time: pass 0 and you get 0 back,
 *              because a device with no clock has no offset to report either.
 *
 * WHY THIS IS A FUNCTION AND NOT THREE LINES AT THE CALL SITE
 * ----------------------------------------------------------
 * The three lines it replaces were wrong. They took the local and UTC
 * breakdowns of the same instant and subtracted `mktime` of one from `mktime`
 * of the other, with `tm_isdst` left at 0 on the UTC side. `mktime` interprets
 * a broken-down time *in local time*, so on a summer day in Toronto it read the
 * UTC breakdown as an EDT wall-clock time, applied the daylight rule to it, and
 * returned an offset an hour off. Every countdown and every forecast hour
 * label on the panel was shifted by that hour for eight months of the year.
 *
 * `timegm` is the correct inverse: it reads a broken-down time as UTC, which is
 * exactly what "how far is local from UTC right now" needs, and it applies no
 * daylight rule of its own. It is available in this toolchain's picolibc and on
 * every host the mirror runs on. `tm_gmtoff` would be the tidier answer and is
 * not available: picolibc declares it only under `__TM_GMTOFF`, which this
 * toolchain does not define.
 *
 * The offset still comes from the TZ the device already has. This function does
 * arithmetic, not timezone policy — same division as the compositor's.
 */
int32_t UtcOffsetSeconds(int64_t epoch);

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_COMPOSE_H
