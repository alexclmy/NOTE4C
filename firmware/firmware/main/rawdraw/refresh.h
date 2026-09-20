/**
 * @file refresh.h
 * @brief EPD partial refresh control with counter-based full refresh trigger
 */

#ifndef RAWDRAW_REFRESH_H
#define RAWDRAW_REFRESH_H

#include <stdint.h>
#include <stdbool.h>

namespace rawdraw {

/**
 * @brief Per-region refresh tracking state
 *
 * Each independently-updated UI region gets its own tracker.
 * When partial_count reaches 100, a full refresh is triggered
 * to clear ghosting artifacts.
 */
struct RegionRefresh {
    int64_t last_refresh_us;  ///< Last refresh timestamp (microseconds)
    int     partial_count;    ///< Consecutive partial refreshes (0-100)
    bool    dirty;            ///< Region needs refresh
    bool    needs_full;       ///< Full refresh required (counter expired)
};

/**
 * @brief Initialize a region refresh tracker
 */
void refresh_tracker_init(RegionRefresh* tracker);

/**
 * @brief Check if region should be refreshed
 *
 * @param tracker Region state
 * @param now_us Current time in microseconds
 * @param min_interval_ms Minimum interval between refreshes
 * @return true if refresh should proceed
 */
bool refresh_should_refresh(const RegionRefresh* tracker, int64_t now_us,
                            int64_t min_interval_ms);

/**
 * @brief Mark region as dirty (needs refresh)
 */
void refresh_mark_dirty(RegionRefresh* tracker);

/**
 * @brief Mark region as clean (refreshed)
 */
void refresh_mark_clean(RegionRefresh* tracker);

/**
 * @brief Update refresh counter after a successful partial refresh
 *
 * Increments partial_count. Sets needs_full when counter reaches 100.
 */
void refresh_update_counter(RegionRefresh* tracker, int64_t now_us);

/**
 * @brief Reset refresh counter (after full refresh)
 */
void refresh_reset_counter(RegionRefresh* tracker);

/**
 * @brief Get current partial refresh count
 */
int refresh_get_partial_count(const RegionRefresh* tracker);

/**
 * @brief Check if full refresh is needed
 */
bool refresh_needs_full(const RegionRefresh* tracker);

}  // namespace rawdraw

#endif  // RAWDRAW_REFRESH_H
