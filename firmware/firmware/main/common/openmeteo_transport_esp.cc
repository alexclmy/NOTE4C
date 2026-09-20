/**
 * @file openmeteo_transport_esp.cc
 * @brief The one part of the forecast fetch that needs a device.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Everything that decides anything lives in openmeteo_client.cc, which is
 * portable and host-tested. This file is the binding: a TLS transport, the
 * certificate bundle, and five methods that do exactly what they are told for
 * exactly as long as they are allowed.
 *
 * NOT COMPILED BY THE HOST SUITE. It **is** compiled by `idf.py build` for
 * esp32s3, which is what the branch's build gate runs. It has never been run on
 * hardware: *compiled* and *hardware-tested* are different words and this file
 * has earned only the first.
 *
 * WHERE THE DEADLINE ACTUALLY LIVES, AND WHY IT MOVED
 * ---------------------------------------------------
 * An earlier revision of this file claimed a whole-operation deadline and did
 * not have one. It computed the milliseconds remaining before each blocking
 * call and pushed them into `esp_http_client_set_timeout_ms`, on the theory
 * that this bounded the call. Reading the IDF this firmware actually builds
 * against (v6.0, components/esp_http_client/esp_http_client.c) shows it does
 * not:
 *
 *   - `esp_http_client_fetch_headers` (l.1579-1590) loops until the headers
 *     are complete, calling `esp_transport_read(..., client->timeout_ms)` each
 *     time round.
 *   - `esp_http_client_read` (l.1357-1372) loops until it has the bytes it was
 *     asked for, doing the same.
 *
 * `client->timeout_ms` bounds ONE socket read, and the loop grants it afresh on
 * every iteration. A server dripping one byte just inside that timeout keeps a
 * single call alive indefinitely, and a caller that only checks the clock
 * *between* calls never gets the chance — control does not come back.
 *
 * IDF's async mode does not fix this either. It makes the socket non-blocking
 * and lets `esp_http_client_perform` return `ESP_ERR_HTTP_EAGAIN`, but the TLS
 * read path is `ssl_read` (tcp_transport/transport_ssl.c l.261), which starts
 * with `esp_transport_poll_read(t, timeout_ms)` — a `select()` that waits the
 * full timeout regardless of the socket's flags. The loops above still turn
 * once per arriving record.
 *
 * So the deadline is enforced at the innermost blocking primitive instead: a
 * transport wrapper, injected via `esp_http_client_config_t::transport`, that
 * consults one absolute `TransportDeadline` on every connect, poll, read and
 * write. Before the deadline it clamps the timeout it was handed; after it, it
 * returns `ERR_TCP_TRANSPORT_CONNECTION_FAILED`. That last detail is the
 * mechanism, not a detail: IDF's loops retry on `ERR_TCP_TRANSPORT_CONNECTION_
 * TIMEOUT` (which is 0) and break on a hard failure, so only a hard failure
 * actually ends them. The bound now holds no matter how the peer behaves,
 * because no byte can reach the parser without passing a deadline check first.
 *
 * `TransportDeadline` is portable and its interaction with those two loops is
 * covered by tests/host/test_http_deadline.cc, which reimplements them and
 * shows the old scheme running 599 s on a 20 s budget and the new one stopping
 * at 20 s.
 *
 * THE CONNECT STEP, WHICH USED NOT TO BE BOUNDED EITHER
 * -----------------------------------------------------
 * A previous revision of this comment said name resolution was outside the
 * bound and left it there. It is inside it now.
 *
 * The problem was real: `ssl_connect` (tcp_transport/transport_ssl.c l.104-118)
 * calls `esp_tls_conn_new_sync`, whose first pass reaches
 * `esp_tls_hostname_to_fd` (esp-tls/esp_tls.c l.203-231) and a synchronous
 * `getaddrinfo` with zeroed hints — no `AI_NUMERICHOST`, so lwIP
 * (api/netdb.c l.467-500) blocks on `netconn_gethostbyname_addrtype` and an
 * untimed semaphore. `esp_tls_conn_new_sync`'s elapsed check (l.570-585) runs
 * only *between* passes, so `cfg->timeout_ms` never applies to it. On this
 * build the resolver's own ceiling is DNS_MAX_SERVERS
 * (CONFIG_LWIP_DNS_MAX_SERVERS = 3) x DNS_MAX_RETRIES (4) x DNS_TMR_INTERVAL
 * (1000 ms) — about twelve seconds, added on top of the budget.
 *
 * So the name is resolved here first, bounded, through lwIP's asynchronous raw
 * resolver `dns_gethostbyname`, and the numeric address is what gets connected.
 * Passing an address back in cannot block: `dns_gethostbyname_addrtype`
 * (core/dns.c l.1766-1774) returns ERR_OK from `ipaddr_aton` before enqueueing
 * anything, so the `getaddrinfo` inside esp-tls short-circuits. The TCP connect
 * and the TLS handshake after it were already bounded by `cfg->timeout_ms` — a
 * non-blocking socket with a `select` (esp_tls.c l.409-425) and the elapsed
 * check above — and both now receive a budget recomputed from what resolution
 * left, so the whole connect is inside the one deadline.
 *
 * TLS is not weakened by the substitution. `esp_transport_ssl_set_common_name`
 * (transport_ssl.c l.485-488) carries the URL's hostname into `cfg->common_name`
 * and `set_client_config` (esp_tls_mbedtls.c l.928-946) hands exactly that to
 * `mbedtls_ssl_set_hostname` — which is both the SNI extension and the name the
 * server certificate's CN/SAN must match. The address decides where the packets
 * go and nothing else. The `Host:` header is untouched: esp_http_client builds
 * it from the parsed URL, not from what it handed the transport.
 *
 * The sequencing, the per-step budget recomputation and the reference counting
 * that makes a late DNS callback safe all live in common/bounded_connect.h,
 * which is portable and driven by tests/host/test_bounded_connect.cc with real
 * threads under ASan/UBSan. This file is the binding: lwIP on one side, that
 * header on the other.
 *
 * The four properties this file must preserve, all decided elsewhere:
 *
 *   1. TLS with the certificate bundle. Because the transport is now ours, the
 *      bundle is attached to the inner SSL transport directly rather than
 *      through `config.crt_bundle_attach` — esp_http_client does not configure
 *      a transport it did not create.
 *   2. No redirects, reported rather than followed.
 *   3. A hard byte ceiling, reported as truncation rather than silently
 *      clipped.
 *   4. A whole-operation deadline, connect included.
 *
 * The fetch runs on the main task, which is also the task the one-second wake
 * tick runs on. The wake-budget backstop cannot rescue an overrunning fetch —
 * it dispatches on the esp_timer task and `ServiceWakeCycle` refuses reentry
 * while the fetch holds the cycle-advance gate — so the deadline is the only
 * thing that ends a bad fetch. That is precisely why it has to be real.
 */

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>

#include <string.h>

#include <new>

#include "common/bounded_connect.h"
#include "common/http_deadline.h"
#include "openmeteo_client.h"

namespace weather {

namespace {

const char* kTag = "OpenMeteo";

/// A ceiling on the resolution step alone, so a resolver that is merely slow
/// cannot spend a budget the handshake still needs. It only ever lowers the
/// resolution budget; the whole-operation deadline is still the total.
constexpr int32_t kResolveCapMs = 5000;

/// Handed to RunBoundedConnect when a transport was somehow built without a
/// deadline. An unarmed deadline clamps nothing, so that transport behaves the
/// way the stock IDF one does. It is the fallback, not the intended path.
const TransportDeadline kUnarmedDeadline{};

int64_t DeviceNowMs() { return esp_timer_get_time() / 1000; }

/// Never hand a transport a zero or negative timeout: it reads that as "block
/// forever", which is the opposite of what a spent deadline means.
int32_t ClampTimeout(int32_t budget_ms) {
    return budget_ms > 1 ? budget_ms : 1;
}

// --------------------------------------------------- bounded name lookup --

/**
 * @brief The four primitives ResolveCell needs, on FreeRTOS.
 *
 * `Signal` is a binary semaphore rather than a task notification or a condition
 * variable because ResolveCell requires it to be **sticky**: `Deliver` can land
 * in the window between the waiter releasing the lock and the waiter sleeping,
 * and a give with no taker has to satisfy the next take rather than evaporate.
 */
class FreeRtosSync {
public:
    FreeRtosSync() {
        mutex_ = xSemaphoreCreateMutex();
        signal_ = xSemaphoreCreateBinary();
    }
    ~FreeRtosSync() {
        if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
        if (signal_ != nullptr) vSemaphoreDelete(signal_);
    }
    FreeRtosSync(const FreeRtosSync&) = delete;
    FreeRtosSync& operator=(const FreeRtosSync&) = delete;

    bool valid() const { return mutex_ != nullptr && signal_ != nullptr; }
    void Lock() { xSemaphoreTake(mutex_, portMAX_DELAY); }
    void Unlock() { xSemaphoreGive(mutex_); }
    void Signal() { xSemaphoreGive(signal_); }
    void WaitMs(int32_t ms) { xSemaphoreTake(signal_, pdMS_TO_TICKS(ms)); }

private:
    SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t signal_ = nullptr;
};

using EspResolveCell = ResolveCell<FreeRtosSync>;

/// What the tcpip task needs to start the query. Heap allocated because the
/// task that fills it in may be gone — or timed out and moved on — before the
/// tcpip task gets round to it. Freed by the tcpip callback, which runs once.
struct DnsRequest {
    EspResolveCell* cell = nullptr;
    char host[kHostNameCap] = {};
};

/**
 * @brief lwIP's found-callback. Runs on the tcpip task.
 *
 * May arrive seconds after the waiter gave up, which is exactly the case the
 * reference count exists for: `arg` is the reference lwIP promised to consume,
 * so the cell is still alive here, and `Deliver` releases it.
 */
void OnDnsFound(const char* name, const ip_addr_t* ipaddr, void* arg) {
    (void)name;
    EspResolveCell* cell = static_cast<EspResolveCell*>(arg);
    if (cell == nullptr) return;
    if (ipaddr == nullptr) {
        cell->Deliver(nullptr);
        return;
    }
    char text[kAddressTextCap] = {};
    ipaddr_ntoa_r(ipaddr, text, static_cast<int>(sizeof(text)));
    cell->Deliver(text);
}

/**
 * @brief Start the query. Runs on the tcpip task, because `dns_gethostbyname`
 *        is lwIP raw API and may not be called from anywhere else.
 *
 * Exactly one of the three branches releases the callback's reference, because
 * lwIP only promises a callback for ERR_INPROGRESS.
 */
void StartDnsOnTcpip(void* ctx) {
    DnsRequest* req = static_cast<DnsRequest*>(ctx);
    if (req == nullptr) return;
    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    const err_t err = dns_gethostbyname(req->host, &addr, OnDnsFound, req->cell);
    if (err == ERR_INPROGRESS) {
        // Enqueued. OnDnsFound owns the callback's reference from here.
    } else if (err == ERR_OK) {
        // Already an address, or cached. No callback will be made.
        char text[kAddressTextCap] = {};
        ipaddr_ntoa_r(&addr, text, static_cast<int>(sizeof(text)));
        req->cell->Deliver(text);
    } else {
        req->cell->Deliver(nullptr);
    }
    delete req;
}

/**
 * @brief Resolve @p host to a textual address in at most @p budget_ms.
 *
 * Must not be called from the tcpip task: it posts work to that task and waits
 * for it. The forecast fetch runs on the main task, which is where the whole
 * wake cycle runs.
 */
ResolveOutcome BoundedResolve(const char* host, int32_t budget_ms, char* out,
                              size_t cap) {
    if (host == nullptr || host[0] == '\0') return ResolveOutcome::kFailed;
    const size_t host_len = strnlen(host, kHostNameCap);
    if (host_len >= kHostNameCap) return ResolveOutcome::kFailed;

    EspResolveCell* cell = EspResolveCell::Create();
    if (cell == nullptr) return ResolveOutcome::kFailed;

    DnsRequest* req = new (std::nothrow) DnsRequest();
    if (req == nullptr) {
        cell->Release();  // the callback that will now never be made
        cell->Release();  // the waiter
        return ResolveOutcome::kFailed;
    }
    req->cell = cell;
    memcpy(req->host, host, host_len + 1);

    // The *try* variant deliberately: the blocking one parks the caller on a
    // full tcpip mailbox for as long as it stays full, which is the kind of
    // unbounded wait this whole file exists to remove. A full mailbox fails the
    // fetch instead, and the next wake tries again.
    if (tcpip_try_callback(StartDnsOnTcpip, req) != ERR_OK) {
        ESP_LOGW(kTag, "could not post the dns request");
        delete req;
        cell->Release();
        cell->Release();
        return ResolveOutcome::kFailed;
    }

    const ResolveOutcome outcome = cell->Wait(budget_ms, out, cap);
    // Whatever happened, this task is done with the cell. If the query is still
    // running, the callback holds the last reference and frees it when it lands.
    cell->Release();
    return outcome;
}

// ------------------------------------------------ the deadline transport --

/**
 * @brief What the wrapper carries: the real transport, the deadline, and the
 *        server name TLS must verify.
 *
 * Stored as the wrapper's context data. Lifetime is the enclosing EspHttpOps,
 * which outlives every call esp_http_client can make through the wrapper —
 * which matters for `server_name`, because
 * `esp_transport_ssl_set_common_name` stores the pointer rather than copying it.
 */
struct DeadlineCtx {
    esp_transport_handle_t inner = nullptr;
    TransportDeadline* deadline = nullptr;
    char server_name[kHostNameCap] = {};
};

DeadlineCtx* Ctx(esp_transport_handle_t t) {
    return static_cast<DeadlineCtx*>(esp_transport_get_context_data(t));
}

/**
 * @brief How long this operation may block, or "the exchange is over".
 *
 * @return false when the deadline has passed and the caller must fail hard.
 */
bool Allow(DeadlineCtx* ctx, int timeout_ms, int* allowed_out) {
    if (ctx == nullptr || ctx->deadline == nullptr) {
        *allowed_out = timeout_ms;
        return true;
    }
    const int32_t allowed =
        ctx->deadline->ClampMs(DeviceNowMs(), static_cast<int32_t>(timeout_ms));
    if (allowed == kDeadlineExpired) return false;
    *allowed_out = static_cast<int>(allowed);
    return true;
}

/**
 * @brief The platform half of RunBoundedConnect: lwIP's resolver, and the inner
 *        TLS transport.
 */
class EspConnectOps : public BoundedConnectOps {
public:
    explicit EspConnectOps(esp_transport_handle_t inner) : inner_(inner) {}

    int64_t NowMs() override { return DeviceNowMs(); }

    ResolveOutcome Resolve(const char* host, int32_t budget_ms, char* out,
                           size_t cap) override {
        return BoundedResolve(host, budget_ms, out, cap);
    }

    bool ConnectTo(const char* address, const char* server_name, int port,
                   int32_t budget_ms) override {
        // The name, never the address, is what the certificate must match.
        // This one call is both the SNI extension and the CN/SAN check
        // (esp_tls_mbedtls.c set_client_config), so the substitution below
        // changes the route and not the trust.
        esp_transport_ssl_set_common_name(inner_, server_name);
        return esp_transport_connect(inner_, address, port,
                                     ClampTimeout(budget_ms)) >= 0;
    }

private:
    esp_transport_handle_t inner_;
};

int DlConnect(esp_transport_handle_t t, const char* host, int port,
              int timeout_ms) {
    DeadlineCtx* ctx = Ctx(t);
    if (ctx == nullptr || ctx->inner == nullptr || host == nullptr) {
        return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }
    // Copied into the context because the common-name pointer outlives this
    // call and esp_http_client's own host string is not ours to depend on.
    const size_t host_len = strnlen(host, kHostNameCap);
    if (host_len >= kHostNameCap) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    memcpy(ctx->server_name, host, host_len + 1);

    // Resolution and handshake both inside the one deadline, with the budget
    // recomputed between them. A peer that completes the TCP connection and
    // then negotiates very slowly used to be able to spend the entire ceiling
    // here; a name that would not resolve used to be able to spend more.
    EspConnectOps ops(ctx->inner);
    const TransportDeadline& deadline =
        (ctx->deadline != nullptr) ? *ctx->deadline : kUnarmedDeadline;
    const ConnectOutcome outcome = RunBoundedConnect(
        ops, deadline, ctx->server_name, port, timeout_ms, kResolveCapMs);
    if (outcome == ConnectOutcome::kConnected) return 0;
    ESP_LOGW(kTag, "connect failed (%d) for %s:%d", static_cast<int>(outcome),
             ctx->server_name, port);
    return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

int DlRead(esp_transport_handle_t t, char* buffer, int len, int timeout_ms) {
    DeadlineCtx* ctx = Ctx(t);
    int allowed = timeout_ms;
    if (!Allow(ctx, timeout_ms, &allowed)) {
        // Hard failure, deliberately. Reporting a timeout here would send
        // esp_http_client's read loop round again against a spent deadline,
        // which is the whole defect this exists to close.
        return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }
    return esp_transport_read(ctx->inner, buffer, len, ClampTimeout(allowed));
}

int DlWrite(esp_transport_handle_t t, const char* buffer, int len,
            int timeout_ms) {
    DeadlineCtx* ctx = Ctx(t);
    int allowed = timeout_ms;
    if (!Allow(ctx, timeout_ms, &allowed)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    return esp_transport_write(ctx->inner, buffer, len, ClampTimeout(allowed));
}

int DlPollRead(esp_transport_handle_t t, int timeout_ms) {
    DeadlineCtx* ctx = Ctx(t);
    int allowed = timeout_ms;
    // -1 is "error" to every caller of poll_read, and ssl_read turns it into
    // ERR_TCP_TRANSPORT_CONNECTION_FAILED. 0 would mean "timed out, try again".
    if (!Allow(ctx, timeout_ms, &allowed)) return -1;
    return esp_transport_poll_read(ctx->inner, ClampTimeout(allowed));
}

int DlPollWrite(esp_transport_handle_t t, int timeout_ms) {
    DeadlineCtx* ctx = Ctx(t);
    int allowed = timeout_ms;
    if (!Allow(ctx, timeout_ms, &allowed)) return -1;
    return esp_transport_poll_write(ctx->inner, ClampTimeout(allowed));
}

int DlClose(esp_transport_handle_t t) {
    DeadlineCtx* ctx = Ctx(t);
    if (ctx == nullptr || ctx->inner == nullptr) return 0;
    return esp_transport_close(ctx->inner);
}

/// Called by esp_transport_destroy on the wrapper. The inner transport and the
/// context are owned by EspHttpOps and torn down there, so this does nothing:
/// freeing them here would double-free the moment anything else destroyed the
/// wrapper first.
int DlDestroy(esp_transport_handle_t t) {
    (void)t;
    return 0;
}

// ------------------------------------------------------------- the client --

/**
 * @brief esp_http_client behind the portable HttpOps interface.
 *
 * No policy, no loop, no arithmetic. One connection, opened once and closed
 * once, with the whole exchange bounded by a deadline the transport enforces.
 */
class EspHttpOps : public HttpOps {
public:
    EspHttpOps(const char* url, int32_t timeout_ms) {
        // Armed before anything is built, so even the connect cannot escape it.
        deadline_.Arm(DeviceNowMs(), timeout_ms);

        inner_ = esp_transport_ssl_init();
        if (inner_ == nullptr) {
            ESP_LOGW(kTag, "could not create the TLS transport");
            return;
        }
        // The bundle is what makes the "https" in the URL mean anything. A
        // transport without it would accept a forecast from whoever is on the
        // network, which is precisely the failure the allowlist cannot catch.
        // It goes on the inner transport because esp_http_client only applies
        // config.crt_bundle_attach to a transport it created itself.
        esp_transport_ssl_crt_bundle_attach(inner_, esp_crt_bundle_attach);

        wrapper_ = esp_transport_init();
        if (wrapper_ == nullptr) {
            ESP_LOGW(kTag, "could not create the deadline transport");
            return;
        }
        ctx_.inner = inner_;
        ctx_.deadline = &deadline_;
        esp_transport_set_context_data(wrapper_, &ctx_);
        esp_transport_set_func(wrapper_, DlConnect, DlRead, DlWrite, DlClose,
                               DlPollRead, DlPollWrite, DlDestroy);

        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_GET;
        // Still set, and still honoured per socket operation — but it is not
        // the bound. It ends a read against a peer that has gone *silent*; the
        // transport deadline is what ends one against a peer that keeps
        // trickling. Set to the whole ceiling rather than something shorter
        // because the transport clamps every read to the time actually left,
        // so a smaller number here would only cause spurious mid-fetch
        // timeouts on a slow-but-honest connection.
        config.timeout_ms = ClampTimeout(timeout_ms);
        // A redirect is an instruction to contact a different host. Following
        // it has already leaked the request, so it is refused rather than
        // followed-and-checked.
        config.disable_auto_redirect = true;
        config.max_redirection_count = 0;
        config.user_agent = kUserAgent;
        config.transport = wrapper_;
        client_ = esp_http_client_init(&config);
        if (client_ == nullptr) ESP_LOGW(kTag, "could not create the http client");
    }

    ~EspHttpOps() override {
        Close();
        // Order matters: the wrapper first, because it refers to the inner
        // handle and to ctx_, and only this object may free those.
        if (wrapper_ != nullptr) {
            esp_transport_destroy(wrapper_);
            wrapper_ = nullptr;
        }
        if (inner_ != nullptr) {
            esp_transport_destroy(inner_);
            inner_ = nullptr;
        }
    }

    int64_t NowMs() override { return DeviceNowMs(); }

    bool Open(int32_t budget_ms) override {
        if (client_ == nullptr || budget_ms <= 0) return false;
        Apply(budget_ms);
        const esp_err_t err = esp_http_client_open(client_, 0);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "open failed: %s", esp_err_to_name(err));
            return false;
        }
        open_ = true;
        return true;
    }

    bool FetchHeaders(int32_t budget_ms, int32_t* status_out,
                      int64_t* content_length_out) override {
        if (client_ == nullptr || !open_ || budget_ms <= 0) return false;
        Apply(budget_ms);
        const int64_t len = esp_http_client_fetch_headers(client_);
        *status_out = static_cast<int32_t>(esp_http_client_get_status_code(client_));
        // A negative return is "no declared length" (chunked) as well as an
        // error; the status tells the two apart and the executor only uses the
        // length to refuse an oversized body early.
        *content_length_out = len;
        return *status_out > 0;
    }

    int ReadBody(char* out, size_t cap, int32_t budget_ms) override {
        if (client_ == nullptr || !open_ || budget_ms <= 0) return -1;
        Apply(budget_ms);
        return esp_http_client_read(client_, out, static_cast<int>(cap));
    }

    bool Complete() override {
        return client_ != nullptr && open_ &&
               esp_http_client_is_complete_data_received(client_);
    }

    void Close() override {
        // The exchange is over: nothing below may block again, even if
        // esp_http_client decides to read while shutting the connection down.
        deadline_.Arm(DeviceNowMs(), 0);
        if (client_ == nullptr) return;
        if (open_) {
            esp_http_client_close(client_);
            open_ = false;
        }
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }

private:
    /// The per-socket-operation timeout. Still applied, because it is what ends
    /// a read against a peer that has gone silent; the deadline is what ends
    /// one against a peer that has not.
    void Apply(int32_t budget_ms) {
        esp_http_client_set_timeout_ms(client_, ClampTimeout(budget_ms));
    }

    TransportDeadline deadline_;
    DeadlineCtx ctx_;
    esp_transport_handle_t inner_ = nullptr;
    esp_transport_handle_t wrapper_ = nullptr;
    esp_http_client_handle_t client_ = nullptr;
    bool open_ = false;
};

}  // namespace

/**
 * @brief The device's transport. Builds the ops, and lets the executor drive.
 */
class EspHttpTransport : public HttpTransport {
public:
    HttpResult Get(const char* url, char* out, size_t cap,
                   int32_t timeout_ms) override {
        EspHttpOps ops(url, timeout_ms);
        return RunBoundedGet(ops, out, cap, timeout_ms);
    }
};

/// The one instance the wake cycle uses. Stateless, so a single static costs
/// nothing and saves the fetch phase an allocation it would have to free on
/// every path out.
HttpTransport& DeviceHttpTransport() {
    static EspHttpTransport transport;
    return transport;
}

}  // namespace weather
