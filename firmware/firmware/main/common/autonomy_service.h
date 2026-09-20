/**
 * @file autonomy_service.h
 * @brief The PUT and GET semantics of /api/v1/autonomy/profile, with no HTTP
 *        in them.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Why this is a separate thing from the route
 * -------------------------------------------
 * Everything that can go wrong with a profile push is a decision: is the token
 * right, is this a replay of a key we have already answered, does the revision
 * go backwards, does the document parse, did the write stick, do the bytes read
 * back identical. None of those needs a socket, and all of them are the part
 * that would be wrong.
 *
 * So they live here, behind an injected SlotIo and an injected token, and the
 * host suite drives every one of them — including the write that fails and the
 * read-back that comes back different. `dashboard_api.cc` is left with header
 * parsing and status codes.
 *
 * This is the same split `device_config_service` already uses, and it is why
 * the config API's closed table could be tested without a board.
 *
 * The rules, and why each is what it is
 * -------------------------------------
 *  - **Constant-time token comparison.** Reused from the record store, so a
 *    comparison cannot leak how many leading bytes matched.
 *  - **A failure lockout.** Ten failures buys sixty seconds of refusal, the
 *    same shape the frame route already uses. Without it the token is a
 *    sixty-four-character secret being guessed at LAN speed.
 *  - **An idempotency ring of eight.** A tower that retried a PUT it never saw
 *    the answer to must not push the revision twice; it gets the original
 *    answer back, marked as a replay.
 *  - **CAS on revision, not on digest.** A revision that is not greater than
 *    the stored one is refused with 409. That is what makes a retry safe and a
 *    stale tower harmless in the same rule.
 *  - **Validate before persist.** ParseProfile runs to completion before a byte
 *    is written. A stored document that cannot be drawn is a panel that fails
 *    at 3am on battery instead of at the moment somebody was looking.
 *  - **Read back and compare.** The slot verifies its own write; this compares
 *    the digest it reports against the one computed from the body received.
 *
 * Free of ESP-IDF headers, by design and by test.
 */

#ifndef COMMON_AUTONOMY_SERVICE_H
#define COMMON_AUTONOMY_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include <mutex>

#include "autonomy_profile.h"
#include "record_slot.h"

namespace autonomy {

/// Keys remembered for replay detection. Eight, matching the frame route's ring.
constexpr size_t kIdempotencyRing = 8;
constexpr size_t kIdempotencyKeyMax = 64;

/// Failures before the lockout, and how long it lasts. Same shape as the frame
/// route's, because a caller should not have to learn two rules.
constexpr int kMaxAuthFailures = 10;
constexpr int64_t kLockoutMs = 60000;

/**
 * @brief What a delegated token check can answer.
 *
 * Deliberately three values and not a bool. "Nobody has paired this device"
 * and "that is the wrong token" are different facts about the world, the tower
 * renders them differently, and collapsing them here would mean the profile
 * route telling an unpaired owner their token was wrong.
 *
 * Mirrors dashboard::AuthResult without depending on it, so this translation
 * unit stays free of the frame store and the host suite keeps compiling it
 * alone.
 */
enum class AuthVerdict : uint8_t {
    kOk = 0,
    kNotProvisioned,
    kBadToken,
};

/// Answers "is this the device's token". @p ctx is passed back untouched.
using TokenVerifier = AuthVerdict (*)(const char* token, void* ctx);

enum class PutOutcome : uint8_t {
    kAccepted = 0,      ///< parsed, stored, read back identical
    kReplay,            ///< this Idempotency-Key was already answered
    kNotProvisioned,    ///< no token installed; pair the device first
    kUnauthorized,
    kLockedOut,
    kTooLarge,          ///< past kProfileMaxBytes
    kInvalid,           ///< failed the closed table; `error` names the field
    kRevisionConflict,  ///< revision did not advance past the stored one
    kShaMismatch,       ///< body does not match the declared digest
    kStoreFailed,       ///< the write did not stick, or read back different
};

const char* PutOutcomeName(PutOutcome outcome);

struct PutResult {
    PutOutcome outcome = PutOutcome::kStoreFailed;
    bool accepted = false;
    bool persisted = false;
    bool replay = false;
    int32_t revision = 0;
    /// Lowercase hex of what is now stored, or of what was already stored.
    char sha256[65] = {};
    /// Only meaningful when outcome is kInvalid.
    ParseError error;
};

struct GetResult {
    bool present = false;
    size_t bytes = 0;
    char sha256[65] = {};
    int32_t revision = 0;
};

/**
 * @brief The profile store and its wire rules.
 *
 * TWO TASKS REACH THIS, AND THAT IS WHY THERE IS A LOCK
 * ----------------------------------------------------
 * It used to say "not internally locked; the API task owns it", and while this
 * was only a store served by a route that was true. It stopped being true when
 * the wake cycle started composing from the profile: the HTTP task can be part
 * way through `Put()` — which parses an incoming 18 KB document straight into
 * the live `Profile` — while the main task is walking that same object to
 * decide what to draw. The failure is not theoretical; `Put()` deliberately
 * uses one buffer for the live profile and the staging area, so a reader during
 * a push sees a half-parsed document.
 *
 * So mutation and reading are both under `mutex_`, and a reader takes a
 * `ProfileLock` for as long as it needs the object to hold still. The lock is
 * never held across a network call or a flash erase of the frame store: the
 * only flash it covers is the profile slot's own write, which is bounded and
 * which `Put()` has to finish before anyone may see the result anyway.
 */
class AutonomyService {
public:
    /**
     * @param io      the two profile slots.
     * @param scratch at least kProfileRecordBytes, caller-owned, PSRAM.
     * @param parsed  where the live parsed profile is kept. Caller-owned
     *                because it is 18 KB and the caller decides where that
     *                lives.
     */
    AutonomyService(record::SlotIo* io, uint8_t* scratch, size_t scratch_len,
                    Profile* parsed);

    /// Adopt whatever survived the last power cut. Safe to call repeatedly.
    /// @return true when a valid profile is now loaded.
    bool Load();

    /// Install the pairing token. Null or empty means unprovisioned.
    ///
    /// Used by the host suite, which has no DashboardManager to ask. On the
    /// device this is not called at all: see SetTokenVerifier.
    void SetToken(const char* token);

    /**
     * @brief Delegate "is this the token" to whoever owns the secret.
     *
     * The device has exactly one dashboard token and it lives in
     * DashboardManager's FrameAuth. A copy of it in here would be a second
     * secret that goes stale the moment somebody re-pairs from the Settings
     * menu — and that is precisely the moment it must not: the operator
     * re-pairs *because* they think the old token is compromised, and a
     * profile route still honouring it would be a hole opened by the act meant
     * to close one.
     *
     * So the verifier answers the yes/no and this service keeps everything
     * else. The failure budget stays here deliberately: the lockout and the
     * idempotency ring are what this route's tests pin, and a delegated
     * yes/no cannot carry them.
     *
     * Passing nullptr restores the internal token, which is what the host
     * suite uses and what a device with no manager falls back to.
     */
    void SetTokenVerifier(TokenVerifier verifier, void* ctx);

    /**
     * @brief Hold the profile still for as long as you are reading it.
     *
     * The only supported way for a task other than the one serving the API to
     * look at the parsed profile. `has_profile()` and `profile()` below are the
     * unlocked accessors the API path and the host suite use, and reading the
     * 18 KB object through them from the main task is the race described above.
     */
    class ProfileLock {
    public:
        explicit ProfileLock(const AutonomyService& service)
            : service_(service), guard_(service.mutex_) {}

        bool has_profile() const { return service_.has_profile_; }
        const Profile& profile() const { return *service_.parsed_; }
        int32_t revision() const { return service_.revision_; }
        const char* sha256() const { return service_.sha_hex_; }
        int64_t applied_epoch() const { return service_.applied_epoch_; }
        bool applied_epoch_known() const { return service_.applied_epoch_ > 0; }

    private:
        const AutonomyService& service_;
        std::lock_guard<std::mutex> guard_;
    };

    /// Unlocked. See ProfileLock: safe from the task that serves the API, and
    /// from a host test, and not from the wake cycle.
    bool has_profile() const { return has_profile_; }
    const Profile& profile() const { return *parsed_; }
    int32_t revision() const { return revision_; }
    const char* sha256() const { return sha_hex_; }
    /// When the stored profile was applied, or 0 when the clock was unset then.
    int64_t applied_epoch() const { return applied_epoch_; }
    bool applied_epoch_known() const { return applied_epoch_ > 0; }

    /**
     * @brief Handle a PUT.
     *
     * @param body,len       the exact bytes received.
     * @param token          X-Auth-Token, or null.
     * @param declared_sha   X-Profile-Sha256, or null to skip the check.
     * @param idempotency_key Idempotency-Key, or null.
     * @param now_ms         monotonic milliseconds, for the lockout.
     * @param now_epoch      UTC seconds, or 0 when the clock is not set.
     */
    PutResult Put(const char* body, size_t len, const char* token,
                  const char* declared_sha, const char* idempotency_key,
                  int64_t now_ms, int64_t now_epoch);

    /**
     * @brief Handle a GET: the stored bytes, verbatim.
     *
     * Verbatim is the whole point. The tower compares what it pushed against
     * what comes back, byte for byte, and a re-serialisation here would make
     * that comparison a test of this device's JSON writer instead of a test of
     * what it is holding.
     */
    GetResult Get(uint8_t* out, size_t cap) const;

    /// True when the caller is authorised. Exposed so the GET route can refuse
    /// with the same rules as the PUT route rather than its own.
    bool Authorise(const char* token, int64_t now_ms, PutOutcome* why);

private:
    void RememberAnswer(const char* key, const PutResult& result);
    const PutResult* FindAnswer(const char* key) const;
    /// The bodies of Load() and Authorise(), for callers already holding
    /// `mutex_`. std::mutex is not recursive and Put() needs both.
    bool LoadLocked();
    bool AuthoriseLocked(const char* token, int64_t now_ms, PutOutcome* why);

    /// Guards everything below. Mutable so a const read can take it.
    mutable std::mutex mutex_;

    record::RecordSlot slot_;
    /**
     * The live profile, and also the staging area for an incoming one.
     *
     * Deliberately one buffer rather than two. A second Profile is 18 KB held
     * for the whole life of the device to serve a path that runs when somebody
     * pushes, and the failure it would prevent is recoverable for free: an
     * incoming document is parsed into here, and if anything after that refuses
     * it, Load() re-reads and re-parses what is still in the slot. The slot is
     * untouched until every check has passed, so the restore cannot fail for
     * any reason the original load would not have failed for too.
     */
    Profile* parsed_ = nullptr;

    char token_[65] = {};
    bool provisioned_ = false;
    TokenVerifier verifier_ = nullptr;
    void* verifier_ctx_ = nullptr;
    int auth_failures_ = 0;
    int64_t locked_until_ms_ = 0;

    bool has_profile_ = false;
    int32_t revision_ = 0;
    char sha_hex_[65] = {};
    int64_t applied_epoch_ = 0;

    struct Answer {
        char key[kIdempotencyKeyMax] = {};
        bool used = false;
        PutResult result;
    };
    Answer ring_[kIdempotencyRing];
    size_t ring_next_ = 0;
};

/// Lowercase hex of a digest, into a 65-byte buffer.
void ShaToHex(const uint8_t* digest, char* out);

}  // namespace autonomy

#endif  // COMMON_AUTONOMY_SERVICE_H
