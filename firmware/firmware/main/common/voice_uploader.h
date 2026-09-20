/**
 * @file voice_uploader.h
 * @brief Device side of POST /v1/voice/utterance. ESP-IDF glue.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The protocol rules live in main/common/voice_transport.h and are host
 * tested. This file is the part that cannot be: a task, an `esp_http_client`,
 * NVS, and the parsing of one JSON body. It owns a `VoiceTransport` and does
 * what it is told.
 *
 * Where the credentials live
 * --------------------------
 * NVS namespace `voice`, keys `hub_url` and `hub_token`. Nothing else reads
 * them and nothing ever logs them: the log lines here carry the host and port,
 * the request id, the byte count and the status, and never the token, never
 * the body, never the transcript and never the reply.
 *
 * Without both keys the uploader reports itself unconfigured, `AudioFsm`'s
 * transport gate stays shut, and the microphone is never opened. That is
 * deliberate: recording with nowhere to send is not a feature.
 *
 * The trust this does and does not have
 * -------------------------------------
 * Plain HTTP on the local network, with a bearer token. Anyone already on the
 * LAN can read an utterance in flight. That is the same trust model the
 * dashboard frame upload already uses (docs/PROVISIONING.md) and it is stated
 * here rather than implied: this is not end-to-end encrypted, and putting a
 * microphone behind it is a decision about the household network.
 *
 * `https://` is accepted by the URL parser and will use the ESP-IDF bundle, but
 * the hub does not serve TLS and no certificate has been pinned, so nothing in
 * this project has exercised that path.
 *
 * What cancellation can actually do
 * ---------------------------------
 * `esp_http_client_perform` is a blocking call on this task and there is no
 * safe way to interrupt it from another one. A cancel is therefore observed
 * between attempts, and in the worst case one attempt-timeout later. The user
 * interface does not wait for that: `AudioFsm` returns to Idle immediately and
 * the answer, if one arrives, is dropped. What cancelling does not do is recall
 * an utterance the hub already has.
 */

#ifndef COMMON_VOICE_UPLOADER_H
#define COMMON_VOICE_UPLOADER_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "voice_transport.h"

namespace voice {

/// Where the hub is and how to authenticate to it.
struct HubCredentials {
    std::string base_url;  ///< for example http://<host-ip>:8653
    std::string token;

    bool configured() const { return !base_url.empty() && !token.empty(); }
    /// Host and port only, safe to log. Never includes the token.
    std::string safe_label() const;
};

/// Read the credentials from NVS namespace `voice`. Never logs them.
HubCredentials LoadHubCredentials();
/// Write them. Passing empty strings clears the configuration.
void SaveHubCredentials(const HubCredentials& credentials);

/// What one completed upload produced, as far as the device is concerned.
struct UploadResult {
    bool ok = false;
    /// The `state` field of the response, for example `answered`.
    std::string state;
    /// The `source` field. `stub` means no real transcription happened, and
    /// the device must not present the answer as if it did.
    std::string source;
    bool duplicate = false;
    /// Length of the reply text. The reply itself is never stored or logged.
    size_t reply_chars = 0;
    /// Whether the response carried audio the device could play. The v1 wire
    /// never does (hub/CONTRACTS.md section 10); the field exists so the day
    /// it might, the caller is already asking rather than assuming.
    bool has_playable_audio = false;
    FailureKind failure = FailureKind::kNone;
    int last_status = 0;
};

/**
 * @brief Uploads one utterance at a time on a task of its own.
 *
 * Submit() copies nothing: it takes the body by move, because the body is up
 * to 192 KiB and the caller has no use for it afterwards.
 */
class VoiceUploader {
public:
    VoiceUploader() = default;
    ~VoiceUploader();

    VoiceUploader(const VoiceUploader&) = delete;
    VoiceUploader& operator=(const VoiceUploader&) = delete;

    /// Called on the uploader task when an upload finishes, however it finished.
    using ResultCallback = std::function<void(const UploadResult&)>;

    bool Start(ResultCallback on_result);
    void Stop();

    /// Re-read NVS. Safe to call while idle; ignored while an upload is running.
    void ReloadCredentials();
    bool configured() const { return configured_.load(std::memory_order_acquire); }

    /**
     * @brief Queue one utterance.
     *
     * @return false when an upload is already in flight, when the uploader is
     *         not configured, or when the body is empty. One at a time is the
     *         whole queue policy; the state machine above cannot produce a
     *         second utterance while it is waiting for this one.
     */
    bool Submit(std::vector<uint8_t>&& body, const std::string& mime);

    /// Abandon the upload in flight. See the file comment for what that means.
    void Cancel();

    bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    struct Job {
        std::vector<uint8_t> body;
        std::string mime;
    };

    static void TaskEntry(void* arg);
    void Run();
    void RunOneJob(Job& job);
    /// Perform one POST. Returns the HTTP status, or 0 if there was no answer.
    int PerformAttempt(const Identity& identity, const Job& job,
                       std::string* response_out);
    /// Fill @p result from a 200 body. Never logs or stores the reply text.
    void ParseAnswer(const std::string& body, UploadResult* result);
    void MintIdentity(Identity* out);

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    ResultCallback on_result_;
    VoiceTransport transport_;
    HubCredentials credentials_;
    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> configured_{false};
    std::atomic<bool> cancel_requested_{false};
    /// Set by the transport's send hook, consumed by the task loop, so the
    /// blocking POST happens on the task rather than inside Tick().
    bool attempt_pending_ = false;
};

}  // namespace voice

#endif  // COMMON_VOICE_UPLOADER_H
