/**
 * @file dashboard_manager.h
 * @brief Device-side glue for the poulailler dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Owns the portable pieces (dashboard_slot, dashboard_service) and supplies
 * everything they deliberately do not know about: SPIFFS files, NVS, PSRAM
 * buffers, a FreeRTOS render task and a monotonic clock.
 *
 * Threading contract: HTTP handlers call Submit()/Status()/CopyFrame() from the
 * httpd task; the render task calls into the panel. A single mutex guards the
 * store and the coordinator, and it is explicitly *not* held across the panel
 * refresh, so a multi-second BWRY refresh never blocks the HTTP server.
 */

#ifndef COMMON_DASHBOARD_MANAGER_H
#define COMMON_DASHBOARD_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "dashboard_service.h"
#include "dashboard_slot.h"

namespace dashboard {

/// What happened to a submitted frame, in HTTP-mappable terms.
enum class PushOutcome {
    kAccepted = 0,     ///< 202 - stored and verified, render started or queued
    kDeduped,          ///< 200 - byte-identical to the stored frame
    kIdempotentReplay, ///< 200 - Idempotency-Key already applied
    kBadLength,        ///< 400 - not exactly kFrameBytes
    kShaMismatch,      ///< 422 - body does not match the declared digest
    kUnauthorized,     ///< 401 - token missing or wrong
    kNotProvisioned,   ///< 503 - no token installed; writes denied by default
    kLockedOut,        ///< 429 - too many recent auth failures
    kBusy,             ///< 409 - another mutating request already in flight
    kStoreFailed,      ///< 500 - write or read-back verification failed
    /**
     * @brief The stored frame moved between the caller reading it and writing.
     *
     * Only reachable from SubmitLocal(), which is the one writer that spends
     * seconds composing before it stores. The tower's PUT never carries an
     * expected sequence, and never loses this race, because it is the frame
     * that wins by rule.
     */
    kSuperseded,
};

/// Result of a submission. `persisted` and `render` are reported separately
/// from acceptance on purpose: a caller that treats 202 as "it is on the
/// screen" would be wrong, and the API should not let it believe that.
struct PushStatus {
    PushOutcome outcome = PushOutcome::kStoreFailed;
    bool persisted = false;
    RenderDisposition render = RenderDisposition::kSkipped;
    uint32_t seq = 0;
    char sha_hex[kTokenHexChars + 1] = {};
};

/// Snapshot for GET /api/v1/dashboard/status.
struct DashboardStatus {
    bool has_stored_frame = false;
    uint32_t stored_seq = 0;
    uint32_t stored_source_epoch = 0;
    char stored_sha_hex[kTokenHexChars + 1] = {};
    /// True when the stored frame was composed by this device rather than
    /// pushed. Read from the record's own header, so a reboot does not lose it
    /// and the device never has to guess which of the two it is showing.
    bool stored_origin_is_local = false;

    bool has_displayed_frame = false;
    uint32_t displayed_seq = 0;
    char displayed_sha_hex[kTokenHexChars + 1] = {};

    /**
     * @brief True when `stored_origin_is_local` also describes the glass.
     *
     * The origin bit lives in the header of the *stored* record. It describes
     * what is displayed only when the frame that completed a refresh is still
     * the frame the store holds — same sequence, same digest. After a deep
     * sleep it is false, because e-paper keeps its image across the reboot and
     * the coordinator that knows what was drawn does not, and between a store
     * and the refresh that follows it it is false too.
     *
     * Callers report "unknown" when this is false. They must not fall back to
     * the stored origin: that is a different frame's provenance.
     */
    bool displayed_origin_known = false;
    bool displayed_origin_is_local = false;

    bool rendering = false;
    bool pending = false;
    bool provisioned = false;
    bool lockdown = false;

    uint32_t skipped_renders = 0;
    uint32_t coalesced_requests = 0;
    uint32_t render_count = 0;
    /**
     * @brief Renders that started and never reached the glass.
     *
     * The painter refuses when another page owns the screen, and the panel can
     * come back without having released BUSY. Neither used to be reported: the
     * render count simply did not move, which is indistinguishable from a device
     * nobody pushed anything to. A tower waiting for the digest to appear under
     * `displayed` had nothing to tell it that its frame had been dropped.
     */
    uint32_t failed_renders = 0;
    /// True when the most recent completed render did not reach the glass.
    bool last_render_failed = false;
    /// Sequence the most recent failed render was carrying; 0 when none failed.
    uint32_t last_failed_seq = 0;

    /**
     * @brief Renders skipped because another page owned the screen.
     *
     * The benign twin of `failed_renders`: a frame that arrived while the user
     * was on another page is stored, not drawn, and appears when the dashboard
     * is reopened. Reported separately so the tower can tell a genuine fault
     * from a user simply reading a different page.
     */
    uint32_t deferred_renders = 0;
    /// True when the most recent completed render was deferred, not failed.
    bool last_render_deferred = false;
    /// Sequence the most recent deferred render was carrying; 0 when none.
    uint32_t last_deferred_seq = 0;

    // Timing split for the most recent render. Reported separately because the
    // panel wait is a hardware property and the rest is ours; conflating them
    // would make our software look responsible for the multi-second refresh.
    uint32_t last_read_ms = 0;     ///< reading and verifying the frame from SPIFFS
    uint32_t last_blit_ms = 0;     ///< copying into the framebuffer
    uint32_t last_panel_ms = 0;    ///< driver transfer plus BUSY wait
    uint32_t last_total_ms = 0;

    // Storage-layer failures since boot. Non-zero means the filesystem is
    // refusing or losing writes, which is otherwise only visible on a serial
    // console nobody is watching.
    uint32_t storage_write_failures = 0;
    uint32_t storage_read_failures = 0;
    uint32_t spiffs_total_bytes = 0;
    uint32_t spiffs_used_bytes = 0;
};

/**
 * @brief Paints a frame and waits for the panel to finish.
 *
 * Supplied by the UI layer. Returns how the render turned out: kDrawn when the
 * frame reached the panel, kFailed on a fault, kDeferred when another page owns
 * the screen and the frame was stored but not drawn. Called from the render
 * task with no manager lock held, so it may block for as long as the hardware
 * needs.
 */
using PanelPaintFn =
    std::function<RenderOutcome(const uint8_t* frame, uint32_t* panel_ms_out)>;

class DashboardManager {
public:
    static DashboardManager& GetInstance();

    /**
     * @brief Start the manager. Requires SPIFFS ("/spiffs") to be mounted.
     * @return false if buffers could not be allocated; the caller should then
     *         treat the dashboard feature as unavailable rather than degraded.
     */
    bool Init();

    /// Register the panel paint callback and start the render task.
    void SetPanelPainter(PanelPaintFn fn);

    bool initialised() const { return initialised_; }

    // ---- frame access ----------------------------------------------------

    /// True when a validated frame exists in flash.
    bool HasFrame();

    /// Copy the stored frame into @p out (kFrameBytes). Re-verifies checksums.
    bool CopyFrame(uint8_t* out);

    /**
     * @brief Validate, persist and schedule a pushed frame.
     *
     * @param token_hex   value of the X-Auth-Token header, may be nullptr.
     * @param sha_hex     value of X-Frame-Sha256, may be nullptr.
     * @param idem_key    value of Idempotency-Key, may be nullptr.
     */
    PushStatus Submit(const uint8_t* body,
                      size_t len,
                      const char* token_hex,
                      const char* sha_hex,
                      const char* idem_key,
                      uint32_t source_epoch);

    /**
     * @brief Persist and schedule a frame this device composed for itself.
     *
     * The autonomous sibling of Submit(), and the differences are all
     * deliberate:
     *
     *  - **No token.** There is no caller to authenticate. The wake cycle is
     *    already inside the trust boundary, and inventing a credential for it
     *    to present to itself would be ceremony, not security. The route that
     *    *is* reachable from the network is unchanged.
     *  - **No idempotency key.** There is no retry to collapse; a wake either
     *    composes or it does not.
     *  - **The origin flag is set.** The record says on its face that the panel
     *    is showing something this device made up, so the answer survives a
     *    reboot and the tower is told rather than left to guess. It is one bit
     *    in a header word that was already reserved and already written as
     *    zero; see dashboard_slot.h's kFlagOriginLocal for why that is not a
     *    change to the format of a store validated on hardware.
     *
     * Everything else is shared with Submit(), and that is the point: the same
     * A/B store, the same read-back verification, the same deduplication, the
     * same refresh coordinator. A locally composed frame and a pushed one are
     * the same kind of object and take the same path onto the glass.
     *
     *  - **It carries an expected sequence, and the check is a compare-and-swap.**
     *    This is the difference that matters. A compose takes seconds of CPU,
     *    and a PUT that lands during those seconds finishes long before the
     *    local frame is ready to store. The single-mutator gate cannot see that:
     *    by the time this function claims it, the tower's write is over and the
     *    gate is free again. Without the sequence check the local frame would
     *    then be stored *on top of* the operator's, which inverts the one rule
     *    this whole feature is built around.
     *
     * @param expected_seq the store's active sequence as it was before the
     *        composition began. kSuperseded is returned, and nothing is
     *        written, when it no longer matches. Pass 0 to mean "there was no
     *        frame before I started".
     *
     * @return kDeduped when the composition matched what is already displayed,
     *         which costs no flash write and no panel refresh. On a panel that
     *         takes twenty-five seconds to redraw, that is the single most
     *         valuable answer this function can give.
     */
    PushStatus SubmitLocal(const uint8_t* body, size_t len, uint32_t source_epoch,
                           uint32_t expected_seq);

    /**
     * @brief Validate a token without submitting anything.
     *
     * Used by mutating routes that carry no body. Counts towards the same
     * failure lockout as a frame push, so it cannot be used to probe tokens
     * more cheaply than the push route.
     */
    AuthResult CheckToken(const char* token_hex);

    /// Ask for the stored frame to be repainted. @p force repaints even when
    /// the panel already shows it. Used by the front button.
    RenderDisposition RequestRedraw(bool force);

    DashboardStatus Status();

    // ---- provisioning ----------------------------------------------------

    /// True once a token has been installed.
    bool Provisioned();

    /**
     * @brief Begin pairing: mint a candidate token and open the claim window.
     *
     * Reachable only from the device Settings menu, i.e. from someone holding
     * the hardware. The currently active token keeps working until the new one
     * is actually claimed, so an accidental press cannot orphan the bridge.
     */
    bool OpenPairing();

    /**
     * @brief Hand out the candidate token, once, while the window is open.
     *
     * On success the candidate becomes the active token. The value is written
     * to @p out_hex (kTokenHexChars+1 bytes) and is never logged or drawn.
     */
    bool ClaimPairing(char* out_hex);

    /// Close the pairing window without claiming.
    void CancelPairing();

    PairingWindow::State PairingState();
    uint32_t PairingRemainingSeconds();

    /// Remove the token, returning the device to deny-by-default.
    void ClearToken();

    // ---- legacy route lockdown -------------------------------------------

    /// When true, the legacy unauthenticated mutating routes are refused.
    bool LockdownEnabled();
    void SetLockdownEnabled(bool enabled);

private:
    DashboardManager() = default;

    static void RenderTaskEntry(void* arg);
    void RenderLoop();
    void NotifyRenderTask();
    /// True when a render task and a painter both exist, so a request made now
    /// will actually be completed. See the definition for why this is checked
    /// before the coordinator is moved into kRendering rather than after.
    bool CanRender() const;

    // Serialises store_ and coord_. Never held during a panel refresh.
    SemaphoreHandle_t lock_ = nullptr;
    TaskHandle_t render_task_ = nullptr;

    SlotIo* io_ = nullptr;
    DashboardSlot* store_ = nullptr;
    uint8_t* scratch_ = nullptr;      ///< kRecordBytes, PSRAM
    uint8_t* render_buf_ = nullptr;   ///< kFrameBytes, PSRAM

    FrameAuth auth_;
    PairingWindow pairing_;
    uint8_t pending_token_[kTokenBytes] = {};
    bool has_pending_token_ = false;
    IdempotencyCache idem_;
    RefreshCoordinator coord_;

    // Guards "one mutating request in flight" without blocking: a second
    // concurrent push is refused with 409 rather than queued behind the first.
    // Claimed with a compare-exchange rather than a read followed by a write.
    // See MutationGate: the HTTP task and the task that stores a locally
    // composed frame run on different cores, and the window between those two
    // operations was wide enough for both to pass.
    MutationGate mutating_;

    bool initialised_ = false;
    bool lockdown_ = true;
    uint32_t render_count_ = 0;
    uint32_t last_read_ms_ = 0;
    uint32_t last_blit_ms_ = 0;
    uint32_t last_panel_ms_ = 0;
    uint32_t last_total_ms_ = 0;

    PanelPaintFn painter_;
};

}  // namespace dashboard

#endif  // COMMON_DASHBOARD_MANAGER_H
