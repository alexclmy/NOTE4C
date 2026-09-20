/**
 * @file test_bounded_connect.cc
 * @brief The connect step: bounded resolution, a safe late callback, and a
 *        deadline that covers DNS, TCP and TLS rather than only the reads.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * TWO HALVES, TWO KINDS OF FAKE, FOR TWO DIFFERENT REASONS
 * ---------------------------------------------------------
 * The connect step has two failure modes and they are not testable the same
 * way, so this suite does not try.
 *
 * 1. `ResolveCell` — a lifetime problem, tested with REAL THREADS and real
 *    time. `FakeLwipDns` below reproduces the contract of lwIP's
 *    `dns_gethostbyname` (core/dns.c l.1687-1691) as vendored in
 *    note4c-firmware/toolchain/esp-idf:
 *
 *      - ERR_OK          the name was already an address, or was cached.
 *                        **No callback will be made.**
 *      - ERR_INPROGRESS  a query was enqueued. The found-callback WILL be made
 *                        later, from the tcpip task, with the void* given —
 *                        on success, on failure, or when the retries expire.
 *                        **There is no way to cancel it.**
 *      - anything else   rejected. No callback.
 *
 *    The uncancellable late callback is the entire difficulty: a waiter that
 *    times out must be able to walk away from memory the resolver will still
 *    write into. A scripted single-threaded fake cannot show that a real
 *    concurrent one is safe, so the fake here answers from a separate thread,
 *    the suite is built with ASan and UBSan, and it counts cell destructions to
 *    prove each one is freed exactly once.
 *
 * 2. `RunBoundedConnect` — a sequencing and arithmetic problem, tested with a
 *    SCRIPTED CLOCK, because the interesting inputs ("resolution took nine of
 *    the twenty seconds") are ones real time cannot be asked for reliably.
 *
 * The regression the parent review asked for is
 * `test_the_legacy_connect_overruns_the_deadline`: `RunLegacyConnect` below is
 * what this adapter used to do — clamp once, then call
 * `esp_transport_connect(host, ...)` and let `getaddrinfo` run inside it with
 * nothing checking the clock again. It is driven by the same script as the new
 * path and asserts the overrun, so it fails if anyone reintroduces the old
 * shape while describing it as bounded.
 */

#include "common/bounded_connect.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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
        printf("%-62s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

// ------------------------------------------- half one: the resolve cell ----

/// Live cells, counted by construction and destruction of the Sync each one
/// owns. The suite asserts this returns to zero after every test: a cell freed
/// twice trips ASan, and one never freed shows up here as a leak.
std::atomic<int> g_sync_live{0};

/**
 * @brief The host stand-in for FreeRtosSync.
 *
 * The sticky flag is not decoration. `Deliver` can raise the signal in the
 * window between the waiter releasing the lock and the waiter sleeping, and a
 * bare condition variable would lose that wakeup and turn a resolved name into
 * a spurious timeout. A FreeRTOS binary semaphore is sticky by nature; this has
 * to be made so on purpose.
 */
class HostSync {
public:
    HostSync() { g_sync_live.fetch_add(1); }
    ~HostSync() { g_sync_live.fetch_sub(1); }
    HostSync(const HostSync&) = delete;
    HostSync& operator=(const HostSync&) = delete;

    bool valid() const { return true; }
    void Lock() { state_.lock(); }
    void Unlock() { state_.unlock(); }

    void Signal() {
        std::lock_guard<std::mutex> guard(wait_);
        signalled_ = true;
        cv_.notify_all();
    }

    void WaitMs(int32_t ms) {
        std::unique_lock<std::mutex> lock(wait_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms),
                     [this] { return signalled_; });
    }

private:
    std::mutex state_;
    std::mutex wait_;
    std::condition_variable cv_;
    bool signalled_ = false;
};

using Cell = ResolveCell<HostSync>;

/**
 * @brief lwIP's `dns_gethostbyname`, with its three returns and its
 *        uncancellable callback.
 */
class FakeLwipDns {
public:
    enum class Start { kImmediate, kEnqueued, kRejected };

    Start start = Start::kEnqueued;
    std::string immediate_address = "";
    /// How long the tcpip task takes to call back. Irrelevant to the waiter's
    /// budget, exactly as it is on the device: there is no cancel.
    int answer_after_ms = 10;
    bool answer_ok = true;
    std::string answer_address = "203.0.113.7";

    ~FakeLwipDns() { Join(); }

    /**
     * @brief Start a query for @p cell, consuming the callback's reference
     *        exactly once on every branch — which is what the device binding
     *        has to get right too.
     */
    void Start_(Cell* cell) {
        switch (start) {
            case Start::kImmediate:
                cell->Deliver(immediate_address.c_str());
                return;
            case Start::kRejected:
                cell->Deliver(nullptr);
                return;
            case Start::kEnqueued:
                break;
        }
        const int delay = answer_after_ms;
        const bool ok = answer_ok;
        const std::string address = answer_address;
        threads_.emplace_back([cell, delay, ok, address] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            cell->Deliver(ok ? address.c_str() : nullptr);
        });
    }

    void Join() {
        for (std::thread& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

private:
    std::vector<std::thread> threads_;
};

/// The device binding's shape, minus ESP-IDF: create, start, wait, release.
ResolveOutcome DriveResolve(FakeLwipDns& dns, int32_t budget_ms, char* out,
                            size_t cap) {
    Cell* cell = Cell::Create();
    if (cell == nullptr) return ResolveOutcome::kFailed;
    dns.Start_(cell);
    const ResolveOutcome outcome = cell->Wait(budget_ms, out, cap);
    cell->Release();
    return outcome;
}

int64_t MonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

static void test_a_cached_or_numeric_name_resolves_without_a_callback() {
    FakeLwipDns dns;
    dns.start = FakeLwipDns::Start::kImmediate;
    dns.immediate_address = "198.51.100.42";

    char out[kAddressTextCap] = {};
    CHECK(DriveResolve(dns, 5000, out, sizeof(out)) == ResolveOutcome::kResolved);
    CHECK(strcmp(out, "198.51.100.42") == 0);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_an_answer_inside_the_budget_is_the_address() {
    FakeLwipDns dns;
    dns.answer_after_ms = 20;
    dns.answer_address = "203.0.113.7";

    char out[kAddressTextCap] = {};
    const int64_t started = MonotonicMs();
    CHECK(DriveResolve(dns, 5000, out, sizeof(out)) == ResolveOutcome::kResolved);
    // It returns when the answer lands, not when the budget ends.
    CHECK(MonotonicMs() - started < 2000);
    CHECK(strcmp(out, "203.0.113.7") == 0);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_a_resolver_that_answers_no_is_a_failure_not_a_timeout() {
    FakeLwipDns dns;
    dns.answer_after_ms = 5;
    dns.answer_ok = false;

    char out[kAddressTextCap] = {};
    CHECK(DriveResolve(dns, 5000, out, sizeof(out)) == ResolveOutcome::kFailed);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_a_rejected_start_releases_both_references() {
    FakeLwipDns dns;
    dns.start = FakeLwipDns::Start::kRejected;

    char out[kAddressTextCap] = {};
    CHECK(DriveResolve(dns, 5000, out, sizeof(out)) == ResolveOutcome::kFailed);
    dns.Join();
    // The whole point: no callback is coming, so the starter had to release the
    // callback's reference itself or this would never reach zero.
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_the_wait_ends_at_the_budget_not_at_the_resolver() {
    FakeLwipDns dns;
    dns.answer_after_ms = 600;

    char out[kAddressTextCap] = {};
    const int64_t started = MonotonicMs();
    const ResolveOutcome outcome = DriveResolve(dns, 60, out, sizeof(out));
    const int64_t waited = MonotonicMs() - started;

    CHECK(outcome == ResolveOutcome::kTimedOut);
    // Bounded by the budget with room for scheduling, and nowhere near the
    // resolver's own 600 ms.
    CHECK(waited < 400);

    // The late callback now lands on a cell whose waiter is gone. Under ASan
    // this is where a stack-allocated or eagerly freed state would be caught.
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_a_zero_budget_does_not_sleep_at_all() {
    FakeLwipDns dns;
    dns.answer_after_ms = 300;

    char out[kAddressTextCap] = {};
    const int64_t started = MonotonicMs();
    CHECK(DriveResolve(dns, 0, out, sizeof(out)) == ResolveOutcome::kTimedOut);
    CHECK(MonotonicMs() - started < 200);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_an_answer_that_lands_before_the_wait_is_still_used() {
    FakeLwipDns dns;
    dns.answer_after_ms = 0;

    // Give the answer time to land before Wait is even entered, which is the
    // race the sticky signal exists for. Driven directly rather than through
    // DriveResolve so the sleep can sit between the start and the wait.
    Cell* cell = Cell::Create();
    CHECK(cell != nullptr);
    dns.Start_(cell);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    char out[kAddressTextCap] = {};
    CHECK(cell->Wait(1000, out, sizeof(out)) == ResolveOutcome::kResolved);
    CHECK(strcmp(out, "203.0.113.7") == 0);
    cell->Release();
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_an_over_long_address_is_refused_rather_than_truncated() {
    FakeLwipDns dns;
    dns.start = FakeLwipDns::Start::kImmediate;
    dns.immediate_address = std::string(kAddressTextCap + 20, '9');

    char out[kAddressTextCap] = {};
    // A truncated address is a different host. Fail closed.
    CHECK(DriveResolve(dns, 1000, out, sizeof(out)) == ResolveOutcome::kFailed);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_an_undersized_output_buffer_is_refused_rather_than_truncated() {
    FakeLwipDns dns;
    dns.start = FakeLwipDns::Start::kImmediate;
    dns.immediate_address = "198.51.100.42";

    char out[6] = {};
    CHECK(DriveResolve(dns, 1000, out, sizeof(out)) == ResolveOutcome::kFailed);
    dns.Join();
    CHECK_EQ_I64(g_sync_live.load(), 0);
}

static void test_the_timeout_and_the_answer_racing_is_safe_either_way() {
    // The interesting window is the one where the answer lands *around* the
    // moment the waiter gives up. Sweeping the delay across the budget puts
    // deliveries on both sides of it and inside it, repeatedly, with ASan and
    // UBSan watching the handoff.
    for (int round = 0; round < 60; ++round) {
        FakeLwipDns dns;
        dns.answer_after_ms = round % 12;  // 0..11 ms around a 6 ms budget

        char out[kAddressTextCap] = {};
        const ResolveOutcome outcome = DriveResolve(dns, 6, out, sizeof(out));
        CHECK(outcome == ResolveOutcome::kResolved ||
              outcome == ResolveOutcome::kTimedOut);
        if (outcome == ResolveOutcome::kResolved) {
            CHECK(strcmp(out, "203.0.113.7") == 0);
        }
        dns.Join();
        CHECK_EQ_I64(g_sync_live.load(), 0);
    }
}

// ------------------------------------------ half two: the connect step ----

/// One scripted connect. The clock is ours so that "resolution took nine of the
/// twenty seconds" is an input rather than something to hope for.
class ScriptedOps : public BoundedConnectOps {
public:
    // --- script ---
    ResolveOutcome resolve_result = ResolveOutcome::kResolved;
    int64_t resolve_cost_ms = 0;
    /// What lwIP would take if nothing bounded it. Used by the legacy path.
    int64_t unbounded_resolve_cost_ms = 12000;
    std::string resolved_address = "203.0.113.7";
    int64_t handshake_cost_ms = 0;
    bool handshake_ok = true;

    // --- what happened ---
    int64_t now = 1000;
    int resolve_calls = 0;
    int connect_calls = 0;
    int32_t last_resolve_budget = -1;
    int32_t last_connect_budget = -1;
    std::string last_address;
    std::string last_server_name;
    int last_port = 0;

    int64_t NowMs() override { return now; }

    ResolveOutcome Resolve(const char* host, int32_t budget_ms, char* out,
                           size_t cap) override {
        ++resolve_calls;
        last_resolve_budget = budget_ms;
        last_server_name = host;
        // A bounded resolver never spends more than it was given, whether it
        // succeeds or gives up.
        if (resolve_result != ResolveOutcome::kResolved) {
            now += resolve_cost_ms < budget_ms ? resolve_cost_ms : budget_ms;
            return resolve_result;
        }
        // A name that needs longer than the budget is a name this resolver
        // gives up on — which is the point of bounding it, and is what makes
        // the legacy comparison below a like-for-like one.
        if (resolve_cost_ms > budget_ms) {
            now += budget_ms;
            return ResolveOutcome::kTimedOut;
        }
        now += resolve_cost_ms;
        if (resolved_address.size() + 1 > cap) return ResolveOutcome::kFailed;
        memcpy(out, resolved_address.c_str(), resolved_address.size() + 1);
        return ResolveOutcome::kResolved;
    }

    bool ConnectTo(const char* address, const char* server_name, int port,
                   int32_t budget_ms) override {
        ++connect_calls;
        last_connect_budget = budget_ms;
        last_address = address;
        last_server_name = server_name;
        last_port = port;
        const int64_t spent =
            handshake_cost_ms < budget_ms ? handshake_cost_ms : budget_ms;
        now += spent;
        return handshake_ok && handshake_cost_ms <= budget_ms;
    }

    /**
     * @brief What the adapter used to do, for the regression below.
     *
     * `esp_transport_connect(host, ...)` with the name, which reaches
     * `esp_tls_hostname_to_fd` and a synchronous `getaddrinfo`. The resolver
     * ignores the timeout it was handed — nothing on that path consults it —
     * and only the handshake after it respects the budget.
     */
    bool LegacyConnectByName(const char* host, int port, int32_t budget_ms) {
        ++connect_calls;
        last_connect_budget = budget_ms;
        last_server_name = host;
        last_port = port;
        now += unbounded_resolve_cost_ms;  // unbounded, by construction
        const int64_t spent =
            handshake_cost_ms < budget_ms ? handshake_cost_ms : budget_ms;
        now += spent;
        return handshake_ok;
    }
};

/// The old shape, preserved so the regression can fail against it.
ConnectOutcome RunLegacyConnect(ScriptedOps& ops, const TransportDeadline& deadline,
                                const char* host, int port, int32_t requested_ms) {
    const int32_t allowed = deadline.ClampMs(ops.NowMs(), requested_ms);
    if (allowed == kDeadlineExpired) return ConnectOutcome::kDeadlineExpired;
    return ops.LegacyConnectByName(host, port, allowed)
               ? ConnectOutcome::kConnected
               : ConnectOutcome::kConnectFailed;
}

const char* kHost = "api.open-meteo.com";

static void test_the_certificate_is_checked_against_the_name_not_the_address() {
    ScriptedOps ops;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnected);
    CHECK_EQ_I64(ops.connect_calls, 1);
    // Where the packets go.
    CHECK(ops.last_address == "203.0.113.7");
    // What must be in the certificate, and what goes in the SNI extension.
    CHECK(ops.last_server_name == kHost);
    CHECK_EQ_I64(ops.last_port, 443);
}

static void test_the_resolution_budget_is_capped_but_the_total_is_not_raised() {
    ScriptedOps ops;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnected);
    // The cap lowers the resolution step.
    CHECK_EQ_I64(ops.last_resolve_budget, 5000);
}

static void test_a_cap_larger_than_the_deadline_does_not_raise_the_budget() {
    ScriptedOps ops;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 2000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnected);
    // The deadline wins: 2000 left, not the 5000 the cap would have allowed.
    CHECK_EQ_I64(ops.last_resolve_budget, 2000);
}

static void test_the_handshake_gets_what_resolution_left_and_not_a_fresh_budget() {
    ScriptedOps ops;
    ops.resolve_cost_ms = 4000;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnected);
    // This is the difference between "each step is bounded" and "the operation
    // is bounded": 20000 - 4000, not 20000 again.
    CHECK_EQ_I64(ops.last_connect_budget, 16000);
}

static void test_a_resolution_that_times_out_fails_closed() {
    ScriptedOps ops;
    ops.resolve_result = ResolveOutcome::kTimedOut;
    ops.resolve_cost_ms = 99999;  // it will only spend what it was given
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kResolveFailed);
    // Nothing was connected to. No fallback to the unresolved name, which would
    // hand the blocking getaddrinfo back to esp-tls.
    CHECK_EQ_I64(ops.connect_calls, 0);
    CHECK_EQ_I64(ops.last_resolve_budget, 5000);
}

static void test_a_resolution_that_answers_no_fails_closed() {
    ScriptedOps ops;
    ops.resolve_result = ResolveOutcome::kFailed;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kResolveFailed);
    CHECK_EQ_I64(ops.connect_calls, 0);
}

static void test_an_expired_deadline_attempts_nothing_at_all() {
    ScriptedOps ops;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 0);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kDeadlineExpired);
    CHECK_EQ_I64(ops.resolve_calls, 0);
    CHECK_EQ_I64(ops.connect_calls, 0);
}

static void test_a_deadline_spent_during_resolution_stops_before_the_handshake() {
    ScriptedOps ops;
    ops.resolve_cost_ms = 20000;  // the resolver succeeds, at the last instant
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 20000) ==
          ConnectOutcome::kDeadlineExpired);
    CHECK_EQ_I64(ops.resolve_calls, 1);
    // A handshake started here would run on a budget that no longer exists.
    CHECK_EQ_I64(ops.connect_calls, 0);
}

static void test_a_refused_handshake_is_reported_as_a_connect_failure() {
    ScriptedOps ops;
    ops.handshake_ok = false;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnectFailed);
}

static void test_an_empty_host_is_refused() {
    ScriptedOps ops;
    TransportDeadline deadline;
    deadline.Arm(ops.now, 20000);

    CHECK(RunBoundedConnect(ops, deadline, "", 443, 20000, 5000) ==
          ConnectOutcome::kResolveFailed);
    CHECK(RunBoundedConnect(ops, deadline, nullptr, 443, 20000, 5000) ==
          ConnectOutcome::kResolveFailed);
    CHECK_EQ_I64(ops.resolve_calls, 0);
}

static void test_an_unarmed_deadline_still_connects() {
    // The fallback path, for a transport built without a deadline: it must
    // still work, it just bounds nothing beyond the requested timeout.
    ScriptedOps ops;
    const TransportDeadline unarmed;

    CHECK(RunBoundedConnect(ops, unarmed, kHost, 443, 9000, 5000) ==
          ConnectOutcome::kConnected);
    CHECK_EQ_I64(ops.last_resolve_budget, 5000);
    CHECK_EQ_I64(ops.last_connect_budget, 9000);
}

static void test_the_whole_connect_stays_inside_the_deadline() {
    // Sweep resolution and handshake costs, including ones far past the budget,
    // and assert the elapsed time of the whole step never exceeds it.
    const int64_t costs[] = {0, 1, 500, 4999, 5000, 15000, 19999, 20000, 90000};
    for (const int64_t resolve_cost : costs) {
        for (const int64_t handshake_cost : costs) {
            ScriptedOps ops;
            ops.resolve_cost_ms = resolve_cost;
            ops.handshake_cost_ms = handshake_cost;
            const int64_t started = ops.now;
            TransportDeadline deadline;
            deadline.Arm(started, 20000);

            const ConnectOutcome outcome =
                RunBoundedConnect(ops, deadline, kHost, 443, 20000, 5000);
            CHECK(ops.now - started <= 20000);
            // And a connect that did not complete is never reported as one.
            if (outcome != ConnectOutcome::kConnected) {
                CHECK(outcome == ConnectOutcome::kDeadlineExpired ||
                      outcome == ConnectOutcome::kResolveFailed ||
                      outcome == ConnectOutcome::kConnectFailed);
            }
        }
    }
}

static void test_the_legacy_connect_overruns_the_deadline() {
    // THE REGRESSION. Same script, same budget, the shape this adapter used to
    // have: one clamp, then a connect-by-name whose getaddrinfo consults no
    // clock. lwIP's own ceiling on this build is DNS_MAX_SERVERS (3) x
    // DNS_MAX_RETRIES (4) x DNS_TMR_INTERVAL (1000 ms).
    ScriptedOps legacy;
    legacy.unbounded_resolve_cost_ms = 12000;
    legacy.handshake_cost_ms = 15000;
    const int64_t legacy_started = legacy.now;
    TransportDeadline legacy_deadline;
    legacy_deadline.Arm(legacy_started, 20000);

    CHECK(RunLegacyConnect(legacy, legacy_deadline, kHost, 443, 20000) ==
          ConnectOutcome::kConnected);
    const int64_t legacy_elapsed = legacy.now - legacy_started;
    // Asserted, not tolerated: 12 000 of resolution plus a 15 000 handshake
    // that only the socket timeout bounded.
    CHECK_EQ_I64(legacy_elapsed, 27000);
    CHECK(legacy_elapsed > 20000);

    // The SAME world — a name that takes 12 s to resolve, a peer that takes 15 s
    // to handshake — through the new path. Resolution is capped at 5 000, the
    // name does not come back inside it, and the step gives up there rather
    // than connecting seven seconds after the cycle should have ended.
    ScriptedOps fixed;
    fixed.resolve_cost_ms = 12000;
    fixed.handshake_cost_ms = 15000;
    const int64_t fixed_started = fixed.now;
    TransportDeadline fixed_deadline;
    fixed_deadline.Arm(fixed_started, 20000);

    CHECK(RunBoundedConnect(fixed, fixed_deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kResolveFailed);
    CHECK_EQ_I64(fixed.now - fixed_started, 5000);
    CHECK_EQ_I64(fixed.connect_calls, 0);
}

static void test_a_resolution_that_fits_still_shortens_the_handshake_budget() {
    // Where the legacy overrun actually came from: after spending time on DNS
    // it handed the handshake a *fresh* budget. Same world for both — a 3 s
    // name and a 25 s peer — and only one of them stays inside 20 s.
    ScriptedOps legacy;
    legacy.unbounded_resolve_cost_ms = 3000;
    legacy.handshake_cost_ms = 25000;
    const int64_t legacy_started = legacy.now;
    TransportDeadline legacy_deadline;
    legacy_deadline.Arm(legacy_started, 20000);
    RunLegacyConnect(legacy, legacy_deadline, kHost, 443, 20000);
    // 3 000 of resolution nobody counted, then a full 20 000 for the handshake.
    CHECK_EQ_I64(legacy.now - legacy_started, 23000);
    CHECK(legacy.now - legacy_started > 20000);

    ScriptedOps fixed;
    fixed.resolve_cost_ms = 3000;
    fixed.handshake_cost_ms = 25000;
    const int64_t fixed_started = fixed.now;
    TransportDeadline fixed_deadline;
    fixed_deadline.Arm(fixed_started, 20000);
    CHECK(RunBoundedConnect(fixed, fixed_deadline, kHost, 443, 20000, 5000) ==
          ConnectOutcome::kConnectFailed);
    CHECK_EQ_I64(fixed.last_connect_budget, 17000);
    CHECK_EQ_I64(fixed.now - fixed_started, 20000);
}

}  // namespace

int main() {
    printf("test_bounded_connect\n\n");

    RUN(test_a_cached_or_numeric_name_resolves_without_a_callback);
    RUN(test_an_answer_inside_the_budget_is_the_address);
    RUN(test_a_resolver_that_answers_no_is_a_failure_not_a_timeout);
    RUN(test_a_rejected_start_releases_both_references);
    RUN(test_the_wait_ends_at_the_budget_not_at_the_resolver);
    RUN(test_a_zero_budget_does_not_sleep_at_all);
    RUN(test_an_answer_that_lands_before_the_wait_is_still_used);
    RUN(test_an_over_long_address_is_refused_rather_than_truncated);
    RUN(test_an_undersized_output_buffer_is_refused_rather_than_truncated);
    RUN(test_the_timeout_and_the_answer_racing_is_safe_either_way);

    RUN(test_the_certificate_is_checked_against_the_name_not_the_address);
    RUN(test_the_resolution_budget_is_capped_but_the_total_is_not_raised);
    RUN(test_a_cap_larger_than_the_deadline_does_not_raise_the_budget);
    RUN(test_the_handshake_gets_what_resolution_left_and_not_a_fresh_budget);
    RUN(test_a_resolution_that_times_out_fails_closed);
    RUN(test_a_resolution_that_answers_no_fails_closed);
    RUN(test_an_expired_deadline_attempts_nothing_at_all);
    RUN(test_a_deadline_spent_during_resolution_stops_before_the_handshake);
    RUN(test_a_refused_handshake_is_reported_as_a_connect_failure);
    RUN(test_an_empty_host_is_refused);
    RUN(test_an_unarmed_deadline_still_connects);
    RUN(test_the_whole_connect_stays_inside_the_deadline);
    RUN(test_the_legacy_connect_overruns_the_deadline);
    RUN(test_a_resolution_that_fits_still_shortens_the_handshake_budget);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
