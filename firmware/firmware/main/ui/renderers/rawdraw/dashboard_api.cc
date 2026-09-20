/**
 * @file dashboard_api.cc
 * @brief Implementation of the /api/v1/dashboard/ routes.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "dashboard_api.h"

#include "config_api.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

#include <cJSON.h>

#include "application.h"
#include "common/autonomy_service.h"
#include "common/autonomy_status.h"
#include "common/dashboard_manager.h"
#include "common/device_config.h"
#include "common/device_config_service.h"
#include "common/power_policy.h"
#include "common/voice_hub_config.h"
#include "dashboard_build_config.h"
#include "product_identity.h"

namespace rawdraw {

namespace {

const char* kTag = "DashboardApi";

constexpr size_t kHeaderValueMax = 128;

/**
 * @brief UTC seconds, or 0 when SNTP has never succeeded.
 *
 * The same test application.cc already uses for the wake schedule — later than
 * 2020 means the clock has been set — so a profile stamped with an unset clock
 * records 0 rather than a time in 1970.
 */
int64_t CurrentEpochOrZero() {
    const time_t now = time(nullptr);
    return now > 1600000000 ? static_cast<int64_t>(now) : 0;
}

/// Read a request header into @p out, returning nullptr when absent.
const char* GetHeader(httpd_req_t* req, const char* name, char* out, size_t out_len) {
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return nullptr;
    }
    if (httpd_req_get_hdr_value_str(req, name, out, out_len) != ESP_OK) {
        return nullptr;
    }
    return out;
}

/**
 * @brief One line per answered request.
 *
 * The method, the path and the status: enough to tell "the handler never
 * completed" from "the client never got through", which is the distinction a
 * device that reports `httpd_start` success and still refuses connections is
 * impossible to debug without.
 *
 * The authentication header is reported as present or absent and never by
 * value. No token, no body, no frame and no hub URL is logged here or anywhere
 * else in this file.
 */
void LogHttpRequest(httpd_req_t* req, const char* status) {
    if (req == nullptr) return;
    const bool auth_hdr = httpd_req_get_hdr_value_len(req, "X-Auth-Token") > 0;
    ESP_LOGI(kTag, "http: %s %s -> %s (auth_hdr=%d)",
             http_method_str(static_cast<enum http_method>(req->method)),
             req->uri, status, auth_hdr ? 1 : 0);
}

esp_err_t SendJson(httpd_req_t* req, const char* status, const char* json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    // Nothing here is cacheable: every response describes a live device state.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    LogHttpRequest(req, status);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* code, const char* detail) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    return SendJson(req, status, buf);
}

const char* RenderName(dashboard::RenderDisposition d) {
    switch (d) {
        case dashboard::RenderDisposition::kStarted:   return "started";
        case dashboard::RenderDisposition::kQueued:    return "queued";
        case dashboard::RenderDisposition::kCoalesced: return "coalesced";
        case dashboard::RenderDisposition::kSkipped:   return "skipped";
    }
    return "unknown";
}

// ------------------------------------------------------ PUT /frame --------

esp_err_t FramePutHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    if (!mgr.initialised()) {
        return SendError(req, "503 Service Unavailable", "unavailable",
                         "dashboard storage is not initialised");
    }

    // Check the declared size before allocating anything, so an oversized
    // Content-Length costs us nothing.
    if (req->content_len != static_cast<int>(dashboard::kFrameBytes)) {
        char detail[96];
        snprintf(detail, sizeof(detail), "expected exactly %u bytes, got %d",
                 static_cast<unsigned>(dashboard::kFrameBytes), req->content_len);
        const char* status = (req->content_len > static_cast<int>(dashboard::kFrameBytes))
                                 ? "413 Payload Too Large" : "400 Bad Request";
        return SendError(req, status, "bad_length", detail);
    }

    char token_buf[kHeaderValueMax];
    char sha_buf[kHeaderValueMax];
    char idem_buf[kHeaderValueMax];
    char epoch_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));
    const char* sha = GetHeader(req, "X-Frame-Sha256", sha_buf, sizeof(sha_buf));
    const char* idem = GetHeader(req, "Idempotency-Key", idem_buf, sizeof(idem_buf));
    const char* epoch_s = GetHeader(req, "X-Frame-Epoch", epoch_buf, sizeof(epoch_buf));
    const uint32_t epoch = epoch_s ? static_cast<uint32_t>(strtoul(epoch_s, nullptr, 10)) : 0u;

    uint8_t* body = static_cast<uint8_t*>(
        heap_caps_malloc(dashboard::kFrameBytes, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        return SendError(req, "503 Service Unavailable", "no_memory",
                         "could not allocate a frame buffer");
    }

    size_t received = 0;
    while (received < dashboard::kFrameBytes) {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(body) + received,
                                     dashboard::kFrameBytes - received);
        if (n <= 0) {
            heap_caps_free(body);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                return SendError(req, "408 Request Timeout", "timeout",
                                 "timed out reading the frame body");
            }
            return SendError(req, "400 Bad Request", "short_body",
                             "connection closed before the frame was complete");
        }
        received += static_cast<size_t>(n);
    }

    const dashboard::PushStatus st =
        mgr.Submit(body, received, token, sha, idem, epoch);
    heap_caps_free(body);

    switch (st.outcome) {
        case dashboard::PushOutcome::kNotProvisioned:
            // Deliberately explicit: the operator needs to know that pairing is
            // missing, and this tells an attacker nothing they can act on.
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        case dashboard::PushOutcome::kUnauthorized:
            return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
        case dashboard::PushOutcome::kLockedOut:
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        case dashboard::PushOutcome::kBusy:
            return SendError(req, "409 Conflict", "busy",
                             "another frame update is already in flight");
        case dashboard::PushOutcome::kBadLength:
            return SendError(req, "400 Bad Request", "bad_length", "frame size rejected");
        case dashboard::PushOutcome::kShaMismatch:
            return SendError(req, "422 Unprocessable Entity", "sha_mismatch",
                             "body does not match X-Frame-Sha256");
        case dashboard::PushOutcome::kStoreFailed:
            return SendError(req, "500 Internal Server Error", "store_failed",
                             "frame could not be persisted and verified");
        case dashboard::PushOutcome::kSuperseded:
            // Unreachable from this route: only SubmitLocal() carries an
            // expected sequence, and a PUT never loses that race by rule. Named
            // rather than defaulted so adding an outcome fails to compile here
            // instead of quietly returning 202 for something new.
            return SendError(req, "409 Conflict", "superseded",
                             "the stored frame moved while this one was prepared");

        case dashboard::PushOutcome::kIdempotentReplay:
        case dashboard::PushOutcome::kDeduped:
        case dashboard::PushOutcome::kAccepted:
            break;
    }

    char buf[320];
    const bool replay = (st.outcome == dashboard::PushOutcome::kIdempotentReplay);
    const bool duplicate = (st.outcome == dashboard::PushOutcome::kDeduped);
    snprintf(buf, sizeof(buf),
             "{\"accepted\":true,\"persisted\":%s,\"deduped\":%s,\"replay\":%s,"
             "\"seq\":%u,\"sha256\":\"%s\",\"render\":\"%s\"}",
             st.persisted ? "true" : "false",
             duplicate ? "true" : "false",
             replay ? "true" : "false",
             static_cast<unsigned>(st.seq), st.sha_hex, RenderName(st.render));

    // 202 for a genuinely new frame, because the render has not happened yet.
    // 200 for dedup/replay, where there is nothing further to wait for.
    return SendJson(req, (st.outcome == dashboard::PushOutcome::kAccepted)
                             ? "202 Accepted" : "200 OK", buf);
}

// ------------------------------------------------------ GET /frame --------

/// Read-back route. Lets the pusher verify that the bytes on the device are the
/// bytes it sent, rather than trusting the acknowledgement it received.
esp_err_t FrameGetHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    if (!mgr.initialised() || !mgr.HasFrame()) {
        return SendError(req, "404 Not Found", "no_frame", "no dashboard frame stored");
    }

    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(dashboard::kFrameBytes, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        return SendError(req, "503 Service Unavailable", "no_memory",
                         "could not allocate a frame buffer");
    }
    if (!mgr.CopyFrame(buf)) {
        heap_caps_free(buf);
        return SendError(req, "500 Internal Server Error", "read_failed",
                         "stored frame failed verification on read");
    }

    const dashboard::DashboardStatus st = mgr.Status();
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Sha256", st.stored_sha_hex);
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buf),
                                          dashboard::kFrameBytes);
    heap_caps_free(buf);
    return err;
}

// ----------------------------------------------------- GET /status --------

/// The capability list buffer this route gives the renderer. Reproduced by
/// tests/host/status_caps_gate.cc, which checks the list still fits it.
constexpr size_t kStatusCapsMax = 256;

/**
 * @brief The status document's own buffer.
 *
 * Worst case is roughly 1.7 kB before autonomy: ~640 for the power block, 256
 * for the capability list, two 64-char digests and the fixed keys. Raised from
 * 2048 when the autonomy block was added: the block is at most 768 bytes and a
 * buffer that had merely "looked big enough" would have started truncating the
 * whole status response instead. The margin is deliberate — snprintf truncates
 * rather than overflows, but a truncated status response is unparseable JSON,
 * which is worse than a larger allocation.
 */
constexpr size_t kStatusBodyMax = 2880;

/**
 * @brief Every buffer the status response is assembled in, in one block.
 *
 * THIS MUST NOT GO BACK ON THE STACK.
 *
 * These four buffers used to be locals of StatusHandler. Together they are
 * 4 544 bytes, and the compiled frame was 4 976 (`entry a1, 0x1370`) inside an
 * httpd task given 6 144. The handler itself survived that; the nested calls
 * underneath its *reply* did not. A single GET /api/v1/dashboard/status on
 * hardware ended in
 *
 *     assert failed: xTaskPriorityDisinherit tasks.c:5157 (pxTCB->uxMutexesHeld)
 *
 * from StatusHandler -> SendJson -> ESP_LOG -> esp_log_impl_unlock ->
 * xQueueGenericSend, then RTC_SW_CPU_RST: the log lock's TCB had been
 * overwritten by the stack that ran past its own limit. The fix is the frame,
 * not a bigger task — every other route on this server lives inside the same
 * 6 144 bytes and has no reason to pay for this one.
 *
 * So the block is heap-allocated, bounded, constant in size, and allocated
 * exactly once per request. tests/host/test_status_capabilities.cc reads this
 * file back and fails if a large array reappears as a local of the handler.
 */
struct StatusScratch {
    char caps[kStatusCapsMax];
    char power_json[power::kPowerJsonMax];
    char autonomy_json[autonomy::kAutonomyJsonMax];
    char body[kStatusBodyMax];
};

/// Frees the scratch block however the handler leaves — including a future
/// early return that has not been written yet.
class StatusScratchGuard {
public:
    explicit StatusScratchGuard(StatusScratch* p) : p_(p) {}
    ~StatusScratchGuard() { if (p_ != nullptr) heap_caps_free(p_); }
    StatusScratchGuard(const StatusScratchGuard&) = delete;
    StatusScratchGuard& operator=(const StatusScratchGuard&) = delete;

private:
    StatusScratch* p_;
};

/**
 * @brief Fill @p scratch.body with the status document.
 *
 * Separated from the handler so the handler holds the pointer and the free, and
 * this function holds no buffer of its own. Everything it writes goes into the
 * block it was handed.
 */
void RenderStatusJson(StatusScratch& scratch) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    const dashboard::DashboardStatus s = mgr.Status();

    // The discovery surface, so it is where the capability list belongs. It
    // stays unauthenticated for the same reason it always was: a client has to
    // be able to find out what it is talking to before it has a token.
    char* const caps = scratch.caps;
    if (devcfg::RenderCapabilitiesJson(VOICE_PTT_ENABLED != 0,
                                       AUTONOMY_COMPILED != 0, caps,
                                       sizeof(scratch.caps)) == 0) {
        snprintf(caps, sizeof(scratch.caps), "[]");
    }

    // The power block, rendered by the portable policy rather than here, so
    // the claim "an uncalibrated battery is reported as null" is checked by a
    // host test reading the actual bytes. See common/power_policy.h.
    char* const power_json = scratch.power_json;
    if (power::RenderPowerJson(Application::GetInstance().PowerSnapshot(),
                               power_json, sizeof(scratch.power_json)) == 0) {
        // Better an explicit null than a truncated object: a tower that sees
        // null knows it learned nothing, where a fragment would fail to parse
        // and take the whole status response down with it.
        snprintf(power_json, sizeof(scratch.power_json), "null");
    }

    // The autonomy block, on the same terms as the power block: rendered by a
    // portable function whose honest nulls are pinned by a host test, and an
    // explicit `null` rather than a fragment when it does not fit.
    //
    // Absent entirely when this build has no profile store, because the
    // capability list is what tells the tower which controls exist and the two
    // must agree: advertising the contract while omitting the block would make
    // the tower render controls for a device that cannot serve them.
    char* const autonomy_json = scratch.autonomy_json;
    if (autonomy::RenderAutonomyJson(Application::GetInstance().AutonomySnapshot(),
                                     autonomy_json,
                                     sizeof(scratch.autonomy_json)) == 0) {
        snprintf(autonomy_json, sizeof(scratch.autonomy_json), "null");
    }

    char* const buf = scratch.body;
    snprintf(buf, sizeof(scratch.body),
             "{\"firmware\":\"%s\",\"api\":%d,\"capabilities\":%s,"
             "\"device\":{\"name\":\"%s\",\"model\":\"%s\",\"hardware\":\"%s\","
             "\"panel\":\"%s\",\"fw\":\"%s\",\"upstream_base\":\"%s\"},"
             "\"config_revision\":%u,\"power\":%s,\"autonomy\":%s,"
             "\"initialised\":%s,\"provisioned\":%s,\"lockdown\":%s,"
             "\"stored\":{\"present\":%s,\"seq\":%u,\"sha256\":\"%s\",\"source_epoch\":%u},"
             "\"displayed\":{\"present\":%s,\"seq\":%u,\"sha256\":\"%s\"},"
             // `failed` and `last_failed_seq` are what turn "the digest under
             // displayed never appeared" from a silence into an answer. A
             // refusal by the painter used to leave no trace at all. Appended
             // to the existing object rather than replacing anything: the
             // tower's zod schema for `refresh` is non-strict, so unknown keys
             // are dropped by an older reader instead of failing the parse.
             //
             // `deferred` (and `last_deferred_seq`) is the benign twin of
             // `failed`: a frame that arrived while the user was on another page
             // is stored and shown later, not dropped. It used to be counted as
             // `failed`, so the tower could not tell a fault from "the user was
             // reading a different page". `failed` now means only genuine
             // faults; existing fields keep their meaning.
             "\"refresh\":{\"state\":\"%s\",\"pending\":%s,\"renders\":%u,"
             "\"skipped\":%u,\"coalesced\":%u,\"failed\":%u,"
             "\"last_failed\":%s,\"last_failed_seq\":%u,"
             "\"deferred\":%u,\"last_deferred\":%s,\"last_deferred_seq\":%u},"
             "\"timing_ms\":{\"read\":%u,\"blit\":%u,\"panel\":%u,\"total\":%u},"
             "\"storage\":{\"write_failures\":%u,\"read_failures\":%u,"
             "\"spiffs_total\":%u,\"spiffs_used\":%u}}",
             // The version of *this* firmware, not the upstream string it was
             // forked from. The upstream number is reported separately and
             // labelled, because it is a fact about provenance and not a fact
             // about what this build does.
             product::kFirmwareVersion, devcfg::kApiLevel, caps,
             product::kName, product::kModel, product::kHardware,
             product::kPanel, product::kFirmwareVersion, product::kUpstreamBase,
             static_cast<unsigned>(
                 devcfg::DeviceConfigService::GetInstance().Revision()),
             power_json,
             autonomy_json,
             mgr.initialised() ? "true" : "false",
             s.provisioned ? "true" : "false",
             s.lockdown ? "true" : "false",
             s.has_stored_frame ? "true" : "false",
             static_cast<unsigned>(s.stored_seq), s.stored_sha_hex,
             static_cast<unsigned>(s.stored_source_epoch),
             s.has_displayed_frame ? "true" : "false",
             static_cast<unsigned>(s.displayed_seq), s.displayed_sha_hex,
             s.rendering ? "rendering" : "idle",
             s.pending ? "true" : "false",
             static_cast<unsigned>(s.render_count),
             static_cast<unsigned>(s.skipped_renders),
             static_cast<unsigned>(s.coalesced_requests),
             static_cast<unsigned>(s.failed_renders),
             s.last_render_failed ? "true" : "false",
             static_cast<unsigned>(s.last_failed_seq),
             static_cast<unsigned>(s.deferred_renders),
             s.last_render_deferred ? "true" : "false",
             static_cast<unsigned>(s.last_deferred_seq),
             static_cast<unsigned>(s.last_read_ms), static_cast<unsigned>(s.last_blit_ms),
             static_cast<unsigned>(s.last_panel_ms), static_cast<unsigned>(s.last_total_ms),
             static_cast<unsigned>(s.storage_write_failures),
             static_cast<unsigned>(s.storage_read_failures),
             static_cast<unsigned>(s.spiffs_total_bytes),
             static_cast<unsigned>(s.spiffs_used_bytes));
}

esp_err_t StatusHandler(httpd_req_t* req) {
    // PSRAM first, like every other large buffer on this server. The fallback
    // is any byte-addressable heap rather than a refusal, because this is the
    // route a tower uses to find out the device exists: answering 503 to a
    // discovery request over a transient 4.5 kB PSRAM shortage would make a
    // healthy device look dead.
    StatusScratch* scratch = static_cast<StatusScratch*>(
        heap_caps_malloc(sizeof(StatusScratch), MALLOC_CAP_SPIRAM));
    if (scratch == nullptr) {
        scratch = static_cast<StatusScratch*>(
            heap_caps_malloc(sizeof(StatusScratch), MALLOC_CAP_8BIT));
    }
    if (scratch == nullptr) {
        // Deliberately no large buffer on this path either: SendError formats
        // into 256 bytes of its own frame, which is what makes "out of memory"
        // an answer the device can still deliver.
        return SendError(req, "503 Service Unavailable", "no_memory",
                         "could not allocate the status response buffer");
    }
    const StatusScratchGuard guard(scratch);

    RenderStatusJson(*scratch);
    // The send is inside the guard's scope on purpose: SendJson logs and can
    // fail, and the block is freed either way.
    return SendJson(req, "200 OK", scratch->body);
}

// ---------------------------------------------------- POST /refresh -------

esp_err_t RefreshHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();

    char token_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));

    // Repainting the panel costs seconds of hardware time and e-paper wear, so
    // it is a mutating operation and is authenticated like one.
    switch (mgr.CheckToken(token)) {
        case dashboard::AuthResult::kNotProvisioned:
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        case dashboard::AuthResult::kBadToken:
            return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
        case dashboard::AuthResult::kLockedOut:
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        case dashboard::AuthResult::kOk:
            break;
    }

    if (!mgr.HasFrame()) {
        return SendError(req, "404 Not Found", "no_frame", "no dashboard frame stored");
    }

    const dashboard::RenderDisposition d = mgr.RequestRedraw(true);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"render\":\"%s\"}", RenderName(d));
    return SendJson(req, (d == dashboard::RenderDisposition::kStarted)
                             ? "202 Accepted" : "200 OK", buf);
}

// ------------------------------------------------------- POST /pair -------

/**
 * Claim the token minted when the operator opened pairing on the device.
 *
 * This route carries no token, and that is the point: it is the bootstrap. It
 * only ever succeeds inside a window that can be opened solely by a physical
 * action on the hardware, and it succeeds at most once per window. Outside a
 * window it is inert, so it is not a remote enrollment path.
 */
esp_err_t PairHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    if (!mgr.initialised()) {
        return SendError(req, "503 Service Unavailable", "unavailable",
                         "dashboard storage is not initialised");
    }

    char token_hex[dashboard::kTokenHexChars + 1] = {};
    if (!mgr.ClaimPairing(token_hex)) {
        const char* detail = "no pairing window is open; "
                             "open Settings > Pair dashboard on the device";
        switch (mgr.PairingState()) {
            case dashboard::PairingWindow::State::kClaimed:
                detail = "this pairing window was already claimed; re-pair on the device";
                break;
            case dashboard::PairingWindow::State::kExpired:
                detail = "the pairing window expired; re-open it on the device";
                break;
            default:
                break;
        }
        return SendError(req, "403 Forbidden", "not_pairing", detail);
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", token_hex);
    const esp_err_t err = SendJson(req, "200 OK", buf);
    // Do not leave the token sitting in this task's stack frame.
    memset(buf, 0, sizeof(buf));
    memset(token_hex, 0, sizeof(token_hex));
    return err;
}

// -------------------------------------------------- POST /voice/hub -------

/**
 * Install the address and token of the terminal hub the device uploads
 * utterances to.
 *
 * This route exists because there is no other way in. The device has three
 * buttons and a panel that takes tens of seconds to redraw; typing a URL and a
 * random token on it is not a user interface, it is a punishment. Without a
 * route the push-to-talk path would be code nobody could ever switch on.
 *
 * It is authenticated with the dashboard token, which is the credential this
 * device already has and which can only be obtained by opening a pairing
 * window with a physical action on the hardware. It is a mutating operation
 * and it is treated as one.
 *
 * The response never echoes the hub token back, and the token is never logged.
 * Configuring a hub does not enable anything by itself: the software mute is
 * separate, defaults to on, and is the only thing that lets the microphone
 * open.
 */
esp_err_t VoiceHubHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();

    char token_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));
    switch (mgr.CheckToken(token)) {
        case dashboard::AuthResult::kNotProvisioned:
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        case dashboard::AuthResult::kBadToken:
            return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
        case dashboard::AuthResult::kLockedOut:
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        case dashboard::AuthResult::kOk:
            break;
    }

    // Small, fixed cap. This body is two short strings; anything larger is a
    // mistake or an attack, and reading it costs memory either way.
    constexpr int kMaxBody = 1024;
    if (req->content_len <= 0 || req->content_len > kMaxBody) {
        return SendError(req, "400 Bad Request", "bad_length",
                         "expected a small JSON object with url and token");
    }
    char body[kMaxBody + 1] = {};
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            return SendError(req, "400 Bad Request", "short_body",
                             "the body ended before its declared length");
        }
        received += n;
    }
    body[received] = '\0';

    std::string raw_url;
    std::string raw_token;
    {
        cJSON* root = cJSON_ParseWithLength(body, static_cast<size_t>(received));
        const bool is_object = root != nullptr && cJSON_IsObject(root);
        if (is_object) {
            const cJSON* url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (cJSON_IsString(url_item) && url_item->valuestring != nullptr) {
                raw_url = url_item->valuestring;
            }
            const cJSON* token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
            if (cJSON_IsString(token_item) && token_item->valuestring != nullptr) {
                raw_token = token_item->valuestring;
            }
        }
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        // Wipe the body as soon as it has been parsed: it held the token, and
        // this buffer is on the HTTP task's stack.
        memset(body, 0, sizeof(body));
        if (!is_object) {
            return SendError(req, "400 Bad Request", "bad_body",
                             "expected a JSON object");
        }
    }

    std::string url;
    std::string hub_token;
    const voice::HubConfigError error =
        voice::ValidateHubPair(raw_url, raw_token, &url, &hub_token);
    if (!raw_token.empty()) {
        memset(&raw_token[0], 0, raw_token.size());
    }
    if (error != voice::HubConfigError::kNone) {
        // The reason is handed back rather than a bare "bad request": the
        // caller is a script on the Mac and the difference between a bad
        // scheme and a missing token is the difference between one edit and
        // half an hour.
        return SendError(req, "400 Bad Request", "bad_config",
                         voice::HubConfigErrorName(error));
    }

    const bool token_set = !hub_token.empty();
    Application::GetInstance().ConfigureVoiceHub(url, hub_token);
    if (!hub_token.empty()) {
        memset(&hub_token[0], 0, hub_token.size());
    }

    // This route changes a setting that GET /api/v1/config reports, so it moves
    // the same revision every other change moves. A hub write that left the
    // revision alone would let a config PATCH that raced it win a
    // compare-and-swap it should have lost.
    auto& service = devcfg::DeviceConfigService::GetInstance();
    service.NoteHub(url, token_set);

    // Only a quote and a backslash can expand here: the URL has already passed
    // ValidateHubUrl, which refuses control characters. Two bytes per character
    // is therefore the real worst case, and this runs on the httpd task's
    // stack, which is not the place to reserve six.
    char escaped[voice::kMaxHubUrlChars * 2 + 1];
    if (devcfg::JsonEscape(url, escaped, sizeof(escaped)) == 0) escaped[0] = '\0';

    // 177 bytes of fixed text plus an escaped URL that can reach 400. Sized
    // from that arithmetic rather than from a round number, because a
    // truncated JSON object is a parse error at the other end.
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"accepted\":true,\"configured\":%s,\"url\":\"%s\",\"token_set\":%s,"
             "\"revision\":%u,"
             "\"note\":\"the software mute is separate "
             "and defaults to on; no microphone opens until it is turned off\"}",
             url.empty() ? "false" : "true", escaped,
             token_set ? "true" : "false",
             static_cast<unsigned>(service.Revision()));
    return SendJson(req, "200 OK", buf);
}

// ----------------------------------------------------------- autonomy ------
//
// WHERE THE PROFILE STORE AND THE COMPOSITOR LIVE, AND WHY THEY MOVED
// ------------------------------------------------------------------
// They used to be here, as file-scope globals, because the routes below were
// the only things on the device that could reach autonomy at all: no wake
// cycle, no timer, no fetch, so no second caller for Application to arbitrate
// between. That comment said the ownership would move when a caller that was
// not a route arrived.
//
// It has. The wake cycle composes from the same profile, into the same 120 KB
// canvas, while the HTTP task may be applying a PUT to it — so the store, the
// buffers and the compose mutex are Application's, and these handlers ask for
// them. See Application::InitializeAutonomy and Application::ComposeLocked.

/**
 * The two profile routes.
 *
 * Deliberately thin. Every decision — token, lockout, replay, CAS, validation,
 * read-back — lives in common/autonomy_service.cc, which is portable and
 * host-tested; what is left here is header parsing and the status-code table.
 * That split is why the interesting failures (a write that reports success and
 * stores nothing, a revision that goes backwards) have tests at all.
 *
 * The status codes are chosen so a tower can act on them without reading the
 * body: 409 means "your revision is stale, recompile", 413 means "shorten the
 * document", 422 means "the bytes did not survive the wire", 400 means "this
 * document is wrong and here is the field".
 */
esp_err_t AutonomyProfilePutHandler(httpd_req_t* req) {
    auto* service = Application::GetInstance().autonomy_service();
    if (service == nullptr) {
        return SendError(req, "404 Not Found", "autonomy_unsupported",
                         "this build has no autonomy profile store");
    }

    // The declared length is checked before anything is allocated, so an
    // oversized Content-Length costs a comparison rather than a buffer.
    if (req->content_len <= 0 ||
        req->content_len > static_cast<int>(autonomy::kProfileMaxBytes)) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "an autonomy profile must be 1..%u bytes; this one declares %d",
                 static_cast<unsigned>(autonomy::kProfileMaxBytes),
                 req->content_len);
        const char* status = req->content_len > 0 ? "413 Payload Too Large"
                                                  : "400 Bad Request";
        return SendError(req, status, "profile_too_large", detail);
    }

    char token_buf[kHeaderValueMax];
    char sha_buf[kHeaderValueMax];
    char idem_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));
    const char* sha = GetHeader(req, "X-Profile-Sha256", sha_buf, sizeof(sha_buf));
    const char* idem = GetHeader(req, "Idempotency-Key", idem_buf, sizeof(idem_buf));

    const size_t want = static_cast<size_t>(req->content_len);
    char* body = static_cast<char*>(heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        return SendError(req, "503 Service Unavailable", "no_memory",
                         "could not allocate a profile buffer");
    }

    size_t received = 0;
    while (received < want) {
        const int n = httpd_req_recv(req, body + received, want - received);
        if (n <= 0) {
            heap_caps_free(body);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                return SendError(req, "408 Request Timeout", "timeout",
                                 "timed out reading the profile body");
            }
            return SendError(req, "400 Bad Request", "short_body",
                             "connection closed before the profile was complete");
        }
        received += static_cast<size_t>(n);
    }
    body[received] = '\0';

    const autonomy::PutResult result =
        service->Put(body, received, token, sha, idem,
                     esp_timer_get_time() / 1000, CurrentEpochOrZero());
    heap_caps_free(body);

    switch (result.outcome) {
        case autonomy::PutOutcome::kNotProvisioned:
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        case autonomy::PutOutcome::kUnauthorized:
            return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
        case autonomy::PutOutcome::kLockedOut:
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        case autonomy::PutOutcome::kTooLarge:
            return SendError(req, "413 Payload Too Large", "profile_too_large",
                             "the profile is larger than this device accepts");
        case autonomy::PutOutcome::kShaMismatch:
            return SendError(req, "422 Unprocessable Entity", "sha_mismatch",
                             "body does not match X-Profile-Sha256");
        case autonomy::PutOutcome::kStoreFailed:
            return SendError(req, "500 Internal Server Error", "store_failed",
                             "profile could not be persisted and verified");
        case autonomy::PutOutcome::kRevisionConflict: {
            // The field the tower needs is the revision we are holding: it
            // recompiles past it rather than guessing.
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "this device holds revision %d; send a higher one",
                     static_cast<int>(result.revision));
            return SendError(req, "409 Conflict", "revision_mismatch", detail);
        }
        case autonomy::PutOutcome::kInvalid: {
            // Named field, named reason. The same discipline config.v2 uses,
            // and the reason the tower can show the owner which control to fix.
            char detail[192];
            snprintf(detail, sizeof(detail), "%s at %s",
                     result.error.code != nullptr ? result.error.code : "invalid",
                     result.error.field[0] != '\0' ? result.error.field : "(root)");
            return SendError(req, "400 Bad Request", "invalid_profile", detail);
        }
        case autonomy::PutOutcome::kAccepted:
        case autonomy::PutOutcome::kReplay:
            break;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"accepted\":true,\"persisted\":%s,\"replay\":%s,\"revision\":%d,"
             "\"sha256\":\"%s\"}",
             result.persisted ? "true" : "false",
             result.replay ? "true" : "false",
             static_cast<int>(result.revision), result.sha256);
    return SendJson(req, "200 OK", buf);
}

/**
 * The stored bytes, verbatim.
 *
 * Verbatim is the contract: the tower compares what it pushed against what
 * comes back, byte for byte. Re-serialising here would turn that comparison
 * into a test of this device's JSON writer instead of a test of what it holds.
 */
esp_err_t AutonomyProfileGetHandler(httpd_req_t* req) {
    auto* service = Application::GetInstance().autonomy_service();
    if (service == nullptr) {
        return SendError(req, "404 Not Found", "autonomy_unsupported",
                         "this build has no autonomy profile store");
    }

    char token_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));
    autonomy::PutOutcome why = autonomy::PutOutcome::kUnauthorized;
    if (!service->Authorise(token, esp_timer_get_time() / 1000, &why)) {
        if (why == autonomy::PutOutcome::kNotProvisioned) {
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        }
        if (why == autonomy::PutOutcome::kLockedOut) {
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        }
        return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
    }

    if (!service->has_profile()) {
        // Not an error. A device with no profile is the default state, and the
        // tower's job is to notice and offer to push one.
        return SendError(req, "404 Not Found", "no_profile",
                         "this device is not holding an autonomy profile");
    }

    uint8_t* out = static_cast<uint8_t*>(
        heap_caps_malloc(autonomy::kProfileMaxBytes, MALLOC_CAP_SPIRAM));
    if (out == nullptr) {
        return SendError(req, "503 Service Unavailable", "no_memory",
                         "could not allocate a profile buffer");
    }
    const autonomy::GetResult got = service->Get(out, autonomy::kProfileMaxBytes);
    if (!got.present) {
        heap_caps_free(out);
        // Validated at Load() and unreadable now: the flash decayed between
        // the two, which is a real thing and is said rather than smoothed over.
        return SendError(req, "500 Internal Server Error", "store_failed",
                         "the stored profile did not read back");
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Profile-Sha256", got.sha256);
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(out),
                                          got.bytes);
    heap_caps_free(out);
    return err;
}

/**
 * Compose one panel locally, now, and store it.
 *
 * THIS ROUTE IS PULL, AND IT REMAINS PULL NOW THAT THE WAKE CYCLE EXISTS.
 * ---------------------------------------------------------------------------
 * The wake cycle composes on its own schedule; this composes because somebody
 * holding the device's token asked, now, and wants to be told what happened to
 * the frame. That is what makes the compositor exercisable on a bench without
 * waiting out a wake interval, and it is why the route survived the increment
 * that gave the device a scheduler.
 *
 * Both go through Application::ComposeLocked, so what this route reports is
 * what the cycle would have drawn — and neither can run while the other is.
 * What this route does *not* do is fetch: it draws from whatever forecast the
 * cache already holds, and says `had_forecast` so the caller can tell the
 * difference between a fresh panel and one drawn with the weather module
 * unavailable.
 *
 * Three gates, all of which must be open, and each of which refuses by name so
 * an operator learns which one is shut:
 *   - the build gate (AUTONOMY_COMPILED) — 404 autonomy_unsupported;
 *   - the runtime kill-switch (`autonomy.enabled`) — 409 autonomy_disabled;
 *   - a stored, validated profile — 404 no_profile.
 *
 * The kill-switch is not persisted, so a power cycle closes the second gate by
 * itself. A bench enable does not outlive the bench.
 */
esp_err_t AutonomyRenderHandler(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    char token_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));

    // The frame route's credential and the frame route's lockout, asked for
    // first: an unauthenticated caller must not be able to learn from the reply
    // whether this build even has autonomy, nor to spend the device's PSRAM and
    // seconds of CPU on a composition.
    switch (mgr.CheckToken(token)) {
        case dashboard::AuthResult::kOk:
            break;
        case dashboard::AuthResult::kNotProvisioned:
            return SendError(req, "503 Service Unavailable", "not_provisioned",
                             "no dashboard token installed; pair the device locally first");
        case dashboard::AuthResult::kLockedOut:
            return SendError(req, "429 Too Many Requests", "locked_out",
                             "too many failed authentications; retry later");
        case dashboard::AuthResult::kBadToken:
            return SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
    }

    const Application::LocalComposeReport report =
        Application::GetInstance().ComposeLocalFrameNow();

    if (report.refusal != nullptr) {
        if (strcmp(report.refusal, "autonomy_unsupported") == 0) {
            return SendError(req, "404 Not Found", "autonomy_unsupported",
                             "this build has no autonomy profile store");
        }
        if (strcmp(report.refusal, "autonomy_disabled") == 0) {
            return SendError(req, "409 Conflict", "autonomy_disabled",
                             "set autonomy.enabled through the config API first");
        }
        if (strcmp(report.refusal, "no_profile") == 0) {
            return SendError(req, "404 Not Found", "no_profile",
                             "this device is not holding an autonomy profile");
        }
        if (strcmp(report.refusal, "compose_busy") == 0) {
            return SendError(req, "409 Conflict", "busy",
                             "another local composition is already in flight");
        }
        if (strcmp(report.refusal, "no_compositor") == 0) {
            return SendError(req, "503 Service Unavailable", "no_memory",
                             "this device has no PSRAM for the compositor");
        }
        return SendError(req, "500 Internal Server Error", "compose_failed",
                         "the compositor refused its own buffers");
    }

    // Past this point the composition happened; what is reported is what the
    // store did with it. A superseded frame is not an error: it is the
    // arbitration rule working, and the tower's frame is the one going up.
    const char* outcome = "unknown";
    const char* status = "200 OK";
    switch (report.push.outcome) {
        case dashboard::PushOutcome::kAccepted:
            outcome = "accepted";
            status = "202 Accepted";
            break;
        case dashboard::PushOutcome::kDeduped:
            outcome = "deduped";
            break;
        case dashboard::PushOutcome::kSuperseded:
            outcome = "superseded";
            status = "409 Conflict";
            break;
        case dashboard::PushOutcome::kBusy:
            outcome = "busy";
            status = "409 Conflict";
            break;
        case dashboard::PushOutcome::kBadLength:
        case dashboard::PushOutcome::kShaMismatch:
        case dashboard::PushOutcome::kUnauthorized:
        case dashboard::PushOutcome::kNotProvisioned:
        case dashboard::PushOutcome::kLockedOut:
        case dashboard::PushOutcome::kIdempotentReplay:
        case dashboard::PushOutcome::kStoreFailed:
            outcome = "store_failed";
            status = "500 Internal Server Error";
            break;
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"composed\":true,\"outcome\":\"%s\",\"persisted\":%s,"
             "\"modules_drawn\":%u,\"empty\":%s,\"expected_seq\":%u,"
             "\"seq\":%u,\"sha256\":\"%s\",\"render\":\"%s\","
             // Two separate facts, and they used to be one hardcoded `true`
             // because this build could not fetch a forecast at all. It can
             // now: `had_forecast` is whether the cache had one to draw from,
             // and `degraded` follows it — a panel drawn with the weather
             // module unavailable says so rather than looking complete.
             "\"had_forecast\":%s,\"degraded\":%s}",
             outcome, report.push.persisted ? "true" : "false",
             static_cast<unsigned>(report.modules_drawn),
             report.empty_panel ? "true" : "false",
             static_cast<unsigned>(report.expected_seq),
             static_cast<unsigned>(report.push.seq), report.push.sha_hex,
             RenderName(report.push.render),
             report.had_forecast ? "true" : "false",
             report.degraded ? "true" : "false");
    return SendJson(req, status, buf);
}

}  // namespace

// ------------------------------------------------------------ registration --

esp_err_t RegisterDashboardApi(httpd_handle_t server) {
    if (server == nullptr) return ESP_ERR_INVALID_ARG;

    // The profile store is brought up by Application::InitializeAutonomy(),
    // during boot and before this server exists, because the wake cycle needs
    // it whether or not anybody ever registers a route. Nothing to do here.

    const httpd_uri_t routes[] = {
        {.uri = "/api/v1/dashboard/frame",   .method = HTTP_PUT,  .handler = FramePutHandler, .user_ctx = nullptr},
        {.uri = "/api/v1/dashboard/frame",   .method = HTTP_GET,  .handler = FrameGetHandler, .user_ctx = nullptr},
        {.uri = "/api/v1/dashboard/status",  .method = HTTP_GET,  .handler = StatusHandler,   .user_ctx = nullptr},
        {.uri = "/api/v1/dashboard/refresh", .method = HTTP_POST, .handler = RefreshHandler,  .user_ctx = nullptr},
        {.uri = "/api/v1/dashboard/pair",    .method = HTTP_POST, .handler = PairHandler,     .user_ctx = nullptr},
        // Not under /dashboard/: it configures the voice path, not the panel.
        // It shares the dashboard token because that is the credential this
        // device has, and it is bootstrapped by a physical pairing action.
        {.uri = "/api/v1/voice/hub",         .method = HTTP_POST, .handler = VoiceHubHandler, .user_ctx = nullptr},
        // The autonomy profile, and the one action that draws from it. Three
        // routes, the same token and the same failure lockout as everything
        // else on this server. These are the only routes this feature adds, and
        // all three answer 404 autonomy_unsupported on a build with
        // CONFIG_AUTONOMY_ENABLED off.
        {.uri = "/api/v1/autonomy/profile",  .method = HTTP_PUT,  .handler = AutonomyProfilePutHandler, .user_ctx = nullptr},
        {.uri = "/api/v1/autonomy/profile",  .method = HTTP_GET,  .handler = AutonomyProfileGetHandler, .user_ctx = nullptr},
        {.uri = "/api/v1/autonomy/render",   .method = HTTP_POST, .handler = AutonomyRenderHandler,     .user_ctx = nullptr},
    };

    for (const auto& r : routes) {
        const esp_err_t err = httpd_register_uri_handler(server, &r);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "failed to register %s: %s", r.uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(kTag, "device API v1 registered (9 routes: 5 dashboard, 1 voice hub, "
                   "3 autonomy)");
    // The v2 config and action routes live in config_api.cc and share this
    // server, this token and this failure lockout.
    return RegisterConfigApi(server);
}

bool LegacyWriteBlocked(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    if (!mgr.initialised() || !mgr.LockdownEnabled()) {
        return false;
    }
    SendError(req, "403 Forbidden", "lockdown",
              "legacy unauthenticated write routes are disabled in dashboard mode; "
              "use /api/v1/dashboard/ or turn lockdown off in device settings");
    return true;
}

}  // namespace rawdraw
