/**
 * @file tts_streamer.h
 * @brief TTS streamer for converting text chunks to audio
 *
 * Feeds text chunks to DashScope TTS API, manages audio playback queue,
 * and coordinates with AudioService for sequential playback.
 */

#ifndef TTS_STREAMER_H
#define TTS_STREAMER_H

#include <string>
#include <functional>
#include <mutex>
#include <queue>
#include <atomic>
#include <esp_timer.h>

// Forward declaration
class AudioService;

namespace streaming {

/**
 * @brief TTS request state
 */
enum class TtsState {
    Idle,       ///< No active requests
    Pending,    ///< Request queued, waiting to send
    Processing, ///< Request sent to TTS API, awaiting response
    Playing,    ///< Audio received, playing
    Complete,   ///< Playback finished
    Error,      ///< Request failed
};

/**
 * @brief TTS audio chunk
 */
struct TtsAudioChunk {
    std::vector<int16_t> pcm_data;
    int sample_rate = 16000;
    bool is_final = false;
};

/**
 * @brief TTS streamer for audio synthesis
 *
 * Manages text-to-audio conversion with:
 * - DashScope TTS API integration
 * - Concurrent request limiting (max 2)
 * - Sequential playback queue
 * - Buffer management for smooth playback
 *
 * Design:
 * - Text chunks from TextChunker → TtsStreamer::FeedText()
 * - Each chunk triggers TTS API request
 * - Audio chunks received → queued for playback
 * - AudioService plays chunks sequentially
 */
class TtsStreamer {
public:
    using TtsAudioCallback = std::function<void(const TtsAudioChunk& chunk)>;
    using TtsStateCallback = std::function<void(TtsState state)>;

    TtsStreamer();
    ~TtsStreamer();

    /**
     * @brief Initialize with audio service reference
     *
     * @param audio_service AudioService for playback
     */
    void Init(AudioService* audio_service);

    /**
     * @brief Set audio chunk callback
     *
     * Called when audio chunk is ready for playback.
     */
    void SetAudioCallback(TtsAudioCallback callback);

    /**
     * @brief Set state change callback
     */
    void SetStateCallback(TtsStateCallback callback);

    /**
     * @brief Feed text chunk for TTS synthesis
     *
     * Queues text for TTS conversion. Will start processing
     * when concurrent slots available.
     */
    void FeedText(const std::string& text);

    /**
     * @brief Flush pending text buffer
     *
     * Force send any buffered text to TTS.
     */
    void Flush();

    /**
     * @brief Begin streaming session
     */
    void BeginStream();

    /**
     * @brief End streaming session
     */
    void EndStream();

    /**
     * @brief Reset streamer state
     */
    void Reset();

    /**
     * @brief Get current state
     */
    TtsState GetState() const { return state_; }

    /**
     * @brief Check if actively processing
     */
    bool IsActive() const;

    /**
     * @brief Get pending text buffer size
     */
    size_t GetPendingSize() const;

private:
    /**
     * @brief Process pending text queue
     *
     * Sends next text chunk to TTS if slot available.
     */
    void ProcessQueue();

    /**
     * @brief Send text to TTS API
     *
     * @param text Text to synthesize
     * @return true if request started
     */
    bool SendTtsRequest(const std::string& text);

    /**
     * @brief Handle TTS response
     *
     * @param pcm Audio PCM data
     * @param samples Number of samples
     * @param is_final Whether this is final chunk
     */
    void OnTtsResponse(const int16_t* pcm, size_t samples, bool is_final);

    /**
     * @brief Play audio chunk through AudioService
     */
    void PlayAudioChunk(const TtsAudioChunk& chunk);

    AudioService* audio_service_ = nullptr;
    TtsAudioCallback audio_callback_;
    TtsStateCallback state_callback_;

    std::queue<std::string> text_queue_;
    std::queue<TtsAudioChunk> audio_queue_;
    std::string pending_buffer_;
    mutable std::mutex mutex_;

    std::atomic<TtsState> state_{TtsState::Idle};
    std::atomic<int> active_requests_{0};
    std::atomic<bool> is_streaming_{false};

    // Configuration
    static constexpr int kMaxConcurrentRequests = 2;
    static constexpr size_t kMinTtsChunkSize = 20;  // Min chars before sending
    static constexpr size_t kMaxTtsChunkSize = 100; // Max chars per TTS request
};

}  // namespace streaming

#endif  // TTS_STREAMER_H