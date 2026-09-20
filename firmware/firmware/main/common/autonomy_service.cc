/**
 * @file autonomy_service.cc
 * @brief Implementation of the profile route's semantics.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * See autonomy_service.h for the rules and why each is what it is. No ESP-IDF
 * headers: the host suite drives every path in here, including the write that
 * does not stick.
 */

#include "autonomy_service.h"

#include <string.h>

namespace autonomy {

const char* PutOutcomeName(PutOutcome outcome) {
    switch (outcome) {
        case PutOutcome::kAccepted: return "accepted";
        case PutOutcome::kReplay: return "replay";
        case PutOutcome::kNotProvisioned: return "not_provisioned";
        case PutOutcome::kUnauthorized: return "unauthorized";
        case PutOutcome::kLockedOut: return "locked_out";
        case PutOutcome::kTooLarge: return "profile_too_large";
        case PutOutcome::kInvalid: return "invalid_profile";
        case PutOutcome::kRevisionConflict: return "revision_mismatch";
        case PutOutcome::kShaMismatch: return "sha_mismatch";
        case PutOutcome::kStoreFailed: return "store_failed";
    }
    return "store_failed";
}

void ShaToHex(const uint8_t* digest, char* out) {
    static const char* kHex = "0123456789abcdef";
    for (size_t i = 0; i < record::kShaBytes; ++i) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[digest[i] & 0xf];
    }
    out[record::kShaBytes * 2] = '\0';
}

namespace {

/// Case-insensitive hex comparison, constant time in the bytes it compares.
/// The declared digest arrives as text from a header, so it is normalised
/// before the comparison rather than the comparison being made lenient.
bool HexEqualsIgnoringCase(const char* a, const char* b) {
    size_t i = 0;
    uint8_t diff = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'F') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = static_cast<char>(cb - 'A' + 'a');
        diff = static_cast<uint8_t>(diff | (ca ^ cb));
    }
    if (a[i] != '\0' || b[i] != '\0') return false;
    return diff == 0;
}

}  // namespace

AutonomyService::AutonomyService(record::SlotIo* io, uint8_t* scratch,
                                 size_t scratch_len, Profile* parsed)
    : slot_(kProfileSpec, io, scratch, scratch_len), parsed_(parsed) {}

void AutonomyService::SetToken(const char* token) {
    std::lock_guard<std::mutex> guard(mutex_);
    memset(token_, 0, sizeof(token_));
    provisioned_ = false;
    if (token == nullptr || token[0] == '\0') return;
    const size_t n = strlen(token);
    if (n >= sizeof(token_)) return;
    memcpy(token_, token, n);
    provisioned_ = true;
}

bool AutonomyService::Load() {
    std::lock_guard<std::mutex> guard(mutex_);
    return LoadLocked();
}

bool AutonomyService::LoadLocked() {
    has_profile_ = false;
    revision_ = 0;
    applied_epoch_ = 0;
    sha_hex_[0] = '\0';
    if (parsed_ == nullptr) return false;
    parsed_->Reset();

    if (!slot_.Load()) return false;

    // Parsed straight out of the slot's scratch buffer, not copied into the
    // profile's own arena first. That distinction is load-bearing: ParseProfile
    // clears the Profile before it starts, so parsing a document that lived in
    // that same arena would erase its own input. Two buffers, no copy, no
    // aliasing.
    //
    // The bytes are parsed rather than trusted. A record can pass its own
    // checksum and still be a document this build cannot draw — written by a
    // newer firmware, then rolled back — and that has to read as "no profile"
    // rather than as a profile full of defaults.
    const uint8_t* body = nullptr;
    size_t len = 0;
    if (!slot_.ReadPayloadInPlace(&body, &len)) return false;

    ParseError error;
    if (!ParseProfile(reinterpret_cast<const char*>(body), len, parsed_, &error)) {
        parsed_->Reset();
        return false;
    }

    uint8_t digest[record::kShaBytes];
    memcpy(digest, slot_.active_sha(), record::kShaBytes);
    ShaToHex(digest, sha_hex_);
    revision_ = parsed_->revision;
    applied_epoch_ = slot_.active_source_epoch();
    has_profile_ = true;
    return true;
}

void AutonomyService::SetTokenVerifier(TokenVerifier verifier, void* ctx) {
    std::lock_guard<std::mutex> guard(mutex_);
    verifier_ = verifier;
    verifier_ctx_ = ctx;
}

bool AutonomyService::Authorise(const char* token, int64_t now_ms,
                                PutOutcome* why) {
    std::lock_guard<std::mutex> guard(mutex_);
    return AuthoriseLocked(token, now_ms, why);
}

bool AutonomyService::AuthoriseLocked(const char* token, int64_t now_ms,
                                      PutOutcome* why) {
    // The lockout is checked before anything else, including the delegated
    // verifier. A locked-out caller that could still make us ask the owner of
    // the secret would be able to probe past the very budget the lockout is.
    if (verifier_ == nullptr && !provisioned_) {
        *why = PutOutcome::kNotProvisioned;
        return false;
    }
    if (now_ms < locked_until_ms_) {
        *why = PutOutcome::kLockedOut;
        return false;
    }

    bool ok = false;
    if (verifier_ != nullptr) {
        const AuthVerdict verdict = verifier_(token, verifier_ctx_);
        if (verdict == AuthVerdict::kNotProvisioned) {
            // Not a guess at a token, so it does not spend the failure budget:
            // a device nobody has paired would otherwise lock itself out
            // against a tower politely retrying.
            *why = PutOutcome::kNotProvisioned;
            return false;
        }
        ok = verdict == AuthVerdict::kOk;
    } else {
        ok = token != nullptr && strlen(token) == strlen(token_) &&
             record::ConstantTimeEquals(
                 reinterpret_cast<const uint8_t*>(token),
                 reinterpret_cast<const uint8_t*>(token_), strlen(token_));
    }

    if (!ok) {
        if (++auth_failures_ >= kMaxAuthFailures) {
            locked_until_ms_ = now_ms + kLockoutMs;
            auth_failures_ = 0;
        }
        *why = PutOutcome::kUnauthorized;
        return false;
    }
    auth_failures_ = 0;
    return true;
}

const PutResult* AutonomyService::FindAnswer(const char* key) const {
    if (key == nullptr || key[0] == '\0') return nullptr;
    for (size_t i = 0; i < kIdempotencyRing; ++i) {
        if (ring_[i].used && strcmp(ring_[i].key, key) == 0) {
            return &ring_[i].result;
        }
    }
    return nullptr;
}

void AutonomyService::RememberAnswer(const char* key, const PutResult& result) {
    if (key == nullptr || key[0] == '\0') return;
    if (strlen(key) >= kIdempotencyKeyMax) return;
    Answer& slot = ring_[ring_next_];
    ring_next_ = (ring_next_ + 1) % kIdempotencyRing;
    memset(slot.key, 0, sizeof(slot.key));
    memcpy(slot.key, key, strlen(key));
    slot.used = true;
    slot.result = result;
}

PutResult AutonomyService::Put(const char* body, size_t len, const char* token,
                               const char* declared_sha,
                               const char* idempotency_key, int64_t now_ms,
                               int64_t now_epoch) {
    // Held for the whole request. The staging area *is* the live profile — see
    // the note on `parsed_` in the header — so a reader admitted part way
    // through the parse would see a document that is neither the old one nor
    // the new one. The lock covers the profile slot's own bounded write and
    // nothing else; no network call happens under it.
    std::lock_guard<std::mutex> guard(mutex_);
    PutResult result;

    PutOutcome why = PutOutcome::kUnauthorized;
    if (!AuthoriseLocked(token, now_ms, &why)) {
        result.outcome = why;
        return result;
    }

    // Replay before anything else that changes state. A tower that retried a
    // PUT it never saw the answer to must get the original answer, not a second
    // revision bump.
    if (const PutResult* remembered = FindAnswer(idempotency_key)) {
        PutResult replay = *remembered;
        replay.outcome = PutOutcome::kReplay;
        replay.replay = true;
        // Persisted describes what this call did, which is nothing.
        replay.persisted = false;
        return replay;
    }

    if (body == nullptr || len == 0) {
        result.outcome = PutOutcome::kInvalid;
        return result;
    }
    if (len > kProfileMaxBytes) {
        result.outcome = PutOutcome::kTooLarge;
        return result;
    }

    uint8_t digest[record::kShaBytes];
    record::Sha256(reinterpret_cast<const uint8_t*>(body), len, digest);
    char hex[65];
    ShaToHex(digest, hex);

    if (declared_sha != nullptr && declared_sha[0] != '\0' &&
        !HexEqualsIgnoringCase(hex, declared_sha)) {
        // The bytes we received are not the bytes that were sent. Refuse rather
        // than store them and find out later.
        result.outcome = PutOutcome::kShaMismatch;
        memcpy(result.sha256, hex, sizeof(hex));
        return result;
    }

    // Validate before persist. Parsed into the live profile, which is restored
    // from the slot on any refusal below; see the note in the header.
    ParseError error;
    if (!ParseProfile(body, len, parsed_, &error)) {
        result.outcome = PutOutcome::kInvalid;
        result.error = error;
        LoadLocked();
        return result;
    }
    const int32_t incoming_revision = parsed_->revision;

    // CAS. Not greater than what we hold is refused, which makes a retry safe
    // and a stale tower harmless under one rule.
    if (has_profile_ && incoming_revision <= revision_) {
        result.outcome = PutOutcome::kRevisionConflict;
        result.revision = revision_;
        memcpy(result.sha256, sha_hex_, sizeof(sha_hex_));
        LoadLocked();
        return result;
    }

    const uint32_t source_epoch =
        now_epoch > 0 ? static_cast<uint32_t>(now_epoch) : 0u;
    const record::StoreResult stored =
        slot_.Store(reinterpret_cast<const uint8_t*>(body), len, source_epoch,
                    digest);

    if (stored != record::StoreResult::kOk &&
        stored != record::StoreResult::kDuplicate) {
        result.outcome = PutOutcome::kStoreFailed;
        LoadLocked();
        return result;
    }

    // Re-read what is actually in the slot rather than trusting the struct we
    // just parsed. The slot verified its own write; this makes the answer we
    // send describe storage rather than intent.
    if (!LoadLocked()) {
        result.outcome = PutOutcome::kStoreFailed;
        return result;
    }
    if (!HexEqualsIgnoringCase(sha_hex_, hex)) {
        result.outcome = PutOutcome::kStoreFailed;
        return result;
    }

    result.outcome = PutOutcome::kAccepted;
    result.accepted = true;
    result.persisted = (stored == record::StoreResult::kOk);
    result.revision = revision_;
    memcpy(result.sha256, sha_hex_, sizeof(sha_hex_));

    RememberAnswer(idempotency_key, result);
    return result;
}

GetResult AutonomyService::Get(uint8_t* out, size_t cap) const {
    std::lock_guard<std::mutex> guard(mutex_);
    GetResult result;
    if (!has_profile_ || out == nullptr) return result;
    size_t len = 0;
    if (!slot_.ReadPayload(out, cap, &len)) return result;
    result.present = true;
    result.bytes = len;
    result.revision = revision_;
    memcpy(result.sha256, sha_hex_, sizeof(sha_hex_));
    return result;
}

}  // namespace autonomy
