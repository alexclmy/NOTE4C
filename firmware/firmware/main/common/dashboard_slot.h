/**
 * @file dashboard_slot.h
 * @brief Power-loss-safe A/B store for the single dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Design notes (deliberate, please read before "simplifying"):
 *
 * SPIFFS gives us no transactional guarantee. `rename()` over an existing file
 * is NOT atomic there, and there is no way to update a data file and a metadata
 * file as one unit. So this module does not rely on rename, on ordering between
 * two files, or on the filesystem reporting write errors truthfully.
 *
 * Instead: two fixed slots, each holding one self-describing, self-verifying
 * record (magic + version + sequence + length + SHA-256 of the payload +
 * CRC-32 of the header). A write only ever targets the slot that is *not*
 * currently active, so the frame being displayed is never in the blast radius.
 * After writing we read the slot back and re-validate it before declaring
 * success. On boot, both slots are validated independently and the valid one
 * with the highest sequence number wins.
 *
 * Consequence: a power cut at any byte offset leaves either the previous
 * complete frame or the new complete frame active, never a torn one, because a
 * partially written slot fails its own checksum and is simply ignored.
 *
 * This file is intentionally free of ESP-IDF dependencies so the host test
 * suite compiles and exercises this exact translation unit rather than a
 * re-implementation of it.
 */

#ifndef COMMON_DASHBOARD_SLOT_H
#define COMMON_DASHBOARD_SLOT_H

#include <stddef.h>
#include <stdint.h>

namespace dashboard {

/// 400x300 pixels, 2 bits per pixel, 4 pixels per byte.
constexpr size_t kFrameBytes = 30000;

/// Size of the record header that precedes the payload in each slot.
constexpr size_t kHeaderBytes = 64;

/// Total bytes one slot occupies once written.
constexpr size_t kRecordBytes = kHeaderBytes + kFrameBytes;

/// Number of A/B slots. Two is what makes the "never touch the active one"
/// property hold; this is not a tunable.
constexpr int kSlotCount = 2;

/// Bytes in a SHA-256 digest.
constexpr size_t kShaBytes = 32;

/// Record format version understood by this build.
constexpr uint32_t kRecordVersion = 1;

/**
 * @brief Flag bits carried in the header's `flags` word, at offset 20.
 *
 * Bit 0 says the payload in this slot was composed by the device rather than
 * pushed by the tower. It lives in the header rather than being inferred,
 * because the tower asks "what is on the glass" after a reboot, and a device
 * that had to guess would answer with the more flattering of the two.
 *
 * WHY THIS IS NOT A CHANGE TO THE RECORD FORMAT
 * ---------------------------------------------
 * The word was already there. Every build since the store was written has
 * reserved offset 20 and stored zero into it, under the header CRC, and every
 * reader has ignored it. So a record written by the hardware-validated build
 * reads back here with flags == 0, i.e. origin = tower, which is exactly what
 * it was; and a record written by this build is parsed by the older one
 * unchanged, because the CRC covers the word either way.
 *
 * Nothing else about the layout, the digest, the A/B discipline or the
 * write-inactive-then-verify sequence moves. That is deliberate: this store
 * holds the panel and was validated on hardware, and the provenance report is
 * not worth a structural edit to it.
 */
constexpr uint32_t kFlagOriginLocal = 1u << 0;

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
    kOk = 0,          ///< New frame written and verified by read-back.
    kDuplicate,       ///< Payload identical to the active frame; nothing written.
    kBadLength,       ///< Payload was not exactly kFrameBytes.
    kWriteFailed,     ///< The underlying write reported failure.
    kVerifyFailed,    ///< Write reported success but read-back did not validate.
};

/// What a single slot looks like after validation.
struct SlotStatus {
    bool valid = false;
    uint32_t seq = 0;
    uint32_t source_epoch = 0;
    /// Header flags word, including kFlagOriginLocal. Zero for every record
    /// written before this build, which reads as "pushed by the tower".
    uint32_t flags = 0;
    uint8_t sha[kShaBytes] = {};
};

/**
 * @brief The dashboard frame store.
 *
 * Not internally locked. The refresh state machine owns it and serialises
 * access; adding a mutex here would hide that ownership rather than clarify it.
 */
class DashboardSlot {
public:
    /**
     * @param io      slot storage backend.
     * @param scratch working buffer of at least kRecordBytes, owned by the
     *                caller and reused for every operation.
     * @param scratch_len size of @p scratch.
     *
     * The scratch buffer is supplied rather than held internally so the device
     * can place these 30 KB in PSRAM instead of burning internal SRAM, and so
     * it is obvious that concurrent calls on one instance are not allowed.
     */
    DashboardSlot(SlotIo* io, uint8_t* scratch, size_t scratch_len)
        : io_(io),
          scratch_(scratch_len >= kRecordBytes ? scratch : nullptr) {}

    /**
     * @brief Validate both slots and select the active one.
     *
     * Safe to call repeatedly. Must be called before any other accessor.
     * @return true if at least one slot holds a valid frame.
     */
    bool Load();

    /// True when a valid frame is available to display.
    bool has_frame() const { return active_ >= 0; }

    /// Index of the active slot, or -1 when no slot is valid.
    int active_slot() const { return active_; }

    /// Sequence number of the active frame, 0 when there is none.
    uint32_t active_seq() const;

    /// Source timestamp recorded by the pusher, 0 when there is none.
    uint32_t active_source_epoch() const;

    /// Header flags of the active record, 0 when there is none. Test
    /// kFlagOriginLocal against it to learn where the stored frame came from.
    uint32_t active_flags() const;

    /**
     * @brief SHA-256 of the active payload.
     * @return pointer to kShaBytes, or nullptr when there is no active frame.
     */
    const uint8_t* active_sha() const;

    /// Per-slot validation result, for status reporting and diagnostics.
    const SlotStatus& slot_status(int slot) const { return status_[slot]; }

    /**
     * @brief Copy the active payload into @p out.
     * @param out buffer of at least kFrameBytes.
     * @return true on success; false when no valid frame exists or the slot
     *         failed re-validation since Load() (e.g. flash decayed).
     */
    bool ReadFrame(uint8_t* out) const;

    /**
     * @brief Persist a new frame into the inactive slot.
     *
     * The active slot is left untouched for the whole call.
     *
     * @param payload      frame bytes.
     * @param len          must equal kFrameBytes.
     * @param source_epoch informational timestamp from the pusher.
     * @param expected_sha optional caller-supplied digest; when non-null the
     *                     payload must hash to it or kBadLength is returned.
     *                     This is what turns a truncated upload into a refusal
     *                     rather than a confidently stored wrong frame.
     * @param flags        header flags, e.g. kFlagOriginLocal. Last and
     *                     defaulted so every existing caller — all of which
     *                     store a pushed frame — keeps writing the zero it
     *                     already wrote.
     */
    StoreResult Store(const uint8_t* payload,
                      size_t len,
                      uint32_t source_epoch,
                      const uint8_t* expected_sha = nullptr,
                      uint32_t flags = 0);

private:
    // Validates raw slot bytes; fills out on success.
    static bool ParseRecord(const uint8_t* raw, size_t len, SlotStatus* out);

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

}  // namespace dashboard

#endif  // COMMON_DASHBOARD_SLOT_H
