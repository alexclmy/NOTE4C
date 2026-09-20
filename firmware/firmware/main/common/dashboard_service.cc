/**
 * @file dashboard_service.cc
 * @brief Implementation of auth, idempotency and refresh coordination.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "dashboard_service.h"

#include <string.h>

namespace dashboard {

// ------------------------------------------------------------------- hex --

namespace {

inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool HexDecode(const char* hex, size_t hex_len, uint8_t* out, size_t out_len) {
    if (hex == nullptr || out == nullptr) return false;
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = HexVal(hex[i * 2]);
        const int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void HexEncode(const uint8_t* data, size_t len, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = d[data[i] >> 4];
        out[i * 2 + 1] = d[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

// ------------------------------------------------------------- FrameAuth --

void FrameAuth::SetToken(const uint8_t* token, size_t len) {
    if (token == nullptr || len != kTokenBytes) {
        memset(token_, 0, sizeof(token_));
        has_token_ = false;
        return;
    }
    memcpy(token_, token, kTokenBytes);
    has_token_ = true;
}

uint64_t FrameAuth::lockout_remaining_ms(uint64_t now_ms) const {
    if (failures_ < kMaxFailures) return 0;
    if (now_ms < window_start_ms_) return 0;   // clock went backwards; fail open on timing only
    const uint64_t elapsed = now_ms - window_start_ms_;
    if (elapsed >= kWindowMs) return 0;
    return kWindowMs - elapsed;
}

AuthResult FrameAuth::Check(const char* hex, size_t hex_len, uint64_t now_ms) {
    // An unprovisioned device denies writes outright. Reported distinctly from
    // a wrong token so the operator can tell "not paired yet" from "bad token"
    // without that distinction telling an attacker anything they could use:
    // both outcomes refuse the write.
    if (!has_token_) {
        return AuthResult::kNotProvisioned;
    }

    // Expire a stale failure window before deciding anything.
    if (failures_ > 0 && (now_ms < window_start_ms_ || now_ms - window_start_ms_ >= kWindowMs)) {
        failures_ = 0;
    }
    if (failures_ >= kMaxFailures && lockout_remaining_ms(now_ms) > 0) {
        return AuthResult::kLockedOut;
    }
    if (failures_ >= kMaxFailures) {
        failures_ = 0;   // window elapsed, start counting again
    }

    uint8_t presented[kTokenBytes];
    bool ok = false;
    if (hex != nullptr && HexDecode(hex, hex_len, presented, kTokenBytes)) {
        ok = ConstantTimeEquals(presented, token_, kTokenBytes);
    }
    // Do not leave the decoded candidate on the stack for the next frame to reuse.
    memset(presented, 0, sizeof(presented));

    if (ok) {
        failures_ = 0;
        return AuthResult::kOk;
    }

    if (failures_ == 0) {
        window_start_ms_ = now_ms;
    }
    ++failures_;
    return AuthResult::kBadToken;
}

// --------------------------------------------------------- PairingWindow --

void PairingWindow::Open(uint64_t now_ms, uint64_t duration_ms) {
    state_ = State::kOpen;
    opened_ms_ = now_ms;
    duration_ms_ = duration_ms;
}

void PairingWindow::Close() {
    state_ = State::kClosed;
    opened_ms_ = 0;
    duration_ms_ = 0;
}

PairingWindow::State PairingWindow::state(uint64_t now_ms) const {
    if (state_ != State::kOpen) {
        return state_;
    }
    // A clock that moved backwards should not extend the window; treat any
    // inconsistency as expiry, because failing closed here costs the operator
    // one extra button press and failing open costs them the token.
    if (now_ms < opened_ms_ || now_ms - opened_ms_ >= duration_ms_) {
        return State::kExpired;
    }
    return State::kOpen;
}

uint64_t PairingWindow::remaining_ms(uint64_t now_ms) const {
    if (state(now_ms) != State::kOpen) return 0;
    return duration_ms_ - (now_ms - opened_ms_);
}

bool PairingWindow::Claim(uint64_t now_ms) {
    if (state(now_ms) != State::kOpen) {
        // Record expiry so the operator sees "expired" rather than "open".
        if (state_ == State::kOpen) state_ = State::kExpired;
        return false;
    }
    state_ = State::kClaimed;
    return true;
}

// ------------------------------------------------------ IdempotencyCache --

bool IdempotencyCache::Seen(const char* key, size_t len) const {
    if (key == nullptr || len == 0 || len > kKeyMax) return false;
    for (size_t i = 0; i < kDepth; ++i) {
        if (entries_[i].used && entries_[i].len == len &&
            memcmp(entries_[i].key, key, len) == 0) {
            return true;
        }
    }
    return false;
}

void IdempotencyCache::Record(const char* key, size_t len) {
    if (key == nullptr || len == 0 || len > kKeyMax) return;
    if (Seen(key, len)) return;
    Entry& e = entries_[next_];
    memcpy(e.key, key, len);
    e.len = len;
    e.used = true;
    next_ = (next_ + 1) % kDepth;
}

void IdempotencyCache::Clear() {
    for (size_t i = 0; i < kDepth; ++i) {
        entries_[i] = Entry{};
    }
    next_ = 0;
}

// ---------------------------------------------------- RefreshCoordinator --

RenderDisposition RefreshCoordinator::Request(const uint8_t* sha, uint32_t seq, bool force) {
    (void)seq;

    // Nothing to do when the glass already shows this exact frame. This is the
    // one genuinely large saving available: it removes a multi-second panel
    // refresh and the wear that comes with it.
    if (!force && state_ == RefreshState::kIdle && !pending_ && has_displayed_ &&
        sha != nullptr && ConstantTimeEquals(sha, displayed_sha_, kShaBytes)) {
        ++skipped_;
        return RenderDisposition::kSkipped;
    }

    if (state_ == RefreshState::kRendering) {
        // Depth-1 coalescing. A second, third and hundredth request while busy
        // all collapse into the same single successor refresh, which will read
        // whatever frame is stored at the moment it starts.
        if (pending_) {
            ++coalesced_;
            return RenderDisposition::kCoalesced;
        }
        pending_ = true;
        return RenderDisposition::kQueued;
    }

    state_ = RefreshState::kRendering;
    return RenderDisposition::kStarted;
}

bool RefreshCoordinator::CompleteRender(const uint8_t* sha, uint32_t seq,
                                        RenderOutcome outcome) {
    if (outcome == RenderOutcome::kDrawn && sha != nullptr) {
        memcpy(displayed_sha_, sha, kShaBytes);
        displayed_seq_ = seq;
        has_displayed_ = true;
        last_failed_ = false;
        last_deferred_ = false;
    } else if (outcome == RenderOutcome::kDeferred) {
        // A frame arrived while another page owned the screen. It is stored and
        // deliberately not drawn — it appears when the dashboard is reopened.
        // This is benign and expected, so it is counted apart from failed_: the
        // tower must be able to tell "your frame was dropped" from "the user is
        // on another page". last_failed_ is cleared because this completion was
        // not a fault.
        ++deferred_;
        last_deferred_ = true;
        last_deferred_seq_ = seq;
        last_failed_ = false;
    } else {
        // A render that started and did not finish on the glass — a fault: a
        // BUSY-pin timeout, or a drawn render that somehow carried no digest.
        // Counted rather than discarded: this is the only record that the frame
        // the store is holding was offered to the panel and not shown, and the
        // status route is where the tower has to be able to see it.
        ++failed_;
        last_failed_ = true;
        last_failed_seq_ = seq;
        last_deferred_ = false;
    }
    state_ = RefreshState::kIdle;

    if (pending_) {
        pending_ = false;
        state_ = RefreshState::kRendering;
        return true;
    }
    return false;
}

void RefreshCoordinator::Reset() {
    state_ = RefreshState::kIdle;
    pending_ = false;
    has_displayed_ = false;
    memset(displayed_sha_, 0, sizeof(displayed_sha_));
    displayed_seq_ = 0;
    skipped_ = 0;
    coalesced_ = 0;
    failed_ = 0;
    last_failed_ = false;
    last_failed_seq_ = 0;
    deferred_ = 0;
    last_deferred_ = false;
    last_deferred_seq_ = 0;
}

// -------------------------------------------------------- RenderHandshake --

AckResult RenderHandshake::Run(const RenderHandshakeHooks& hooks, uint32_t timeout_ms) {
    if (in_progress_) {
        // Rule 4. Two handshakes would race for one signal and at least one of
        // them would draw the wrong conclusion.
        ++overlaps_refused_;
        return AckResult::kNotAttempted;
    }
    if (!hooks.drain || !hooks.trigger || !hooks.wait || !hooks.panel_ok) {
        return AckResult::kNotAttempted;
    }

    in_progress_ = true;

    // Rule 1. Anything still queued belongs to an earlier refresh.
    const int stale = hooks.drain();
    if (stale > 0) {
        stale_discarded_ += static_cast<uint32_t>(stale);
    }

    // Rule 1a. A refresh that is already running has not posted its completion
    // signal yet, so draining cannot remove it. Triggering now would mean
    // adopting that signal — and that refresh's panel_ok — as though they
    // described our frame. Wait it out first, inside the same bound.
    if (hooks.panel_busy && hooks.panel_busy()) {
        ++foreign_awaited_;
        if (!hooks.wait(timeout_ms)) {
            ++timeouts_;
            in_progress_ = false;
            return AckResult::kTimedOut;
        }
        const int late = hooks.drain();
        if (late > 0) {
            stale_discarded_ += static_cast<uint32_t>(late);
        }
    }

    hooks.trigger();

    // Rule 2. Bounded, always.
    const bool signalled = hooks.wait(timeout_ms);

    AckResult result;
    if (!signalled) {
        ++timeouts_;
        result = AckResult::kTimedOut;
    } else if (!hooks.panel_ok()) {
        // Rule 3. The refresh task came back, but the panel never released
        // BUSY, so what is on the glass is unknown.
        ++panel_failures_;
        result = AckResult::kPanelFailed;
    } else {
        ++completions_;
        result = AckResult::kCompleted;
    }

    in_progress_ = false;
    return result;
}

void RenderHandshake::Reset() {
    in_progress_ = false;
    completions_ = 0;
    timeouts_ = 0;
    panel_failures_ = 0;
    overlaps_refused_ = 0;
    stale_discarded_ = 0;
    foreign_awaited_ = 0;
}

bool DisplayedFrameIsStoredFrame(bool has_stored, uint32_t stored_seq,
                                 const char* stored_sha_hex, bool has_displayed,
                                 uint32_t displayed_seq,
                                 const char* displayed_sha_hex) {
    if (!has_stored || !has_displayed) return false;
    if (stored_seq != displayed_seq) return false;
    if (stored_sha_hex == nullptr || displayed_sha_hex == nullptr) return false;
    // An empty digest on either side is an absence, not a match. Two devices
    // holding nothing are not holding the same thing.
    if (stored_sha_hex[0] == '\0' || displayed_sha_hex[0] == '\0') return false;
    return strcmp(stored_sha_hex, displayed_sha_hex) == 0;
}

}  // namespace dashboard
