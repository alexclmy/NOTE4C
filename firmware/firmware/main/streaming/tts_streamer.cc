/**
 * @file tts_streamer.cc
 * @brief TTS streamer - manages text-to-speech request queue
 *
 * CURRENT STATE:
 * - Text queue management: COMPLETE - buffers text, splits into TTS requests
 * - State machine: COMPLETE - Idle → Processing → Complete
 * - DashScope TTS API: NOT CONNECTED - SendTtsRequest() is a placeholder
 * - Audio playback: PARTIAL - PlayAudioChunk() logs but doesn't push to AudioService
 *
 * TODO:
 * - Implement DashScope WebSocket TTS client (HTTP POST → PCM stream)
 * - Connect OnTtsResponse() PCM chunks to PlayAudioChunk()
 * - Wire PlayAudioChunk() → AudioService decode queue for real playback
 * - Add timeout handling per request (5s max)
 */

#include "tts_streamer.h"
#include "audio/audio_service.h"
#include <esp_log.h>
#include <algorithm>

namespace streaming {

static constexpr char kTag[] = "TtsStreamer";

TtsStreamer::TtsStreamer() = default;
TtsStreamer::~TtsStreamer() = default;

void TtsStreamer::Init(AudioService* audio_service) {
    audio_service_ = audio_service;
    ESP_LOGI(kTag, "Initialized with audio service");
}

void TtsStreamer::SetAudioCallback(TtsAudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_callback_ = std::move(callback);
}

void TtsStreamer::SetStateCallback(TtsStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callback_ = std::move(callback);
}

void TtsStreamer::FeedText(const std::string& text) {
    if (text.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    pending_buffer_ += text;

    ESP_LOGD(kTag, "FeedText: +%zu chars, buffer=%zu", text.size(), pending_buffer_.size());

    // Process if buffer is large enough
    if (pending_buffer_.size() >= kMinTtsChunkSize) {
        ProcessQueue();
    }
}

void TtsStreamer::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pending_buffer_.empty()) {
        text_queue_.push(pending_buffer_);
        pending_buffer_.clear();
        ESP_LOGI(kTag, "Flush: queued %zu chars", text_queue_.back().size());
    }

    ProcessQueue();
}

void TtsStreamer::BeginStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_streaming_ = true;
    state_ = TtsState::Idle;
    active_requests_ = 0;

    ESP_LOGI(kTag, "BeginStream");

    if (state_callback_) {
        state_callback_(TtsState::Idle);
    }
}

void TtsStreamer::EndStream() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Flush any remaining buffer
    if (!pending_buffer_.empty()) {
        text_queue_.push(pending_buffer_);
        pending_buffer_.clear();
    }

    ProcessQueue();
    is_streaming_ = false;

    ESP_LOGI(kTag, "EndStream: pending=%zu requests", text_queue_.size());
}

void TtsStreamer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    text_queue_ = {};
    audio_queue_ = {};
    pending_buffer_.clear();
    is_streaming_ = false;
    state_ = TtsState::Idle;
    active_requests_ = 0;

    ESP_LOGI(kTag, "Reset");
}

bool TtsStreamer::IsActive() const {
    return is_streaming_ || active_requests_ > 0 || !text_queue_.empty();
}

size_t TtsStreamer::GetPendingSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_buffer_.size() + text_queue_.size();
}

void TtsStreamer::ProcessQueue() {
    // Must be called under mutex lock

    while (!text_queue_.empty() && active_requests_ < kMaxConcurrentRequests) {
        std::string text = text_queue_.front();
        text_queue_.pop();

        if (SendTtsRequest(text)) {
            active_requests_++;
            state_ = TtsState::Processing;
        }
    }

    // Update state based on activity
    if (active_requests_ == 0 && text_queue_.empty() && pending_buffer_.empty()) {
        state_ = TtsState::Idle;
    }

    if (state_callback_) {
        state_callback_(state_);
    }
}

bool TtsStreamer::SendTtsRequest(const std::string& text) {
    // TODO: Integrate with DashScope TTS API
    // Current implementation: placeholder for future integration
    //
    // DashScope TTS flow:
    // 1. HTTP/WebSocket request to DashScope API
    // 2. Stream PCM audio chunks back
    // 3. Call OnTtsResponse() for each chunk
    //
    // For now, we just log the request

    ESP_LOGI(kTag, "SendTtsRequest: %zu chars (TTS API pending)", text.size());

    // Placeholder: simulate immediate completion
    // Real implementation would async request to DashScope

    return true;
}

void TtsStreamer::OnTtsResponse(const int16_t* pcm, size_t samples, bool is_final) {
    std::lock_guard<std::mutex> lock(mutex_);

    TtsAudioChunk chunk;
    chunk.pcm_data.assign(pcm, pcm + samples);
    chunk.sample_rate = 16000;
    chunk.is_final = is_final;

    ESP_LOGD(kTag, "OnTtsResponse: %zu samples, final=%d", samples, is_final);

    // Queue for playback or immediate callback
    if (audio_callback_) {
        audio_callback_(chunk);
    } else if (audio_service_) {
        PlayAudioChunk(chunk);
    }

    // Handle request completion
    if (is_final) {
        active_requests_--;
        state_ = TtsState::Complete;
        ProcessQueue();
    }

    if (state_callback_) {
        state_callback_(state_);
    }
}

void TtsStreamer::PlayAudioChunk(const TtsAudioChunk& chunk) {
    if (!audio_service_ || chunk.pcm_data.empty()) return;

    // AudioService expects decoded audio packets
    // We need to wrap PCM data in appropriate format
    //
    // For now, use PlaySound for simple cases
    // Real implementation would use PushPacketToDecodeQueue

    ESP_LOGD(kTag, "PlayAudioChunk: %zu samples", chunk.pcm_data.size());

    // TODO: Integrate with AudioService::PushPacketToDecodeQueue
    // for proper streaming audio playback
}

}  // namespace streaming