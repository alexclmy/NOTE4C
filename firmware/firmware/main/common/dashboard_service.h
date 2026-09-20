/**
 * @file dashboard_service.h
 * @brief Auth, idempotency and refresh coordination for the dashboard API.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Like dashboard_slot, this stays free of ESP-IDF headers so the host tests
 * exercise these exact translation units. Anything that needs FreeRTOS, NVS or
 * esp_http_server lives in the device-side glue, not here.
 *
 * Three separable concerns, deliberately not merged into one class:
 *   FrameAuth          - is this caller allowed to write?
 *   IdempotencyCache   - have we already applied this exact request?
 *   RefreshCoordinator - may a render start now, or must it be coalesced?
 */

#ifndef COMMON_DASHBOARD_SERVICE_H
#define COMMON_DASHBOARD_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>

#include "dashboard_slot.h"

namespace dashboard {

/// Tokens are 32 random bytes, transported as 64 lowercase hex characters.
constexpr size_t kTokenBytes = 32;
constexpr size_t kTokenHexChars = kTokenBytes * 2;

// ------------------------------------------------------------- FrameAuth --

enum class AuthResult {
    kOk = 0,
    kNotProvisioned,  ///< No token installed. Writes are denied, by design.
    kBadToken,        ///< Token present but wrong (or malformed).
    kLockedOut,       ///< Too many recent failures from this device.
};

/**
 * @brief Bearer-token check with a failure lockout.
 *
 * The default state of a device that has never been provisioned is
 * kNotProvisioned, which denies every mutating request. There is deliberately
 * no remote enrollment path: a device that would accept a token from the
 * network is a device where the first caller to arrive owns the screen. The
 * token must be installed through a trusted local channel (see
 * docs/PROVISIONING.md).
 */
class FrameAuth {
public:
    /// Failures allowed inside one window before the lockout engages.
    static constexpr int kMaxFailures = 10;
    /// Lockout window, milliseconds.
    static constexpr uint64_t kWindowMs = 60u * 1000u;

    /// Install the active token. @p len must be kTokenBytes. Passing nullptr
    /// or a wrong length clears it and returns the device to kNotProvisioned.
    void SetToken(const uint8_t* token, size_t len);

    /// True once a token is installed.
    bool provisioned() const { return has_token_; }

    /**
     * @brief Validate a presented token.
     * @param hex     caller-supplied value, expected as kTokenHexChars hex chars.
     * @param hex_len length of @p hex.
     * @param now_ms  monotonic milliseconds, for the lockout window.
     *
     * Comparison is constant-time and happens on the decoded bytes, so neither
     * the value nor the number of matching leading characters leaks by timing.
     */
    AuthResult Check(const char* hex, size_t hex_len, uint64_t now_ms);

    /// Consecutive failures currently counted in the window.
    int failure_count() const { return failures_; }

    /// Milliseconds until the lockout lifts; 0 when not locked out.
    uint64_t lockout_remaining_ms(uint64_t now_ms) const;

private:
    uint8_t token_[kTokenBytes] = {};
    bool has_token_ = false;
    int failures_ = 0;
    uint64_t window_start_ms_ = 0;
};

/// Decode @p hex_len hex characters into @p out (hex_len/2 bytes).
/// Rejects odd lengths, wrong lengths and non-hex characters.
bool HexDecode(const char* hex, size_t hex_len, uint8_t* out, size_t out_len);

/// Encode @p len bytes as lowercase hex into @p out, which must hold
/// len*2 + 1 characters. Always NUL-terminates.
void HexEncode(const uint8_t* data, size_t len, char* out);

// --------------------------------------------------------- PairingWindow --

/**
 * @brief A short, single-claim window during which a new token may be fetched.
 *
 * Why this shape, given the constraint that pairing must be locally initiated
 * and must never be an unauthenticated remote enrollment:
 *
 *  - The window can only be opened by a physical action on the device (the
 *    Settings menu). Nothing reachable over the network opens it.
 *  - It closes on the first successful claim, so a second caller cannot also
 *    obtain the token.
 *  - It expires on its own, so a window left open by mistake does not stay open.
 *  - The token is never drawn on the panel. That matters specifically here:
 *    e-paper holds its last image with the power off, so anything shown on it
 *    is effectively written down and left on the desk.
 *
 * Residual risk, stated rather than hidden: during the open window, another
 * host on the same LAN could claim the token first. The operator sees on the
 * device whether the window was claimed, and can immediately re-pair to
 * invalidate a token that went to the wrong place. This is a deliberate trade
 * against requiring a serial cable for every pairing.
 */
class PairingWindow {
public:
    enum class State {
        kClosed = 0,
        kOpen,
        kClaimed,
        kExpired,
    };

    /// Default open duration.
    static constexpr uint64_t kDefaultDurationMs = 120u * 1000u;

    /// Open (or re-open) the window, discarding any previous unclaimed one.
    void Open(uint64_t now_ms, uint64_t duration_ms = kDefaultDurationMs);

    /// Close without claiming.
    void Close();

    /// Current state, resolving expiry against @p now_ms.
    State state(uint64_t now_ms) const;

    /// Milliseconds left, 0 when not open.
    uint64_t remaining_ms(uint64_t now_ms) const;

    /**
     * @brief Consume the window.
     * @return true exactly once, and only while the window is open.
     */
    bool Claim(uint64_t now_ms);

private:
    State state_ = State::kClosed;
    uint64_t opened_ms_ = 0;
    uint64_t duration_ms_ = 0;
};

// ------------------------------------------------------ IdempotencyCache --

/**
 * @brief Remembers recently applied Idempotency-Key values.
 *
 * A push that is retried after a timeout must not repaint the panel a second
 * time. Small fixed ring: the pusher retries within seconds, so depth 8 covers
 * the realistic window without unbounded growth.
 */
class IdempotencyCache {
public:
    static constexpr size_t kDepth = 8;
    static constexpr size_t kKeyMax = 64;

    /// True if @p key was already recorded.
    bool Seen(const char* key, size_t len) const;

    /// Record @p key, evicting the oldest entry. No-op for empty/oversized keys.
    void Record(const char* key, size_t len);

    void Clear();

private:
    struct Entry {
        char key[kKeyMax] = {};
        size_t len = 0;
        bool used = false;
    };
    Entry entries_[kDepth];
    size_t next_ = 0;
};

// ---------------------------------------------------- RefreshCoordinator --

/// Lifecycle of one frame, reported separately because they mean different
/// things to the caller: accepted != persisted != on the glass.
enum class RenderDisposition {
    kStarted = 0,  ///< Panel refresh began now.
    kQueued,       ///< A refresh is in flight; this one is the pending successor.
    kCoalesced,    ///< A pending refresh already exists; it will pick up the newer frame.
    kSkipped,      ///< Displayed frame already matches; nothing to do.
};

enum class RefreshState {
    kIdle = 0,
    kRendering,
};

/**
 * @brief How a completed render turned out, for the counters the status route
 *        exposes.
 *
 * A render that starts but does not reach the glass has two entirely different
 * causes that used to be indistinguishable:
 *
 *   - `kFailed` is a *fault*: the panel was asked to draw and did not come back
 *     (a BUSY-pin timeout), or the frame could not be handed to the panel at
 *     all. The store holds a frame the glass has never shown and something is
 *     wrong.
 *   - `kDeferred` is *benign and expected*: a frame arrived while the user was
 *     on another page, so it was stored but deliberately not drawn. It appears
 *     the next time the dashboard is opened. Nothing failed.
 *
 * Folding both into `failed_` meant the tower could not tell a dropped frame
 * from a user simply reading another page. Keeping them apart is the whole
 * point of this enum.
 */
enum class RenderOutcome : uint8_t {
    kDrawn = 0,   ///< Reached the glass.
    kFailed,      ///< Started, did not reach the glass — a fault.
    kDeferred,    ///< Not drawn because another page owns the screen — benign.
};

/**
 * @brief Single-flight refresh gate with depth-1 coalescing.
 *
 * The pending slot is a flag, not a queue of frames: the frame store already
 * holds exactly one current frame, so "what to render when free" is always
 * "whatever is stored now". That is what makes coalescing land on the correct
 * last frame instead of replaying a backlog of stale ones.
 */
class RefreshCoordinator {
public:
    RefreshState state() const { return state_; }
    bool pending() const { return pending_; }

    /// True once a frame has actually reached the panel.
    bool has_displayed() const { return has_displayed_; }

    /// SHA-256 of the frame currently on the panel, or nullptr if none.
    const uint8_t* displayed_sha() const { return has_displayed_ ? displayed_sha_ : nullptr; }

    uint32_t displayed_seq() const { return displayed_seq_; }

    /// Number of refreshes suppressed because the panel already showed that frame.
    uint32_t skipped_count() const { return skipped_; }

    /// Number of requests folded into an existing pending refresh.
    uint32_t coalesced_count() const { return coalesced_; }

    /**
     * @brief Refreshes that were asked for, started, and did not reach the glass.
     *
     * The painter can refuse — another page is holding the screen — or the panel
     * can come back without having released BUSY. Either way the store holds a
     * frame the glass has never shown, and until this counter existed that fact
     * left no trace anywhere: `renders` simply did not go up, which is also what
     * a device that was never asked looks like. A tower polling the status route
     * could not tell "nothing was pushed" from "your frame was dropped".
     */
    uint32_t failed_count() const { return failed_; }

    /// True when the most recent completed render did not reach the glass.
    /// Cleared by the next render that does.
    bool last_render_failed() const { return last_failed_; }

    /// Sequence of the frame the most recent failed render was carrying. 0 when
    /// no render has failed since the last reset.
    uint32_t last_failed_seq() const { return last_failed_seq_; }

    /**
     * @brief Renders that were skipped because another page owned the screen.
     *
     * The benign twin of `failed_count()`. A frame that arrives while the user
     * is on another page is stored and deliberately not drawn — it appears when
     * the dashboard is reopened. This is counted apart from `failed_` precisely
     * so the tower can tell "your frame was dropped" from "the user is on
     * another page".
     */
    uint32_t deferred_count() const { return deferred_; }

    /// True when the most recent completed render was deferred rather than
    /// drawn or failed. Cleared by the next render that draws or fails.
    bool last_render_deferred() const { return last_deferred_; }

    /// Sequence of the frame the most recent deferred render was carrying. 0
    /// when no render has been deferred since the last reset.
    uint32_t last_deferred_seq() const { return last_deferred_seq_; }

    /**
     * @brief Ask to display the frame identified by @p sha / @p seq.
     *
     * @param force when true, render even if the digest matches what is shown
     *              (used by an explicit user-initiated redraw).
     */
    RenderDisposition Request(const uint8_t* sha, uint32_t seq, bool force);

    /**
     * @brief Report that the in-flight render finished.
     *
     * @param sha      digest of what actually reached the panel.
     * @param seq      sequence of what actually reached the panel.
     * @param outcome  kDrawn updates the displayed frame; kFailed counts a
     *                 fault; kDeferred counts a benign not-drawn. Only kDrawn
     *                 changes the displayed frame.
     * @return true if a pending refresh was waiting and the caller should
     *         immediately render the currently stored frame.
     */
    bool CompleteRender(const uint8_t* sha, uint32_t seq, RenderOutcome outcome);

    /// Drop any pending request without rendering (shutdown, sleep).
    void ClearPending() { pending_ = false; }

    void Reset();

private:
    RefreshState state_ = RefreshState::kIdle;
    bool pending_ = false;
    bool has_displayed_ = false;
    uint8_t displayed_sha_[kShaBytes] = {};
    uint32_t displayed_seq_ = 0;
    uint32_t skipped_ = 0;
    uint32_t coalesced_ = 0;
    uint32_t failed_ = 0;
    bool last_failed_ = false;
    uint32_t last_failed_seq_ = 0;
    uint32_t deferred_ = 0;
    bool last_deferred_ = false;
    uint32_t last_deferred_seq_ = 0;
};

// -------------------------------------------------------- RenderHandshake --

/// Outcome of waiting for a panel refresh to be acknowledged.
enum class AckResult {
    kCompleted = 0,   ///< Signalled, and the panel reported a clean refresh.
    kPanelFailed,     ///< Signalled, but the panel reported a BUSY-pin timeout.
    kTimedOut,        ///< No completion signal arrived within the bound.
    kNotAttempted,    ///< Refused: a handshake was already in progress.
};

/// Platform primitives the handshake drives. Supplied by the UI layer on the
/// device and by the test on the host.
struct RenderHandshakeHooks {
    /// Discard completion signals left over from earlier refreshes.
    /// Returns how many were discarded.
    std::function<int()> drain;
    /// Start the panel refresh.
    std::function<void()> trigger;
    /// Block up to @p timeout_ms for a completion signal. True if one arrived.
    std::function<bool(uint32_t timeout_ms)> wait;
    /// Whether the refresh that just completed did so without a BUSY timeout.
    std::function<bool()> panel_ok;
    /**
     * @brief Is the panel already refreshing something else right now?
     *
     * Optional. When it is supplied and answers true, the handshake lets that
     * refresh finish before triggering its own — see rule 1a below. Leaving it
     * unset preserves the previous behaviour exactly.
     */
    std::function<bool()> panel_busy;
};

/**
 * @brief Enforces the ordering rules for acknowledging a panel refresh.
 *
 * These rules are individually obvious and collectively easy to get wrong, so
 * they live in one portable place that the host tests exercise directly:
 *
 *  1. **Drain before triggering.** A completion signal from a previous refresh
 *     would otherwise satisfy this wait instantly, and the caller would record
 *     a frame as displayed before the panel had started drawing it.
 *
 *  1a. **Draining is not enough while a refresh is still running.** Draining
 *     removes signals that have already been posted. It cannot remove the one a
 *     refresh that is *in flight right now* is going to post when it finishes,
 *     and that signal arrives within milliseconds of our trigger while the panel
 *     is still showing the other refresh's image. The caller then records our
 *     frame as displayed on the strength of somebody else's completion, and
 *     `panel_ok` describes that other refresh too.
 *
 *     This is reachable on the device: the clock tick, the status bar and the
 *     navigation pump all trigger refreshes from the main task while the
 *     dashboard render task is running this handshake. So when @c panel_busy
 *     reports a refresh already in progress, the handshake waits for it (inside
 *     the same bound), drains again, and only then triggers its own.
 *
 *  2. **Bound the wait.** An unbounded wait on a wedged panel deadlocks the
 *     render task, and every later frame with it.
 *
 *  3. **Never acknowledge on the signal alone.** The driver's read_busy() gives
 *     up after its own timeout and carries on regardless, so "the refresh task
 *     finished" does not imply "the panel drew the frame". Success requires the
 *     signal *and* a clean panel report.
 *
 *  4. **One at a time.** A second concurrent handshake would consume the first
 *     one's signal. Overlapping calls are refused rather than interleaved.
 */
class RenderHandshake {
public:
    AckResult Run(const RenderHandshakeHooks& hooks, uint32_t timeout_ms);

    bool in_progress() const { return in_progress_; }
    uint32_t completions() const { return completions_; }
    uint32_t timeouts() const { return timeouts_; }
    uint32_t panel_failures() const { return panel_failures_; }
    uint32_t overlaps_refused() const { return overlaps_refused_; }
    uint32_t stale_signals_discarded() const { return stale_discarded_; }
    /// Times the handshake had to let a refresh that was already running finish
    /// before it could trigger its own. See rule 1a.
    uint32_t foreign_refreshes_awaited() const { return foreign_awaited_; }

    void Reset();

private:
    bool in_progress_ = false;
    uint32_t completions_ = 0;
    uint32_t timeouts_ = 0;
    uint32_t panel_failures_ = 0;
    uint32_t overlaps_refused_ = 0;
    uint32_t stale_discarded_ = 0;
    uint32_t foreign_awaited_ = 0;
};

// ------------------------------------------------------ displayed origin --

/**
 * @brief Is the frame the panel last drew still the frame the store holds?
 *
 * The question anything that wants to describe the glass has to answer first.
 * A fact about the *stored* record — its provenance, its age, its source — is a
 * fact about the panel only while the two are the same frame: after a deep
 * sleep e-paper keeps its image while the coordinator that knows what was drawn
 * does not, and between a store and the refresh that follows it the two are
 * simply different frames.
 *
 * Both halves of the comparison are required. The sequence, because the store
 * may have moved on since the refresh completed; the digest, because a record
 * rewritten at the same sequence would otherwise pass.
 *
 * Here rather than inline in DashboardManager because it is the claim the
 * status route makes about provenance, and a claim is only testable if a host
 * can call the function that decides it. @p stored_sha_hex and
 * @p displayed_sha_hex are NUL-terminated lowercase hex, or empty.
 */
bool DisplayedFrameIsStoredFrame(bool has_stored, uint32_t stored_seq,
                                 const char* stored_sha_hex, bool has_displayed,
                                 uint32_t displayed_seq,
                                 const char* displayed_sha_hex);

// ---------------------------------------------------------- MutationGate --

/**
 * @brief One writer at a time, claimed atomically.
 *
 * Two things can write a frame to this device: the HTTP task, serving a PUT
 * from the tower, and the task that stores a panel this device composed for
 * itself. They run on different cores. The rule has always been "one mutating
 * request at a time, refused rather than queued" — but it was enforced by
 * reading a plain `bool` and then writing it, which is two operations with a
 * window between them wide enough for both writers to pass. A missed refusal
 * there is not a tidy 409: it is two writers inside the A/B store's
 * write-inactive-then-swap sequence at once.
 *
 * So the claim is a compare-exchange, and the flag is atomic. Both matter: the
 * atomic alone would still allow check-then-set, and the compare-exchange alone
 * would be a data race.
 *
 * Deliberately not a mutex. A blocking lock here would mean the HTTP task
 * waiting on a compose, and a compose waiting on a flash write; refusing is
 * both cheaper and more honest, because a client that gets a 409 knows its
 * frame was not applied while one that times out is left guessing.
 */
class MutationGate {
public:
    /// Claim the gate. False means another mutation is already in flight and
    /// the caller must refuse rather than wait.
    bool TryEnter() {
        bool expected = false;
        return busy_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }

    void Leave() { busy_.store(false, std::memory_order_release); }

    bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> busy_{false};
};

/**
 * @brief A scoped claim on a MutationGate. `entered()` says whether it got one.
 *
 * Every early return in a mutating path used to have to remember to clear the
 * flag by hand, and there are seven of them. One that forgot would wedge the
 * device into refusing every later push until it rebooted.
 */
class MutationClaim {
public:
    explicit MutationClaim(MutationGate& gate)
        : gate_(gate), entered_(gate.TryEnter()) {}
    ~MutationClaim() { if (entered_) gate_.Leave(); }

    MutationClaim(const MutationClaim&) = delete;
    MutationClaim& operator=(const MutationClaim&) = delete;

    bool entered() const { return entered_; }

private:
    MutationGate& gate_;
    bool entered_;
};

}  // namespace dashboard

#endif  // COMMON_DASHBOARD_SERVICE_H
