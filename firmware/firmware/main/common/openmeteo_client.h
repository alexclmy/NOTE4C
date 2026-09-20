/**
 * @file openmeteo_client.h
 * @brief The device's one outbound connector, and the bounds it runs inside.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * One origin, as a compile-time constant
 * --------------------------------------
 * `https://api.open-meteo.com`, and nothing else, ever. The autonomy profile
 * has no URL field at any depth, so there is no document the tower — or anyone
 * who could push one — can send that moves where this device connects. That is
 * the single most important property of this file, and the allowlist check
 * below exists so it is enforced at the call rather than merely intended.
 *
 * Why the transport is an interface
 * ---------------------------------
 * Everything interesting here is policy: which host, how many bytes, how long,
 * how many redirects, what to do with a 429. None of that needs a socket to be
 * tested, and all of it is the part that would be wrong. So `HttpTransport` is
 * abstract, the host suite drives the whole flow through a fake that can return
 * a captive portal's HTML or hang up mid-body, and the only part that needs a
 * device is the twenty-line esp_http_client adapter in
 * openmeteo_transport_esp.cc.
 *
 * The bounds, and why each one is here
 * ------------------------------------
 *  - **HTTPS only, certificate bundle verified.** Anything else is a forecast
 *    from whoever is on the network.
 *  - **Zero redirects.** A redirect is an instruction to contact a different
 *    host, which is exactly the thing the allowlist exists to prevent. Refused
 *    rather than followed-and-checked, because following it has already leaked
 *    the request.
 *  - **32 KB ceiling.** Three days of hourly data is a few kilobytes. An order
 *    of magnitude past that is not a forecast, and reading it would be spending
 *    the wake budget on something that cannot help.
 *  - **20 seconds, inside the fetch phase's own cap.** The budget is the
 *    guarantee; this never gets to exceed it.
 *  - **No retry inside one wake.** The next wake is the retry, and it arrives
 *    with the existing backoff already applied. Retrying here would turn one
 *    bounded failure into several.
 *
 * Free of ESP-IDF headers. The adapter that is not lives in its own file.
 */

#ifndef COMMON_OPENMETEO_CLIENT_H
#define COMMON_OPENMETEO_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "openmeteo_parse.h"

namespace weather {

/// The only origin this firmware will contact for a forecast.
extern const char* const kForecastOrigin;
/// The only host. Compared exactly, never by suffix: "api.open-meteo.com.evil"
/// ends with our host and is not our host.
extern const char* const kForecastHost;

/// User-Agent. Identifying rather than anonymous: a service being polled by
/// this device is entitled to know what is polling it.
extern const char* const kUserAgent;

constexpr int32_t kFetchTimeoutMs = 20000;
constexpr size_t kMaxRedirects = 0;

/**
 * @brief Build the forecast URL for a location.
 *
 * @param latitude,longitude already coarsened to two decimals by the tower.
 * @return bytes written, or 0 when @p cap was too small or the coordinates
 *         were out of range.
 *
 * Formatted here rather than at the call site so the query string is one
 * testable thing: the `timezone=UTC` in it is what makes the parser's naive
 * timestamps correct, and a caller that forgot it would produce a forecast
 * silently shifted by the panel's offset.
 */
size_t BuildForecastUrl(double latitude, double longitude, char* out, size_t cap);

/**
 * @brief Is this URL one we are allowed to contact?
 *
 * Checked immediately before every request, against the exact host. This is
 * belt and braces — the URL is built by the function above from a profile that
 * cannot carry one — and it is here because the cost of being wrong is a device
 * that talks to whoever asked it to.
 */
bool IsAllowedUrl(const char* url);

/// What a transport managed to do.
struct HttpResult {
    /// HTTP status, or -1 when no response was received at all. The two are
    /// different facts and the status route reports them differently.
    int32_t status = -1;
    size_t bytes = 0;
    uint32_t duration_ms = 0;
    /// True when the body was cut off because it passed the ceiling.
    bool truncated = false;
    /// True when the transport was asked to follow a redirect and refused.
    bool redirected = false;
    /**
     * @brief True when the whole-operation deadline ran out.
     *
     * A separate fact from "no response". A server that answered and then
     * trickled the body for twenty seconds is a different field report from a
     * router that dropped the connection, and the wake cycle treats them the
     * same way — one attempt, no retry — only because both are bounded.
     */
    bool timed_out = false;
};

/**
 * @brief A bounded HTTPS GET.
 *
 * Abstract so the host suite drives the whole fetch path — captive portals,
 * truncated bodies, timeouts, redirects — without a socket.
 */
class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    /**
     * @param url      already allowlisted by the caller.
     * @param out      body buffer.
     * @param cap      its size; the implementation must not write past it and
     *                 must report truncation rather than silently clipping.
     * @param timeout_ms hard bound for the whole exchange.
     */
    virtual HttpResult Get(const char* url, char* out, size_t cap,
                           int32_t timeout_ms) = 0;
};

/**
 * @brief The blocking operations a real HTTP client offers, as an interface.
 *
 * WHY THIS EXISTS, AND WHAT IT DOES *NOT* BY ITSELF GUARANTEE
 * -----------------------------------------------------------
 * `HttpTransport::Get` is a contract — "a hard bound for the whole exchange" —
 * and for a while the device's implementation of it did not keep that contract.
 * It handed `timeout_ms` to esp_http_client, which applies it *per socket
 * operation*, and then read the body in a loop with no elapsed check at all.
 *
 * Splitting the exchange into these five methods and putting the arithmetic in
 * `RunBoundedGet` was necessary but NOT sufficient, and it is worth being
 * precise about why, because an earlier revision of these comments claimed
 * otherwise. `RunBoundedGet` can only check the clock *between* calls. Whether
 * the whole operation is bounded therefore depends entirely on whether each
 * implementation of the five methods below actually returns inside the budget
 * it was handed — and the obvious esp_http_client implementation does not.
 * `esp_http_client_fetch_headers` and `esp_http_client_read` (ESP-IDF v6.0,
 * l.1579-1590 and l.1357-1372) each loop over `esp_transport_read`, granting it
 * the full per-operation timeout every time round, so one call can outlive any
 * budget when the peer keeps trickling bytes.
 *
 * So the contract below is a *requirement placed on implementations*, not a
 * property this interface confers. The device implementation earns it by
 * enforcing an absolute deadline inside its tcp_transport, where every byte has
 * to pass, and by resolving the name itself — bounded — before connecting,
 * because the `getaddrinfo` inside esp-tls's connect consults no clock at all.
 * See `openmeteo_transport_esp.cc`, `common/http_deadline.h` and
 * `common/bounded_connect.h`.
 *
 * Three suites divide the work, and none is sufficient alone:
 *
 *   - `test_openmeteo_client.cc` drives `RunBoundedGet` through a `FakeOps`
 *     that honours its budget by construction. That tests the arithmetic here,
 *     and *cannot* catch an implementation that overruns.
 *   - `test_http_deadline.cc` reimplements the two IDF loops above and drives a
 *     trickling server past both the old and the new transport, which is where
 *     the overrun is actually demonstrated and the fix actually shown.
 *   - `test_bounded_connect.cc` does the same for `Open`: it reproduces lwIP's
 *     asynchronous resolver contract with real threads, and drives the old
 *     connect-by-name shape alongside the new resolve-then-connect one.
 *
 * Every method is handed the milliseconds it may block for, already clamped to
 * what is left of the whole-operation deadline. An implementation MUST NOT
 * exceed it, and MUST return promptly when it is zero. An implementation built
 * on a library whose calls loop internally has to bound them below the library,
 * not around it.
 */
class HttpOps {
public:
    virtual ~HttpOps() = default;

    /// Monotonic milliseconds. Supplied rather than read so the host suite can
    /// drive a twenty-second fetch in no time at all.
    virtual int64_t NowMs() = 0;

    /// Connect, negotiate TLS and send the request line and headers.
    virtual bool Open(int32_t budget_ms) = 0;

    /**
     * @brief Read the response headers.
     * @param status_out         HTTP status, or -1 when none was received.
     * @param content_length_out declared length, or -1 when the server did not
     *                           declare one (chunked).
     */
    virtual bool FetchHeaders(int32_t budget_ms, int32_t* status_out,
                              int64_t* content_length_out) = 0;

    /**
     * @brief Read up to @p cap body bytes.
     * @return >0 bytes read, 0 when nothing was available inside the budget,
     *         <0 on a transport error.
     */
    virtual int ReadBody(char* out, size_t cap, int32_t budget_ms) = 0;

    /// True once the whole declared body has been received.
    virtual bool Complete() = 0;

    /// Release the connection. Called exactly once, on every path out.
    virtual void Close() = 0;
};

/**
 * @brief One bounded GET, with the whole-operation deadline actually enforced.
 *
 * The deadline is absolute: it is taken from `ops.NowMs()` once, at entry, and
 * every blocking call afterwards is clamped to what is left of it. Nothing is
 * started when the time left will not cover it, and the body loop gives up the
 * moment the deadline passes rather than after one more read.
 *
 * `kFetchErrDeadline` is reported through `HttpResult::status == -1` plus
 * `timed_out`, because a fetch that ran out of time and a fetch that got no
 * response are different field reports.
 */
HttpResult RunBoundedGet(HttpOps& ops, char* out, size_t cap, int32_t timeout_ms);

/// Why a fetch did not produce a forecast. Stable tokens for the status route.
extern const char* const kFetchErrDisallowed;
extern const char* const kFetchErrNoResponse;
extern const char* const kFetchErrHttpStatus;
extern const char* const kFetchErrTruncated;
extern const char* const kFetchErrRedirect;
extern const char* const kFetchErrBadBody;
extern const char* const kFetchErrDeadline;

struct FetchResult {
    bool ok = false;
    /// Null on success. One of the tokens above, or a parser token, otherwise.
    const char* error = nullptr;
    /// Detail from the parser, when the body was the problem.
    char detail[64] = {};
    HttpResult http;
};

/**
 * @brief Fetch, validate and normalise a forecast.
 *
 * @param body      caller-owned buffer of at least kMaxResponseBytes. Supplied
 *                  rather than allocated so the fetch phase decides where 32 KB
 *                  lives, which on this device means PSRAM.
 * @param now_epoch UTC seconds, for the coverage contract.
 * @param out       filled only when the return value says ok.
 * @param timeout_ms the whole-operation deadline. Passed in rather than taken
 *        from kFetchTimeoutMs, because the wake cycle's remaining budget is
 *        often tighter than the client's own ceiling and the fetch must end
 *        inside whichever is smaller. Clamped here to kFetchTimeoutMs; a
 *        non-positive value refuses the fetch rather than making an unbounded
 *        one.
 *
 * Never retries. Never follows a redirect. Never contacts anything but the one
 * origin. On any failure the caller keeps whatever is in its cache and reports
 * the cycle as degraded, which is a state the panel shows rather than hides.
 */
FetchResult FetchForecast(HttpTransport& transport, double latitude,
                          double longitude, char* body, size_t body_cap,
                          int64_t now_epoch, Forecast* out,
                          int32_t timeout_ms = kFetchTimeoutMs);

/**
 * @brief The device's one HTTP transport, over esp_http_client.
 *
 * Defined in openmeteo_transport_esp.cc, which is the only file in this pair
 * that includes ESP-IDF. Declared here so the wake cycle has something to call
 * without reaching into that translation unit, and so the host suite — which
 * links the other file and never this one — still fails to link if it ever
 * tries to make a real request.
 */
HttpTransport& DeviceHttpTransport();

}  // namespace weather

#endif  // COMMON_OPENMETEO_CLIENT_H
