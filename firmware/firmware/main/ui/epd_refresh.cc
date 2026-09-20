/**
 * @file epd_refresh.cc
 * @brief EPD refresh scheduler implementation
 */

#include "epd_refresh.h"
#include <esp_log.h>

static const char* kTag = "EpdRefresh";

namespace ui {

EpdRefreshScheduler::EpdRefreshScheduler()
    : config_()
    , callback_(nullptr)
    , task_handle_(nullptr)
    , mutex_(nullptr)
    , event_group_(nullptr)
    , dirty_rect_({0, 0, 0, 0})
    , has_dirty_(false)
    , partial_count_(0)
    , full_refresh_pending_(false)
    , running_(false) {
}

EpdRefreshScheduler::~EpdRefreshScheduler() {
    Stop();
    if (mutex_) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void EpdRefreshScheduler::Init(EpdRefreshCallback callback, const EpdRefreshConfig& config) {
    callback_ = callback;
    config_ = config;

    // Create mutex for dirty rect protection
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        ESP_LOGE(kTag, "Failed to create mutex");
        return;
    }

    // Create event group for task signaling
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(kTag, "Failed to create event group");
        return;
    }

    ESP_LOGI(kTag, "EPD refresh scheduler initialized (partial threshold: %d)",
             config_.partial_count_threshold);
}

void EpdRefreshScheduler::Start() {
    if (running_ || task_handle_) {
        ESP_LOGW(kTag, "Task already running");
        return;
    }

    running_ = true;

    // Create task pinned to CPU1
    BaseType_t ret = xTaskCreatePinnedToCore(
        RefreshTask,
        "epd_refresh",
        config_.task_stack_size,
        this,
        config_.task_priority,
        &task_handle_,
        config_.cpu_core
    );

    if (ret != pdPASS) {
        ESP_LOGE(kTag, "Failed to create refresh task");
        running_ = false;
        task_handle_ = nullptr;
        return;
    }

    ESP_LOGI(kTag, "Refresh task started on CPU%d", config_.cpu_core);
}

void EpdRefreshScheduler::Stop() {
    if (!running_ || !task_handle_) {
        return;
    }

    running_ = false;

    // Signal task to stop
    xEventGroupSetBits(event_group_, kStopTaskBit);

    // Wait for task to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }

    ESP_LOGI(kTag, "Refresh task stopped");
}

void EpdRefreshScheduler::RefreshTask(void* arg) {
    EpdRefreshScheduler* self = static_cast<EpdRefreshScheduler*>(arg);

    ESP_LOGI(kTag, "Refresh task running");

    while (self->running_) {
        // Wait for refresh request or stop signal
        EventBits_t bits = xEventGroupWaitBits(
            self->event_group_,
            kRefreshRequestBit | kStopTaskBit,
            pdTRUE,  // Clear bits on exit
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(100)  // Timeout for periodic check
        );

        if (bits & kStopTaskBit) {
            break;
        }

        // Process refresh if requested or if we have pending dirty
        if (bits & kRefreshRequestBit || self->has_dirty_) {
            self->ProcessRefresh();
        }
    }

    ESP_LOGI(kTag, "Refresh task exiting");
    vTaskDelete(NULL);
}

void EpdRefreshScheduler::MarkDirty(const rawdraw::Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return;

    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);

    if (has_dirty_) {
        // Union with existing dirty rect
        dirty_rect_ = rawdraw::rect_union(dirty_rect_, rect);
    } else {
        dirty_rect_ = rect;
        has_dirty_ = true;
    }

    if (mutex_) xSemaphoreGive(mutex_);
}

void EpdRefreshScheduler::TriggerRefresh(bool urgent) {
    // Signal refresh task
    if (event_group_) {
        xEventGroupSetBits(event_group_, kRefreshRequestBit);
    }

    // For urgent refresh, process immediately in this context
    // (caller may need immediate feedback)
    if (urgent && callback_) {
        ProcessRefresh();
    }
}

void EpdRefreshScheduler::RequestFullRefresh() {
    full_refresh_pending_ = true;
    partial_count_ = 0;
    TriggerRefresh(true);
}

void EpdRefreshScheduler::ResetPartialCount() {
    partial_count_ = 0;
}

rawdraw::Rect EpdRefreshScheduler::AlignRect(const rawdraw::Rect& rect) const {
    return rawdraw::align_x8(rect);
}

void EpdRefreshScheduler::ProcessRefresh() {
    if (!callback_) return;

    rawdraw::Rect rect_to_refresh;
    bool do_full = false;

    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);

    // Check if full refresh is pending
    if (full_refresh_pending_) {
        do_full = true;
        full_refresh_pending_ = false;
        // Full refresh: entire screen
        rect_to_refresh = {0, 0, 400, 300};
        partial_count_ = 0;
    } else if (has_dirty_) {
        // Partial refresh with dirty rect
        rect_to_refresh = AlignRect(dirty_rect_);
        has_dirty_ = false;
        dirty_rect_ = {0, 0, 0, 0};

        // Increment partial count
        partial_count_++;

        // Check threshold for automatic full refresh
        if (partial_count_ >= config_.partial_count_threshold) {
            do_full = true;
            rect_to_refresh = {0, 0, 400, 300};
            partial_count_ = 0;
            ESP_LOGD(kTag, "Auto full refresh after %d partials", partial_count_);
        }
    }

    if (mutex_) xSemaphoreGive(mutex_);

    // Call refresh callback
    if (rect_to_refresh.w > 0 && rect_to_refresh.h > 0) {
        EpdRefreshMode mode = do_full ? EpdRefreshMode::Full : EpdRefreshMode::Partial;
        callback_(rect_to_refresh, mode);
    }
}

}  // namespace ui