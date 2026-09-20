/**
 * @file voice_transport.cc
 * @brief Implementation of the portable upload protocol state machine.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "voice_transport.h"

#include <string.h>

namespace voice {

Outcome ClassifyHttpStatus(int status) {
    if (status == 0) {
        // No answer at all: the connection failed, or the attempt timed out
        // before a status line arrived. Nothing is known about whether the hub
        // saw it, which is exactly what the idempotency key is for.
        return Outcome::kRetryable;
    }
    if (status >= 200 && status < 300) {
        return Outcome::kSuccess;
    }
    if (status == 429) {
        return Outcome::kRetryable;
    }
    if (status >= 500) {
        return Outcome::kRetryable;
    }
    if (status >= 300 && status < 400) {
        // The hub does not redirect. Something else answered, or something is
        // in the middle, and following it would send household speech to an
        // address nobody configured.
        return Outcome::kPermanent;
    }
    return Outcome::kPermanent;
}

void VoiceTransport::Finish(TransportState state, FailureKind failure) {
    state_ = state;
    failure_ = failure;
    if (hooks_.finished) hooks_.finished(state == TransportState::kSucceeded);
}

void VoiceTransport::StartAttempt(uint64_t now_ms) {
    ++attempts_made_;
    attempt_started_ms_ = now_ms;
    state_ = TransportState::kSending;
    if (hooks_.send) hooks_.send(identity_, attempts_made_);
}

bool VoiceTransport::Begin(const Identity& identity, uint64_t now_ms) {
    if (busy()) {
        return false;
    }
    identity_ = identity;
    // Defensive: the glue builds these from a UUID formatter, but a missing
    // terminator here would be read past the end of the struct by every
    // logging and header call downstream.
    identity_.request_id[kRequestIdChars - 1] = '\0';
    identity_.idempotency_key[kIdempotencyKeyChars - 1] = '\0';
    attempts_made_ = 0;
    failure_ = FailureKind::kNone;
    backoff_until_ms_ = 0;
    deadline_ms_ = now_ms + config_.total_deadline_ms;
    if (config_.max_attempts == 0) {
        // A configuration that permits no attempts is a configuration error,
        // not a silent success. Fail it here rather than sit in kSending with
        // nothing in flight.
        Finish(TransportState::kFailed, FailureKind::kAttemptsExhausted);
        return true;
    }
    StartAttempt(now_ms);
    return true;
}

void VoiceTransport::OnAttemptResult(Outcome outcome, uint64_t now_ms) {
    if (state_ != TransportState::kSending) {
        // A result for an attempt that was already abandoned: a cancel and a
        // completion racing, which on the device is two tasks. Dropping it is
        // right; acting on it would resurrect a cancelled upload.
        return;
    }
    switch (outcome) {
        case Outcome::kSuccess:
            Finish(TransportState::kSucceeded, FailureKind::kNone);
            return;
        case Outcome::kPermanent:
            Finish(TransportState::kFailed, FailureKind::kPermanentRejection);
            return;
        case Outcome::kRetryable:
            break;
    }
    if (attempts_made_ >= config_.max_attempts) {
        Finish(TransportState::kFailed, FailureKind::kAttemptsExhausted);
        return;
    }
    if (now_ms >= deadline_ms_) {
        Finish(TransportState::kFailed, FailureKind::kDeadlineExceeded);
        return;
    }
    state_ = TransportState::kBackoff;
    backoff_until_ms_ = now_ms + config_.backoff_ms;
}

void VoiceTransport::Tick(uint64_t now_ms) {
    if (!busy()) {
        return;
    }
    // The overall deadline is checked before anything else, so a ladder that
    // has run out of time cannot start one more attempt on its way out.
    if (now_ms >= deadline_ms_) {
        if (state_ == TransportState::kSending && hooks_.abort) hooks_.abort();
        Finish(TransportState::kFailed, FailureKind::kDeadlineExceeded);
        return;
    }
    if (state_ == TransportState::kSending) {
        if (now_ms - attempt_started_ms_ >= config_.attempt_timeout_ms) {
            if (hooks_.abort) hooks_.abort();
            // A timed-out attempt is retryable and is routed through the same
            // path as any other retryable outcome, so the attempt counting and
            // the deadline cannot drift apart between the two.
            state_ = TransportState::kSending;
            OnAttemptResult(Outcome::kRetryable, now_ms);
        }
        return;
    }
    if (state_ == TransportState::kBackoff && now_ms >= backoff_until_ms_) {
        StartAttempt(now_ms);
    }
}

void VoiceTransport::Cancel(uint64_t now_ms) {
    (void)now_ms;
    if (!busy()) {
        // Cancelling something already finished must not rewrite how it
        // finished. A failed upload that is then cancelled stays failed.
        return;
    }
    if (state_ == TransportState::kSending && hooks_.abort) {
        hooks_.abort();
    }
    Finish(TransportState::kCancelled, FailureKind::kCancelled);
}

void VoiceTransport::Release() {
    if (busy()) {
        return;
    }
    state_ = TransportState::kIdle;
    failure_ = FailureKind::kNone;
    attempts_made_ = 0;
    memset(&identity_, 0, sizeof(identity_));
}

const char* TransportStateName(TransportState state) {
    switch (state) {
        case TransportState::kIdle:      return "Idle";
        case TransportState::kSending:   return "Sending";
        case TransportState::kBackoff:   return "Backoff";
        case TransportState::kSucceeded: return "Succeeded";
        case TransportState::kFailed:    return "Failed";
        case TransportState::kCancelled: return "Cancelled";
    }
    return "unknown";
}

const char* FailureKindName(FailureKind kind) {
    switch (kind) {
        case FailureKind::kNone:               return "none";
        case FailureKind::kAttemptsExhausted:  return "every attempt failed";
        case FailureKind::kPermanentRejection: return "the hub refused it outright";
        case FailureKind::kCancelled:          return "cancelled by the user";
        case FailureKind::kDeadlineExceeded:   return "ran out of time";
    }
    return "unknown";
}

const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::kSuccess:   return "success";
        case Outcome::kRetryable: return "retryable";
        case Outcome::kPermanent: return "permanent";
    }
    return "unknown";
}

}  // namespace voice
