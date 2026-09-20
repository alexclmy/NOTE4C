/**
 * @file stream_pipeline.cc
 * @brief Streaming pipeline implementation - LLM text → UI + TTS
 *
 * CURRENT STATE (Phase 7):
 * - TextChunker integration: COMPLETE - splits LLM output on sentence boundaries
 * - TtsStreamer integration: PARTIAL - text queue works, but DashScope TTS API not connected
 * - UI routing: COMPLETE - chunks routed to ChatRenderer::AppendText with 300ms throttle
 * - Audio playback: TODO - OnTtsAudio() receives PCM but doesn't push to AudioService decode queue
 *
 * TODO PLAN (Phase 8+):
 * 1. Connect TtsStreamer::SendTtsRequest() to DashScope TTS WebSocket API
 * 2. Wire OnTtsAudio() PCM chunks → AudioService::PushPacketToDecodeQueue()
 * 3. Add TTS queue management (max 2 concurrent requests)
 * 4. Implement error recovery (TTS timeout → skip chunk, continue streaming)
 * 5. Add latency measurement (FeedLlmText → first audio playback)
 *
 * DATA FLOW:
 *   LLM WebSocket → FeedLlmText() → TextChunker → OnTextChunk() → ChatRenderer
 *                                → TtsStreamer  → OnTtsAudio()  → [TODO: AudioService]
 */

#include "stream_pipeline.h"
#include "audio/audio_service.h"
#include "ui/renderers/rawdraw/chat_renderer.h"
#include <esp_log.h>
#include <esp_timer.h>

namespace streaming {

static constexpr char kTag[] = "StreamPipeline";

StreamPipeline::StreamPipeline() = default;
StreamPipeline::~StreamPipeline() = default;

void StreamPipeline::Init(AudioService* audio_service, rawdraw::ChatRenderer* chat_renderer) {
    audio_service_ = audio_service;
    chat_renderer_ = chat_renderer;

    // Initialize TTS streamer with audio service
    if (audio_service_) {
        tts_streamer_.Init(audio_service_);
    }

    // Set text chunker callback → UI update
    text_chunker_.SetCallback([this](const std::string& chunk) {
        OnTextChunk(chunk);
    });

    // Set TTS streamer audio callback
    tts_streamer_.SetAudioCallback([this](const TtsAudioChunk& chunk) {
        OnTtsAudio(chunk);
    });

    ESP_LOGI(kTag, "Initialized: audio=%p, chat_renderer=%p",
             audio_service_, chat_renderer_);
}

void StreamPipeline::SetUiCallback(UiTextCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_callback_ = std::move(callback);
}

void StreamPipeline::FeedLlmText(const std::string& chunk) {
    if (chunk.empty()) return;

    ESP_LOGD(kTag, "FeedLlmText: %zu chars", chunk.size());

    // Route to text chunker (UI display)
    text_chunker_.Feed(chunk);

    // Route to TTS streamer (audio synthesis)
    tts_streamer_.FeedText(chunk);
}

void StreamPipeline::BeginStream() {
    std::lock_guard<std::mutex> lock(mutex_);

    is_streaming_ = true;

    // Reset chunkers
    text_chunker_.Reset();
    tts_streamer_.BeginStream();

    // Create new AI bubble in chat renderer
    if (chat_renderer_) {
        chat_renderer_->BeginStream();
    }

    last_ui_update_ms_ = 0;

    ESP_LOGI(kTag, "BeginStream");
}

void StreamPipeline::EndStream() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Flush all pending buffers
    text_chunker_.Flush();
    tts_streamer_.EndStream();

    is_streaming_ = false;

    // Complete UI bubble
    if (chat_renderer_) {
        chat_renderer_->EndStream();
    }

    ESP_LOGI(kTag, "EndStream");
}

void StreamPipeline::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    is_streaming_ = false;
    text_chunker_.Reset();
    tts_streamer_.Reset();

    last_ui_update_ms_ = 0;

    ESP_LOGI(kTag, "Reset");
}

void StreamPipeline::OnTextChunk(const std::string& chunk) {
    if (chunk.empty()) return;

    ESP_LOGD(kTag, "OnTextChunk: %zu chars", chunk.size());

    // Check throttle: only update UI every 300ms minimum
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed = now_ms - last_ui_update_ms_;

    // Update UI
    if (chat_renderer_) {
        chat_renderer_->AppendText(chunk.c_str());

        // Throttle: mark for refresh but don't force immediate
        // The rawdraw ChatRenderer manages dirty rect internally
        ESP_LOGD(kTag, "UI update: %zu chars to ChatRenderer", chunk.size());
    }

    // Fallback: use legacy UI callback if set
    if (ui_callback_) {
        ui_callback_(chunk);
    }

    last_ui_update_ms_ = now_ms;
}

void StreamPipeline::OnTtsAudio(const TtsAudioChunk& chunk) {
    if (chunk.pcm_data.empty()) return;

    ESP_LOGD(kTag, "OnTtsAudio: %zu samples, rate=%d",
             chunk.pcm_data.size(), chunk.sample_rate);

    // TODO: Route audio to AudioService for playback
    // Current implementation uses placeholder in TtsStreamer
    //
    // Full integration would:
    // 1. Convert PCM to AudioStreamPacket format
    // 2. Push to AudioService::PushPacketToDecodeQueue()
    // 3. AudioService plays sequentially
}

}  // namespace streaming