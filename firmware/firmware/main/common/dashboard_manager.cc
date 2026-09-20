/**
 * @file dashboard_manager.cc
 * @brief Device-side glue for the poulailler dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "dashboard_manager.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_spiffs.h>
#include <esp_timer.h>

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "settings.h"

namespace dashboard {

namespace {

const char* kTag = "DashboardMgr";

// Same SPIFFS mount photo_storage uses (the 8M "assets" partition). We add two
// fixed files and never grow beyond them.
const char* kSlotPath[kSlotCount] = {
    "/spiffs/dash0.rec",
    "/spiffs/dash1.rec",
};

// The assets partition photo_storage mounts at /spiffs. The frame slots live
// on that same filesystem: the partition table is deliberately unchanged, so
// there is nowhere else to put them. See docs/STORAGE.md for what that does and
// does not guarantee.
const char* kSpiffsPartitionLabel = "assets";

// Slack required beyond the record itself before a write is attempted.
constexpr size_t kFreeSpaceMargin = 8 * 1024;

const char* kNvsNamespace = "dashboard";
const char* kNvsTokenKey = "token";
const char* kNvsLockdownKey = "lockdown";

inline uint64_t NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

/// SPIFFS-backed slot storage.
class SpiffsSlotIo : public SlotIo {
public:
    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        if (slot < 0 || slot >= kSlotCount) return -1;
        FILE* f = fopen(kSlotPath[slot], "rb");
        if (f == nullptr) {
            return 0;   // absent is not an error; it is just an empty slot
        }
        const size_t n = fread(buf, 1, max, f);
        const bool bad = ferror(f) != 0;
        fclose(f);
        if (bad) {
            ESP_LOGE(kTag, "read %s failed: %s", kSlotPath[slot], strerror(errno));
            ++read_failures;
            return -1;
        }
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (slot < 0 || slot >= kSlotCount) return false;

        // Refuse early if the filesystem plainly cannot hold this record.
        // fopen(..., "wb") truncates the target immediately, so discovering
        // ENOSPC halfway through means we have already destroyed the old
        // contents of this slot for nothing. The active slot is a different
        // file and is unaffected either way, but there is no reason to burn a
        // rollback copy to learn something we can check first.
        size_t total = 0, used = 0;
        if (esp_spiffs_info(kSpiffsPartitionLabel, &total, &used) == ESP_OK) {
            const size_t free_bytes = (total > used) ? (total - used) : 0;
            struct stat st;
            const size_t reclaimed =
                (stat(kSlotPath[slot], &st) == 0) ? static_cast<size_t>(st.st_size) : 0;
            // SPIFFS needs slack for its own metadata; require a margin rather
            // than exactly the record size.
            if (free_bytes + reclaimed < len + kFreeSpaceMargin) {
                ESP_LOGE(kTag,
                         "refusing to write %s: %u bytes free (+%u reclaimable), "
                         "need %u + %u margin",
                         kSlotPath[slot], static_cast<unsigned>(free_bytes),
                         static_cast<unsigned>(reclaimed), static_cast<unsigned>(len),
                         static_cast<unsigned>(kFreeSpaceMargin));
                ++write_failures;
                return false;
            }
        } else {
            // Not fatal: the write below reports its own errors. Worth knowing.
            ESP_LOGW(kTag, "esp_spiffs_info(%s) failed; writing without a space check",
                     kSpiffsPartitionLabel);
        }

        FILE* f = fopen(kSlotPath[slot], "wb");
        if (f == nullptr) {
            ESP_LOGE(kTag, "open %s failed: %s", kSlotPath[slot], strerror(errno));
            ++write_failures;
            return false;
        }
        const size_t written = fwrite(data, 1, len, f);
        const int write_errno = (written != len) ? errno : 0;
        // Push through the VFS cache before declaring anything. The caller
        // re-reads and re-validates afterwards regardless, but there is no
        // reason to make it catch a failure we can see here.
        const bool flushed = (fflush(f) == 0);
        if (flushed) {
            fsync(fileno(f));
        }
        const bool closed = (fclose(f) == 0);

        if (written != len || !flushed || !closed) {
            ESP_LOGE(kTag, "write %s short: %u/%u flushed=%d closed=%d errno=%d (%s)",
                     kSlotPath[slot], static_cast<unsigned>(written),
                     static_cast<unsigned>(len), flushed ? 1 : 0, closed ? 1 : 0,
                     write_errno, write_errno ? strerror(write_errno) : "-");
            // Remove the stump. It would fail its own checksum and be ignored,
            // so it is not a correctness problem, but leaving 30 KB of garbage
            // on a full filesystem would make the next attempt fail too.
            if (unlink(kSlotPath[slot]) != 0) {
                ESP_LOGW(kTag, "could not remove partial %s: %s",
                         kSlotPath[slot], strerror(errno));
            }
            ++write_failures;
            return false;
        }
        return true;
    }

    /// Counts writes this layer refused or could not complete. Surfaced in the
    /// status route so a filesystem that is quietly failing is visible from the
    /// Mac rather than only in a serial log nobody is watching.
    uint32_t write_failures = 0;
    uint32_t read_failures = 0;
};

SpiffsSlotIo g_spiffs_io;

}  // namespace

DashboardManager& DashboardManager::GetInstance() {
    static DashboardManager instance;
    return instance;
}

bool DashboardManager::Init() {
    if (initialised_) return true;

    lock_ = xSemaphoreCreateMutex();
    if (lock_ == nullptr) {
        ESP_LOGE(kTag, "mutex allocation failed");
        return false;
    }

    // Both buffers go to PSRAM: 60 KB of internal SRAM is far too precious on
    // this board, and neither buffer is touched from an ISR.
    scratch_ = static_cast<uint8_t*>(heap_caps_malloc(kRecordBytes, MALLOC_CAP_SPIRAM));
    render_buf_ = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (scratch_ == nullptr || render_buf_ == nullptr) {
        ESP_LOGE(kTag, "PSRAM allocation failed (scratch=%p render=%p)",
                 static_cast<void*>(scratch_), static_cast<void*>(render_buf_));
        heap_caps_free(scratch_);
        heap_caps_free(render_buf_);
        scratch_ = nullptr;
        render_buf_ = nullptr;
        return false;
    }

    io_ = &g_spiffs_io;
    static DashboardSlot slot_store(io_, scratch_, kRecordBytes);
    store_ = &slot_store;

    const bool have = store_->Load();
    ESP_LOGI(kTag, "slot scan: frame=%d slot=%d seq=%u (slot0 valid=%d, slot1 valid=%d)",
             have ? 1 : 0, store_->active_slot(),
             static_cast<unsigned>(store_->active_seq()),
             store_->slot_status(0).valid ? 1 : 0,
             store_->slot_status(1).valid ? 1 : 0);

    // Load the token. Deliberately never logged, not even its length.
    {
        Settings s(kNvsNamespace, false);
        const std::string hex = s.GetString(kNvsTokenKey, "");
        if (hex.size() == kTokenHexChars) {
            uint8_t token[kTokenBytes];
            if (HexDecode(hex.c_str(), hex.size(), token, kTokenBytes)) {
                auth_.SetToken(token, kTokenBytes);
            }
            memset(token, 0, sizeof(token));
        }
        lockdown_ = s.GetBool(kNvsLockdownKey, true);
    }
    ESP_LOGI(kTag, "provisioned=%d lockdown=%d", auth_.provisioned() ? 1 : 0,
             lockdown_ ? 1 : 0);

    initialised_ = true;
    return true;
}

void DashboardManager::SetPanelPainter(PanelPaintFn fn) {
    painter_ = std::move(fn);
    if (render_task_ == nullptr && painter_) {
        // Small stack: this task only shuffles buffers and calls the painter.
        const BaseType_t created =
            xTaskCreatePinnedToCore(&DashboardManager::RenderTaskEntry, "dash_render",
                                    4096, this, 4, &render_task_, 1);
        if (created != pdPASS) {
            // FreeRTOS leaves the handle untouched when it refuses, so
            // render_task_ is still null and CanRender() will say so. Say it
            // out loud too: a dashboard that silently never paints is the
            // hardest kind of failure to diagnose from the other end of a
            // status route.
            render_task_ = nullptr;
            ESP_LOGE(kTag, "dashboard render task could not be created; "
                           "frames will be stored but never drawn");
        }
    }
}

/**
 * @brief Whether a request made now can ever be completed.
 *
 * Checked *before* RefreshCoordinator::Request(), not after, and that ordering
 * is the whole point. Request() unconditionally moves the coordinator into
 * kRendering and returns kStarted; only CompleteRender() moves it back, and
 * only the render task calls that. With no task to notify — the painter was
 * never registered, or xTaskCreatePinnedToCore refused at boot — the state
 * machine enters kRendering and stays there for the life of the boot. Two
 * things follow, and both are silent:
 *
 *  - every later request coalesces into a refresh that will never run, so the
 *    dashboard stops painting permanently, and
 *  - Status() reports rendering=true forever, which application.cc feeds into
 *    WakeInputs::refresh_in_flight, so power_policy refuses sleep with reason
 *    "refresh_in_flight" — a device kept awake by a refresh that does not exist.
 *
 * This is a precondition, deliberately not a timeout. A timeout would have to
 * be longer than the worst real refresh (the panel's own BUSY timeout is 120 s,
 * and Frigo v3 measured 18972 ms), and any timeout at all risks declaring a
 * legitimately slow refresh dead while the panel is still drawing it. Refusing
 * to enter kRendering when nothing can leave it costs nothing and cannot
 * interrupt work that is genuinely in flight.
 */
bool DashboardManager::CanRender() const {
    return render_task_ != nullptr && static_cast<bool>(painter_);
}

// ------------------------------------------------------------ frame access --

bool DashboardManager::HasFrame() {
    if (!initialised_) return false;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool has = store_->has_frame();
    xSemaphoreGive(lock_);
    return has;
}

bool DashboardManager::CopyFrame(uint8_t* out) {
    if (!initialised_ || out == nullptr) return false;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool ok = store_->ReadFrame(out);
    xSemaphoreGive(lock_);
    return ok;
}

PushStatus DashboardManager::Submit(const uint8_t* body,
                                    size_t len,
                                    const char* token_hex,
                                    const char* sha_hex,
                                    const char* idem_key,
                                    uint32_t source_epoch) {
    PushStatus st;

    if (!initialised_) {
        st.outcome = PushOutcome::kStoreFailed;
        return st;
    }

    // Authorise before looking at the body at all, so an unauthorised caller
    // cannot use us as an oracle for anything about the payload.
    switch (CheckToken(token_hex)) {
        case AuthResult::kNotProvisioned: st.outcome = PushOutcome::kNotProvisioned; return st;
        case AuthResult::kLockedOut:      st.outcome = PushOutcome::kLockedOut;      return st;
        case AuthResult::kBadToken:       st.outcome = PushOutcome::kUnauthorized;   return st;
        case AuthResult::kOk:             break;
    }

    if (len != kFrameBytes) {
        st.outcome = PushOutcome::kBadLength;
        return st;
    }

    uint8_t declared[kShaBytes];
    bool have_declared = false;
    if (sha_hex != nullptr && *sha_hex != '\0') {
        if (!HexDecode(sha_hex, strlen(sha_hex), declared, kShaBytes)) {
            st.outcome = PushOutcome::kShaMismatch;
            return st;
        }
        have_declared = true;
    }

    // One mutating request at a time. Refused, not queued: a client that gets a
    // 409 knows its frame was not applied, whereas a queued request that times
    // out leaves it guessing. Claimed atomically — see MutationGate — and
    // released by the scope guard on every path out, including the early
    // returns below.
    MutationClaim claim(mutating_);
    if (!claim.entered()) {
        st.outcome = PushOutcome::kBusy;
        return st;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (idem_key != nullptr && *idem_key != '\0' && idem_.Seen(idem_key, strlen(idem_key))) {
        st.outcome = PushOutcome::kIdempotentReplay;
        st.persisted = true;
        st.seq = store_->active_seq();
        if (store_->active_sha() != nullptr) {
            HexEncode(store_->active_sha(), kShaBytes, st.sha_hex);
        }
        xSemaphoreGive(lock_);
        return st;
    }

    const StoreResult sr = store_->Store(body, len, source_epoch,
                                         have_declared ? declared : nullptr);

    switch (sr) {
        case StoreResult::kBadLength:
            // Store() reports a declared-digest mismatch through the same code;
            // we know the length was right, so this is the digest case.
            st.outcome = have_declared ? PushOutcome::kShaMismatch : PushOutcome::kBadLength;
            xSemaphoreGive(lock_);
            return st;

        case StoreResult::kWriteFailed:
        case StoreResult::kVerifyFailed:
            ESP_LOGE(kTag, "store failed (%d)", static_cast<int>(sr));
            st.outcome = PushOutcome::kStoreFailed;
            xSemaphoreGive(lock_);
            return st;

        case StoreResult::kDuplicate:
            st.outcome = PushOutcome::kDeduped;
            st.persisted = true;
            break;

        case StoreResult::kOk:
            st.outcome = PushOutcome::kAccepted;
            st.persisted = true;
            break;
    }

    if (idem_key != nullptr && *idem_key != '\0') {
        idem_.Record(idem_key, strlen(idem_key));
    }

    st.seq = store_->active_seq();
    if (store_->active_sha() != nullptr) {
        HexEncode(store_->active_sha(), kShaBytes, st.sha_hex);
    }
    const bool can_render = CanRender();
    st.render = can_render
                    ? coord_.Request(store_->active_sha(), store_->active_seq(), false)
                    : RenderDisposition::kSkipped;
    const bool wake = (st.render == RenderDisposition::kStarted);

    xSemaphoreGive(lock_);

    if (!can_render) {
        // The frame is stored and verified; only the drawing is impossible.
        // Reported as "skipped" rather than started, so the caller is not told
        // to wait for a refresh that nobody will perform.
        ESP_LOGE(kTag, "frame stored but no render task exists; it will be drawn "
                       "at the next boot, not now");
    }
    if (wake) NotifyRenderTask();
    return st;
}

PushStatus DashboardManager::SubmitLocal(const uint8_t* body, size_t len,
                                         uint32_t source_epoch,
                                         uint32_t expected_seq) {
    PushStatus st;

    if (!initialised_) {
        st.outcome = PushOutcome::kStoreFailed;
        return st;
    }
    if (len != kFrameBytes) {
        st.outcome = PushOutcome::kBadLength;
        return st;
    }

    // The same single-mutator rule as Submit(), claimed the same atomic way. A
    // push arriving while a local frame is being composed is refused here rather
    // than queued — and refused on *this* side, so the operator's frame is never
    // the one that loses. The device simply does not draw its own this time.
    MutationClaim claim(mutating_);
    if (!claim.entered()) {
        st.outcome = PushOutcome::kBusy;
        return st;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    // The compare half of the compare-and-swap, taken under the same lock as
    // the store itself so nothing can slip between them. The gate above only
    // covers writers that overlap *now*; this covers the seconds of composing
    // that happened before we asked for the gate at all, which is where a PUT
    // actually lands. The tower always wins: it is the one that already wrote.
    if (store_->active_seq() != expected_seq) {
        st.outcome = PushOutcome::kSuperseded;
        st.seq = store_->active_seq();
        if (store_->active_sha() != nullptr) {
            HexEncode(store_->active_sha(), kShaBytes, st.sha_hex);
        }
        ESP_LOGI(kTag, "local frame dropped: seq moved %u -> %u while composing",
                 static_cast<unsigned>(expected_seq),
                 static_cast<unsigned>(store_->active_seq()));
        xSemaphoreGive(lock_);
        return st;
    }

    // The origin flag is what makes this answerable after a reboot. It is a bit
    // in the record's own header, so the device reads where the frame came from
    // rather than remembering it — and a device that has just woken from deep
    // sleep still knows.
    const StoreResult sr =
        store_->Store(body, len, source_epoch, nullptr, kFlagOriginLocal);

    switch (sr) {
        case StoreResult::kBadLength:
            st.outcome = PushOutcome::kBadLength;
            xSemaphoreGive(lock_);
            return st;

        case StoreResult::kWriteFailed:
        case StoreResult::kVerifyFailed:
            ESP_LOGE(kTag, "local store failed (%d)", static_cast<int>(sr));
            st.outcome = PushOutcome::kStoreFailed;
            xSemaphoreGive(lock_);
            return st;

        case StoreResult::kDuplicate:
            // The cheapest good news this device produces: the panel already
            // shows exactly these bytes, so there is no write and no refresh.
            st.outcome = PushOutcome::kDeduped;
            st.persisted = true;
            break;

        case StoreResult::kOk:
            st.outcome = PushOutcome::kAccepted;
            st.persisted = true;
            break;
    }

    st.seq = store_->active_seq();
    if (store_->active_sha() != nullptr) {
        HexEncode(store_->active_sha(), kShaBytes, st.sha_hex);
    }
    const bool can_render = CanRender();
    st.render = can_render
                    ? coord_.Request(store_->active_sha(), store_->active_seq(), false)
                    : RenderDisposition::kSkipped;
    const bool wake = (st.render == RenderDisposition::kStarted);

    xSemaphoreGive(lock_);

    if (!can_render) {
        ESP_LOGE(kTag, "local frame stored but no render task exists; it will be "
                       "drawn at the next boot, not now");
    }
    if (wake) NotifyRenderTask();
    return st;
}

AuthResult DashboardManager::CheckToken(const char* token_hex) {
    if (!initialised_) return AuthResult::kNotProvisioned;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const AuthResult r = auth_.Check(token_hex, token_hex ? strlen(token_hex) : 0, NowMs());
    xSemaphoreGive(lock_);
    return r;
}

RenderDisposition DashboardManager::RequestRedraw(bool force) {
    if (!initialised_ || !CanRender()) return RenderDisposition::kSkipped;

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (!store_->has_frame()) {
        xSemaphoreGive(lock_);
        return RenderDisposition::kSkipped;
    }
    const RenderDisposition d = coord_.Request(store_->active_sha(), store_->active_seq(), force);
    xSemaphoreGive(lock_);

    if (d == RenderDisposition::kStarted) NotifyRenderTask();
    return d;
}

DashboardStatus DashboardManager::Status() {
    DashboardStatus s;
    if (!initialised_) return s;

    xSemaphoreTake(lock_, portMAX_DELAY);
    s.has_stored_frame = store_->has_frame();
    s.stored_seq = store_->active_seq();
    s.stored_source_epoch = store_->active_source_epoch();
    if (store_->active_sha() != nullptr) {
        HexEncode(store_->active_sha(), kShaBytes, s.stored_sha_hex);
    }
    s.stored_origin_is_local = (store_->active_flags() & kFlagOriginLocal) != 0;

    s.has_displayed_frame = coord_.has_displayed();
    s.displayed_seq = coord_.displayed_seq();
    if (coord_.displayed_sha() != nullptr) {
        HexEncode(coord_.displayed_sha(), kShaBytes, s.displayed_sha_hex);
    }

    // Whether the origin bit in the stored record also describes the glass.
    // Nothing is remembered here — the answer is derived from the facts the
    // manager already holds, which is why it stays right across a push, a local
    // compose and a reboot. The rule itself is portable and host-tested.
    s.displayed_origin_known = DisplayedFrameIsStoredFrame(
        s.has_stored_frame, s.stored_seq, s.stored_sha_hex, s.has_displayed_frame,
        s.displayed_seq, s.displayed_sha_hex);
    s.displayed_origin_is_local =
        s.displayed_origin_known && s.stored_origin_is_local;

    s.rendering = (coord_.state() == RefreshState::kRendering);
    s.pending = coord_.pending();
    s.provisioned = auth_.provisioned();
    s.lockdown = lockdown_;
    s.skipped_renders = coord_.skipped_count();
    s.coalesced_requests = coord_.coalesced_count();
    s.render_count = render_count_;
    s.failed_renders = coord_.failed_count();
    s.last_render_failed = coord_.last_render_failed();
    s.last_failed_seq = coord_.last_failed_seq();
    s.deferred_renders = coord_.deferred_count();
    s.last_render_deferred = coord_.last_render_deferred();
    s.last_deferred_seq = coord_.last_deferred_seq();
    s.last_read_ms = last_read_ms_;
    s.last_blit_ms = last_blit_ms_;
    s.last_panel_ms = last_panel_ms_;
    s.last_total_ms = last_total_ms_;
    s.storage_write_failures = g_spiffs_io.write_failures;
    s.storage_read_failures = g_spiffs_io.read_failures;
    xSemaphoreGive(lock_);

    // Outside the lock: this touches the filesystem and nothing above depends
    // on it being consistent with the snapshot.
    size_t total = 0, used = 0;
    if (esp_spiffs_info(kSpiffsPartitionLabel, &total, &used) == ESP_OK) {
        s.spiffs_total_bytes = static_cast<uint32_t>(total);
        s.spiffs_used_bytes = static_cast<uint32_t>(used);
    }
    return s;
}

// ----------------------------------------------------------- provisioning --

bool DashboardManager::Provisioned() {
    if (!initialised_) return false;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool p = auth_.provisioned();
    xSemaphoreGive(lock_);
    return p;
}

bool DashboardManager::OpenPairing() {
    if (!initialised_) return false;

    uint8_t token[kTokenBytes];
    // Hardware RNG. Seeded from RF noise once Wi-Fi/BT is up, which it is by
    // the time anyone can reach the Settings menu.
    esp_fill_random(token, sizeof(token));

    xSemaphoreTake(lock_, portMAX_DELAY);
    memcpy(pending_token_, token, kTokenBytes);
    has_pending_token_ = true;
    pairing_.Open(NowMs());
    xSemaphoreGive(lock_);

    memset(token, 0, sizeof(token));
    ESP_LOGI(kTag, "pairing window opened for %u s",
             static_cast<unsigned>(PairingWindow::kDefaultDurationMs / 1000));
    return true;
}

bool DashboardManager::ClaimPairing(char* out_hex) {
    if (!initialised_ || out_hex == nullptr) return false;

    char hex[kTokenHexChars + 1] = {};
    bool claimed = false;

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (has_pending_token_ && pairing_.Claim(NowMs())) {
        HexEncode(pending_token_, kTokenBytes, hex);
        auth_.SetToken(pending_token_, kTokenBytes);
        // A new pairing must not inherit the previous peer's replay window.
        idem_.Clear();
        memset(pending_token_, 0, sizeof(pending_token_));
        has_pending_token_ = false;
        claimed = true;
    }
    xSemaphoreGive(lock_);

    if (!claimed) {
        return false;
    }

    // Persist outside the lock: NVS writes can take a while and nothing else
    // depends on this handle.
    {
        Settings s(kNvsNamespace, true);
        s.SetString(kNvsTokenKey, hex);
    }

    memcpy(out_hex, hex, sizeof(hex));
    memset(hex, 0, sizeof(hex));

    // Intentionally no token material in this log line, now or ever.
    ESP_LOGI(kTag, "pairing claimed; dashboard token installed");
    return true;
}

void DashboardManager::CancelPairing() {
    if (!initialised_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    pairing_.Close();
    memset(pending_token_, 0, sizeof(pending_token_));
    has_pending_token_ = false;
    xSemaphoreGive(lock_);
    ESP_LOGI(kTag, "pairing window closed");
}

PairingWindow::State DashboardManager::PairingState() {
    if (!initialised_) return PairingWindow::State::kClosed;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const PairingWindow::State st = pairing_.state(NowMs());
    xSemaphoreGive(lock_);
    return st;
}

uint32_t DashboardManager::PairingRemainingSeconds() {
    if (!initialised_) return 0;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const uint64_t ms = pairing_.remaining_ms(NowMs());
    xSemaphoreGive(lock_);
    return static_cast<uint32_t>(ms / 1000);
}

void DashboardManager::ClearToken() {
    if (!initialised_) return;
    {
        Settings s(kNvsNamespace, true);
        s.EraseKey(kNvsTokenKey);
    }
    xSemaphoreTake(lock_, portMAX_DELAY);
    auth_.SetToken(nullptr, 0);
    idem_.Clear();
    pairing_.Close();
    memset(pending_token_, 0, sizeof(pending_token_));
    has_pending_token_ = false;
    xSemaphoreGive(lock_);
    ESP_LOGI(kTag, "dashboard token cleared; writes denied until re-provisioned");
}

bool DashboardManager::LockdownEnabled() {
    return lockdown_;
}

void DashboardManager::SetLockdownEnabled(bool enabled) {
    lockdown_ = enabled;
    Settings s(kNvsNamespace, true);
    s.SetBool(kNvsLockdownKey, enabled);
    ESP_LOGI(kTag, "legacy route lockdown %s", enabled ? "enabled" : "DISABLED");
}

// ------------------------------------------------------------ render task --

void DashboardManager::NotifyRenderTask() {
    if (render_task_ != nullptr) {
        xTaskNotifyGive(render_task_);
    }
}

void DashboardManager::RenderTaskEntry(void* arg) {
    static_cast<DashboardManager*>(arg)->RenderLoop();
}

void DashboardManager::RenderLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Drain: CompleteRender tells us when a coalesced successor is waiting.
        bool again = true;
        while (again) {
            const int64_t t_start = esp_timer_get_time();

            // Take the frame and its identity under the lock, then let go. The
            // panel refresh below can take many seconds and must not hold off
            // the HTTP server for any of them.
            xSemaphoreTake(lock_, portMAX_DELAY);
            const bool have = store_->ReadFrame(render_buf_);
            uint8_t sha[kShaBytes] = {};
            uint32_t seq = 0;
            if (have && store_->active_sha() != nullptr) {
                memcpy(sha, store_->active_sha(), kShaBytes);
                seq = store_->active_seq();
            }
            xSemaphoreGive(lock_);

            const int64_t t_read = esp_timer_get_time();

            RenderOutcome outcome = RenderOutcome::kFailed;
            uint32_t panel_ms = 0;
            if (have && painter_) {
                outcome = painter_(render_buf_, &panel_ms);
            } else if (!have) {
                ESP_LOGW(kTag, "render requested with no valid stored frame");
            }
            const bool ok = (outcome == RenderOutcome::kDrawn);

            const int64_t t_done = esp_timer_get_time();
            const uint32_t read_ms = static_cast<uint32_t>((t_read - t_start) / 1000);
            const uint32_t total_ms = static_cast<uint32_t>((t_done - t_start) / 1000);
            // Everything that is not the panel and not the flash read is our
            // own blit and bookkeeping.
            const uint32_t blit_ms = (total_ms > read_ms + panel_ms)
                                         ? total_ms - read_ms - panel_ms : 0;

            xSemaphoreTake(lock_, portMAX_DELAY);
            last_read_ms_ = read_ms;
            last_blit_ms_ = blit_ms;
            last_panel_ms_ = panel_ms;
            last_total_ms_ = total_ms;
            if (ok) ++render_count_;
            again = coord_.CompleteRender(ok ? sha : nullptr, seq, outcome);
            xSemaphoreGive(lock_);

            ESP_LOGI(kTag,
                     "render seq=%u ok=%d deferred=%d read=%ums blit=%ums panel=%ums total=%ums pending=%d",
                     static_cast<unsigned>(seq), ok ? 1 : 0,
                     outcome == RenderOutcome::kDeferred ? 1 : 0,
                     static_cast<unsigned>(read_ms), static_cast<unsigned>(blit_ms),
                     static_cast<unsigned>(panel_ms), static_cast<unsigned>(total_ms),
                     again ? 1 : 0);

            if (!ok && !again) {
                break;   // nothing pending and we failed; wait for a new request
            }
        }
    }
}

}  // namespace dashboard
