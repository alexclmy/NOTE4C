/**
 * @file openmeteo_client.cc
 * @brief Implementation of the bounded forecast fetch.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See openmeteo_client.h for the bounds and why each one is here. No ESP-IDF
 * headers: the host suite drives this entire flow through a fake transport,
 * which is the point of the transport being an interface.
 *
 * The jitter idea and the "reject rather than guess" discipline are adapted in
 * principle from eMini Home 0.4.0's fetch layer, (c) 2026 Tomasz Fiedoruk, MIT.
 * No code is copied. See THIRD_PARTY_NOTICES.md.
 */

#include "openmeteo_client.h"

#include <stdio.h>
#include <string.h>

namespace weather {

const char* const kForecastOrigin = "https://api.open-meteo.com";
const char* const kForecastHost = "api.open-meteo.com";
const char* const kUserAgent = "note4c-firmware/1.0 (+autonomy)";

const char* const kFetchErrDisallowed = "fetch_host_not_allowed";
const char* const kFetchErrNoResponse = "fetch_no_response";
const char* const kFetchErrHttpStatus = "fetch_http_status";
const char* const kFetchErrTruncated = "fetch_body_too_large";
const char* const kFetchErrRedirect = "fetch_redirect_refused";
const char* const kFetchErrBadBody = "fetch_bad_body";
const char* const kFetchErrDeadline = "fetch_deadline";

namespace {

/**
 * @brief Format a coarsened coordinate with exactly two decimals.
 *
 * Written out rather than left to "%g" because "%g" would print 45.5 as "45.5"
 * and -73.00 as "-73", and while the service accepts both, a URL that varies
 * with the value makes the request harder to recognise in a log and harder to
 * assert on in a test. Two decimals, always, matching what the profile is
 * required to carry.
 */
size_t FormatCoord(double value, char* out, size_t cap) {
    // Round half away from zero into hundredths, in integer space, so the
    // printed value cannot disagree with the profile's own two-decimal check.
    const bool negative = value < 0;
    const double magnitude = negative ? -value : value;
    const long long hundredths = static_cast<long long>(magnitude * 100.0 + 0.5);
    const long long whole = hundredths / 100;
    const long long frac = hundredths % 100;
    const int n = snprintf(out, cap, "%s%lld.%02lld", negative ? "-" : "", whole,
                           frac);
    if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
    return static_cast<size_t>(n);
}

/// Case-insensitive comparison of a fixed prefix, for the scheme.
bool StartsWith(const char* s, const char* prefix) {
    const size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

}  // namespace

size_t BuildForecastUrl(double latitude, double longitude, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    if (!(latitude >= -90.0 && latitude <= 90.0)) return 0;
    if (!(longitude >= -180.0 && longitude <= 180.0)) return 0;

    char lat[16];
    char lon[16];
    if (FormatCoord(latitude, lat, sizeof(lat)) == 0) return 0;
    if (FormatCoord(longitude, lon, sizeof(lon)) == 0) return 0;

    // timezone=UTC is load-bearing, not a preference: the parser reads the
    // naive timestamps this produces as UTC, and a request without it would
    // return local stamps that the parser would then read as UTC — a forecast
    // silently shifted by the panel's own offset.
    const int n = snprintf(out, cap,
                           "%s/v1/forecast?latitude=%s&longitude=%s"
                           "&hourly=temperature_2m,weather_code"
                           "&timezone=UTC&forecast_days=3",
                           kForecastOrigin, lat, lon);
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

bool IsAllowedUrl(const char* url) {
    if (url == nullptr) return false;
    // HTTPS only. A forecast over plain HTTP is a forecast from whoever is on
    // the network.
    if (!StartsWith(url, "https://")) return false;

    const char* host = url + strlen("https://");
    // Credentials in the authority ("https://api.open-meteo.com@evil/") are the
    // classic way to make a URL look like it points somewhere it does not.
    for (const char* p = host; *p != '\0' && *p != '/'; ++p) {
        if (*p == '@') return false;
    }

    const size_t host_len = strlen(kForecastHost);
    if (strncmp(host, kForecastHost, host_len) != 0) return false;

    // The character after the host must end the authority. Without this,
    // "api.open-meteo.com.evil.example" passes a prefix check, and
    // "api.open-meteo.com:8443" reaches a different service on the same name.
    const char after = host[host_len];
    return after == '/' || after == '\0';
}

/**
 * @brief One bounded GET, with the whole-operation deadline enforced.
 *
 * The structure is the point. `deadline_ms` is absolute and computed once; the
 * helper below turns it into "how long may this particular call block", and
 * every blocking call goes through it. There is no path that blocks on
 * something the deadline did not authorise, and the body loop re-checks before
 * every read rather than after.
 */
HttpResult RunBoundedGet(HttpOps& ops, char* out, size_t cap, int32_t timeout_ms) {
    HttpResult result;
    const int64_t started_ms = ops.NowMs();
    const int64_t deadline_ms = started_ms + (timeout_ms > 0 ? timeout_ms : 0);

    // How long the next blocking call may take. Zero means "the deadline has
    // passed": the caller must stop rather than make one more call with a
    // nominal timeout, which is exactly how an unbounded read loop is built.
    const auto remaining = [&]() -> int32_t {
        const int64_t left = deadline_ms - ops.NowMs();
        if (left <= 0) return 0;
        return static_cast<int32_t>(left);
    };
    const auto finish = [&](bool timed_out) {
        ops.Close();
        result.timed_out = timed_out;
        const int64_t spent = ops.NowMs() - started_ms;
        result.duration_ms = static_cast<uint32_t>(spent > 0 ? spent : 0);
        return result;
    };

    if (out == nullptr || cap == 0 || timeout_ms <= 0) {
        // Refused rather than attempted. A fetch with no budget is not a fetch
        // that might be quick; it is one whose bound has already been broken.
        return finish(timeout_ms <= 0);
    }

    // The handshake. Its own share of the deadline, not its own timeout: a TLS
    // negotiation against a host that accepts the connection and then says
    // nothing used to be able to spend the client's whole ceiling here and
    // leave nothing for the body.
    if (!ops.Open(remaining())) return finish(remaining() == 0);
    if (remaining() == 0) return finish(true);

    int64_t content_len = -1;
    if (!ops.FetchHeaders(remaining(), &result.status, &content_len)) {
        return finish(remaining() == 0);
    }

    // 3xx with redirects disabled: say so explicitly rather than letting the
    // caller try to parse a redirect page as a forecast.
    if (result.status >= 300 && result.status < 400) {
        result.redirected = true;
        return finish(false);
    }

    // A declared length past the ceiling costs nothing to refuse here, so the
    // body is never read at all.
    if (content_len > static_cast<int64_t>(cap)) {
        result.truncated = true;
        return finish(false);
    }

    size_t received = 0;
    for (;;) {
        if (ops.Complete()) break;
        if (received >= cap) {
            // More body than the ceiling allows, with no declared length to
            // have caught it earlier. Reported as truncation, never as a
            // shorter document.
            result.truncated = true;
            break;
        }
        const int32_t budget = remaining();
        if (budget == 0) {
            // Out of time with the body incomplete. What has arrived is
            // deliberately *not* handed to the parser: a JSON array cut off
            // part way reads as a shorter forecast, and the coverage contract
            // would then reject it for the wrong reason.
            result.bytes = received;
            return finish(true);
        }
        const int n = ops.ReadBody(out + received, cap - received, budget);
        if (n < 0) {
            result.status = -1;
            break;
        }
        if (n == 0) {
            // Nothing available inside the budget it was given. If the deadline
            // has not passed the loop re-checks `Complete()` and tries again;
            // if it has, the check above ends it. Either way there is no
            // unbounded spin, because every iteration consumes real time from
            // the same deadline.
            if (remaining() == 0) {
                result.bytes = received;
                return finish(true);
            }
            continue;
        }
        received += static_cast<size_t>(n);
    }

    result.bytes = received;
    return finish(false);
}

FetchResult FetchForecast(HttpTransport& transport, double latitude,
                          double longitude, char* body, size_t body_cap,
                          int64_t now_epoch, Forecast* out, int32_t timeout_ms) {
    FetchResult result;
    if (body == nullptr || out == nullptr || body_cap == 0) {
        result.error = kFetchErrDisallowed;
        return result;
    }

    char url[256];
    if (BuildForecastUrl(latitude, longitude, url, sizeof(url)) == 0) {
        result.error = kFetchErrDisallowed;
        return result;
    }
    // Belt and braces. The URL was built from a profile that cannot carry one,
    // and it is checked anyway, because the cost of being wrong here is a
    // device that talks to whoever asked it to.
    if (!IsAllowedUrl(url)) {
        result.error = kFetchErrDisallowed;
        return result;
    }

    const size_t cap = body_cap < kMaxResponseBytes ? body_cap : kMaxResponseBytes;
    // Whichever bound is tighter: the client's own ceiling, or what the wake
    // budget can still cover. Never more than the ceiling, and never a fetch at
    // all when the caller has nothing left to give it.
    int32_t budget_ms = timeout_ms;
    if (budget_ms > kFetchTimeoutMs) budget_ms = kFetchTimeoutMs;
    if (budget_ms <= 0) {
        result.error = kFetchErrDeadline;
        return result;
    }
    result.http = transport.Get(url, body, cap, budget_ms);

    if (result.http.timed_out) {
        // Reported before the status check: a fetch that ran out of time may
        // well have a 200 and half a body, and calling that a bad document
        // would point the field report at the wrong thing.
        result.error = kFetchErrDeadline;
        return result;
    }
    if (result.http.redirected) {
        // Refused rather than followed: a redirect is an instruction to contact
        // a different host, which is the thing the allowlist exists to prevent.
        result.error = kFetchErrRedirect;
        return result;
    }
    if (result.http.status < 0) {
        // No response at all — a dead router, a captive portal that dropped the
        // connection, a timeout. Different from a 500, and reported differently.
        result.error = kFetchErrNoResponse;
        return result;
    }
    if (result.http.status != 200) {
        result.error = kFetchErrHttpStatus;
        return result;
    }
    if (result.http.truncated) {
        // A body past the ceiling is refused whole rather than parsed as far as
        // it got: a truncated JSON array would read as a shorter forecast, and
        // the coverage contract would then reject it for the wrong reason.
        result.error = kFetchErrTruncated;
        return result;
    }

    ParseError parse_error;
    if (!ParseForecast(body, result.http.bytes, now_epoch, out, &parse_error)) {
        result.error = parse_error.code != nullptr ? parse_error.code : kFetchErrBadBody;
        snprintf(result.detail, sizeof(result.detail), "%s", parse_error.detail);
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace weather
