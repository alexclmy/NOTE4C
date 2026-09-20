/**
 * @file voice_capture.cc
 * @brief Implementation of the capture drain task.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "voice_capture.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "audio_service.h"

#define TAG "VoiceCapture"

namespace voice {

namespace {

/// Matches OPUS_FRAME_DURATION_MS and the 16 kHz mono the codec is opened with
/// (main/boards/zectrix-s3-epaper-4.2/config.h). The header records these so a
/// decoder does not have to guess, and so a change here is visible on the wire.
constexpr uint16_t kFrameDurationMs = OPUS_FRAME_DURATION_MS;
constexpr uint32_t kSampleRate = 16000;

uint64_t NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace

VoiceCapture::~VoiceCapture() {
    Stop();
}

bool VoiceCapture::Start(AudioService* service, DoneCallback on_done) {
    if (service == nullptr) {
        ESP_LOGE(TAG, "No audio service; capture is unavailable");
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    service_ = service;
    on_done_ = std::move(on_done);
    buffer_.Configure(kSampleRate, kFrameDurationMs, 1, Limits{});
    running_.store(true, std::memory_order_release);
    // Priority 6: above the uploader, below the audio input task, because the
    // point of this task is to keep the send queue from filling up.
    if (xTaskCreate(TaskEntry, "voice_capture", 4 * 1024, this, 6, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the capture task");
        running_.store(false, std::memory_order_release);
        return false;
    }
    ESP_LOGI(TAG, "Voice capture ready (the microphone on this board has never been heard)");
    return true;
}

void VoiceCapture::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    recording_.store(false, std::memory_order_release);
    phase_.store(Phase::kIdle, std::memory_order_release);
}

void VoiceCapture::BeginUtterance() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    // Anything the encoder produced before this moment belongs to a previous
    // press, or to nothing at all. It must not become the head of this
    // utterance.
    service_->ClearSendQueue();
    recording_.store(true, std::memory_order_release);
    // The codec is not opened here. This runs on the timer task that ticks the
    // state machine, and opening the input takes the shared I2C bus lock and
    // resets the decoder. It happens on the capture task, after the
    // listen-start tone has finished playing. See kToneGuardMs.
    phase_.store(Phase::kToneGuard, std::memory_order_release);
    ESP_LOGI(TAG, "Capture arming, holding %u ms for the listen tone",
             static_cast<unsigned>(kToneGuardMs));
}

void VoiceCapture::EndUtterance() {
    if (!recording_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const bool inside_guard =
        phase_.load(std::memory_order_acquire) == Phase::kToneGuard;
    // Called even when the input was never opened. It is a no-op in that case,
    // and taking the branch out removes the question of which of two paths
    // left the processor running.
    service_->EnableVoiceProcessing(false);
    // kSettling either way, including from inside the guard. The completion
    // callback is what releases the state machine from kWaitingResponse, so a
    // path that ends here without firing it would leave the device waiting for
    // twenty-five seconds on an utterance that was never going to arrive. An
    // empty result is reported as empty; it is not silently dropped.
    phase_.store(Phase::kSettling, std::memory_order_release);
    if (inside_guard) {
        ESP_LOGW(TAG, "Released inside the %u ms listen-tone guard; "
                      "the microphone was never opened",
                 static_cast<unsigned>(kToneGuardMs));
        return;
    }
    ESP_LOGI(TAG, "Capture stopping, draining the encoder");
}

void VoiceCapture::Discard() {
    const bool was_recording = recording_.exchange(false, std::memory_order_acq_rel);
    if (was_recording) {
        service_->EnableVoiceProcessing(false);
    }
    // Discarding is handled on the capture task so the buffer has exactly one
    // owner. Setting the phase here and clearing there is what keeps a cancel
    // from racing a drain that is halfway through an append.
    phase_.store(Phase::kDiscarding, std::memory_order_release);
    ESP_LOGI(TAG, "Capture discarded");
}

uint32_t VoiceCapture::DrainOnce() {
    // Bounded so one call cannot monopolise the task. The send queue holds a
    // hundred packets and this runs every 20 ms, so this ceiling is never
    // reached in normal operation; it exists so that a pathological producer
    // cannot keep this loop from ever yielding.
    constexpr int kMaxPacketsPerPass = 64;
    uint32_t taken = 0;
    for (int i = 0; i < kMaxPacketsPerPass; ++i) {
        auto packet = service_->PopPacketFromSendQueue();
        if (packet == nullptr) {
            break;
        }
        // A frame that does not fit is dropped, not retried, and the packet is
        // still popped: leaving it in the queue would back the encoder up and
        // stall the input task, which is the exact failure this task exists to
        // prevent. buffer_.truncated() records that it happened.
        if (buffer_.AppendFrame(packet->payload.data(), packet->payload.size())) {
            ++taken;
        }
    }
    return taken;
}

void VoiceCapture::TaskEntry(void* arg) {
    static_cast<VoiceCapture*>(arg)->Run();
    vTaskDelete(nullptr);
}

void VoiceCapture::Run() {
    uint64_t settle_until_ms = 0;
    uint64_t guard_until_ms = 0;
    while (running_.load(std::memory_order_acquire)) {
        const Phase phase = phase_.load(std::memory_order_acquire);
        switch (phase) {
            case Phase::kIdle:
                break;

            case Phase::kToneGuard: {
                if (guard_until_ms == 0) {
                    guard_until_ms = NowMs() + kToneGuardMs;
                    // The buffer has exactly one owner, this task, so it is
                    // emptied here rather than by whoever pressed the button.
                    buffer_.Reset();
                    break;
                }
                if (NowMs() < guard_until_ms) {
                    break;
                }
                guard_until_ms = 0;
                // Only now is the codec input opened. Doing it here rather
                // than in BeginUtterance keeps the I2C work off the timer task
                // and lets the listen tone finish first.
                service_->EnableVoiceProcessing(true);
                service_->MarkPttStart(static_cast<int64_t>(NowMs()));
                // The reset inside EnableVoiceProcessing does not touch the
                // send queue, but a tone that was mid-flight could have left
                // an encoded frame behind. Clear once more, cheaply.
                service_->ClearSendQueue();
                // Only move on if nothing else changed the phase while the
                // codec was being opened: a release or a cancel during those
                // milliseconds must win over this.
                Phase expected = Phase::kToneGuard;
                if (phase_.compare_exchange_strong(expected, Phase::kRecording,
                                                   std::memory_order_acq_rel)) {
                    ESP_LOGI(TAG, "Capture started");
                }
                break;
            }

            case Phase::kRecording:
                DrainOnce();
                settle_until_ms = 0;
                break;

            case Phase::kSettling: {
                if (settle_until_ms == 0) {
                    settle_until_ms = NowMs() + kSettleMs;
                }
                DrainOnce();
                if (NowMs() < settle_until_ms) {
                    break;
                }
                settle_until_ms = 0;
                buffer_.Finalize();
                CaptureResult result;
                result.frames = buffer_.frame_count();
                result.duration_ms = buffer_.duration_ms();
                result.truncated = buffer_.truncated();
                if (result.frames > 0) {
                    result.body.assign(buffer_.body(), buffer_.body() + buffer_.size());
                }
                ESP_LOGI(TAG, "Capture finished: %u frames, %u ms, %u bytes%s",
                         static_cast<unsigned>(result.frames),
                         static_cast<unsigned>(result.duration_ms),
                         static_cast<unsigned>(result.body.size()),
                         result.truncated ? ", TRUNCATED" : "");
                buffer_.Reset();
                phase_.store(Phase::kIdle, std::memory_order_release);
                if (on_done_) {
                    on_done_(std::move(result));
                }
                break;
            }

            case Phase::kDiscarding:
                service_->EnableVoiceProcessing(false);
                service_->ClearSendQueue();
                buffer_.Reset();
                settle_until_ms = 0;
                guard_until_ms = 0;
                phase_.store(Phase::kIdle, std::memory_order_release);
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
    task_ = nullptr;
    ESP_LOGW(TAG, "Voice capture task stopped");
}

}  // namespace voice
