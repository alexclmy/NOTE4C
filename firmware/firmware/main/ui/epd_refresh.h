/**
 * @file epd_refresh.h
 * @brief EPD refresh scheduler for managing partial/full refresh cycles
 *
 * Features:
 * - FreeRTOS task pinned to CPU1 for EPD refresh
 * - Accumulates dirty rects for partial refresh (EPD_DisplayPart)
 * - Every N partial refreshes triggers full refresh (EPD_Display) to clear ghosting
 * - Page switch triggers full refresh
 * - x/width alignment for 8-byte boundary (EPD hardware requirement)
 */

#ifndef UI_EPD_REFRESH_H
#define UI_EPD_REFRESH_H

#include "rawdraw/rawdraw.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <functional>

namespace ui {

/**
 * @brief EPD refresh mode
 */
enum class EpdRefreshMode {
    Partial,    ///< Partial refresh (faster, but accumulates ghosting)
    Full,       ///< Full refresh (slower, clears ghosting)
};

/**
 * @brief EPD refresh scheduler configuration
 */
struct EpdRefreshConfig {
    int partial_count_threshold = 10;  ///< Full refresh after N partials
    int task_stack_size = 4096;         ///< Stack size for refresh task
    int task_priority = 5;              ///< Task priority
    int cpu_core = 1;                   ///< Pin to CPU1 for UI/display tasks
};

/**
 * @brief EPD refresh callback type
 *
 * Called when refresh is triggered.
 * @param rect Dirty rect (aligned to 8-byte boundary)
 * @param mode Refresh mode
 */
using EpdRefreshCallback = std::function<void(const rawdraw::Rect& rect, EpdRefreshMode mode)>;

/**
 * @brief EPD Refresh Scheduler
 *
 * Manages EPD refresh scheduling with dirty rect accumulation and
 * automatic full refresh to prevent ghosting.
 *
 * Usage:
 * 1. Create scheduler with callback
 * 2. Call MarkDirty() when framebuffer regions are updated
 * 3. Call TriggerRefresh() to request immediate refresh
 * 4. Call RequestFullRefresh() on page switch or for ghosting cleanup
 */
class EpdRefreshScheduler {
public:
    EpdRefreshScheduler();
    ~EpdRefreshScheduler();

    /**
     * @brief Initialize scheduler
     *
     * @param callback Function to call when refresh is triggered
     * @param config Configuration options
     */
    void Init(EpdRefreshCallback callback, const EpdRefreshConfig& config = {});

    /**
     * @brief Start refresh task
     */
    void Start();

    /**
     * @brief Stop refresh task
     */
    void Stop();

    /**
     * @brief Mark a region as dirty
     *
     * Accumulates dirty rect. Will be flushed on next refresh trigger.
     *
     * @param rect Region that needs refresh
     */
    void MarkDirty(const rawdraw::Rect& rect);

    /**
     * @brief Request refresh (triggers task to process dirty rects)
     *
     * @param urgent If true, process immediately; otherwise may wait for more dirties
     */
    void TriggerRefresh(bool urgent = false);

    /**
     * @brief Request full refresh
     *
     * Resets partial count and triggers full refresh.
     * Call on page switch or when ghosting becomes visible.
     */
    void RequestFullRefresh();

    /**
     * @brief Get current partial refresh count
     */
    int GetPartialCount() const { return partial_count_; }

    /**
     * @brief Reset partial refresh count
     */
    void ResetPartialCount();

    /**
     * @brief Check if scheduler is running
     */
    bool IsRunning() const { return task_handle_ != nullptr; }

private:
    // Task function
    static void RefreshTask(void* arg);

    // Process accumulated dirty rects
    void ProcessRefresh();

    // Align rect to 8-byte boundary
    rawdraw::Rect AlignRect(const rawdraw::Rect& rect) const;

    // Configuration
    EpdRefreshConfig config_;

    // Callback
    EpdRefreshCallback callback_;

    // Task handle
    TaskHandle_t task_handle_ = nullptr;

    // Mutex for protecting dirty rect
    SemaphoreHandle_t mutex_ = nullptr;

    // Event group for signaling refresh request
    EventGroupHandle_t event_group_ = nullptr;

    // Dirty rect accumulation
    rawdraw::Rect dirty_rect_;
    bool has_dirty_ = false;

    // Partial refresh counter
    int partial_count_ = 0;

    // Full refresh pending flag
    bool full_refresh_pending_ = false;

    // Task running flag
    bool running_ = false;

    // Event bits
    static constexpr uint32_t kRefreshRequestBit = 1 << 0;
    static constexpr uint32_t kStopTaskBit = 1 << 1;
};

}  // namespace ui

#endif  // UI_EPD_REFRESH_H