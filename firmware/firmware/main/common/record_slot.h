/**
 * @file record_slot.h
 * @brief The power-loss-safe A/B store for variable-length records. The
 *        autonomy profile's store, and nothing else's.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * What this is, and what it deliberately is not
 * ---------------------------------------------
 * The profile needs the guarantee the frame store already has: that a power cut
 * at any byte offset leaves either the previous complete record or the new
 * complete record, never a torn one. That guarantee does not come from the idea
 * of A/B slots, it comes from the details — write the inactive slot only, read
 * it back, re-validate it, and let the checksum rather than the filesystem
 * decide whether the record is there.
 *
 * So the details are reproduced here, in the same shape and the same order as
 * dashboard_slot.cc, with the frame's fixed assumptions turned into a spec: the
 * magic, and the payload's bounds. What is NOT done is refactoring the frame
 * store to sit on top of this one. The frame store is the code that holds the
 * panel, it is validated on hardware, and its 493 write-cut cases drive that
 * file. Rewriting it to gain a shared base class would put a profile feature's
 * edit inside the path that draws the glass, for a saving of a few kilobytes of
 * flash. The duplication is the cheaper risk, and it is the deliberate choice
 * rather than an oversight: SHA-256, CRC-32 and the header layout exist twice
 * in this firmware, in two translation units that must agree byte for byte and
 * that are each tested against their own fixtures.
 *
 * The record layout is the frame's, field for field, because there is no reason
 * for it to differ and because the two stores share a filesystem. The magic
 * keeps them apart: a frame in a profile slot fails validation and reads as "no
 * profile" rather than as a profile of garbage.
 *
 * What is deliberately NOT in the spec
 * ------------------------------------
 * The header layout, the slot count and the digest. Two slots is what makes
 * "never touch the active one" hold, not a tunable. The 64-byte header with its
 * CRC-32 over the fields and SHA-256 over the payload is what a record *is*
 * here; a record type wanting a different one would be a different store.
 *
 * Free of ESP-IDF headers on purpose: the host suite compiles this translation
 * unit rather than a model of it.
 */

#ifndef COMMON_RECORD_SLOT_H
#define COMMON_RECORD_SLOT_H

#include <stddef.h>
#include <stdint.h>

namespace record {

/// Size of the record header that precedes the payload in each slot.
constexpr size_t kHeaderBytes = 64;

/// Number of A/B slots. Two is what makes the "never touch the active one"
/// property hold; this is not a tunable.
constexpr int kSlotCount = 2;

/// Bytes in a SHA-256 digest.
constexpr size_t kShaBytes = 32;

/// Record format version understood by this build.
constexpr uint32_t kRecordVersion = 1;

/**
 * @brief What makes one record type different from another.
 *
 * `payload_min == payload_max` is the fixed-size case, where any other length is
 * a corrupt record rather than a shorter payload. The profile is the variable
 * case, bounded above by what the device will parse.
 */
struct RecordSpec {
    /// Eight bytes, compared exactly. A profile record in a frame slot fails
    /// this and is ignored, which is the cross-type protection that makes it
    /// safe for both stores to live on the same filesystem.
    uint8_t magic[8];
    size_t payload_min;
    size_t payload_max;

    /// Scratch a RecordSlot with this spec needs.
    size_t scratch_bytes() const { return kHeaderBytes + payload_max; }
};

/**
 * @brief Byte-level access to the two slots.
 *
 * Kept abstract so the device implementation (SPIFFS) and the host test
 * implementation (memory, with fault injection) drive identical logic.
 */
class SlotIo {
public:
    virtual ~SlotIo() = default;

    /**
     * @brief Read up to @p max bytes of slot @p slot into @p buf.
     * @return bytes read, 0 if the slot does not exist, negative on error.
     */
    virtual int ReadSlot(int slot, uint8_t* buf, size_t max) = 0;

    /**
     * @brief Replace the entire contents of slot @p slot with @p len bytes.
     *
     * Implementations must not be trusted to be atomic; the caller verifies.
     * @return true if the implementation believes the write succeeded.
     */
    virtual bool WriteSlot(int slot, const uint8_t* data, size_t len) = 0;
};

/// Outcome of a Store() call. Distinguishes "we chose not to write" from
/// "we tried and it did not stick", because the HTTP layer reports them
/// differently and callers must not conflate the two.
enum class StoreResult {
    kOk = 0,          ///< New payload written and verified by read-back.
    kDuplicate,       ///< Payload identical to the active one; nothing written.
    kBadLength,       ///< Payload length outside the spec, or digest mismatch.
    kWriteFailed,     ///< The underlying write reported failure.
    kVerifyFailed,    ///< Write reported success but read-back did not validate.
};

/// What a single slot looks like after validation.
struct SlotStatus {
    bool valid = false;
    uint32_t seq = 0;
    uint32_t source_epoch = 0;
    /// Payload length this record actually holds: the real JSON length for a
    /// profile, rather than the capacity the slot allows.
    uint32_t payload_len = 0;
    uint8_t sha[kShaBytes] = {};
};

/**
 * @brief The A/B record store.
 *
 * Not internally locked. The owner serialises access; adding a mutex here
 * would hide that ownership rather than clarify it.
 */
class RecordSlot {
public:
    /**
     * @param spec        what kind of record this store holds.
     * @param io          slot storage backend.
     * @param scratch     working buffer of at least spec.scratch_bytes(),
     *                    owned by the caller and reused for every operation.
     * @param scratch_len size of @p scratch.
     *
     * The scratch buffer is supplied rather than held internally so the device
     * can place a frame's 30 KB in PSRAM instead of burning internal SRAM, and
     * so it is obvious that concurrent calls on one instance are not allowed.
     */
    RecordSlot(const RecordSpec& spec, SlotIo* io, uint8_t* scratch, size_t scratch_len)
        : spec_(spec),
          io_(io),
          scratch_(scratch_len >= spec.scratch_bytes() ? scratch : nullptr) {}

    /**
     * @brief Validate both slots and select the active one.
     *
     * Safe to call repeatedly. Must be called before any other accessor.
     * @return true if at least one slot holds a valid record.
     */
    bool Load();

    /// True when a valid record is available.
    bool has_record() const { return active_ >= 0; }

    /// Index of the active slot, or -1 when no slot is valid.
    int active_slot() const { return active_; }

    uint32_t active_seq() const;
    uint32_t active_source_epoch() const;
    uint32_t active_payload_len() const;

    /**
     * @brief SHA-256 of the active payload.
     * @return pointer to kShaBytes, or nullptr when there is no active record.
     */
    const uint8_t* active_sha() const;

    /// Per-slot validation result, for status reporting and diagnostics.
    const SlotStatus& slot_status(int slot) const { return status_[slot]; }

    /**
     * @brief Copy the active payload into @p out.
     *
     * @param out     buffer of at least @p cap bytes.
     * @param cap     capacity; the call fails rather than truncating.
     * @param len_out set to the payload length on success.
     * @return true on success; false when no valid record exists, @p cap is
     *         too small, or the slot failed re-validation since Load().
     */
    bool ReadPayload(uint8_t* out, size_t cap, size_t* len_out) const;

    /**
     * @brief Re-read and validate the active record, and hand back a pointer
     *        into the scratch buffer rather than copying.
     *
     * For the one caller that needs to parse a payload into a structure whose
     * own storage is the size of the payload — the autonomy profile, which
     * interns its strings into an arena as large as the document. Copying the
     * document into that arena and parsing it in place cannot work: the parse
     * clears the structure before it starts, which would erase its own input.
     *
     * The returned pointer is valid until the next call on this slot, because
     * it is the scratch buffer. Callers must not hold it across a Store().
     *
     * @return false when nothing valid is stored.
     */
    bool ReadPayloadInPlace(const uint8_t** data, size_t* len_out);

    /**
     * @brief Persist a new payload into the inactive slot.
     *
     * The active slot is left untouched for the whole call.
     *
     * @param payload      the bytes.
     * @param len          must be within the spec's bounds.
     * @param source_epoch informational timestamp from whoever produced it.
     * @param expected_sha optional caller-supplied digest; when non-null the
     *                     payload must hash to it or kBadLength is returned.
     *                     This is what turns a truncated upload into a refusal
     *                     rather than a confidently stored wrong record.
     *
     * A payload identical to the active one returns kDuplicate and writes
     * nothing. That is the flash-wear guarantee the profile push leans on: a
     * tower that re-pushes the same document costs no writes at all.
     */
    StoreResult Store(const uint8_t* payload,
                      size_t len,
                      uint32_t source_epoch,
                      const uint8_t* expected_sha = nullptr);

private:
    // Validates raw slot bytes against the spec; fills out on success.
    bool ParseRecord(const uint8_t* raw, size_t len, SlotStatus* out) const;

    RecordSpec spec_;
    SlotIo* io_ = nullptr;
    uint8_t* scratch_ = nullptr;   ///< nullptr when the caller gave too small a buffer
    SlotStatus status_[kSlotCount];
    int active_ = -1;
};

/// SHA-256 over @p len bytes of @p data into @p out (kShaBytes).
/// Self-contained on purpose: device and host must hash identically, so this
/// must not resolve to mbedtls on one side and something else on the other.
void Sha256(const uint8_t* data, size_t len, uint8_t* out);

/// CRC-32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF) over @p len bytes.
uint32_t Crc32(const uint8_t* data, size_t len);

/// Constant-time comparison of @p len bytes. Used for tokens and digests so
/// that a comparison does not leak how many leading bytes matched.
bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace record

#endif  // COMMON_RECORD_SLOT_H
