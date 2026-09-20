/**
 * @file text_chunker.h
 * @brief Text chunker for streaming LLM output to e-paper display
 *
 * Splits streaming text into chunks at natural sentence boundaries,
 * respecting Chinese punctuation and word boundaries.
 * Throttles output to match e-paper refresh rate (300ms minimum).
 */

#ifndef TEXT_CHUNKER_H
#define TEXT_CHUNKER_H

#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <esp_timer.h>

namespace streaming {

/**
 * @brief Text chunker for LLM streaming output
 *
 * Receives streaming text chunks from LLM, buffers until natural
 * sentence boundary (Chinese/English punctuation), then emits
 * complete chunks for UI rendering.
 *
 * Key features:
 * - Split on Chinese punctuation: 。！？；：
 * - Split on English punctuation: .!?;:\n
 * - Minimum 300ms between chunks (e-paper refresh limit)
 * - UTF-8 safe boundary handling
 * - Flush on stream end
 */
class TextChunker {
public:
    using ChunkCallback = std::function<void(const std::string& chunk)>;

    TextChunker();
    ~TextChunker();

    /**
     * @brief Set callback for chunk emission
     *
     * Called when a complete chunk is ready for display.
     */
    void SetCallback(ChunkCallback callback);

    /**
     * @brief Feed text chunk from LLM stream
     *
     * Buffers text until sentence boundary or buffer limit.
     * May trigger multiple chunk emissions.
     */
    void Feed(const std::string& text);

    /**
     * @brief Flush remaining buffer
     *
     * Called at stream end to emit any remaining text.
     */
    void Flush();

    /**
     * @brief Reset chunker state
     *
     * Clear buffer and timing state.
     */
    void Reset();

    /**
     * @brief Check if chunker has pending data
     */
    bool HasPendingData() const;

private:
    /**
     * @brief Try to emit a complete chunk
     *
     * @return true if chunk was emitted
     */
    bool TryEmitChunk();

    /**
     * @brief Find sentence boundary in buffer
     *
     * @return position of boundary, or 0 if none found
     */
    size_t FindSentenceBoundary();

    /**
     * @brief Check if character is sentence-ending punctuation
     */
    bool IsSentenceEndPunctuation(char ch) const;

    /**
     * @brief Check if UTF-8 character at position is Chinese punctuation
     */
    bool IsChinesePunctuation(const std::string& str, size_t pos) const;

    ChunkCallback callback_;
    std::string buffer_;
    mutable std::mutex mutex_;
    int64_t last_emit_time_ms_ = 0;

    // Configuration
    static constexpr int64_t kMinChunkIntervalMs = 300;   // E-paper refresh throttle
    static constexpr size_t kMaxChunkSize = 100;          // Max chars per chunk
    static constexpr size_t kMinChunkSize = 5;            // Min chars for flush
};

}  // namespace streaming

#endif  // TEXT_CHUNKER_H