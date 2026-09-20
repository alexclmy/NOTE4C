/**
 * @file stream_pipeline.h
 * @brief Streaming pipeline for LLM → UI + TTS
 *
 * Coordinates text chunking for e-paper display updates
 * and TTS audio synthesis from LLM streaming output.
 */

#ifndef STREAM_PIPELINE_H
#define STREAM_PIPELINE_H

#include <string>
#include <functional>
#include <mutex>
#include <atomic>

#include "text_chunker.h"
#include "tts_streamer.h"

// Forward declarations
class AudioService;

namespace rawdraw {
class ChatRenderer;
}

namespace streaming {

/**
 * @brief Streaming pipeline for LLM output
 *
 * Routes LLM text chunks through:
 * 1. TextChunker → UI (ChatRenderer::AppendText) with 300ms throttle
 * 2. TtsStreamer → Audio playback (AudioService)
 *
 * Design:
 * - LLM WebSocket stream → FeedLlmText()
 * - TextChunker splits on sentence boundaries
 * - Each chunk goes to BOTH UI and TTS simultaneously
 * - UI updates throttled to 300ms (e-paper limit)
 * - TTS manages concurrent requests (max 2)
 *
 * Usage:
 * 1. Init() with AudioService and ChatRenderer
 * 2. BeginStream() before LLM response
 * 3. FeedLlmText() for each chunk from WebSocket
 * 4. EndStream() after LLM completes
 * 5. Reset() to clear state
 */
class StreamPipeline {
public:
    StreamPipeline();
    ~StreamPipeline();

    /**
     * @brief Initialize pipeline with dependencies
     *
     * @param audio_service Audio playback service
     * @param chat_renderer UI chat renderer (rawdraw)
     */
    void Init(AudioService* audio_service, rawdraw::ChatRenderer* chat_renderer);

    /**
     * @brief Set fallback UI callback (for LVGL ChatPage)
     */
    using UiTextCallback = std::function<void(const std::string& chunk)>;
    void SetUiCallback(UiTextCallback callback);

    /**
     * @brief Feed LLM text chunk
     *
     * Routes to TextChunker and TtsStreamer.
     */
    void FeedLlmText(const std::string& chunk);

    /**
     * @brief Begin streaming session
     *
     * Clears buffers, prepares UI for new message.
     */
    void BeginStream();

    /**
     * @brief End streaming session
     *
     * Flushes remaining buffers, completes UI bubble.
     */
    void EndStream();

    /**
     * @brief Check if actively streaming
     */
    bool IsStreaming() const { return is_streaming_; }

    /**
     * @brief Reset pipeline state
     */
    void Reset();

    /**
     * @brief Get text chunker for direct access
     */
    TextChunker& GetTextChunker() { return text_chunker_; }

    /**
     * @brief Get TTS streamer for direct access
     */
    TtsStreamer& GetTtsStreamer() { return tts_streamer_; }

private:
    /**
     * @brief Handle text chunk from TextChunker
     *
     * Sends to UI renderer.
     */
    void OnTextChunk(const std::string& chunk);

    /**
     * @brief Handle TTS audio chunk
     */
    void OnTtsAudio(const TtsAudioChunk& chunk);

    TextChunker text_chunker_;
    TtsStreamer tts_streamer_;

    AudioService* audio_service_ = nullptr;
    rawdraw::ChatRenderer* chat_renderer_ = nullptr;
    UiTextCallback ui_callback_;

    std::mutex mutex_;
    std::atomic<bool> is_streaming_{false};
    int64_t last_ui_update_ms_ = 0;
};

}  // namespace streaming

#endif  // STREAM_PIPELINE_H