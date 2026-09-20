/**
 * @file test_http_deadline.cc
 * @brief The whole-operation deadline, tested against IDF's actual read loops.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS SUITE EXISTS, AND WHY test_openmeteo_client.cc's FakeOps COULD NOT
 * --------------------------------------------------------------------------
 * `test_openmeteo_client.cc` drives `RunBoundedGet` through a `FakeOps` whose
 * every method advances the clock by `min(cost, budget)`. That fake *honours
 * its budget by construction*, so the suite proves the executor's arithmetic
 * and nothing else. The defect was never in the arithmetic. It was in the
 * assumption underneath it — that an `HttpOps` implementation backed by
 * esp_http_client returns inside the budget it was handed. It does not, and a
 * fake that cannot exceed its budget can never show that.
 *
 * So this suite fakes one layer lower: the `FakeIdfClient` below reimplements
 * the two ESP-IDF v6.0 loops that actually read the socket, from
 * components/esp_http_client/esp_http_client.c as vendored in
 * note4c-firmware/toolchain/esp-idf:
 *
 *   `esp_http_client_fetch_headers`, l.1579-1590:
 *       while (client->state < HTTP_STATE_RES_COMPLETE_HEADER) {
 *           buffer->len = esp_transport_read(client->transport, buffer->data,
 *                                            client->buffer_size_rx,
 *                                            client->timeout_ms);
 *           if (buffer->len <= 0) {
 *               if (buffer->len == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT)
 *                   return -ESP_ERR_HTTP_EAGAIN;
 *               return ESP_FAIL;
 *           }
 *           http_parser_execute(...);
 *       }
 *
 *   `esp_http_client_read`, l.1357-1372:
 *       while (need_read > 0 && is_data_remain) {
 *           ...
 *           rlen = esp_transport_read(client->transport, res_buffer->data,
 *                                     byte_to_read, client->timeout_ms);
 *           if (rlen <= 0) { ... }
 *       }
 *
 * The two properties that make the bug reproducible are both preserved here:
 * the loop condition restarts the read whenever bytes arrive, and each read is
 * granted `client->timeout_ms` afresh. A timeout return ends the loop; a byte
 * return continues it.
 *
 * With those in place the suite can run the SAME scripted trickling server past
 * two transports:
 *
 *   - `LegacyTransport` — what the adapter used to do. The whole remaining
 *     budget is pushed into `client->timeout_ms` once, before the call. Every
 *     individual read then fits comfortably inside it, and the exchange runs
 *     for as long as the server cares to keep dripping.
 *   - `DeadlineTransport` — what it does now. One absolute `TransportDeadline`,
 *     consulted on every read, clamping before expiry and returning a *hard
 *     error* after it.
 *
 * The legacy tests below are the regression: they assert the overrun, so they
 * would fail if anyone reintroduced the old scheme while claiming a bound.
 */

#include "common/http_deadline.h"

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

#define CHECK_EQ_I64(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const int64_t va = (int64_t)(a);                                   \
        const int64_t vb = (int64_t)(b);                                   \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %lld == %lld\n", __FILE__,         \
                   __LINE__, g_current, (long long)va, (long long)vb);     \
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

// ------------------------------------------------- the fake socket layer --

/// `esp_transport_read` return codes, with IDF's meanings. The distinction
/// between the two negatives is the whole mechanism: IDF's loops treat a
/// timeout as "nothing yet, the caller may retry" and anything else as fatal.
/// These are IDF's own values, from `enum esp_tcp_transport_err_t` in
/// tcp_transport/include/esp_transport.h — note that a timeout is ZERO, not a
/// negative, which is why both loops test `<= 0` before distinguishing them.
constexpr int kErrTimeout = 0;   // ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT
constexpr int kErrFailed = -2;   // ERR_TCP_TRANSPORT_CONNECTION_FAILED

/**
 * @brief A peer that emits one byte every `interval_ms`, forever if asked.
 *
 * Deliberately not a "slow but finite" server. The defect is that a server
 * which never stops dripping is never cut off, so the script models exactly
 * that: bytes are always available eventually, and only a deadline ends it.
 */
class ScriptedServer {
public:
    int64_t clock_ms = 0;
    /// Gap between consecutive bytes.
    int64_t interval_ms = 1000;
    /// When the clock reaches this, the next byte is ready.
    int64_t next_byte_at_ms = 0;
    /// Bytes handed out so far, across headers and body.
    int64_t emitted = 0;

    /**
     * @brief Wait up to @p timeout_ms for a byte, exactly as select+recv would.
     *
     * Advances the fake clock by however long the wait actually took. Returns
     * the number of bytes produced (always 1 when it does not time out),
     * or kErrTimeout when the budget ran out first.
     */
    int WaitForByte(int64_t timeout_ms) {
        const int64_t wait_needed = next_byte_at_ms - clock_ms;
        if (wait_needed > timeout_ms) {
            // select() slept the whole timeout and nothing became readable.
            clock_ms += timeout_ms;
            return kErrTimeout;
        }
        clock_ms += wait_needed > 0 ? wait_needed : 0;
        next_byte_at_ms = clock_ms + interval_ms;
        ++emitted;
        return 1;
    }
};

/**
 * @brief The transport seam, in the two shapes the adapter has had.
 *
 * `Read` stands in for `esp_transport_read(t, buf, len, timeout_ms)`.
 */
class Transport {
public:
    virtual ~Transport() = default;
    virtual int Read(char* out, int len, int32_t timeout_ms) = 0;
    /// How many times the innermost read was entered. Evidence that the IDF
    /// loop really did iterate rather than the fake short-circuiting it.
    int reads = 0;
};

/// The old adapter: no deadline below the client. Whatever timeout the client
/// passes is honoured in full, every time it is passed.
class LegacyTransport : public Transport {
public:
    explicit LegacyTransport(ScriptedServer& s) : server_(s) {}

    int Read(char* out, int len, int32_t timeout_ms) override {
        ++reads;
        if (len <= 0) return kErrFailed;
        const int n = server_.WaitForByte(timeout_ms);
        if (n <= 0) return n;
        out[0] = 'x';
        return 1;
    }

private:
    ScriptedServer& server_;
};

/// The new adapter: one absolute deadline, consulted on every read.
class DeadlineTransport : public Transport {
public:
    DeadlineTransport(ScriptedServer& s, TransportDeadline& d)
        : server_(s), deadline_(d) {}

    int Read(char* out, int len, int32_t timeout_ms) override {
        ++reads;
        if (len <= 0) return kErrFailed;
        const int32_t allowed = deadline_.ClampMs(server_.clock_ms, timeout_ms);
        if (allowed == kDeadlineExpired) {
            // A HARD error, not a timeout. IDF's loops retry on a timeout and
            // only break on a failure, so reporting this as a timeout would
            // leave the loop spinning against a spent deadline.
            ++expired_reads;
            return kErrFailed;
        }
        const int n = server_.WaitForByte(allowed);
        if (n <= 0) return n;
        out[0] = 'x';
        return 1;
    }

    int expired_reads = 0;

private:
    ScriptedServer& server_;
    TransportDeadline& deadline_;
};

// ------------------------------------------ the fake esp_http_client core --

constexpr int kEspFail = -3;
constexpr int kEspEagain = -4;  // -ESP_ERR_HTTP_EAGAIN

/**
 * @brief ESP-IDF v6.0's two read loops, reproduced.
 *
 * Nothing here is policy. It exists to be as unhelpful as the real thing: it
 * loops until it has what it was asked for, and it re-grants `timeout_ms` to
 * every read it makes.
 */
class FakeIdfClient {
public:
    FakeIdfClient(Transport& t, int64_t header_bytes, int64_t body_bytes)
        : transport_(t), header_bytes_(header_bytes), body_bytes_(body_bytes) {}

    /// `esp_http_client_set_timeout_ms`.
    void SetTimeoutMs(int32_t ms) { timeout_ms_ = ms > 1 ? ms : 1; }

    /// `esp_http_client_fetch_headers`, l.1579-1590.
    int64_t FetchHeaders() {
        char scratch[64];
        while (header_read_ < header_bytes_) {
            const int n = transport_.Read(scratch, 1, timeout_ms_);
            if (n <= 0) {
                if (n == kErrTimeout) return kEspEagain;
                return kEspFail;
            }
            header_read_ += n;
        }
        return body_bytes_;
    }

    /// `esp_http_client_read`, l.1355-1418.
    int Read(char* out, int len) {
        int ridx = 0;
        int need_read = len;
        bool is_data_remain = true;
        while (need_read > 0 && is_data_remain) {
            is_data_remain = body_read_ < body_bytes_;
            if (!is_data_remain) break;
            const int n = transport_.Read(out + ridx, 1, timeout_ms_);
            if (n <= 0) {
                // l.1388-1394: a timeout yields what was read so far, or EAGAIN.
                if (n == kErrTimeout) return ridx > 0 ? ridx : kEspEagain;
                // l.1402-1407: a hard failure ends the loop.
                return ridx > 0 ? ridx : kEspFail;
            }
            ridx += n;
            need_read -= n;
            body_read_ += n;
        }
        return ridx;
    }

    bool Complete() const { return body_read_ >= body_bytes_; }

private:
    Transport& transport_;
    int64_t header_bytes_;
    int64_t body_bytes_;
    int64_t header_read_ = 0;
    int64_t body_read_ = 0;
    int32_t timeout_ms_ = 1;
};

/// A whole exchange, driven the way the adapter drives one. Returns elapsed ms.
/// `guard_ms` is a test-only tripwire: without it a legacy run against an
/// endless trickle would never return, and a hung suite is a worse bug report
/// than a failed assertion.
struct ExchangeOutcome {
    int64_t elapsed_ms = 0;
    bool hit_guard = false;
    bool headers_done = false;
    bool body_done = false;
    int64_t bytes = 0;
};

ExchangeOutcome RunExchange(ScriptedServer& server, FakeIdfClient& client,
                            int32_t budget_ms, int64_t guard_ms) {
    ExchangeOutcome out;
    const int64_t started = server.clock_ms;

    // What the old adapter did, and what the new one still does at the HttpOps
    // layer: hand the call the budget that is left. The difference is entirely
    // in whether anything below enforces it.
    client.SetTimeoutMs(budget_ms);

    const int64_t hdr = client.FetchHeaders();
    out.headers_done = hdr >= 0;
    if (out.headers_done) {
        char buf[512];
        while (!client.Complete()) {
            if (server.clock_ms - started > guard_ms) {
                out.hit_guard = true;
                break;
            }
            const int32_t left =
                static_cast<int32_t>(budget_ms - (server.clock_ms - started));
            client.SetTimeoutMs(left > 0 ? left : 1);
            const int n = client.Read(buf, static_cast<int>(sizeof(buf)));
            if (n <= 0) break;
            out.bytes += n;
        }
        out.body_done = client.Complete();
    }
    if (server.clock_ms - started > guard_ms) out.hit_guard = true;
    out.elapsed_ms = server.clock_ms - started;
    return out;
}

}  // namespace

// ------------------------------------------------- the clamp on its own --

static void test_an_unarmed_deadline_imposes_nothing() {
    TransportDeadline d;
    CHECK(!d.armed());
    CHECK_EQ_I64(d.ClampMs(0, 5000), 5000);
    CHECK_EQ_I64(d.ClampMs(1000000, 5000), 5000);
}

static void test_a_clamp_never_authorises_a_read_past_the_deadline() {
    TransportDeadline d;
    d.Arm(1000, 20000);  // ends at 21000
    CHECK_EQ_I64(d.deadline_ms(), 21000);
    // Asking for less than what is left gets what it asked for.
    CHECK_EQ_I64(d.ClampMs(1000, 5000), 5000);
    // Asking for more than what is left gets only what is left.
    CHECK_EQ_I64(d.ClampMs(19000, 5000), 2000);
    CHECK_EQ_I64(d.ClampMs(20999, 5000), 1);
    // A request for "no timeout" is still bounded.
    CHECK_EQ_I64(d.ClampMs(11000, -1), 10000);
}

static void test_the_moment_the_deadline_passes_is_a_hard_error() {
    TransportDeadline d;
    d.Arm(1000, 20000);
    // Exactly at the deadline is already over: there is no time in which to
    // perform an operation that ends at the same instant it starts.
    CHECK_EQ_I64(d.ClampMs(21000, 5000), kDeadlineExpired);
    CHECK_EQ_I64(d.ClampMs(999999, 5000), kDeadlineExpired);
}

static void test_a_zero_budget_deadline_is_born_expired() {
    TransportDeadline d;
    d.Arm(1000, 0);
    CHECK_EQ_I64(d.ClampMs(1000, 5000), kDeadlineExpired);
    CHECK_EQ_I64(d.RemainingMs(1000), 0);
    // Which is how cancellation is expressed: arm with nothing left and no
    // transport operation will block at all.
    d.Arm(1000, -5);
    CHECK_EQ_I64(d.ClampMs(1000, 5000), kDeadlineExpired);
}

static void test_remaining_is_floored_rather_than_negative() {
    TransportDeadline d;
    d.Arm(0, 1000);
    CHECK_EQ_I64(d.RemainingMs(400), 600);
    CHECK_EQ_I64(d.RemainingMs(1000), 0);
    CHECK_EQ_I64(d.RemainingMs(9999), 0);
    d.Disarm();
    CHECK_EQ_I64(d.RemainingMs(0), 0);
}

// --------------------------------------------- the regression: the header --

static void test_legacy_transport_runs_far_past_the_deadline_on_a_header() {
    // THE BUG. A 20 s budget, a server dripping one header byte per second,
    // and a header long enough to outlast any patience. The old adapter set
    // client->timeout_ms once and every read fitted inside it.
    ScriptedServer server;
    server.interval_ms = 1000;
    LegacyTransport transport(server);
    FakeIdfClient client(transport, /*header_bytes=*/600, /*body_bytes=*/0);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    // It read the whole 600-byte header — 600 seconds — on a 20-second budget,
    // and nothing stopped it. This assertion is the regression: it documents
    // the overrun, and it fails if the legacy scheme is ever described as
    // bounded again.
    // It read the whole 600-byte header on a 20-second budget and nothing
    // stopped it: 599 s, because the first byte is already waiting at t=0.
    CHECK(out.headers_done);
    CHECK_EQ_I64(out.elapsed_ms, 599000);
    CHECK(out.elapsed_ms > 20000 * 25);
    CHECK_EQ_I64(transport.reads, 600);
}

static void test_deadline_transport_bounds_a_trickling_header() {
    // Same server, same loop, same budget. The only change is that the
    // transport consults an absolute deadline on every read.
    ScriptedServer server;
    server.interval_ms = 1000;
    TransportDeadline deadline;
    deadline.Arm(server.clock_ms, 20000);
    DeadlineTransport transport(server, deadline);
    FakeIdfClient client(transport, /*header_bytes=*/600, /*body_bytes=*/0);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    // Cut off at the deadline, not at byte 600.
    CHECK(!out.headers_done);
    CHECK(!out.hit_guard);
    CHECK(out.elapsed_ms <= 20000);
    // And it ended because the deadline said so, not because the server
    // happened to pause.
    CHECK(transport.expired_reads >= 1);
    CHECK(transport.reads <= 21 + 1);
}

// ----------------------------------------------- the regression: the body --

static void test_legacy_transport_runs_far_past_the_deadline_on_a_body() {
    // The same defect one layer down. Headers arrive promptly; the body then
    // trickles. `esp_http_client_read`'s own loop keeps going for as long as
    // bytes keep arriving, and the caller's between-call check never runs.
    ScriptedServer server;
    server.interval_ms = 1000;
    LegacyTransport transport(server);
    FakeIdfClient client(transport, /*header_bytes=*/2, /*body_bytes=*/400);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    // A single `Read(buf, 512)` call swallowed the whole 400-second body,
    // because its internal loop only exits when the data runs out.
    CHECK(out.body_done);
    CHECK(out.elapsed_ms >= 400000);
    CHECK(out.elapsed_ms > 20000 * 15);
}

static void test_deadline_transport_bounds_a_trickling_body() {
    ScriptedServer server;
    server.interval_ms = 1000;
    TransportDeadline deadline;
    deadline.Arm(server.clock_ms, 20000);
    DeadlineTransport transport(server, deadline);
    FakeIdfClient client(transport, /*header_bytes=*/2, /*body_bytes=*/400);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    CHECK(!out.body_done);
    CHECK(!out.hit_guard);
    CHECK(out.elapsed_ms <= 20000);
    CHECK(transport.expired_reads >= 1);
}

// ---------------------------------------------------- it is still a client --

static void test_a_prompt_exchange_is_not_slowed_by_the_deadline() {
    // The deadline must cost nothing when nothing is wrong: a server that
    // answers immediately finishes immediately.
    ScriptedServer server;
    server.interval_ms = 0;
    TransportDeadline deadline;
    deadline.Arm(server.clock_ms, 20000);
    DeadlineTransport transport(server, deadline);
    FakeIdfClient client(transport, /*header_bytes=*/80, /*body_bytes=*/200);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    CHECK(out.headers_done);
    CHECK(out.body_done);
    CHECK_EQ_I64(out.bytes, 200);
    CHECK_EQ_I64(out.elapsed_ms, 0);
    CHECK_EQ_I64(transport.expired_reads, 0);
}

static void test_a_stalled_server_ends_at_the_deadline_not_at_a_socket_timeout() {
    // A peer that accepts the connection and then says nothing at all. The
    // socket timeout would end each read, but only the deadline ends the
    // exchange — and it must, rather than retrying forever.
    ScriptedServer server;
    server.interval_ms = 1000000;
    server.next_byte_at_ms = 1000000;
    TransportDeadline deadline;
    deadline.Arm(server.clock_ms, 20000);
    DeadlineTransport transport(server, deadline);
    FakeIdfClient client(transport, /*header_bytes=*/10, /*body_bytes=*/0);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    CHECK(!out.headers_done);
    CHECK(!out.hit_guard);
    CHECK_EQ_I64(out.elapsed_ms, 20000);
}

static void test_an_already_expired_deadline_blocks_nothing_at_all() {
    // Cancellation. The exchange is handed a spent deadline and must not touch
    // the socket for even one timeout.
    ScriptedServer server;
    server.interval_ms = 1000;
    TransportDeadline deadline;
    deadline.Arm(server.clock_ms, 0);
    DeadlineTransport transport(server, deadline);
    FakeIdfClient client(transport, /*header_bytes=*/600, /*body_bytes=*/400);

    const ExchangeOutcome out =
        RunExchange(server, client, 20000, /*guard_ms=*/10 * 60 * 1000);

    CHECK(!out.headers_done);
    CHECK_EQ_I64(out.elapsed_ms, 0);
    CHECK_EQ_I64(transport.expired_reads, 1);
    CHECK_EQ_I64(transport.reads, 1);
}

static void test_the_bound_holds_across_a_range_of_drip_rates() {
    // The overrun is a function of the drip rate, so the bound is checked
    // across rates rather than at one convenient one.
    const int64_t rates[] = {1, 7, 50, 333, 1000, 4999};
    for (const int64_t rate : rates) {
        ScriptedServer server;
        server.interval_ms = rate;
        TransportDeadline deadline;
        deadline.Arm(server.clock_ms, 20000);
        DeadlineTransport transport(server, deadline);
        FakeIdfClient client(transport, /*header_bytes=*/100000,
                             /*body_bytes=*/100000);

        const ExchangeOutcome out = RunExchange(server, client, 20000,
                                                /*guard_ms=*/10 * 60 * 1000);
        CHECK(!out.hit_guard);
        CHECK(out.elapsed_ms <= 20000);
    }
}

int main() {
    printf("test_http_deadline\n\n");

    RUN(test_an_unarmed_deadline_imposes_nothing);
    RUN(test_a_clamp_never_authorises_a_read_past_the_deadline);
    RUN(test_the_moment_the_deadline_passes_is_a_hard_error);
    RUN(test_a_zero_budget_deadline_is_born_expired);
    RUN(test_remaining_is_floored_rather_than_negative);

    RUN(test_legacy_transport_runs_far_past_the_deadline_on_a_header);
    RUN(test_deadline_transport_bounds_a_trickling_header);
    RUN(test_legacy_transport_runs_far_past_the_deadline_on_a_body);
    RUN(test_deadline_transport_bounds_a_trickling_body);

    RUN(test_a_prompt_exchange_is_not_slowed_by_the_deadline);
    RUN(test_a_stalled_server_ends_at_the_deadline_not_at_a_socket_timeout);
    RUN(test_an_already_expired_deadline_blocks_nothing_at_all);
    RUN(test_the_bound_holds_across_a_range_of_drip_rates);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
