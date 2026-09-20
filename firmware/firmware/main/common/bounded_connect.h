/**
 * @file bounded_connect.h
 * @brief The connect step of the exchange, inside the same whole-operation deadline.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS CLOSES
 * ----------------
 * `http_deadline.h` bounds every read, write and poll of an exchange. It did
 * not bound the *connect*, and one step inside connect was not bounded by
 * anything this firmware controls: name resolution.
 *
 * In the ESP-IDF v6.0 tree this firmware builds against, `ssl_connect`
 * (tcp_transport/transport_ssl.c l.104-118) calls `esp_tls_conn_new_sync`, and
 * the first thing that does is `tcp_connect` -> `esp_tls_hostname_to_fd`
 * (esp-tls/esp_tls.c l.203-231), which calls
 *
 *     getaddrinfo(use_host, NULL, &hints, &address_info);
 *
 * with `hints` memset to zero — so no `AI_NUMERICHOST`, and lwIP's
 * `lwip_getaddrinfo` (api/netdb.c l.467-500) falls through to
 * `netconn_gethostbyname_addrtype`, which blocks on an untimed semaphore until
 * lwIP's own DNS state machine gives up. `cfg->timeout_ms` is never consulted
 * on that path: `esp_tls_conn_new_sync`'s elapsed check (l.570-585) only runs
 * *between* `esp_tls_low_level_conn` passes, and the resolver blocks inside the
 * first one. On this build the resolver's own ceiling is DNS_MAX_SERVERS
 * (CONFIG_LWIP_DNS_MAX_SERVERS = 3) x DNS_MAX_RETRIES (4) x DNS_TMR_INTERVAL
 * (1000 ms) — roughly twelve seconds added on top of the budget.
 *
 * THE SHAPE OF THE FIX
 * --------------------
 * Resolve the name ourselves, bounded, *before* connecting, then hand the
 * connect a numeric address:
 *
 *   1. `ResolveCell` below is the bounded wait. The device drives it from
 *      lwIP's asynchronous raw resolver, `dns_gethostbyname` (core/dns.c
 *      l.1687-1691), whose contract is "returns ERR_OK if the name was already
 *      an address or is cached, ERR_INPROGRESS if a query was enqueued and the
 *      callback WILL be made later, or an error". We wait on the callback for
 *      as long as the deadline allows and no longer.
 *
 *   2. A numeric address short-circuits the resolver entirely on the way back
 *      in: `dns_gethostbyname_addrtype` (core/dns.c l.1766-1774) returns ERR_OK
 *      from `ipaddr_aton` before any query is enqueued, so the `getaddrinfo`
 *      inside `esp_tls_hostname_to_fd` cannot block once we pass an address.
 *
 *   3. TLS still sees the *name*. `esp_transport_ssl_set_common_name`
 *      (tcp_transport/transport_ssl.c l.485-488) puts it in `cfg->common_name`,
 *      and `set_client_config` (esp-tls/esp_tls_mbedtls.c l.928-946) passes
 *      exactly that to `mbedtls_ssl_set_hostname`, which is both the SNI
 *      extension and the name the certificate's CN/SAN is checked against. So
 *      substituting the address changes where the packets go and nothing about
 *      what is trusted. The `Host:` header is unaffected — esp_http_client
 *      builds it from the parsed URL, not from what it handed the transport.
 *
 * THE LIFETIME PROBLEM, WHICH IS THE WHOLE DIFFICULTY
 * ---------------------------------------------------
 * Giving up on a DNS query does not cancel it. lwIP has no cancel: once
 * `dns_gethostbyname` returns ERR_INPROGRESS the entry sits in `dns_table` and
 * `dns_check_entry` will call the found-callback later — on success, on
 * failure, or after its retries expire — from the tcpip task, with the `void*`
 * we gave it. If the waiter's state lived on the waiter's stack, that callback
 * would write into a dead frame.
 *
 * So the state is heap allocated and has exactly two owners: the waiter, and
 * the callback lwIP promised to make. Each releases once; the last one out
 * frees. A waiter that times out drops its reference and walks away, and the
 * late callback — seconds later, on another task — writes into memory that is
 * still valid and then frees it. When no callback will be made (ERR_OK,
 * an error return, or a failure to even post the request) the starter releases
 * that second reference itself, so there is exactly one release per reference
 * on every path.
 *
 * Everything here is free of ESP-IDF and FreeRTOS types so the host suite can
 * drive it with real threads under ASan/UBSan, including the late-callback and
 * timeout races. See tests/host/test_bounded_connect.cc.
 */
#ifndef NOTE4C_COMMON_BOUNDED_CONNECT_H_
#define NOTE4C_COMMON_BOUNDED_CONNECT_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>

#include "common/http_deadline.h"

namespace weather {

/// Enough for any textual address lwIP can produce, IPv6 included
/// (INET6_ADDRSTRLEN is 46).
constexpr size_t kAddressTextCap = 48;

/// Enough for a DNS name: lwIP refuses anything at or above DNS_MAX_NAME_LENGTH
/// (256) before it enqueues a query, so a longer name could never resolve.
constexpr size_t kHostNameCap = 256;

enum class ResolveOutcome {
    kResolved,   ///< An address was produced inside the budget.
    kTimedOut,   ///< The budget ran out first. The query may still be running.
    kFailed,     ///< The resolver answered "no", or could not be started.
};

/**
 * @brief The shared cell between a bounded waiter and a callback that may
 *        never come back in time — or at all, if it was never started.
 *
 * Two references, created live. The waiter releases when `Wait` returns; the
 * resolver side releases inside `Deliver`. `delete this` happens on whichever
 * of those runs last, which is deliberately not knowable in advance.
 *
 * @tparam Sync supplies four operations. `Lock`/`Unlock` guard the cell's
 *         fields and its reference count. `Signal` wakes a waiter and must be
 *         **sticky** — a signal raised before anyone waits has to satisfy the
 *         next `WaitMs` rather than being lost, because `Deliver` can land in
 *         the window between the waiter unlocking and the waiter sleeping. A
 *         FreeRTOS binary semaphore is sticky; a bare condition variable is
 *         not, and the host stand-in adds a flag to make it so.
 */
template <typename Sync>
class ResolveCell {
public:
    /// @return nullptr if the cell or its primitives could not be allocated.
    static ResolveCell* Create() {
        ResolveCell* cell = new (std::nothrow) ResolveCell();
        if (cell == nullptr) return nullptr;
        if (!cell->sync_.valid()) {
            // Never started, so neither reference will ever be delivered.
            delete cell;
            return nullptr;
        }
        return cell;
    }

    /**
     * @brief The resolver's answer. Called at most once per reference, from
     *        whatever task the resolver runs on, at whatever time it likes.
     *
     * @param address textual address, or nullptr for "could not resolve".
     *
     * Safe after the waiter has gone: this call owns a reference, so the cell
     * it writes into is still alive, and releasing that reference at the end is
     * what finally frees it.
     */
    void Deliver(const char* address) {
        sync_.Lock();
        if (!settled_) {
            settled_ = true;
            if (address != nullptr && address[0] != '\0') {
                size_t n = 0;
                while (n + 1 < kAddressTextCap && address[n] != '\0') {
                    address_[n] = address[n];
                    ++n;
                }
                address_[n] = '\0';
                resolved_ = address[n] == '\0';  // refuse a truncated address
            }
        }
        sync_.Unlock();
        // Before Release, never after: once the reference is gone the cell may
        // be freed by the other owner at any instant.
        sync_.Signal();
        Release();
    }

    /**
     * @brief Wait up to @p budget_ms for an answer.
     *
     * A non-positive budget does not sleep at all but still reports an answer
     * that has already landed, which is what makes an expired deadline cost
     * nothing rather than cost one more timeout.
     */
    ResolveOutcome Wait(int32_t budget_ms, char* out, size_t cap) {
        sync_.Lock();
        const bool settled_early = settled_;
        sync_.Unlock();
        if (!settled_early && budget_ms > 0) sync_.WaitMs(budget_ms);

        sync_.Lock();
        ResolveOutcome outcome;
        if (!settled_) {
            outcome = ResolveOutcome::kTimedOut;
        } else if (!resolved_) {
            outcome = ResolveOutcome::kFailed;
        } else {
            outcome = ResolveOutcome::kResolved;
            if (out != nullptr && cap > 0) {
                size_t n = 0;
                while (n + 1 < cap && address_[n] != '\0') {
                    out[n] = address_[n];
                    ++n;
                }
                out[n] = '\0';
                if (address_[n] != '\0') outcome = ResolveOutcome::kFailed;
            }
        }
        sync_.Unlock();
        return outcome;
    }

    /// Drop one reference. The cell is gone the moment this returns zero, so
    /// nothing may touch it afterwards — including the caller.
    void Release() {
        sync_.Lock();
        const int left = --refs_;
        sync_.Unlock();
        if (left == 0) delete this;
    }

private:
    ResolveCell() = default;
    ~ResolveCell() = default;

    Sync sync_;
    char address_[kAddressTextCap] = {};
    bool settled_ = false;
    bool resolved_ = false;
    /// The waiter and the promised callback. Guarded by sync_.
    int refs_ = 2;
};

// ------------------------------------------------------ the connect step --

enum class ConnectOutcome {
    kConnected,
    kDeadlineExpired,  ///< No budget left; nothing was attempted.
    kResolveFailed,    ///< Including "the resolver did not answer in time".
    kConnectFailed,    ///< TCP or TLS refused, or ran out of the budget.
};

/**
 * @brief The three things the connect step needs from the platform.
 *
 * Deliberately small. The device implementation of each is a handful of lines
 * in openmeteo_transport_esp.cc; the host suite implements them with a scripted
 * resolver and a scripted handshake so the sequencing below is tested rather
 * than assumed.
 */
class BoundedConnectOps {
public:
    virtual ~BoundedConnectOps() = default;

    virtual int64_t NowMs() = 0;

    /// Bounded name resolution. Must not block longer than @p budget_ms.
    virtual ResolveOutcome Resolve(const char* host, int32_t budget_ms, char* out,
                                   size_t cap) = 0;

    /**
     * @brief TCP + TLS to a numeric @p address, verifying @p server_name.
     *
     * @p server_name is the name from the URL and is what the certificate must
     * match; @p address is only where the packets go. An implementation that
     * verified @p address instead would be a TLS downgrade, which is why the
     * two are separate parameters rather than one.
     */
    virtual bool ConnectTo(const char* address, const char* server_name, int port,
                           int32_t budget_ms) = 0;
};

/**
 * @brief Resolve then connect, both inside one already-armed deadline.
 *
 * The budget is recomputed from the deadline between the two steps, so a
 * resolution that took most of it leaves the handshake correspondingly less
 * rather than starting it afresh. That per-step recomputation is the difference
 * between "each step is bounded" and "the operation is bounded".
 *
 * @param requested_ms what the caller asked for, clamped down by the deadline.
 * @param resolve_cap_ms a further ceiling on the resolution step alone, so a
 *        slow resolver cannot eat a budget the handshake still needs. It only
 *        ever lowers the resolution budget; it never raises the total.
 *
 * Fail-closed throughout: every path that does not end in a verified connection
 * returns a failure, and no path falls back to an unresolved name, an
 * unverified peer or an unbounded wait.
 */
inline ConnectOutcome RunBoundedConnect(BoundedConnectOps& ops,
                                        const TransportDeadline& deadline,
                                        const char* host, int port,
                                        int32_t requested_ms,
                                        int32_t resolve_cap_ms) {
    if (host == nullptr || host[0] == '\0') return ConnectOutcome::kResolveFailed;

    int32_t allowed = deadline.ClampMs(ops.NowMs(), requested_ms);
    if (allowed == kDeadlineExpired) return ConnectOutcome::kDeadlineExpired;

    int32_t resolve_ms = allowed;
    if (resolve_cap_ms > 0 && resolve_cap_ms < resolve_ms) resolve_ms = resolve_cap_ms;

    char address[kAddressTextCap] = {};
    if (ops.Resolve(host, resolve_ms, address, sizeof(address)) !=
        ResolveOutcome::kResolved) {
        return ConnectOutcome::kResolveFailed;
    }

    // Recomputed, not reused: resolution has spent some of the exchange.
    allowed = deadline.ClampMs(ops.NowMs(), requested_ms);
    if (allowed == kDeadlineExpired) return ConnectOutcome::kDeadlineExpired;

    return ops.ConnectTo(address, host, port, allowed) ? ConnectOutcome::kConnected
                                                       : ConnectOutcome::kConnectFailed;
}

}  // namespace weather

#endif  // NOTE4C_COMMON_BOUNDED_CONNECT_H_
