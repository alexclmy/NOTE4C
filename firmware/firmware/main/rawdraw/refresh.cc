/**
 * @file refresh.cc
 * @brief EPD partial refresh control with counter-based full refresh trigger
 *
 * Manages per-region refresh counters, throttling, and partial window
 * configuration for the SSD1683 EPD controller.
 *
 * Key features:
 * - Per-region refresh counter (triggers full refresh every 100 partials)
 * - Throttle control (default 300ms, configurable)
 * - SET_PARTIAL_WINDOW (0x90) and Partial Display Enable (0x91) support
 *
 * Hardware constraints:
 * - 1bpp x-coordinates must be 8-byte aligned (use align_x8)
 * - Minimum refresh interval >= 300ms (default), 2s for clock
 * - Every 100 partial refreshes triggers a full refresh to clear ghosting
 */

#include <esp_log.h>
#include <esp_timer.h>
#include "rawdraw.h"
#include "rawdraw/framebuffer.h"
#include "refresh.h"

#define TAG "epd_refresh"

namespace rawdraw {

// ============================================================
// SSD1683 partial window commands
// ============================================================
static constexpr uint8_t kCmdPartialWindow = 0x90;  ///< SET_PARTIAL_WINDOW
static constexpr uint8_t kCmdPartialEnable = 0x91;  ///< Partial Display Enable

// ============================================================
// Region tracker
// ============================================================

void refresh_tracker_init(RegionRefresh* tracker) {
    if (!tracker) return;
    tracker->last_refresh_us = 0;
    tracker->partial_count = 0;
    tracker->dirty = false;
    tracker->needs_full = false;
}

bool refresh_should_refresh(const RegionRefresh* tracker, int64_t now_us,
                            int64_t min_interval_ms) {
    if (!tracker) return false;

    // Force full refresh if counter exceeded
    if (tracker->partial_count >= 100) return true;

    // Check throttle interval
    if (now_us - tracker->last_refresh_us < min_interval_ms * 1000) {
        return false;
    }

    return tracker->dirty;
}

void refresh_mark_dirty(RegionRefresh* tracker) {
    if (tracker) tracker->dirty = true;
}

void refresh_mark_clean(RegionRefresh* tracker) {
    if (tracker) tracker->dirty = false;
}

void refresh_update_counter(RegionRefresh* tracker, int64_t now_us) {
    if (!tracker) return;
    tracker->partial_count++;
    tracker->last_refresh_us = now_us;
    tracker->dirty = false;

    if (tracker->partial_count >= 100) {
        tracker->needs_full = true;
    }
}

void refresh_reset_counter(RegionRefresh* tracker) {
    if (!tracker) return;
    tracker->partial_count = 0;
    tracker->needs_full = false;
}

int refresh_get_partial_count(const RegionRefresh* tracker) {
    return tracker ? tracker->partial_count : 0;
}

bool refresh_needs_full(const RegionRefresh* tracker) {
    return tracker && tracker->needs_full;
}

}  // namespace rawdraw
