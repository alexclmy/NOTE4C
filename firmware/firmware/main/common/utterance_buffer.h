/**
 * @file utterance_buffer.h
 * @brief Bounded container for one captured utterance. No ESP-IDF, no codec.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why a container at all
 * ----------------------
 * The encoder hands out Opus *packets*, one per 60 ms frame. Concatenating
 * them produces a byte string nothing can decode: Opus packets are not
 * self-delimiting, so a decoder needs the length of each one. The usual answer
 * is Ogg encapsulation, which is what the MIME type `audio/opus` means.
 *
 * This does not write Ogg. Ogg pages carry granule positions, CRCs and a
 * serial number, and getting them subtly wrong produces a file that plays on
 * one decoder and not another. What it writes instead is the smallest thing
 * that is unambiguous: a fixed header and a length before every frame.
 *
 * The consequence is stated rather than hidden. The body is declared as
 * `audio/x-note4c-opus-frames`, **not** as `audio/opus`. Sending this under
 * the name of a format it is not would cost whoever writes the first real
 * transcriber an afternoon, and the lie would be discovered by a decoder
 * failing rather than by reading this file.
 *
 * Wire format, little endian throughout
 * -------------------------------------
 * | Offset | Size | Field |
 * | --- | --- | --- |
 * | 0 | 4 | magic, ASCII `N4CO` |
 * | 4 | 1 | version, currently 1 |
 * | 5 | 1 | channels, currently 1 |
 * | 6 | 2 | frame duration, milliseconds |
 * | 8 | 4 | sample rate, Hz |
 * | 12 | 4 | frame count |
 *
 * Then, `frame count` times: a two-byte length followed by that many bytes of
 * Opus packet.
 *
 * Bounds
 * ------
 * Two caps, whichever is reached first, and truncation is recorded rather than
 * silently absorbed. A truncated utterance is still uploaded, because half of
 * what somebody said is more useful than nothing, but the caller can see that
 * it happened and say so.
 */

#ifndef COMMON_UTTERANCE_BUFFER_H
#define COMMON_UTTERANCE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace voice {

/// The declared MIME type. Deliberately not `audio/opus`; see the file comment.
constexpr const char* kUtteranceMime = "audio/x-note4c-opus-frames";

constexpr uint8_t kContainerVersion = 1;
constexpr size_t kHeaderBytes = 16;

/// One Opus packet cannot plausibly be larger than this at 16 kHz mono. A
/// frame that claims to be is a bug upstream, and appending it would let one
/// bad packet eat the whole budget.
constexpr size_t kMaxFrameBytes = 4096;

struct Limits {
    /**
     * @brief Hard cap on the serialised body, header included.
     *
     * 192 KiB is roughly eight times what 15 s of 16 kHz mono Opus at the
     * encoder's complexity-0 settings is expected to produce, so reaching it
     * means something is wrong rather than that somebody talked for a while.
     * The hub's own cap is 2 MiB, which this stays an order of magnitude
     * inside on purpose: the device should refuse before the server has to.
     */
    size_t max_bytes = 192 * 1024;
    /// Cap on frames, so a stream of empty packets cannot spin forever.
    uint32_t max_frames = 512;
};

/**
 * @brief Accumulates encoded frames for one utterance.
 *
 * Not thread-safe. The capture task owns it while recording; the upload task
 * reads it only after the state machine has left kRecording, which is the same
 * ordering that makes mic_stop-before-upload safe in audio_fsm.cc.
 */
class UtteranceBuffer {
public:
    UtteranceBuffer() { Reset(); }

    /// Set the header fields. Resets the contents.
    void Configure(uint32_t sample_rate, uint16_t frame_duration_ms,
                   uint8_t channels, Limits limits);

    /// Empty the buffer and clear the truncation flag. Keeps the configuration.
    void Reset();

    /**
     * @brief Append one encoded frame.
     *
     * @return true if it was stored. false means it did not fit, or was
     *         rejected as implausible; either way the buffer stays valid and
     *         truncated() becomes true for the "did not fit" case.
     */
    bool AppendFrame(const uint8_t* data, size_t size);

    /// Patch the frame count into the header. Call once, before reading body().
    void Finalize();

    /// The serialised body. Only meaningful after Finalize().
    const uint8_t* body() const { return body_.data(); }
    size_t size() const { return body_.size(); }

    uint32_t frame_count() const { return frame_count_; }
    bool empty() const { return frame_count_ == 0; }
    /// True when at least one frame was dropped because a cap was reached.
    bool truncated() const { return truncated_; }
    /// Captured duration implied by the frames actually stored.
    uint32_t duration_ms() const {
        return frame_count_ * static_cast<uint32_t>(frame_duration_ms_);
    }
    /// Bytes still available for frame payloads, length prefixes included.
    size_t remaining_bytes() const;

private:
    std::vector<uint8_t> body_;
    uint32_t sample_rate_ = 16000;
    uint16_t frame_duration_ms_ = 60;
    uint8_t channels_ = 1;
    uint32_t frame_count_ = 0;
    bool truncated_ = false;
    bool finalized_ = false;
    Limits limits_{};
};

/// Read the frame count out of a serialised body. Returns false if @p data is
/// not a well-formed container. Used by the host tests, and by anything that
/// ever has to read one of these back.
bool ParseHeader(const uint8_t* data, size_t size, uint32_t* sample_rate_out,
                 uint16_t* frame_duration_ms_out, uint8_t* channels_out,
                 uint32_t* frame_count_out);

/// Walk the frames of a serialised body, checking that every length prefix
/// lands inside it. Returns the number of frames found, or -1 if malformed.
int64_t ValidateFrames(const uint8_t* data, size_t size);

}  // namespace voice

#endif  // COMMON_UTTERANCE_BUFFER_H
