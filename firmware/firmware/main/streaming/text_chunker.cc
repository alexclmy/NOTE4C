/**
 * @file text_chunker.cc
 * @brief Text chunker - splits LLM stream into sentence-boundary chunks
 *
 * CURRENT STATE: COMPLETE
 * - UTF-8 safe splitting: IMPLEMENTED - backtracks to valid char boundary
 * - Chinese punctuation detection: IMPLEMENTED - 。！？；：
 * - ASCII punctuation detection: IMPLEMENTED - .!?:;\n\r
 * - Throttle (300ms min between chunks): IMPLEMENTED
 * - Flush on stream end: IMPLEMENTED - emits remaining buffer
 *
 * This module has no TODO items. It is fully functional for e-paper display.
 */

#include "text_chunker.h"
#include <algorithm>
#include <esp_log.h>

namespace streaming {

static constexpr char kTag[] = "TextChunker";

TextChunker::TextChunker() = default;
TextChunker::~TextChunker() = default;

void TextChunker::SetCallback(ChunkCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TextChunker::Feed(const std::string& text) {
    if (text.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    buffer_ += text;

    ESP_LOGD(kTag, "Feed: +%zu chars, buffer=%zu", text.size(), buffer_.size());

    // Try to emit chunks until buffer is empty or no boundary found
    while (TryEmitChunk()) {}
}

void TextChunker::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!buffer_.empty() && callback_) {
        ESP_LOGI(kTag, "Flush: emitting %zu chars", buffer_.size());
        callback_(buffer_);
        buffer_.clear();
    }

    last_emit_time_ms_ = 0;
}

void TextChunker::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    last_emit_time_ms_ = 0;
    ESP_LOGI(kTag, "Reset");
}

bool TextChunker::HasPendingData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !buffer_.empty();
}

bool TextChunker::TryEmitChunk() {
    // Must be called under mutex lock

    if (buffer_.empty() || !callback_) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed = now_ms - last_emit_time_ms_;

    // Check throttle: must wait at least kMinChunkIntervalMs
    if (elapsed < kMinChunkIntervalMs && last_emit_time_ms_ > 0) {
        return false;
    }

    // Find natural sentence boundary
    size_t boundary = FindSentenceBoundary();

    // If no boundary found and buffer is too large, force split at UTF-8 boundary
    if (boundary == 0 && buffer_.size() > kMaxChunkSize) {
        // Find safe UTF-8 boundary
        boundary = kMaxChunkSize;
        // Backtrack to complete UTF-8 character
        while (boundary > 0 && (buffer_[boundary] & 0xC0) == 0x80) {
            boundary--;
        }
        if (boundary == 0) {
            boundary = kMaxChunkSize;  // Fallback
        }
    }

    // If we found a boundary, emit chunk
    if (boundary > 0 && boundary <= buffer_.size()) {
        std::string chunk = buffer_.substr(0, boundary);
        callback_(chunk);
        buffer_.erase(0, boundary);
        last_emit_time_ms_ = now_ms;

        ESP_LOGD(kTag, "Emit: %zu chars, remaining=%zu", chunk.size(), buffer_.size());
        return true;
    }

    // No boundary found, wait for more input
    return false;
}

size_t TextChunker::FindSentenceBoundary() {
    // Search for sentence-ending punctuation in buffer
    // Priority: Chinese punctuation > English punctuation > newline

    size_t best_boundary = 0;

    for (size_t i = 0; i < buffer_.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(buffer_[i]);

        // Check ASCII punctuation
        if (ch < 0x80) {
            if (IsSentenceEndPunctuation(buffer_[i])) {
                // Include the punctuation
                best_boundary = i + 1;
                // Prefer earlier boundaries for shorter chunks
                if (best_boundary >= kMinChunkSize) {
                    return best_boundary;
                }
            }
        } else {
            // Check Chinese punctuation (UTF-8 multi-byte)
            if (IsChinesePunctuation(buffer_, i)) {
                // Chinese punctuation is 3 bytes UTF-8
                best_boundary = i + 3;
                if (best_boundary >= kMinChunkSize) {
                    return best_boundary;
                }
            }
        }
    }

    return best_boundary;
}

bool TextChunker::IsSentenceEndPunctuation(char ch) const {
    // ASCII sentence-ending punctuation
    switch (ch) {
        case '.':
        case '!':
        case '?':
        case ';':
        case ':':
        case '\n':
        case '\r':
            return true;
        default:
            return false;
    }
}

bool TextChunker::IsChinesePunctuation(const std::string& str, size_t pos) const {
    // Chinese sentence-ending punctuation UTF-8 encoding:
    // 。(U+3002): E3 80 82
    // ！(U+FF01): EF BC 81
    // ？(U+FF1F): EF BC 9F
    // ；(U+FF1B): EF BC 9B
    // ：(U+FF1A): EF BC 9A

    if (pos + 2 >= str.size()) return false;

    unsigned char b0 = static_cast<unsigned char>(str[pos]);
    unsigned char b1 = static_cast<unsigned char>(str[pos + 1]);
    unsigned char b2 = static_cast<unsigned char>(str[pos + 2]);

    // Check for 。 (E3 80 82)
    if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x82) {
        return true;
    }

    // Check for ！？；： (EF BC xx)
    if (b0 == 0xEF && b1 == 0xBC) {
        switch (b2) {
            case 0x81:  // ！
            case 0x9F:  // ？
            case 0x9B:  // ；
            case 0x9A:  // ：
                return true;
            default:
                return false;
        }
    }

    return false;
}

}  // namespace streaming