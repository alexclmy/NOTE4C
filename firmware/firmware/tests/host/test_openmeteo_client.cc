/**
 * @file test_openmeteo_client.cc
 * @brief Host tests for the device's one outbound connector.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The transport is an interface precisely so this file can exist: every
 * interesting thing about the fetch path is policy, and none of it needs a
 * socket. What is driven here is a captive portal, a truncated body, a
 * timeout, a redirect, a 429, and a handful of hostnames that look like ours
 * and are not.
 *
 * The allowlist tests are the ones to read first. The claim they defend is the
 * most important single property of the autonomy feature — that no document
 * anyone can push moves where this device connects — and a prefix check that
 * accepted "api.open-meteo.com.evil.example" would quietly retire it.
 */

#include "common/openmeteo_client.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

using namespace weather;

// ----------------------------------------------------------- tiny harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,         \
                   g_current, #cond);                                      \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const long long va = (long long)(a);                               \
        const long long vb = (long long)(b);                               \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n",        \
                   __FILE__, __LINE__, g_current, #a, #b, va, vb);         \
        }                                                                  \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        ++g_checks;                                                        \
        if (strcmp((a), (b)) != 0) {                                       \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: \"%s\" == \"%s\"\n", __FILE__,     \
                   __LINE__, g_current, (a), (b));                         \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

constexpr int64_t kNow = 1789387200ll;  // 2026-09-14T12:00:00Z

/// A transport that returns whatever the test told it to.
class FakeTransport : public HttpTransport {
public:
    std::string body;
    int32_t status = 200;
    bool redirect = false;
    /// Pretend the real body was larger than the buffer.
    bool oversize = false;

    std::string last_url;
    int calls = 0;
    int32_t last_timeout_ms = 0;

    HttpResult Get(const char* url, char* out, size_t cap,
                   int32_t timeout_ms) override {
        last_url = url;
        ++calls;
        last_timeout_ms = timeout_ms;
        // Never more than the client's own ceiling, whatever the caller asked
        // for. The wake cycle often has less than that and never more.
        CHECK(timeout_ms > 0 && timeout_ms <= kFetchTimeoutMs);

        HttpResult r;
        r.duration_ms = 1900;
        if (redirect) {
            r.redirected = true;
            r.status = 302;
            return r;
        }
        r.status = status;
        if (status < 0) return r;

        const size_t n = body.size() < cap ? body.size() : cap;
        memcpy(out, body.data(), n);
        r.bytes = n;
        r.truncated = oversize || body.size() > cap;
        return r;
    }
};

std::string Stamp(int64_t epoch) {
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld",
             (long long)y, (long long)m, (long long)d, (long long)(rem / 3600),
             (long long)((rem % 3600) / 60));
    return buf;
}

/// A response that satisfies the coverage contract.
std::string GoodBody() {
    std::string times = "[";
    std::string temps = "[";
    std::string codes = "[";
    for (int i = 0; i < 26; ++i) {
        if (i) {
            times += ",";
            temps += ",";
            codes += ",";
        }
        times += "\"" + Stamp(kNow + i * 3600ll) + "\"";
        char t[16];
        snprintf(t, sizeof(t), "%.1f", 10.0 + (i % 5));
        temps += t;
        codes += "3";
    }
    times += "]";
    temps += "]";
    codes += "]";
    return "{\"hourly\":{\"time\":" + times + ",\"temperature_2m\":" + temps +
           ",\"weather_code\":" + codes + "}}";
}

}  // namespace

// ----------------------------------------------------------- the allowlist --

static void test_our_own_url_is_allowed() {
    char url[256];
    CHECK(BuildForecastUrl(45.51, -73.56, url, sizeof(url)) > 0);
    CHECK(IsAllowedUrl(url));
}

static void test_a_host_that_merely_starts_with_ours_is_refused() {
    // The prefix-check bug, written down so it cannot come back. This host ends
    // with our name and is not our host.
    CHECK(!IsAllowedUrl("https://api.open-meteo.com.evil.example/v1/forecast"));
    CHECK(!IsAllowedUrl("https://api.open-meteo.commercial/v1/forecast"));
}

static void test_a_different_port_is_refused() {
    // Same name, different service.
    CHECK(!IsAllowedUrl("https://api.open-meteo.com:8443/v1/forecast"));
}

static void test_credentials_in_the_authority_are_refused() {
    // The classic way to make a URL look like it points somewhere it does not.
    CHECK(!IsAllowedUrl("https://api.open-meteo.com@evil.example/v1/forecast"));
    CHECK(!IsAllowedUrl("https://user:pass@api.open-meteo.com/v1/forecast"));
}

static void test_plain_http_is_refused() {
    // A forecast over HTTP is a forecast from whoever is on the network.
    CHECK(!IsAllowedUrl("http://api.open-meteo.com/v1/forecast"));
}

static void test_other_hosts_are_refused() {
    CHECK(!IsAllowedUrl("https://example.com/v1/forecast"));
    CHECK(!IsAllowedUrl("https://192.168.0.10/v1/forecast"));
    CHECK(!IsAllowedUrl("https://localhost/v1/forecast"));
    CHECK(!IsAllowedUrl(""));
    CHECK(!IsAllowedUrl(nullptr));
}

static void test_the_bare_host_with_no_path_is_allowed() {
    CHECK(IsAllowedUrl("https://api.open-meteo.com"));
    CHECK(IsAllowedUrl("https://api.open-meteo.com/"));
}

// ------------------------------------------------------------------- the url --

static void test_the_url_pins_utc_and_the_hourly_fields() {
    // timezone=UTC is load-bearing: without it the service returns local
    // stamps that the parser would read as UTC, shifting the whole forecast.
    char url[256];
    CHECK(BuildForecastUrl(45.51, -73.56, url, sizeof(url)) > 0);
    CHECK(strstr(url, "timezone=UTC") != nullptr);
    CHECK(strstr(url, "hourly=temperature_2m,weather_code") != nullptr);
    CHECK(strstr(url, "forecast_days=3") != nullptr);
    CHECK(strstr(url, "latitude=45.51") != nullptr);
    CHECK(strstr(url, "longitude=-73.56") != nullptr);
}

static void test_coordinates_print_with_exactly_two_decimals() {
    char url[256];
    CHECK(BuildForecastUrl(45.5, -73.0, url, sizeof(url)) > 0);
    CHECK(strstr(url, "latitude=45.50") != nullptr);
    CHECK(strstr(url, "longitude=-73.00") != nullptr);

    CHECK(BuildForecastUrl(0.0, 0.0, url, sizeof(url)) > 0);
    CHECK(strstr(url, "latitude=0.00") != nullptr);
    CHECK(strstr(url, "longitude=0.00") != nullptr);

    CHECK(BuildForecastUrl(-0.05, 179.99, url, sizeof(url)) > 0);
    CHECK(strstr(url, "latitude=-0.05") != nullptr);
    CHECK(strstr(url, "longitude=179.99") != nullptr);
}

static void test_out_of_range_coordinates_build_nothing() {
    char url[256];
    CHECK_EQ_INT(BuildForecastUrl(91.0, 0.0, url, sizeof(url)), 0);
    CHECK_EQ_INT(BuildForecastUrl(0.0, 181.0, url, sizeof(url)), 0);
    CHECK_EQ_INT(url[0], '\0');
}

static void test_a_short_buffer_builds_nothing_rather_than_a_fragment() {
    char url[32];
    CHECK_EQ_INT(BuildForecastUrl(45.51, -73.56, url, sizeof(url)), 0);
    CHECK_EQ_INT(url[0], '\0');
}

// ------------------------------------------------------------- the fetch --

static void test_a_good_response_becomes_a_forecast() {
    FakeTransport transport;
    transport.body = GoodBody();
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(r.ok);
    CHECK(r.error == nullptr);
    CHECK_EQ_INT(r.http.status, 200);
    CHECK(forecast.valid());
    CHECK_EQ_INT(forecast.fetched_epoch, kNow);
    // And it went where it was supposed to.
    CHECK(transport.last_url.find("api.open-meteo.com") != std::string::npos);
    CHECK_EQ_INT(transport.calls, 1);
}

static void test_a_failed_fetch_never_retries_inside_one_wake() {
    // The next wake is the retry, and it arrives with the existing backoff
    // applied. Retrying here would turn one bounded failure into several.
    FakeTransport transport;
    transport.status = 500;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrHttpStatus);
    CHECK_EQ_INT(transport.calls, 1);
}

static void test_no_response_is_distinguished_from_a_server_error() {
    // A dead router and a broken service need different field reports.
    FakeTransport transport;
    transport.status = -1;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrNoResponse);
    CHECK_EQ_INT(r.http.status, -1);
}

static void test_a_redirect_is_refused_rather_than_followed() {
    // A redirect is an instruction to contact a different host, which is the
    // thing the allowlist exists to prevent.
    FakeTransport transport;
    transport.redirect = true;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrRedirect);
}

static void test_an_oversized_body_is_refused_whole() {
    // A truncated JSON array would read as a shorter forecast, and the coverage
    // contract would then reject it for the wrong reason.
    FakeTransport transport;
    transport.body = GoodBody();
    transport.oversize = true;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrTruncated);
}

static void test_a_captive_portal_is_refused_with_the_parsers_reason() {
    FakeTransport transport;
    transport.body = "<!DOCTYPE html><html><body>Sign in to WiFi</body></html>";
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kErrShape);
}

static void test_a_rate_limited_response_is_reported_with_its_status() {
    FakeTransport transport;
    transport.status = 429;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrHttpStatus);
    CHECK_EQ_INT(r.http.status, 429);
}

static void test_a_stale_forecast_from_the_service_is_refused() {
    // The service answered 200 with a well-formed document that does not cover
    // the next day. Accepting it would put a partial forecast on frozen ink.
    FakeTransport transport;
    transport.body =
        R"({"hourly":{"time":["2020-01-01T00:00"],"temperature_2m":[1.0],)"
        R"("weather_code":[3]}})";
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;

    const FetchResult r = FetchForecast(transport, 45.51, -73.56, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kErrCoverage);
}

static void test_a_missing_buffer_is_refused_before_any_request() {
    FakeTransport transport;
    Forecast forecast;
    const FetchResult r =
        FetchForecast(transport, 45.51, -73.56, nullptr, 0, kNow, &forecast);
    CHECK(!r.ok);
    CHECK_EQ_INT(transport.calls, 0);
}

static void test_impossible_coordinates_never_reach_the_network() {
    FakeTransport transport;
    std::vector<char> body(kMaxResponseBytes);
    Forecast forecast;
    const FetchResult r = FetchForecast(transport, 95.0, 0.0, body.data(),
                                        body.size(), kNow, &forecast);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrDisallowed);
    CHECK_EQ_INT(transport.calls, 0);
}

static void test_the_user_agent_identifies_this_device() {
    // A service being polled by this device is entitled to know what is polling
    // it. Anonymity here would be a discourtesy, not a privacy measure.
    CHECK(strstr(kUserAgent, "note4c") != nullptr);
}

// ------------------------------------------- the whole-operation deadline --
//
// WHY THESE EXIST
// ---------------
// `HttpTransport::Get` promises "a hard bound for the whole exchange", and the
// device's implementation of it did not keep that promise. It handed its
// timeout to esp_http_client, which applies it per socket operation, and then
// read the body in a loop with no elapsed check at all. A server trickling one
// byte every nineteen seconds satisfied every individual read and held the wake
// open indefinitely.
//
// The arithmetic that keeps the bound lives in RunBoundedGet, which is what
// openmeteo_transport_esp.cc calls. These drive that exact function through a
// fake clock and a fake socket. It is the adapter's own loop, not a copy of it.
//
// WHAT THESE TESTS DO NOT PROVE
// -----------------------------
// `FakeOps` advances its clock by `min(cost, budget)` in every method, so it
// honours its budget by construction. That makes it the right fake for the
// arithmetic and the WRONG fake for the thing that was actually broken: an
// HttpOps implementation that overruns the budget it was handed. No assertion
// in this file can fail because of such an implementation, because no FakeOps
// can be one.
//
// The real esp_http_client does overrun — `esp_http_client_fetch_headers` and
// `esp_http_client_read` loop internally, re-granting the per-operation timeout
// on every iteration. That defect, and the transport-level deadline that closes
// it, are covered by tests/host/test_http_deadline.cc, which reimplements those
// loops instead of assuming them away. Read the two suites together.

namespace {

/// A fake HTTP connection with a controllable clock and a controllable drip.
class FakeOps : public HttpOps {
public:
    int64_t clock_ms = 0;

    // What the socket does.
    int32_t open_cost_ms = 50;
    bool open_ok = true;
    int32_t header_cost_ms = 100;
    bool header_ok = true;
    int32_t status = 200;
    int64_t content_length = -1;

    std::string body;
    size_t sent = 0;
    /// Bytes handed over per read, and how long each read costs.
    size_t chunk = 4096;
    int32_t read_cost_ms = 10;
    /// Reads that return 0 without consuming their whole budget, forever.
    bool stall = false;
    /// The read that fails, or -1 for none.
    int fail_after_reads = -1;
    int reads = 0;

    bool closed = false;
    /// Every budget this connection was handed, in order. None may be > 0 past
    /// the deadline, and none may exceed what the deadline had left.
    std::vector<int32_t> budgets;

    int64_t NowMs() override { return clock_ms; }

    bool Open(int32_t budget_ms) override {
        budgets.push_back(budget_ms);
        clock_ms += open_cost_ms < budget_ms ? open_cost_ms : budget_ms;
        return open_ok;
    }

    bool FetchHeaders(int32_t budget_ms, int32_t* status_out,
                      int64_t* content_length_out) override {
        budgets.push_back(budget_ms);
        clock_ms += header_cost_ms < budget_ms ? header_cost_ms : budget_ms;
        *status_out = header_ok ? status : -1;
        *content_length_out = content_length;
        return header_ok;
    }

    int ReadBody(char* out, size_t cap, int32_t budget_ms) override {
        budgets.push_back(budget_ms);
        ++reads;
        if (fail_after_reads >= 0 && reads > fail_after_reads) {
            clock_ms += read_cost_ms < budget_ms ? read_cost_ms : budget_ms;
            return -1;
        }
        if (stall) {
            // The pathological server: answers, then says nothing. Each read
            // spends its whole budget and returns no bytes.
            clock_ms += budget_ms;
            return 0;
        }
        clock_ms += read_cost_ms < budget_ms ? read_cost_ms : budget_ms;
        size_t n = body.size() - sent;
        if (n > chunk) n = chunk;
        if (n > cap) n = cap;
        memcpy(out, body.data() + sent, n);
        sent += n;
        return static_cast<int>(n);
    }

    bool Complete() override { return sent >= body.size() && !stall; }

    void Close() override { closed = true; }
};

}  // namespace

static void test_a_healthy_response_is_read_whole_and_the_socket_is_closed() {
    FakeOps ops;
    ops.body = "hello, forecast";
    char buf[64] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK_EQ_INT(r.status, 200);
    CHECK_EQ_INT(r.bytes, ops.body.size());
    CHECK(!r.truncated);
    CHECK(!r.timed_out);
    CHECK(ops.closed);
    CHECK(memcmp(buf, ops.body.data(), ops.body.size()) == 0);
    // And it spent about what the fake said it would, not the whole ceiling.
    CHECK(r.duration_ms < 1000);
}

static void test_a_trickling_server_is_cut_off_at_the_deadline() {
    // The defect, exactly. One byte at a time, each read comfortably inside any
    // per-operation timeout, and the whole thing must still end at 20 s.
    FakeOps ops;
    ops.body = std::string(100000, 'x');
    ops.chunk = 1;
    ops.read_cost_ms = 2000;  // one byte every two seconds
    char buf[1024] = {};

    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK(r.timed_out);
    CHECK(ops.closed);
    CHECK(ops.clock_ms <= 20000);
    CHECK_EQ_INT(r.duration_ms, static_cast<uint32_t>(ops.clock_ms));
    // Fewer than ten reads got in, and nothing ran past the bound.
    CHECK(ops.reads <= 10);
}

static void test_a_server_that_answers_and_then_says_nothing_still_ends() {
    // No bytes at all after the headers. Without an elapsed check this is an
    // infinite loop; with one it is a twenty-second failure.
    FakeOps ops;
    ops.stall = true;
    ops.body = "never arrives";
    char buf[64] = {};

    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK(r.timed_out);
    CHECK_EQ_INT(r.bytes, 0);
    CHECK(ops.closed);
    CHECK(ops.clock_ms <= 20000);
}

static void test_no_blocking_call_is_ever_given_more_than_the_deadline_has() {
    FakeOps ops;
    ops.body = std::string(20000, 'y');
    ops.chunk = 64;
    ops.read_cost_ms = 300;
    char buf[32768] = {};

    const int32_t deadline = 5000;
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), deadline);
    (void)r;
    int32_t spent = 0;
    for (size_t i = 0; i < ops.budgets.size(); ++i) {
        ++g_checks;
        if (ops.budgets[i] <= 0 || ops.budgets[i] > deadline - spent) {
            ++g_failures;
            printf("  FAIL in %s: call %zu got %d ms with %d left\\n", g_current, i,
                   ops.budgets[i], deadline - spent);
            break;
        }
        // Not exact — the fake decides its own costs — but monotone enough to
        // prove the budgets are falling rather than being reissued in full.
        spent = deadline - ops.budgets[i];
    }
    CHECK(ops.clock_ms <= deadline);
}

static void test_a_deadline_that_expires_during_the_handshake_reads_nothing() {
    FakeOps ops;
    ops.open_cost_ms = 9000;
    ops.body = "too late";
    char buf[64] = {};

    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 8000);
    CHECK(r.timed_out);
    CHECK_EQ_INT(ops.reads, 0);
    CHECK(ops.closed);
}

static void test_a_refused_connection_is_no_response_rather_than_a_timeout() {
    FakeOps ops;
    ops.open_ok = false;
    char buf[64] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK_EQ_INT(r.status, -1);
    CHECK(!r.timed_out);
    CHECK(ops.closed);
}

static void test_an_error_part_way_through_the_body_is_not_a_short_document() {
    FakeOps ops;
    ops.body = std::string(4096, 'z');
    ops.chunk = 256;
    ops.fail_after_reads = 3;
    char buf[8192] = {};

    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK_EQ_INT(r.status, -1);   // reported as no response, never as 200
    CHECK(!r.timed_out);
    CHECK(ops.closed);
}

static void test_a_declared_length_past_the_ceiling_reads_no_body_at_all() {
    FakeOps ops;
    ops.content_length = 100000;
    ops.body = std::string(100000, 'q');
    char buf[1024] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK(r.truncated);
    CHECK_EQ_INT(ops.reads, 0);
    CHECK(ops.closed);
}

static void test_an_undeclared_body_past_the_ceiling_is_truncation_not_content() {
    FakeOps ops;
    ops.body = std::string(4096, 'w');
    ops.chunk = 512;
    char buf[1024] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK(r.truncated);
    CHECK_EQ_INT(r.bytes, sizeof(buf));
    CHECK(ops.closed);
}

static void test_a_redirect_is_reported_without_reading_its_page() {
    FakeOps ops;
    ops.status = 302;
    ops.body = "<html>go somewhere else</html>";
    char buf[256] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 20000);
    CHECK(r.redirected);
    CHECK_EQ_INT(ops.reads, 0);
    CHECK(ops.closed);
}

static void test_a_cancelled_fetch_makes_no_request_at_all() {
    // The wake cycle's "there is no budget for this" case, handed straight
    // through. Nothing is opened, so nothing is leaked.
    FakeOps ops;
    ops.body = "unreachable";
    char buf[64] = {};
    const HttpResult r = RunBoundedGet(ops, buf, sizeof(buf), 0);
    CHECK(r.timed_out);
    CHECK_EQ_INT(r.status, -1);
    CHECK(ops.budgets.empty());
    CHECK(ops.closed);
}

static void test_the_fetch_reports_a_deadline_differently_from_a_dead_router() {
    // Through the whole FetchForecast path, which is what the wake cycle calls
    // and what the status route reports the error from.
    class TimeoutTransport : public HttpTransport {
    public:
        HttpResult Get(const char*, char*, size_t, int32_t timeout_ms) override {
            HttpResult r;
            r.status = 200;
            r.timed_out = true;
            r.duration_ms = static_cast<uint32_t>(timeout_ms);
            return r;
        }
    } transport;

    char body[1024];
    Forecast out;
    const FetchResult r = FetchForecast(transport, 45.5, -73.6, body, sizeof(body),
                                        1789387200, &out, 20000);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrDeadline);
}

static void test_a_caller_with_no_budget_never_opens_a_socket() {
    FakeTransport transport;
    transport.body = "{}";
    char body[1024];
    Forecast out;
    const FetchResult r = FetchForecast(transport, 45.5, -73.6, body, sizeof(body),
                                        1789387200, &out, 0);
    CHECK(!r.ok);
    CHECK_STR(r.error, kFetchErrDeadline);
    CHECK_EQ_INT(transport.calls, 0);
}

static void test_a_tight_budget_is_passed_through_and_a_loose_one_is_clamped() {
    FakeTransport transport;
    transport.body = GoodBody();
    char body[32768];
    Forecast out;

    FetchForecast(transport, 45.5, -73.6, body, sizeof(body), 1789387200, &out,
                  6000);
    CHECK_EQ_INT(transport.last_timeout_ms, 6000);

    FetchForecast(transport, 45.5, -73.6, body, sizeof(body), 1789387200, &out,
                  90000);
    CHECK_EQ_INT(transport.last_timeout_ms, kFetchTimeoutMs);
}

int main() {
    RUN(test_our_own_url_is_allowed);
    RUN(test_a_host_that_merely_starts_with_ours_is_refused);
    RUN(test_a_different_port_is_refused);
    RUN(test_credentials_in_the_authority_are_refused);
    RUN(test_plain_http_is_refused);
    RUN(test_other_hosts_are_refused);
    RUN(test_the_bare_host_with_no_path_is_allowed);

    RUN(test_the_url_pins_utc_and_the_hourly_fields);
    RUN(test_coordinates_print_with_exactly_two_decimals);
    RUN(test_out_of_range_coordinates_build_nothing);
    RUN(test_a_short_buffer_builds_nothing_rather_than_a_fragment);

    RUN(test_a_good_response_becomes_a_forecast);
    RUN(test_a_failed_fetch_never_retries_inside_one_wake);
    RUN(test_no_response_is_distinguished_from_a_server_error);
    RUN(test_a_redirect_is_refused_rather_than_followed);
    RUN(test_an_oversized_body_is_refused_whole);
    RUN(test_a_captive_portal_is_refused_with_the_parsers_reason);
    RUN(test_a_rate_limited_response_is_reported_with_its_status);
    RUN(test_a_stale_forecast_from_the_service_is_refused);
    RUN(test_a_missing_buffer_is_refused_before_any_request);
    RUN(test_impossible_coordinates_never_reach_the_network);
    RUN(test_the_user_agent_identifies_this_device);

    RUN(test_a_healthy_response_is_read_whole_and_the_socket_is_closed);
    RUN(test_a_trickling_server_is_cut_off_at_the_deadline);
    RUN(test_a_server_that_answers_and_then_says_nothing_still_ends);
    RUN(test_no_blocking_call_is_ever_given_more_than_the_deadline_has);
    RUN(test_a_deadline_that_expires_during_the_handshake_reads_nothing);
    RUN(test_a_refused_connection_is_no_response_rather_than_a_timeout);
    RUN(test_an_error_part_way_through_the_body_is_not_a_short_document);
    RUN(test_a_declared_length_past_the_ceiling_reads_no_body_at_all);
    RUN(test_an_undeclared_body_past_the_ceiling_is_truncation_not_content);
    RUN(test_a_redirect_is_reported_without_reading_its_page);
    RUN(test_a_cancelled_fetch_makes_no_request_at_all);
    RUN(test_the_fetch_reports_a_deadline_differently_from_a_dead_router);
    RUN(test_a_caller_with_no_budget_never_opens_a_socket);
    RUN(test_a_tight_budget_is_passed_through_and_a_loose_one_is_clamped);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
