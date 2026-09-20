/**
 * @file autonomy_status.cc
 * @brief Implementation of the status route's `autonomy` block.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See autonomy_status.h for the claims this block makes and why they are
 * rendered by a portable function. No ESP-IDF headers: the host suite reads
 * these exact bytes back.
 */

#include "autonomy_status.h"

#include <stdio.h>
#include <string.h>

namespace autonomy {

const char* CycleOutcomeName(CycleOutcome outcome) {
    switch (outcome) {
        case CycleOutcome::kUpdated: return "updated";
        case CycleOutcome::kUnchanged: return "unchanged";
        case CycleOutcome::kDegraded: return "degraded";
        case CycleOutcome::kFailed: return "failed";
    }
    return "unchanged";
}

namespace {

/// Append, refusing rather than truncating. Same discipline as the power and
/// config renderers: a partial object is worse than none.
bool Append(char* out, size_t cap, size_t* used, const char* text) {
    const size_t n = strlen(text);
    if (*used + n + 1 > cap) return false;
    memcpy(out + *used, text, n);
    *used += n;
    out[*used] = '\0';
    return true;
}

bool AppendI64(char* out, size_t cap, size_t* used, int64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    return Append(out, cap, used, buf);
}

bool AppendU32(char* out, size_t cap, size_t* used, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
    return Append(out, cap, used, buf);
}

bool AppendBool(char* out, size_t cap, size_t* used, bool value) {
    return Append(out, cap, used, value ? "true" : "false");
}

/**
 * @brief A number, or JSON null when it is not a measurement.
 *
 * The one function in this file that carries the whole honesty argument. An
 * epoch derived from an unset clock is a fabricated timestamp, and a tower
 * that received one would display it as fact. Null is the only correct answer,
 * and it has to be produced here rather than left to a caller to remember.
 */
bool AppendI64OrNull(char* out, size_t cap, size_t* used, int64_t value,
                     bool known) {
    if (!known) return Append(out, cap, used, "null");
    return AppendI64(out, cap, used, value);
}

/// A quoted string, refusing anything that would need escaping.
///
/// The only strings this block emits are a hex digest and a fixed set of
/// enum names, none of which can contain a quote or a backslash. Rather than
/// carry an escaper that would never run, this refuses — so a future field
/// holding free text fails loudly here instead of silently emitting a broken
/// document.
bool AppendQuotedSafe(char* out, size_t cap, size_t* used, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!safe) return false;
    }
    return Append(out, cap, used, "\"") && Append(out, cap, used, text) &&
           Append(out, cap, used, "\"");
}

}  // namespace

size_t RenderAutonomyJson(const AutonomyStatus& s, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    bool ok = true;

    ok = ok && Append(out, out_len, &used, "{\"enabled\":");
    ok = ok && AppendBool(out, out_len, &used, s.enabled);

    // ------------------------------------------------------------- profile --
    ok = ok && Append(out, out_len, &used, ",\"profile\":");
    if (!s.profile.present) {
        // Present:false with no digest, rather than an object full of zeros
        // that would read as a profile of revision 0.
        ok = ok && Append(out, out_len, &used,
                          "{\"present\":false,\"sha256\":\"\",\"revision\":0,"
                          "\"profile_version\":0,\"applied_epoch\":null}");
    } else {
        ok = ok && Append(out, out_len, &used, "{\"present\":true,\"sha256\":");
        ok = ok && AppendQuotedSafe(out, out_len, &used, s.profile.sha256);
        ok = ok && Append(out, out_len, &used, ",\"revision\":");
        ok = ok && AppendI64(out, out_len, &used, s.profile.revision);
        ok = ok && Append(out, out_len, &used, ",\"profile_version\":");
        ok = ok && AppendI64(out, out_len, &used, s.profile.profile_version);
        ok = ok && Append(out, out_len, &used, ",\"applied_epoch\":");
        ok = ok && AppendI64OrNull(out, out_len, &used, s.profile.applied_epoch,
                                   s.profile.applied_epoch_known);
        ok = ok && Append(out, out_len, &used, "}");
    }

    // ---------------------------------------------------------- last cycle --
    ok = ok && Append(out, out_len, &used, ",\"last_cycle\":");
    if (!s.last_cycle.known) {
        // A device that has just booted has not completed a content cycle.
        // Reporting a default outcome would tell the tower the last update
        // succeeded on a device that has never updated at all.
        ok = ok && Append(out, out_len, &used, "null");
    } else {
        ok = ok && Append(out, out_len, &used, "{\"origin\":");
        ok = ok && AppendQuotedSafe(out, out_len, &used,
                                    OriginName(s.last_cycle.origin));
        ok = ok && Append(out, out_len, &used, ",\"outcome\":");
        ok = ok && AppendQuotedSafe(out, out_len, &used,
                                    CycleOutcomeName(s.last_cycle.outcome));
        ok = ok && Append(out, out_len, &used, ",\"rendered_epoch\":");
        ok = ok && AppendI64OrNull(out, out_len, &used, s.last_cycle.rendered_epoch,
                                   s.last_cycle.rendered_epoch_known);

        ok = ok && Append(out, out_len, &used, ",\"wifi\":");
        if (!s.last_cycle.wifi_attempted) {
            // A wake that never touched the radio. In Device mode this is the
            // desirable case, not a missing measurement, so it is reported as
            // an explicit "not connected, no time spent" rather than null.
            ok = ok && Append(out, out_len, &used,
                              "{\"connected\":false,\"duration_ms\":0}");
        } else {
            ok = ok && Append(out, out_len, &used, "{\"connected\":");
            ok = ok && AppendBool(out, out_len, &used, s.last_cycle.wifi_connected);
            ok = ok && Append(out, out_len, &used, ",\"duration_ms\":");
            ok = ok && AppendU32(out, out_len, &used, s.last_cycle.wifi_duration_ms);
            ok = ok && Append(out, out_len, &used, "}");
        }

        ok = ok && Append(out, out_len, &used, ",\"fetch\":");
        if (!s.last_cycle.fetch.attempted) {
            ok = ok && Append(out, out_len, &used, "{\"weather\":null}");
        } else {
            const FetchStatus& f = s.last_cycle.fetch;
            ok = ok && Append(out, out_len, &used,
                              "{\"weather\":{\"attempted\":true,\"ok\":");
            ok = ok && AppendBool(out, out_len, &used, f.ok);
            ok = ok && Append(out, out_len, &used, ",\"http_status\":");
            // -1 means no response arrived at all, which is a different fact
            // from a 500 and must not be reported as one.
            ok = ok && AppendI64OrNull(out, out_len, &used, f.http_status,
                                       f.http_status >= 0);
            ok = ok && Append(out, out_len, &used, ",\"duration_ms\":");
            ok = ok && AppendU32(out, out_len, &used, f.duration_ms);
            ok = ok && Append(out, out_len, &used, ",\"bytes\":");
            ok = ok && AppendU32(out, out_len, &used, f.bytes);
            ok = ok && Append(out, out_len, &used, ",\"fetched_epoch\":");
            ok = ok && AppendI64OrNull(out, out_len, &used, f.fetched_epoch,
                                       f.fetched_epoch > 0);
            ok = ok && Append(out, out_len, &used, ",\"cache_age_s\":");
            ok = ok && AppendI64OrNull(out, out_len, &used, f.cache_age_s, f.age_known);
            ok = ok && Append(out, out_len, &used, "}}");
        }
        ok = ok && Append(out, out_len, &used, "}");
    }

    // ------------------------------------------------------------- display --
    //
    // Two fields, because they are two different claims and only one of them is
    // always answerable. `stored_origin` is read from the header of the record
    // the store has active. `displayed_origin` is what the panel is actually
    // showing, and after a deep sleep this device genuinely does not know:
    // e-paper keeps its image across the reboot, the coordinator that knows
    // what was drawn does not. "unknown" is the answer then — never "tower",
    // which is what an earlier revision said and which the tower would have
    // displayed as fact.
    ok = ok && Append(out, out_len, &used, ",\"stored_origin\":");
    ok = ok && AppendQuotedSafe(out, out_len, &used, OriginName(s.stored_origin));
    ok = ok && Append(out, out_len, &used, ",\"displayed_origin\":");
    ok = ok && AppendQuotedSafe(out, out_len, &used, OriginName(s.displayed_origin));
    ok = ok && Append(out, out_len, &used, ",\"next_wake_epoch\":");
    ok = ok && AppendI64OrNull(out, out_len, &used, s.next_wake_epoch,
                               s.next_wake_epoch_known);
    ok = ok && Append(out, out_len, &used, "}");

    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

}  // namespace autonomy
