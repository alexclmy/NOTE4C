/**
 * @file utterance_buffer.cc
 * @brief Implementation of the bounded utterance container.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "utterance_buffer.h"

#include <string.h>

namespace voice {

namespace {

void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void UtteranceBuffer::Configure(uint32_t sample_rate, uint16_t frame_duration_ms,
                                uint8_t channels, Limits limits) {
    sample_rate_ = sample_rate;
    frame_duration_ms_ = frame_duration_ms;
    channels_ = channels;
    limits_ = limits;
    // A cap smaller than the header would make remaining_bytes() underflow and
    // AppendFrame accept everything. Clamp rather than trust the caller.
    if (limits_.max_bytes < kHeaderBytes) {
        limits_.max_bytes = kHeaderBytes;
    }
    Reset();
}

void UtteranceBuffer::Reset() {
    body_.assign(kHeaderBytes, 0);
    memcpy(body_.data(), "N4CO", 4);
    body_[4] = kContainerVersion;
    body_[5] = channels_;
    PutU16(body_.data() + 6, frame_duration_ms_);
    PutU32(body_.data() + 8, sample_rate_);
    PutU32(body_.data() + 12, 0);
    frame_count_ = 0;
    truncated_ = false;
    finalized_ = false;
}

size_t UtteranceBuffer::remaining_bytes() const {
    if (body_.size() >= limits_.max_bytes) {
        return 0;
    }
    return limits_.max_bytes - body_.size();
}

bool UtteranceBuffer::AppendFrame(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        // An empty frame carries nothing and would still cost two bytes of
        // length prefix. Not a truncation: nothing was lost.
        return false;
    }
    if (size > kMaxFrameBytes) {
        // Implausible for 60 ms of 16 kHz mono. Refusing it is not truncation
        // of the utterance, it is refusing a frame that should not exist, and
        // the distinction matters because truncated() is reported to the user.
        return false;
    }
    if (frame_count_ >= limits_.max_frames) {
        truncated_ = true;
        return false;
    }
    const size_t needed = size + 2;
    if (needed > remaining_bytes()) {
        truncated_ = true;
        return false;
    }
    uint8_t prefix[2];
    PutU16(prefix, static_cast<uint16_t>(size));
    body_.insert(body_.end(), prefix, prefix + 2);
    body_.insert(body_.end(), data, data + size);
    ++frame_count_;
    // Appending after Finalize() would leave a header that understates the
    // contents, so the count is re-patched on the next Finalize().
    finalized_ = false;
    return true;
}

void UtteranceBuffer::Finalize() {
    PutU32(body_.data() + 12, frame_count_);
    finalized_ = true;
}

bool ParseHeader(const uint8_t* data, size_t size, uint32_t* sample_rate_out,
                 uint16_t* frame_duration_ms_out, uint8_t* channels_out,
                 uint32_t* frame_count_out) {
    if (data == nullptr || size < kHeaderBytes) {
        return false;
    }
    if (memcmp(data, "N4CO", 4) != 0) {
        return false;
    }
    if (data[4] != kContainerVersion) {
        return false;
    }
    if (sample_rate_out) *sample_rate_out = GetU32(data + 8);
    if (frame_duration_ms_out) *frame_duration_ms_out = GetU16(data + 6);
    if (channels_out) *channels_out = data[5];
    if (frame_count_out) *frame_count_out = GetU32(data + 12);
    return true;
}

int64_t ValidateFrames(const uint8_t* data, size_t size) {
    uint32_t declared = 0;
    if (!ParseHeader(data, size, nullptr, nullptr, nullptr, &declared)) {
        return -1;
    }
    size_t offset = kHeaderBytes;
    uint32_t seen = 0;
    while (offset < size) {
        if (offset + 2 > size) {
            // A dangling length prefix. Reporting this as malformed rather
            // than as "the last frame is short" is what stops a reader from
            // handing a decoder a truncated packet.
            return -1;
        }
        const uint16_t len = GetU16(data + offset);
        offset += 2;
        if (len == 0 || offset + len > size) {
            return -1;
        }
        offset += len;
        ++seen;
    }
    if (seen != declared) {
        return -1;
    }
    return static_cast<int64_t>(seen);
}

}  // namespace voice
