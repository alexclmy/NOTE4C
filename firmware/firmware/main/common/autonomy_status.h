/**
 * @file autonomy_status.h
 * @brief The `autonomy` block of GET /api/v1/dashboard/status, rendered by a
 *        portable function so its honesty is testable without a panel.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why this is not written inline in the route
 * -------------------------------------------
 * The same reason RenderPowerJson is not: the claims this block makes are the
 * whole point of it, and a claim is only testable if the bytes are produced by
 * a function a host can call. The claims are:
 *
 *   - an age is never reported as a number when the clock was not set,
 *   - `degraded` is a first-class outcome and is never folded into `updated`,
 *   - a cycle that has not happened reports null rather than a plausible
 *     default,
 *   - a device with no profile says so rather than reporting an empty one,
 *   - no part of the profile's *content* appears here.
 *
 * Every one of those is a way a status route could be quietly reassuring, and
 * every one is pinned by a host test that reads the rendered bytes back.
 *
 * What is deliberately absent
 * ---------------------------
 * The profile's contents. This block reports that a profile is present, its
 * digest, its revision and when it was applied — never a module, never a
 * string, never the coordinates. The tower already has the document it pushed,
 * and can read it back verbatim from GET /api/v1/autonomy/profile when it wants
 * to compare. Repeating the content here would put the panel's text in every
 * status poll and every log line that captured one.
 */

#ifndef COMMON_AUTONOMY_STATUS_H
#define COMMON_AUTONOMY_STATUS_H

#include <stddef.h>
#include <stdint.h>

#include "autonomy_policy.h"

namespace autonomy {

/// What the last completed wake cycle achieved, for the content path.
/// Deliberately a different enum from power::CycleOutcome: that one reports
/// what the *power* cycle did, and `degraded` — we drew something, from a
/// cache, having failed to reach what we wanted — has no member there.
enum class CycleOutcome : uint8_t {
    kUpdated = 0,   ///< composed or received something new, and drew it
    kUnchanged,     ///< everything matched what was already displayed
    kDegraded,      ///< drew, but from a cache or with a source missing
    kFailed,        ///< could not draw at all
};

const char* CycleOutcomeName(CycleOutcome outcome);

/// The forecast fetch this cycle attempted, if any.
struct FetchStatus {
    bool attempted = false;
    bool ok = false;
    /// -1 when no response was received at all, which is different from a 500.
    int32_t http_status = -1;
    uint32_t duration_ms = 0;
    uint32_t bytes = 0;
    /// When the cached value was fetched, or 0 when there is none.
    int64_t fetched_epoch = 0;
    /// Age of the cache in seconds, only meaningful when `age_known`.
    int64_t cache_age_s = 0;
    bool age_known = false;
};

struct ProfileStatus {
    bool present = false;
    /// Lowercase hex, 64 characters, of the stored bytes. Empty when absent.
    char sha256[65] = {};
    int32_t revision = 0;
    int32_t profile_version = 0;
    int64_t applied_epoch = 0;
    /// False when the clock was not set when the profile was stored, in which
    /// case applied_epoch is not a time and is reported as null.
    bool applied_epoch_known = false;
};

struct CycleStatus {
    /// False until a content cycle has actually finished. A device that has
    /// just booted reports null rather than a cycle it never ran.
    bool known = false;
    Origin origin = Origin::kNone;
    CycleOutcome outcome = CycleOutcome::kUnchanged;
    int64_t rendered_epoch = 0;
    bool rendered_epoch_known = false;
    bool wifi_connected = false;
    uint32_t wifi_duration_ms = 0;
    /// False when this cycle never touched the radio, which is a real and
    /// desirable state in Device mode, not a missing measurement.
    bool wifi_attempted = false;
    FetchStatus fetch;
};

struct AutonomyStatus {
    bool enabled = false;
    ProfileStatus profile;
    CycleStatus last_cycle;
    /**
     * @brief Where the frame the device is *holding* came from.
     *
     * Always answerable: it is the origin bit in the header of the record the
     * A/B store has active, read from that record rather than remembered.
     * kNone when the device is holding nothing.
     */
    Origin stored_origin = Origin::kNone;
    /**
     * @brief Where the frame actually on the glass came from.
     *
     * A weaker claim than `stored_origin`, and deliberately a separate field.
     * It is only kTower or kLocal when this boot has seen a refresh complete
     * *and* the frame it completed is still the one the store holds. Otherwise
     * it is kUnknown — which is the honest answer after a deep sleep, because
     * the panel retains its image across a reboot while the coordinator that
     * knows what was drawn does not.
     *
     * An earlier revision reported this as kTower in that state, and as kLocal
     * only at the single instant a local compose was accepted. It was therefore
     * wrong after a tower push, wrong after a reboot, and wrong once a refresh
     * finished. Three claims, none measured.
     */
    Origin displayed_origin = Origin::kUnknown;
    int64_t next_wake_epoch = 0;
    bool next_wake_epoch_known = false;
};

/**
 * @brief Render the `autonomy` object.
 *
 * @return bytes written, or 0 when @p out was too small, in which case @p out
 *         is left empty rather than holding a truncated JSON fragment. A tower
 *         that sees null knows it learned nothing; a fragment would fail to
 *         parse and take the whole status response down with it.
 */
size_t RenderAutonomyJson(const AutonomyStatus& status, char* out, size_t out_len);

/// Bytes a caller should reserve for RenderAutonomyJson. Pinned by a host test
/// against the largest object this can produce, so the headroom is measured.
constexpr size_t kAutonomyJsonMax = 768;

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_STATUS_H
