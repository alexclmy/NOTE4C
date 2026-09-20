/**
 * @file voice_uploader.cc
 * @brief Implementation of the device-side utterance upload.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "voice_uploader.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

#include <cJSON.h>

#include "settings.h"

#define TAG "VoiceUploader"

namespace voice {

namespace {

constexpr const char* kNamespace = "voice";
constexpr const char* kUrlKey = "hub_url";
constexpr const char* kTokenKey = "hub_token";

/// The hub answers small JSON. Anything larger is not something to keep
/// reading into memory on a device with this much internal RAM.
constexpr size_t kMaxResponseBytes = 8 * 1024;

uint64_t NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

/// Trim a trailing slash so base + path never produces a double one.
std::string NormaliseBase(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

}  // namespace

std::string HubCredentials::safe_label() const {
    if (base_url.empty()) {
        return "(no hub configured)";
    }
    // The base URL has no credentials in it by construction: the token lives
    // in its own NVS key and goes in a header. Returning it whole is still
    // written as a deliberate choice rather than an accident, because this is
    // the string that ends up in the log.
    return base_url;
}

HubCredentials LoadHubCredentials() {
    HubCredentials creds;
    Settings nvs(kNamespace, false);
    creds.base_url = NormaliseBase(nvs.GetString(kUrlKey, ""));
    creds.token = nvs.GetString(kTokenKey, "");
    return creds;
}

void SaveHubCredentials(const HubCredentials& credentials) {
    Settings nvs(kNamespace, true);
    nvs.SetString(kUrlKey, NormaliseBase(credentials.base_url));
    nvs.SetString(kTokenKey, credentials.token);
    // The token is not logged, not even its length: a length is a hint.
    ESP_LOGI(TAG, "Hub credentials updated for %s",
             credentials.base_url.empty() ? "(cleared)" : "the configured hub");
}

VoiceUploader::~VoiceUploader() {
    Stop();
}

bool VoiceUploader::Start(ResultCallback on_result) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    on_result_ = std::move(on_result);
    ReloadCredentials();

    TransportHooks hooks;
    // The send hook does not send. It records that an attempt is owed, and the
    // task loop performs it, so the blocking HTTP call never happens inside
    // Begin() or Tick() where it would stall the very clock that is supposed
    // to time it out.
    hooks.send = [this](const Identity& identity, uint8_t attempt) {
        attempt_pending_ = true;
        ESP_LOGI(TAG, "Utterance %s: attempt %u to %s",
                 identity.request_id, static_cast<unsigned>(attempt),
                 credentials_.safe_label().c_str());
    };
    hooks.abort = []() {
        // esp_http_client_perform cannot be interrupted from another task, and
        // this hook is called from the uploader task itself, between attempts.
        // There is nothing to abort at this point, and pretending otherwise
        // would be worse than saying so.
        ESP_LOGW(TAG, "Attempt abandoned");
    };
    hooks.finished = nullptr;
    transport_.SetHooks(std::move(hooks));
    transport_.SetConfig(TransportConfig{});

    queue_ = xQueueCreate(1, sizeof(Job*));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create the upload queue");
        return false;
    }
    running_.store(true, std::memory_order_release);
    // 6 KB: esp_http_client plus a cJSON parse of a few kilobytes. Priority 4,
    // below the audio input task, because a late upload is better than a
    // dropped microphone frame.
    if (xTaskCreate(TaskEntry, "voice_upload", 6 * 1024, this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the upload task");
        running_.store(false, std::memory_order_release);
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Voice uploader ready, hub %s",
             configured_.load(std::memory_order_acquire) ? "configured" : "NOT configured");
    return true;
}

void VoiceUploader::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    cancel_requested_.store(true, std::memory_order_release);
    if (queue_ != nullptr) {
        Job* wake = nullptr;
        xQueueSend(queue_, &wake, 0);
    }
}

void VoiceUploader::ReloadCredentials() {
    if (busy_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Credential reload ignored: an upload is in flight");
        return;
    }
    credentials_ = LoadHubCredentials();
    configured_.store(credentials_.configured(), std::memory_order_release);
    ESP_LOGI(TAG, "Hub configuration: %s",
             credentials_.configured() ? credentials_.safe_label().c_str()
                                       : "missing url or token");
}

bool VoiceUploader::Submit(std::vector<uint8_t>&& body, const std::string& mime) {
    if (!running_.load(std::memory_order_acquire) || queue_ == nullptr) {
        return false;
    }
    if (!configured_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Refusing to upload: no hub url and token configured");
        return false;
    }
    if (body.empty()) {
        return false;
    }
    if (busy_.exchange(true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Refusing to upload: one is already in flight");
        return false;
    }
    auto* job = new Job{std::move(body), mime};
    cancel_requested_.store(false, std::memory_order_release);
    if (xQueueSend(queue_, &job, 0) != pdTRUE) {
        delete job;
        busy_.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "Upload queue rejected the job");
        return false;
    }
    return true;
}

void VoiceUploader::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

void VoiceUploader::TaskEntry(void* arg) {
    static_cast<VoiceUploader*>(arg)->Run();
    vTaskDelete(nullptr);
}

void VoiceUploader::Run() {
    while (running_.load(std::memory_order_acquire)) {
        Job* job = nullptr;
        if (xQueueReceive(queue_, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job == nullptr) {
            continue;  // the wake-up from Stop()
        }
        std::unique_ptr<Job> owned(job);
        if (running_.load(std::memory_order_acquire)) {
            RunOneJob(*owned);
        }
        busy_.store(false, std::memory_order_release);
    }
    task_ = nullptr;
    ESP_LOGW(TAG, "Voice upload task stopped");
}

void VoiceUploader::MintIdentity(Identity* out) {
    // A version-4 UUID from the hardware RNG. The hub refuses anything that is
    // not the canonical 8-4-4-4-12 form (hub/CONTRACTS.md section 0), so this
    // is formatted rather than assembled by hand.
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    std::snprintf(out->request_id, sizeof(out->request_id),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    // The idempotency key identifies the utterance rather than the delivery.
    // Deriving it from the request id is fine here because there is exactly
    // one delivery per utterance: retries reuse both.
    std::snprintf(out->idempotency_key, sizeof(out->idempotency_key),
                  "utterance-%s", out->request_id);
}

void VoiceUploader::RunOneJob(Job& job) {
    Identity identity{};
    MintIdentity(&identity);

    UploadResult result;
    std::string response;
    int last_status = 0;

    transport_.Release();
    transport_.Begin(identity, NowMs());

    while (transport_.busy()) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            transport_.Cancel(NowMs());
            break;
        }
        if (attempt_pending_) {
            attempt_pending_ = false;
            response.clear();
            last_status = PerformAttempt(identity, job, &response);
            transport_.OnAttemptResult(ClassifyHttpStatus(last_status), NowMs());
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
        transport_.Tick(NowMs());
    }

    result.last_status = last_status;
    result.failure = transport_.failure();
    if (transport_.state() == TransportState::kSucceeded) {
        ParseAnswer(response, &result);
        result.ok = true;
    }
    ESP_LOGI(TAG, "Utterance %s finished: %s after %u attempt(s), status %d",
             identity.request_id, TransportStateName(transport_.state()),
             static_cast<unsigned>(transport_.attempts_made()), last_status);
    if (!result.ok) {
        ESP_LOGW(TAG, "Utterance %s not delivered: %s", identity.request_id,
                 FailureKindName(transport_.failure()));
    }
    transport_.Release();

    if (on_result_ && !cancel_requested_.load(std::memory_order_acquire)) {
        on_result_(result);
    } else if (on_result_) {
        // The user cancelled. The upload may still have reached the hub, and
        // the hub has no undo, so this is reported as what it is rather than
        // being silently dropped: the answer is discarded, not recalled.
        ESP_LOGI(TAG, "Utterance %s: answer discarded, the user cancelled",
                 identity.request_id);
    }
}

int VoiceUploader::PerformAttempt(const Identity& identity, const Job& job,
                                  std::string* response_out) {
    const std::string url = credentials_.base_url + "/v1/voice/utterance";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    // The transport's attempt timeout is the budget; the client enforces it,
    // because there is no way to interrupt this call once it has started.
    config.timeout_ms = static_cast<int>(transport_.config().attempt_timeout_ms);
    config.disable_auto_redirect = true;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = nullptr;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create the HTTP client");
        return 0;
    }

    const std::string authorization = "Bearer " + credentials_.token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "X-Request-Id", identity.request_id);
    esp_http_client_set_header(client, "X-Idempotency-Key", identity.idempotency_key);
    esp_http_client_set_header(client, "Content-Type", job.mime.c_str());
    esp_http_client_set_header(client, "Connection", "close");

    int status = 0;
    esp_err_t err = esp_http_client_open(client, static_cast<int>(job.body.size()));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Utterance %s: connect failed (%s)", identity.request_id,
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return 0;
    }

    const int written = esp_http_client_write(
        client, reinterpret_cast<const char*>(job.body.data()), job.body.size());
    if (written < 0 || static_cast<size_t>(written) != job.body.size()) {
        ESP_LOGW(TAG, "Utterance %s: sent %d of %u bytes", identity.request_id,
                 written, static_cast<unsigned>(job.body.size()));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return 0;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    // Read the body whatever the status, and bound it. An unread body on a
    // connection that is about to be reused is the bug hub/CONTRACTS.md
    // section 0 describes from the other side.
    if (content_length != 0) {
        char chunk[512];
        size_t total = 0;
        while (total < kMaxResponseBytes) {
            const int read = esp_http_client_read(client, chunk, sizeof(chunk));
            if (read <= 0) {
                break;
            }
            response_out->append(chunk, static_cast<size_t>(read));
            total += static_cast<size_t>(read);
        }
        if (total >= kMaxResponseBytes) {
            ESP_LOGW(TAG, "Utterance %s: response truncated at %u bytes",
                     identity.request_id, static_cast<unsigned>(kMaxResponseBytes));
        }
    }

    ESP_LOGI(TAG, "Utterance %s: %u bytes sent, status %d, %u bytes back",
             identity.request_id, static_cast<unsigned>(job.body.size()), status,
             static_cast<unsigned>(response_out->size()));

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

void VoiceUploader::ParseAnswer(const std::string& body, UploadResult* result) {
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr || !cJSON_IsObject(root)) {
        // A 200 with a body nobody can read is not a success worth reporting
        // as one: the caller would tell the user it was answered.
        ESP_LOGW(TAG, "Response was not a JSON object");
        result->ok = false;
        result->failure = FailureKind::kPermanentRejection;
        if (root != nullptr) cJSON_Delete(root);
        return;
    }

    const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && state->valuestring != nullptr) {
        result->state = state->valuestring;
    }
    const cJSON* source = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (cJSON_IsString(source) && source->valuestring != nullptr) {
        result->source = source->valuestring;
    }
    const cJSON* duplicate = cJSON_GetObjectItemCaseSensitive(root, "duplicate");
    result->duplicate = cJSON_IsTrue(duplicate);

    const cJSON* response = cJSON_GetObjectItemCaseSensitive(root, "response");
    if (cJSON_IsObject(response)) {
        const cJSON* reply = cJSON_GetObjectItemCaseSensitive(response, "reply");
        if (cJSON_IsString(reply) && reply->valuestring != nullptr) {
            // The length, and only the length. The reply is what the household
            // asked about and what the assistant said back; it does not go in
            // a log and it is not kept here.
            result->reply_chars = std::strlen(reply->valuestring);
        }
    }

    // The v1 wire carries no audio (hub/CONTRACTS.md section 10). The field is
    // read rather than assumed absent, so the day it appears the device
    // notices instead of quietly ignoring it.
    const cJSON* audio = cJSON_GetObjectItemCaseSensitive(root, "response_audio");
    result->has_playable_audio = cJSON_IsObject(audio);

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Hub state=%s source=%s duplicate=%s reply_chars=%u audio=%s",
             result->state.empty() ? "(none)" : result->state.c_str(),
             result->source.empty() ? "(none)" : result->source.c_str(),
             result->duplicate ? "yes" : "no",
             static_cast<unsigned>(result->reply_chars),
             result->has_playable_audio ? "yes" : "no");
    if (result->source == "stub") {
        // Loud on purpose. A stub answer must never be presented as a real one.
        ESP_LOGW(TAG, "The hub answered with a stub adapter: nothing was "
                      "transcribed and nothing was asked");
    }
}

}  // namespace voice
