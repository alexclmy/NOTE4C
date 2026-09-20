/**
 * @file record_slot.cc
 * @brief Implementation of the A/B record store.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See record_slot.h for why this is structured the way it is, and for why it
 * deliberately repeats dashboard_slot.cc rather than replacing it. No ESP-IDF
 * headers: the host test suite compiles this same file.
 */

#include "record_slot.h"

#include <string.h>

namespace record {

namespace {

// Header field offsets. Little-endian, fixed width, so a record written by one
// build is readable by another regardless of struct packing rules. The same
// offsets dashboard_slot.cc uses, deliberately: the two stores hold different
// kinds of record and there is no reason for their headers to disagree.
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 8;
constexpr size_t kOffSeq = 12;
constexpr size_t kOffPayloadLen = 16;
constexpr size_t kOffFlags = 20;
constexpr size_t kOffSha = 24;
constexpr size_t kOffSourceEpoch = 56;
constexpr size_t kOffHeaderCrc = 60;

inline uint32_t LoadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void StoreU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

// ---------------------------------------------------------------- SHA-256 --

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t Ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

void Sha256Block(uint32_t* h, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

}  // namespace

void Sha256(const uint8_t* data, size_t len, uint8_t* out) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    const size_t full_blocks = len / 64;
    for (size_t i = 0; i < full_blocks; ++i) {
        Sha256Block(h, data + i * 64);
    }

    // Tail: remaining bytes, 0x80 terminator, zero padding, 64-bit bit length.
    uint8_t tail[128];
    const size_t rem = len - full_blocks * 64;
    if (rem > 0) {
        memcpy(tail, data + full_blocks * 64, rem);
    }
    tail[rem] = 0x80;

    const size_t tail_len = (rem < 56) ? 64 : 128;
    memset(tail + rem + 1, 0, tail_len - rem - 1 - 8);

    const uint64_t bits = static_cast<uint64_t>(len) * 8u;
    for (int i = 0; i < 8; ++i) {
        tail[tail_len - 1 - i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
    }

    Sha256Block(h, tail);
    if (tail_len == 128) {
        Sha256Block(h, tail + 64);
    }

    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
    }
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = (crc & 1u) ? 0xedb88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc ^ 0xffffffffu;
}

bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

// --------------------------------------------------------------- RecordSlot --

bool RecordSlot::ParseRecord(const uint8_t* raw, size_t len, SlotStatus* out) const {
    out->valid = false;

    if (len < kHeaderBytes) {
        return false;
    }
    if (memcmp(raw + kOffMagic, spec_.magic, sizeof(spec_.magic)) != 0) {
        return false;
    }
    if (LoadU32(raw + kOffVersion) != kRecordVersion) {
        return false;
    }

    // Check the header's own integrity before trusting any field inside it.
    // Without this, a corrupted payload_len could send us reading out of range.
    const uint32_t stored_crc = LoadU32(raw + kOffHeaderCrc);
    if (Crc32(raw, kOffHeaderCrc) != stored_crc) {
        return false;
    }

    const uint32_t payload_len = LoadU32(raw + kOffPayloadLen);
    if (payload_len < spec_.payload_min || payload_len > spec_.payload_max) {
        return false;
    }
    if (len < kHeaderBytes + payload_len) {
        return false;
    }

    uint8_t computed[kShaBytes];
    Sha256(raw + kHeaderBytes, payload_len, computed);
    if (!ConstantTimeEquals(computed, raw + kOffSha, kShaBytes)) {
        return false;
    }

    out->valid = true;
    out->seq = LoadU32(raw + kOffSeq);
    out->source_epoch = LoadU32(raw + kOffSourceEpoch);
    out->payload_len = payload_len;
    memcpy(out->sha, raw + kOffSha, kShaBytes);
    return true;
}

bool RecordSlot::Load() {
    active_ = -1;
    for (int i = 0; i < kSlotCount; ++i) {
        status_[i] = SlotStatus{};
    }
    if (io_ == nullptr || scratch_ == nullptr) {
        return false;
    }

    uint8_t* raw = scratch_;
    const size_t record_cap = spec_.scratch_bytes();

    for (int slot = 0; slot < kSlotCount; ++slot) {
        const int n = io_->ReadSlot(slot, raw, record_cap);
        if (n <= 0) {
            continue;
        }
        SlotStatus st;
        if (!ParseRecord(raw, static_cast<size_t>(n), &st)) {
            continue;
        }
        status_[slot] = st;
        // Highest sequence wins. Both slots valid is the normal steady state:
        // the older one is the rollback copy, not an error.
        if (active_ < 0 || st.seq > status_[active_].seq) {
            active_ = slot;
        }
    }
    return active_ >= 0;
}

uint32_t RecordSlot::active_seq() const {
    return active_ >= 0 ? status_[active_].seq : 0u;
}

uint32_t RecordSlot::active_source_epoch() const {
    return active_ >= 0 ? status_[active_].source_epoch : 0u;
}

uint32_t RecordSlot::active_payload_len() const {
    return active_ >= 0 ? status_[active_].payload_len : 0u;
}

const uint8_t* RecordSlot::active_sha() const {
    return active_ >= 0 ? status_[active_].sha : nullptr;
}

bool RecordSlot::ReadPayload(uint8_t* out, size_t cap, size_t* len_out) const {
    if (active_ < 0 || io_ == nullptr || out == nullptr || scratch_ == nullptr) {
        return false;
    }

    uint8_t* raw = scratch_;
    const int n = io_->ReadSlot(active_, raw, spec_.scratch_bytes());
    if (n <= 0) {
        return false;
    }

    // Re-validate rather than trusting the Load()-time verdict: the point of
    // storing a checksum is to use it on every read back out of flash.
    SlotStatus st;
    if (!ParseRecord(raw, static_cast<size_t>(n), &st)) {
        return false;
    }
    if (st.payload_len > cap) {
        // Refuse rather than copy what fits. A caller that asked for a payload
        // and got a prefix of one has no way to tell.
        return false;
    }
    memcpy(out, raw + kHeaderBytes, st.payload_len);
    if (len_out != nullptr) *len_out = st.payload_len;
    return true;
}

bool RecordSlot::ReadPayloadInPlace(const uint8_t** data, size_t* len_out) {
    if (active_ < 0 || io_ == nullptr || scratch_ == nullptr || data == nullptr) {
        return false;
    }
    const int n = io_->ReadSlot(active_, scratch_, spec_.scratch_bytes());
    if (n <= 0) return false;

    // Re-validated on every read, exactly as ReadPayload does. The point of
    // storing a checksum is to use it every time the bytes come out of flash.
    SlotStatus st;
    if (!ParseRecord(scratch_, static_cast<size_t>(n), &st)) return false;

    *data = scratch_ + kHeaderBytes;
    if (len_out != nullptr) *len_out = st.payload_len;
    return true;
}

StoreResult RecordSlot::Store(const uint8_t* payload,
                              size_t len,
                              uint32_t source_epoch,
                              const uint8_t* expected_sha) {
    if (io_ == nullptr || payload == nullptr || scratch_ == nullptr) {
        return StoreResult::kWriteFailed;
    }
    if (len < spec_.payload_min || len > spec_.payload_max) {
        return StoreResult::kBadLength;
    }

    uint8_t sha[kShaBytes];
    Sha256(payload, len, sha);

    // A caller-declared digest that does not match means the bytes we received
    // are not the bytes that were sent. Refuse rather than persist them.
    if (expected_sha != nullptr && !ConstantTimeEquals(sha, expected_sha, kShaBytes)) {
        return StoreResult::kBadLength;
    }

    // An identical payload is a no-op: the same document pushed twice costs no
    // write, which is what makes a tower's retry free.
    if (active_ >= 0 && status_[active_].payload_len == len &&
        ConstantTimeEquals(sha, status_[active_].sha, kShaBytes)) {
        return StoreResult::kDuplicate;
    }

    // Target the slot we are not currently displaying from. When nothing is
    // valid yet, slot 0 is as good as any.
    const int target = (active_ < 0) ? 0 : (1 - active_);
    const uint32_t seq = (active_ < 0) ? 1u : status_[active_].seq + 1u;

    uint8_t* raw = scratch_;
    memset(raw, 0, kHeaderBytes);
    memcpy(raw + kOffMagic, spec_.magic, sizeof(spec_.magic));
    StoreU32(raw + kOffVersion, kRecordVersion);
    StoreU32(raw + kOffSeq, seq);
    StoreU32(raw + kOffPayloadLen, static_cast<uint32_t>(len));
    // Reserved, and written as zero by every build that has ever written one of
    // these headers. Kept in the layout rather than repurposed so the two
    // stores' records stay field-for-field the same shape.
    StoreU32(raw + kOffFlags, 0);
    memcpy(raw + kOffSha, sha, kShaBytes);
    StoreU32(raw + kOffSourceEpoch, source_epoch);
    StoreU32(raw + kOffHeaderCrc, Crc32(raw, kOffHeaderCrc));
    memcpy(raw + kHeaderBytes, payload, len);

    if (!io_->WriteSlot(target, raw, kHeaderBytes + len)) {
        return StoreResult::kWriteFailed;
    }

    // Read-back verification. A filesystem that returns success on a short
    // write is exactly the failure mode this whole design exists to survive,
    // so "the write call returned true" is not accepted as proof.
    //
    // The read-back reuses the same scratch buffer, overwriting the record we
    // just built. That is safe precisely because nothing below trusts it: the
    // expected seq and digest are held in locals, and the bytes coming back
    // must re-derive them on their own.
    const int n = io_->ReadSlot(target, raw, spec_.scratch_bytes());
    if (n <= 0) {
        return StoreResult::kVerifyFailed;
    }
    SlotStatus verified;
    if (!ParseRecord(raw, static_cast<size_t>(n), &verified)) {
        return StoreResult::kVerifyFailed;
    }
    if (verified.seq != seq || verified.payload_len != len ||
        !ConstantTimeEquals(verified.sha, sha, kShaBytes)) {
        return StoreResult::kVerifyFailed;
    }

    status_[target] = verified;
    active_ = target;
    return StoreResult::kOk;
}

}  // namespace record
