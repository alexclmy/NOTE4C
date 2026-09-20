/**
 * @file voice_transport.h
 * @brief Portable upload protocol state machine. No sockets, no ESP-IDF.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * What this owns
 * --------------
 * The rules about *when* to send, how many times, and what a given answer
 * means. It never touches a socket: the glue calls Begin(), performs one
 * attempt however it likes, and reports the outcome back with
 * OnAttemptResult(). Time is injected, so a twenty-second retry ladder is a
 * test that runs in microseconds.
 *
 * Identity is fixed for the whole utterance
 * -----------------------------------------
 * One `request_id` and one `idempotency_key`, minted once in Begin() and
 * **constant across every retry**. This is the property that makes a retry
 * safe: `hub/CONTRACTS.md` section 1 answers a repeated delivery with the
 * stored result and `duplicate: true` rather than running the pipeline again.
 *
 * Minting a fresh `request_id` per attempt would be the obvious alternative
 * and is the wrong one here. The contract treats one delivery id with two
 * different idempotency keys as a `409`, and treats a new delivery id with a
 * known key as a duplicate of it — that path exists for a device that lost its
 * state between boots, not for a retry inside one utterance, where the device
 * still knows exactly what it is retrying.
 *
 * What a retry can and cannot fix
 * -------------------------------
 * Retryable: the transport failed, the attempt timed out, the hub answered
 * 5xx, or the hub answered 429. Every one of those may succeed a second later.
 *
 * Not retryable: 400, 401, 409, 413 and any other 4xx. A 401 is a wrong token
 * and will be wrong three times; a 413 is a body that is too large and will be
 * too large three times. Retrying them turns one clear log line into three and
 * delays the error the user is waiting for.
 *
 * What cancellation does not do
 * -----------------------------
 * Cancel abandons the attempt in flight. It does not ask the hub to undo
 * anything, because the hub has no such route and inventing one would suggest
 * an utterance can be recalled after it arrives. If the upload had already
 * reached the hub, the hub has it; what cancelling buys is that the device
 * stops waiting and does not play the answer.
 */

#ifndef COMMON_VOICE_TRANSPORT_H
#define COMMON_VOICE_TRANSPORT_H

#include <stdint.h>

#include <functional>

namespace voice {

/// Canonical UUID text is 36 characters plus a terminator.
constexpr size_t kRequestIdChars = 37;
/// Long enough for "utterance-" plus a UUID, with room to spare.
constexpr size_t kIdempotencyKeyChars = 64;

enum class TransportState {
    kIdle = 0,
    /// An attempt is in flight. The glue owes exactly one OnAttemptResult().
    kSending,
    /// Waiting out the backoff before the next attempt.
    kBackoff,
    kSucceeded,
    kFailed,
    kCancelled,
};

/// What one attempt produced, as classified by the glue.
enum class Outcome {
    /// The hub answered 200 and the body was accepted.
    kSuccess = 0,
    /// Worth trying again: transport error, timeout, 5xx, 429.
    kRetryable,
    /// Trying again cannot help: 400, 401, 409, 413, other 4xx.
    kPermanent,
};

/// Why the machine ended where it did. Reported, never inferred from the state.
enum class FailureKind {
    kNone = 0,
    kAttemptsExhausted,
    kPermanentRejection,
    kCancelled,
    /// The whole ladder outlived its budget. Distinct from a single attempt
    /// timing out, which is merely retryable.
    kDeadlineExceeded,
};

struct TransportConfig {
    /**
     * @brief Attempts, including the first.
     *
     * Three, with a 6 s attempt timeout and 1 s of backoff, is a worst case of
     * 6 + 1 + 6 + 1 + 6 = 20 s. `audio_ui::Config::response_timeout_ms` is 25 s
     * so that the state machine above never reports a timeout while this one
     * is still legitimately retrying.
     */
    uint8_t max_attempts = 3;
    uint32_t attempt_timeout_ms = 6000;
    uint32_t backoff_ms = 1000;
    /// Hard stop for the whole ladder, whatever the arithmetic above says.
    uint32_t total_deadline_ms = 22000;
};

struct Identity {
    char request_id[kRequestIdChars];
    char idempotency_key[kIdempotencyKeyChars];
};

struct TransportHooks {
    /// Perform one attempt. @p attempt is 1-based, for logs.
    std::function<void(const Identity&, uint8_t attempt)> send;
    /// Abandon the attempt in flight. Called on cancel and on attempt timeout.
    std::function<void()> abort;
    /// The ladder finished. @p ok is true only for kSucceeded.
    std::function<void(bool ok)> finished;
};

/**
 * @brief Classify an HTTP status into an Outcome.
 *
 * Pure, so the table is testable without a server. A status of 0 means the
 * request never got an answer, which is retryable.
 */
Outcome ClassifyHttpStatus(int status);

class VoiceTransport {
public:
    VoiceTransport() = default;

    void SetConfig(const TransportConfig& config) { config_ = config; }
    void SetHooks(TransportHooks hooks) { hooks_ = std::move(hooks); }

    /**
     * @brief Start uploading one utterance.
     *
     * @return false when an upload is already in flight. One utterance at a
     *         time is the whole queue policy: the state machine above cannot
     *         start a second recording while it is waiting, so a second Begin()
     *         means something has gone wrong and starting it would leave the
     *         first upload with nobody to report to.
     */
    bool Begin(const Identity& identity, uint64_t now_ms);

    /// Report what the attempt in flight produced.
    void OnAttemptResult(Outcome outcome, uint64_t now_ms);

    /// Advance time: fires attempt timeouts, backoff expiry and the deadline.
    void Tick(uint64_t now_ms);

    /// Abandon everything. Safe to call in any state.
    void Cancel(uint64_t now_ms);

    /// Return to kIdle so the next utterance can start. Does nothing while an
    /// attempt is in flight, because the glue still owes a result.
    void Release();

    TransportState state() const { return state_; }
    FailureKind failure() const { return failure_; }
    uint8_t attempts_made() const { return attempts_made_; }
    const Identity& identity() const { return identity_; }
    bool busy() const {
        return state_ == TransportState::kSending || state_ == TransportState::kBackoff;
    }

    const TransportConfig& config() const { return config_; }

private:
    void StartAttempt(uint64_t now_ms);
    void Finish(TransportState state, FailureKind failure);

    TransportConfig config_{};
    TransportHooks hooks_{};
    TransportState state_ = TransportState::kIdle;
    FailureKind failure_ = FailureKind::kNone;
    Identity identity_{};
    uint8_t attempts_made_ = 0;
    uint64_t attempt_started_ms_ = 0;
    uint64_t backoff_until_ms_ = 0;
    uint64_t deadline_ms_ = 0;
};

const char* TransportStateName(TransportState state);
const char* FailureKindName(FailureKind kind);
const char* OutcomeName(Outcome outcome);

}  // namespace voice

#endif  // COMMON_VOICE_TRANSPORT_H
