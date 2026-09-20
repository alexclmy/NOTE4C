#include <stdio.h>
#include <cstring>
#include <algorithm>

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "config.h"
#ifdef HAVE_LVGL
#include "esp_lvgl_port.h"
#endif
#include "settings.h"
#include "custom_lcd_display.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/framebuffer.h"
#include "common/sleep_manager.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(SourceHanSansSC_Medium_slim);
LV_FONT_DECLARE(font_zectrix_16_1);  // Icon font 16px (IcoMoon)
LV_FONT_DECLARE(font_zectrix_48_1);  // Icon font 48px (IcoMoon)
LV_FONT_DECLARE(weather_icons_16);   // Weather icons 16px (FontAwesome)
LV_FONT_DECLARE(weather_icons_48);   // Weather icons 48px (FontAwesome)

#define TAG "CustomLcdDisplay"
static constexpr uint32_t kDisplayKickMs = 1000;
static constexpr int kFourColorSampleIntervalMs = 12000;

#ifndef EXAMPLE_LCD_WIDTH
#define EXAMPLE_LCD_WIDTH  400
#endif
#ifndef EXAMPLE_LCD_HEIGHT
#define EXAMPLE_LCD_HEIGHT 300
#endif

#ifdef HAVE_LVGL
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT * BYTES_PER_PIXEL)
#endif

#undef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) ((void)0)

// --------------------
// Rect helpers
// --------------------
static inline int rect_area(const Rect &r) {
    return (r.w > 0 && r.h > 0) ? (r.w * r.h) : 0;
}
static inline Rect rect_union(const Rect &a, const Rect &b) {
    if (rect_area(a) == 0) return b;
    if (rect_area(b) == 0) return a;
    int x1 = std::min(a.x, b.x);
    int y1 = std::min(a.y, b.y);
    int x2 = std::max(a.x + a.w, b.x + b.w);
    int y2 = std::max(a.y + a.h, b.y + b.h);
    return { x1, y1, x2 - x1, y2 - y1 };
}
static inline Rect clamp_rect(const Rect &r, int W, int H) {
    int x1 = std::max(0, r.x);
    int y1 = std::max(0, r.y);
    int x2 = std::min(W, r.x + r.w);
    int y2 = std::min(H, r.y + r.h);
    return { x1, y1, x2 - x1, y2 - y1 };
}
static inline Rect align_x8(const Rect &r) {
    Rect out = r;
    int x0 = (out.x / 8) * 8;
    int x1 = ((out.x + out.w + 7) / 8) * 8;
    out.x = x0;
    out.w = x1 - x0;
    return out;
}

// --------------------
// RGB565 -> BW/2bpp helpers
// --------------------
static inline bool rgb565_is_white(uint16_t c, uint8_t thr) {
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5)  & 0x3F;
    uint8_t b5 = (c)       & 0x1F;

    uint8_t R = (uint8_t)((r5 * 255 + 15) / 31);
    uint8_t G = (uint8_t)((g6 * 255 + 31) / 63);
    uint8_t B = (uint8_t)((b5 * 255 + 15) / 31);

    uint16_t y = (uint16_t)((77 * R + 150 * G + 29 * B) >> 8);
    return y >= thr;
}

static inline uint8_t WhiteFillByte() {
    return 0x55;
}

static inline uint8_t Pack2bppRowTo1bppByte(const uint8_t* row_2bpp, int pixel_x) {
    uint8_t out = 0x00;
    for (int bit = 0; bit < 8; ++bit) {
        const int x = pixel_x + bit;
        const uint8_t packed = row_2bpp[x >> 2];
        const uint8_t shift = static_cast<uint8_t>(6 - ((x & 0x03) << 1));
        const uint8_t color = (packed >> shift) & 0x03;
        // 1bpp panel fallback: only WHITE remains white. BLACK, RED and
        // YELLOW become black so semantic emphasis stays visible.
        if (color == rawdraw::WHITE) {
            out |= static_cast<uint8_t>(1U << (7 - bit));
        }
    }
    return out;
}

// =======================================================
// LVGL flush callback (async) — only compiled when HAVE_LVGL
// =======================================================
#ifdef HAVE_LVGL
void CustomLcdDisplay::lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    assert(disp && area && color_p);
    CustomLcdDisplay *driver = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    assert(driver);

    if (driver->dirty_mutex) {
        xSemaphoreTake(driver->dirty_mutex, portMAX_DELAY);
    }
    const uint16_t *src = (const uint16_t *)color_p;

    int x1 = std::max(0, (int)area->x1);
    int y1 = std::max(0, (int)area->y1);
    int x2 = std::min(driver->Width - 1,  (int)area->x2);
    int y2 = std::min(driver->Height - 1, (int)area->y2);

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int src_w = (area->x2 - area->x1 + 1);

    //ESP_LOGI(TAG, "[FLUSH] LVGL area: x=%d-%d, y=%d-%d, w=%d, h=%d", x1, x2, y1, y2, w, h);

    // Convert RGB565 -> 2bpp into driver->buffer (current threshold mapping remains black/white)
    for (int yy = 0; yy < h; yy++) {
        int y = y1 + yy;
        const uint16_t *row = src + (yy + (y1 - area->y1)) * src_w + (x1 - area->x1);
        for (int xx = 0; xx < w; xx++) {
            int x = x1 + xx;
            bool white = rgb565_is_white(row[xx], driver->bw_threshold);
            rawdraw::set_pixel(driver->buffer, driver->Width, x, y, white ? rawdraw::WHITE : rawdraw::BLACK);
        }
    }

    // Merge dirty rect + notify refresh task
    Rect r = { x1, y1, w, h };
    r = clamp_rect(align_x8(r), driver->Width, driver->Height);
    if (rect_area(r) > 0) {
        driver->dirty = rect_union(driver->dirty, r);
        driver->pending = true;
        driver->refresh_in_progress = true;
        driver->UpdateDisplayBusyLocked();
        uint32_t kick_ms = kDisplayKickMs;
        if (driver->next_kick_ms_ > 0) {
            kick_ms = driver->next_kick_ms_;
            driver->next_kick_ms_ = 0;
        }
        sm_kick(kick_ms, "display_flush");
        //ESP_LOGI(TAG, "[FLUSH] Aligned rect: x=%d, y=%d, w=%d, h=%d", r.x, r.y, r.w, r.h);
        //ESP_LOGI(TAG, "[FLUSH] Merged dirty: x=%d, y=%d, w=%d, h=%d (area=%d)",
        //         driver->dirty.x, driver->dirty.y, driver->dirty.w, driver->dirty.h,
        //         rect_area(driver->dirty));

        if (driver->refresh_task) {
            xTaskNotifyGive(driver->refresh_task);
        }
    }

    if (driver->dirty_mutex) {
        xSemaphoreGive(driver->dirty_mutex);
    }

    lv_disp_flush_ready(disp);
}
#endif  // HAVE_LVGL

// =======================================================
// ctor/dtor
// =======================================================
CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y,
                                   bool mirror_x, bool mirror_y, bool swap_xy, custom_lcd_spi_t _lcd_spi_data) :
    LcdDisplay(panel_io, panel, width, height),
    lcd_spi_data(_lcd_spi_data),
    Width(width), Height(height),
    panel_type_(static_cast<epd_panel_type_t>(_lcd_spi_data.panel_type)) {

    ESP_LOGI(TAG, "Initialize SPI, panel=%s",
             IsFourColorPanel() ? "4-color SSD2683" : "1bpp black/white");
    spi_port_init();
    spi_gpio_init();

#ifdef HAVE_LVGL
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    lvgl_port_lock(0);
#endif

    buffer = (uint8_t *)heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
    assert(buffer);
    memset(buffer, WhiteFillByte(), lcd_spi_data.buffer_len);

    prev_buffer = (uint8_t *)heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
    assert(prev_buffer);
    memset(prev_buffer, WhiteFillByte(), lcd_spi_data.buffer_len);

    // tx_buf: dirty rect snapshot to avoid tearing during flush
    tx_buf = (uint8_t *)heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
    assert(tx_buf);
    memset(tx_buf, WhiteFillByte(), lcd_spi_data.buffer_len);

#ifdef HAVE_LVGL
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, lvgl_flush_cb);
    lv_display_set_user_data(display_, this);

    uint8_t *buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    lv_display_set_buffers(display_, buffer_1, NULL, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

    bw_threshold       = 200;

    // async defaults
    sample_interval_ms = 300;
    last_sample_tick = 0;
    if (IsFourColorPanel()) {
        sample_interval_ms = kFourColorSampleIntervalMs;
    }

    ESP_LOGI(TAG, "EPD init");
    EPD_Init();

    // buffer init
    EPD_Clear();
    memcpy(prev_buffer, buffer, lcd_spi_data.buffer_len);
#if CONFIG_ZECTRIX_EPD_4COLOR_BOOT_TEST_PATTERN
    if (IsFourColorPanel()) {
        EPD_DisplayFourColorTestPattern();
    } else {
        EPD_Display();
    }
#else
    EPD_Display();
#endif
    prev_buffer_synced = true;
    if (IsFourColorPanel()) {
        last_sample_tick = xTaskGetTickCount();
    }
    // start async refresh
    dirty_mutex = xSemaphoreCreateMutex();
    // Binary: we only ever care that *a* refresh finished, not how many.
    refresh_done_sem_ = xSemaphoreCreateBinary();
    assert(dirty_mutex);
    start_refresh_task();

#ifdef HAVE_LVGL
    lvgl_port_unlock();

    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    ESP_LOGI(TAG, "ui start");
    SetupUI();
#else
    ESP_LOGI(TAG, "Rawdraw mode: EPD initialized without LVGL");
#endif
}

CustomLcdDisplay::~CustomLcdDisplay() {
    stop_refresh_task();
    if (dirty_mutex) {
        vSemaphoreDelete(dirty_mutex);
        dirty_mutex = nullptr;
    }
    // 如需释放 buffer/prev_buffer/tx_buf 可在此处补充
}

// =======================================================
// Async refresh task
// =======================================================

// 差异分析结果
struct FrameDiffResult {
    size_t diff_bits;                // 差异bit总数
    float diff_ratio;                // 差异比例 (diff_bits / total_bits)
};

// 统一的差异分析函数（仅统计差异字节比例）
static FrameDiffResult analyze_frame_diff(
    const uint8_t* prev_buffer,
    const uint8_t* tx_buf,
    int width,
    int height
) {
    FrameDiffResult result = {};
    result.diff_bits = 0;
    result.diff_ratio = 0.0f;

    if (!prev_buffer || !tx_buf || width <= 0 || height <= 0) {
        return result;
    }

    // RawDraw always keeps a 2bpp semantic framebuffer, including when a
    // 1bpp panel is selected and down-converted before transmission.
    const int bytes_per_row = (width * 2 + 7) >> 3;
    const size_t total_bytes = bytes_per_row * height;
    const size_t total_bits = total_bytes * 8;

    // 逐行扫描，统计差异bit数
    for (int y = 0; y < height; ++y) {
        const uint8_t* prow = prev_buffer + y * bytes_per_row;
        const uint8_t* crow = tx_buf + y * bytes_per_row;
        for (int xb = 0; xb < bytes_per_row; ++xb) {
            uint8_t x = (uint8_t)(prow[xb] ^ crow[xb]);
            if (x != 0) {
                result.diff_bits += (size_t)__builtin_popcount((unsigned)x);
            }
        }
    }

    result.diff_ratio = (total_bits > 0) ? (float)result.diff_bits / (float)total_bits : 0.0f;

    return result;
}

void CustomLcdDisplay::start_refresh_task() {
    if (refresh_task) return;
    xTaskCreatePinnedToCore(refresh_task_entry, "epd_refresh", 4096, this, 3, &refresh_task, 1);
}

void CustomLcdDisplay::stop_refresh_task() {
    if (!refresh_task) return;
    TaskHandle_t t = refresh_task;
    refresh_task = nullptr;
    vTaskDelete(t);
}

void CustomLcdDisplay::UpdateDisplayBusyLocked() {
    const bool busy = pending || urgent_refresh || force_full_refresh_ || refresh_in_progress;
    sm_set_busy(SleepBusySrc::Display, busy);
}

bool CustomLcdDisplay::CheckRefreshIdleLocked() {
    const bool busy = pending || urgent_refresh || force_full_refresh_ || refresh_in_progress;
    if (busy) {
        refresh_busy_seen_ = true;
        return false;
    }
    if (!refresh_busy_seen_) {
        return false;
    }
    refresh_busy_seen_ = false;
    return true;
}

bool CustomLcdDisplay::IsRefreshPending() {
    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    }
    const bool busy = pending || urgent_refresh || force_full_refresh_ || refresh_in_progress;
    if (dirty_mutex) {
        xSemaphoreGive(dirty_mutex);
    }
    return busy;
}

void CustomLcdDisplay::SignalRefreshDone() {
    if (refresh_done_sem_) {
        xSemaphoreGive(refresh_done_sem_);
    }
}

int CustomLcdDisplay::DrainRefreshDoneSignals() {
    if (!refresh_done_sem_) return 0;
    int drained = 0;
    while (xSemaphoreTake(refresh_done_sem_, 0) == pdTRUE) {
        drained++;
    }
    return drained;
}

bool CustomLcdDisplay::WaitRefreshDone(uint32_t timeout_ms) {
    if (!refresh_done_sem_) return false;
    return xSemaphoreTake(refresh_done_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void CustomLcdDisplay::RequestUrgentRefresh() {
    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    }
    urgent_refresh = true;
    refresh_in_progress = true;
    const uint32_t default_kick_ms = IsFourColorPanel() ? (uint32_t)sample_interval_ms : kDisplayKickMs;
    const uint32_t kick_ms = (next_kick_ms_ > 0) ? next_kick_ms_ : default_kick_ms;
    next_kick_ms_ = 0;
    UpdateDisplayBusyLocked();
    if (dirty_mutex) {
        xSemaphoreGive(dirty_mutex);
    }
    sm_kick(kick_ms, "display_urgent");
    if (refresh_task) {
        xTaskNotifyGive(refresh_task);
    }
}

void CustomLcdDisplay::RequestUrgentFullRefresh() {
    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    }
    urgent_refresh = true;
    force_full_refresh_ = true;
    refresh_in_progress = true;
    const uint32_t default_kick_ms = IsFourColorPanel() ? (uint32_t)sample_interval_ms : kDisplayKickMs;
    const uint32_t kick_ms = (next_kick_ms_ > 0) ? next_kick_ms_ : default_kick_ms;
    next_kick_ms_ = 0;
    UpdateDisplayBusyLocked();
    if (dirty_mutex) {
        xSemaphoreGive(dirty_mutex);
    }
    sm_kick(kick_ms, "display_urgent");
    if (refresh_task) {
        xTaskNotifyGive(refresh_task);
    }
}

void CustomLcdDisplay::SetOnRefreshIdle(std::function<void()> cb) {
    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    }
    on_refresh_idle_ = std::move(cb);
    if (dirty_mutex) {
        xSemaphoreGive(dirty_mutex);
    }
}

void CustomLcdDisplay::SetNextKickMs(uint32_t kick_ms) {
    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    }
    next_kick_ms_ = kick_ms;
    if (dirty_mutex) {
        xSemaphoreGive(dirty_mutex);
    }
}

void CustomLcdDisplay::refresh_task_entry(void *arg) {
    CustomLcdDisplay *d = (CustomLcdDisplay *)arg;
    d->refresh_task_loop();
}

void CustomLcdDisplay::refresh_task_loop() {
    int partial_since_full = 0;
    int tiny_diff_streak = 0;
    size_t tiny_diff_accum_bits = 0;
    TickType_t tiny_diff_first_tick = 0;

    uint32_t stat_refresh = 0;
    uint32_t stat_full = 0;
    uint32_t stat_partial = 0;
    uint32_t stat_urgent = 0;
    uint32_t stat_skip_throttle = 0;
    uint32_t stat_skip_nodiff = 0;
    uint32_t stat_skip_tiny = 0;
    uint32_t stat_tiny_forced = 0;
    TickType_t last_stat_tick = 0;

    const TickType_t kDebounceTicks = IsFourColorPanel() ? pdMS_TO_TICKS(3000) : pdMS_TO_TICKS(50);
    const TickType_t kUrgentDebounceTicks = pdMS_TO_TICKS(30);
    const float kMinDiffBitRatio = 0.001f;  // 0.1%
    const float kForceFullDiffRatio = 0.30f;  // 30%
    const int kTinyMaxStreak = 4;
    const size_t kTinyMaxAccumBits = 64 * 8;
    const TickType_t kTinyMaxHoldTicks = pdMS_TO_TICKS(1200);
    const TickType_t kStatPeriodTicks = pdMS_TO_TICKS(3000);

    auto maybe_log_stats = [&](TickType_t now_tick) {
        if (last_stat_tick == 0) {
            last_stat_tick = now_tick;
            return;
        }
        if ((now_tick - last_stat_tick) >= kStatPeriodTicks) {
            ESP_LOGI(TAG,
                     "[REFRESH] Stat 3s: refresh=%u (full=%u, partial=%u, urgent=%u), "
                     "skip(throttle=%u, nodiff=%u, tiny=%u, tiny_forced=%u)",
                     (unsigned)stat_refresh, (unsigned)stat_full, (unsigned)stat_partial,
                     (unsigned)stat_urgent, (unsigned)stat_skip_throttle,
                     (unsigned)stat_skip_nodiff, (unsigned)stat_skip_tiny,
                     (unsigned)stat_tiny_forced);
            last_stat_tick = now_tick;
        }
    };

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        TickType_t now = xTaskGetTickCount();

        bool urgent = false;
        bool force_full = false;
        Rect r = {0, 0, 0, 0};

        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
        if (urgent_refresh) {
            urgent = true;
            urgent_refresh = false;
        }
        if (force_full_refresh_) {
            force_full = true;
            force_full_refresh_ = false;
        }
        if (pending && rect_area(dirty) > 0) {
            r = dirty;
            dirty = {0, 0, 0, 0};
            pending = false;
            ESP_LOGI(TAG, "[REFRESH] Got dirty rect: x=%d, y=%d, w=%d, h=%d, area=%d",
                     r.x, r.y, r.w, r.h, rect_area(r));
        }
        if (urgent || rect_area(r) > 0) {
            refresh_in_progress = true;
        }
        UpdateDisplayBusyLocked();
        (void)CheckRefreshIdleLocked();
        xSemaphoreGive(dirty_mutex);

        // 周期性采样：按 last_sample_tick 计时，非刷新结束时间
        TickType_t min_ticks = pdMS_TO_TICKS(sample_interval_ms);
        if (!urgent) {
            TickType_t elapsed = (last_sample_tick == 0) ? min_ticks : (now - last_sample_tick);
            if (elapsed < min_ticks) {
                stat_skip_throttle++;
                maybe_log_stats(now);
                // Avoid spinning when notifications arrive too frequently.
                TickType_t wait_ticks = min_ticks - elapsed;
                vTaskDelay(wait_ticks > 0 ? wait_ticks : 1);
                continue;
            }
        }

        // debounce merge
        TickType_t debounce_ticks = (urgent || force_full) ? kUrgentDebounceTicks : kDebounceTicks;
        TickType_t t0 = xTaskGetTickCount();
        while (debounce_ticks > 0 && (xTaskGetTickCount() - t0) < debounce_ticks) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
            xSemaphoreTake(dirty_mutex, portMAX_DELAY);
            if (pending && rect_area(dirty) > 0) {
                r = rect_union(r, dirty);
                dirty = {0, 0, 0, 0};
                pending = false;
                ESP_LOGI(TAG, "[REFRESH] Debounce merged dirty: x=%d, y=%d, w=%d, h=%d, area=%d",
                         r.x, r.y, r.w, r.h, rect_area(r));
            }
            xSemaphoreGive(dirty_mutex);
        }

        // Take full-frame snapshot into tx_buf under mutex (avoid tearing)
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
        memcpy(tx_buf, buffer, lcd_spi_data.buffer_len);
        xSemaphoreGive(dirty_mutex);
        last_sample_tick = xTaskGetTickCount();

        // 统一差异分析：仅统计差异比例
        FrameDiffResult result = analyze_frame_diff(prev_buffer, tx_buf, Width, Height);

        // 快速退出：没有任何变化
        if (result.diff_bits == 0 && !force_full) {
            tiny_diff_streak = 0;
            tiny_diff_accum_bits = 0;
            tiny_diff_first_tick = 0;
            stat_skip_nodiff++;
            maybe_log_stats(last_sample_tick);
            bool fire_idle_cb = false;
            xSemaphoreTake(dirty_mutex, portMAX_DELAY);
            urgent_refresh = false;
            refresh_in_progress = false;
            UpdateDisplayBusyLocked();
            fire_idle_cb = CheckRefreshIdleLocked();
            // Nothing was sent to the panel, so nothing could have gone wrong
            // with it. The requester is still owed an answer.
            last_refresh_ok_ = true;
            xSemaphoreGive(dirty_mutex);
            SignalRefreshDone();
            if (fire_idle_cb && on_refresh_idle_) {
                on_refresh_idle_();
            }
            vTaskDelay(1);
            continue;
        }

        // 可选：过滤超小差异（防止抗锯齿/边界振荡导致的无意义刷新）
        // 注意：不立即同步 prev_buffer，避免累计误差；达到阈值后再强制刷新
        if (!urgent && !force_full && result.diff_ratio < kMinDiffBitRatio) {
            if (tiny_diff_streak == 0) {
                tiny_diff_first_tick = last_sample_tick;
            }
            tiny_diff_streak++;
            tiny_diff_accum_bits += result.diff_bits;

            bool force_due = (tiny_diff_streak >= kTinyMaxStreak) ||
                             (tiny_diff_accum_bits >= kTinyMaxAccumBits) ||
                             (last_sample_tick - tiny_diff_first_tick >= kTinyMaxHoldTicks);
            if (!force_due) {
                ESP_LOGI(TAG,
                         "[REFRESH] Diff too small (bits=%.2f%%), "
                         "skip to reduce flicker (streak=%d, accum=%ub)",
                         result.diff_ratio * 100.0f,
                         tiny_diff_streak, (unsigned)tiny_diff_accum_bits);
                stat_skip_tiny++;
                maybe_log_stats(last_sample_tick);
                // Nothing was sent to the panel on this pass, so the panel is
                // not busy and must not keep saying it is. refresh_in_progress
                // was set at the top of the loop when the dirty rect was
                // consumed, and this path used to leave it set: the display's
                // busy vote stayed asserted with no refresh in flight, and
                // CheckRefreshIdleLocked() never saw the busy -> idle edge, so
                // the input latch that RawDrawUiManager closes on every dirty
                // mark was never reopened and a held navigation intent waited
                // on a refresh that had not started. The deferred diff is not
                // lost: prev_buffer is deliberately not synced, so the next
                // pass sees it again and the streak/accum/hold rules above
                // force it through.
                {
                    bool fire_idle_cb = false;
                    xSemaphoreTake(dirty_mutex, portMAX_DELAY);
                    refresh_in_progress = false;
                    UpdateDisplayBusyLocked();
                    fire_idle_cb = CheckRefreshIdleLocked();
                    xSemaphoreGive(dirty_mutex);
                    // Deliberately no SignalRefreshDone(): nothing completed.
                    // The semaphore means "a refresh finished", and posting it
                    // for a refresh that was skipped is exactly the false
                    // acknowledgement RenderHandshake exists to prevent.
                    // last_refresh_ok_ is left alone for the same reason - the
                    // no-diff path above may claim success because the panel
                    // already shows the frame, which is not true here.
                    if (fire_idle_cb && on_refresh_idle_) {
                        on_refresh_idle_();
                    }
                }
                vTaskDelay(1);
                continue;
            }
            stat_tiny_forced++;
        } else {
            tiny_diff_streak = 0;
            tiny_diff_accum_bits = 0;
            tiny_diff_first_tick = 0;
        }

        const size_t total_bytes = lcd_spi_data.buffer_len;
        const size_t total_bits = total_bytes * 8;
        ESP_LOGI(TAG, "[STRATEGY] Diff analysis: bits=%u/%u (%.2f%%)",
                 (unsigned)result.diff_bits, (unsigned)total_bits, result.diff_ratio * 100.0f);

        // Decide FULL vs PARTIAL
        bool should_full = !prev_buffer_synced || !prev_buffer;
        if (!should_full && force_full) {
            should_full = true;
        }
        if (!should_full && IsFourColorPanel()) {
            should_full = true;
        }
        if (!should_full && result.diff_ratio >= kForceFullDiffRatio) {
            should_full = true;
            ESP_LOGI(TAG, "[STRATEGY] diff_ratio>=30%% -> FULL");
        }
        if (!should_full && partial_since_full >= 10) {
            should_full = true;
        }

        stat_refresh++;
        if (urgent) {
            stat_urgent++;
        }

        // Sample the BUSY-timeout counter either side of the panel work. If it
        // moved, read_busy() gave up on the BUSY pin and the driver continued
        // anyway, which means the panel is not known to show this frame.
        const uint32_t busy_timeouts_before = busy_timeout_count_.load(std::memory_order_relaxed);

        if (should_full) {
            stat_full++;
            ESP_LOGI(TAG, "[REFRESH] Performing FULL refresh");
            EPD_Init();
            EPD_Display();

            memcpy(prev_buffer, tx_buf, lcd_spi_data.buffer_len);
            prev_buffer_synced = true;
            partial_since_full = 0;
        } else
        {
            stat_partial++;
            ESP_LOGI(TAG, "[REFRESH] Performing PARTIAL refresh");
            EPD_Init();
            EPD_DisplayPart();
            memcpy(prev_buffer, tx_buf, lcd_spi_data.buffer_len);
            prev_buffer_synced = true;
            partial_since_full++;
        }
        const bool refresh_ok =
            (busy_timeout_count_.load(std::memory_order_relaxed) == busy_timeouts_before);
        if (!refresh_ok) {
            ESP_LOGE(TAG, "[REFRESH] BUSY timed out during this refresh; "
                          "panel contents are not known to match the framebuffer");
        }

        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
        refresh_in_progress = false;
        last_refresh_ok_ = refresh_ok;
        UpdateDisplayBusyLocked();
        bool fire_idle_cb = CheckRefreshIdleLocked();
        xSemaphoreGive(dirty_mutex);
        SignalRefreshDone();
        if (fire_idle_cb && on_refresh_idle_) {
            on_refresh_idle_();
        }
        tiny_diff_streak = 0;
        tiny_diff_accum_bits = 0;
        tiny_diff_first_tick = 0;
        maybe_log_stats(xTaskGetTickCount());
    }
}

// =======================================================
// GPIO/SPI init
// =======================================================
void CustomLcdDisplay::spi_gpio_init() {
    int rst  = lcd_spi_data.rst;
    int cs   = lcd_spi_data.cs;
    int dc   = lcd_spi_data.dc;
    int busy = lcd_spi_data.busy;

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst) | (0x1ULL << dc) | (0x1ULL << cs);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_conf.mode         = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << busy);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    set_rst_1();
}

void CustomLcdDisplay::spi_port_init() {
    int              mosi     = lcd_spi_data.mosi;
    int              scl      = lcd_spi_data.scl;
    int              spi_host = lcd_spi_data.spi_host;

    if (spi && spi_bus_inited) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_remove_device(spi));
        spi = nullptr;
    }
    if (spi_bus_inited) {
        esp_err_t free_ret = spi_bus_free((spi_host_device_t)spi_host);
        if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(free_ret);
        }
        spi_bus_inited = false;
    }

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num      = -1;
    buscfg.mosi_io_num      = mosi;
    buscfg.sclk_io_num      = scl;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = lcd_spi_data.buffer_len;

    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num                  = -1;
    devcfg.clock_speed_hz                = 40 * 1000 * 1000;
    devcfg.mode                          = 0;
    devcfg.queue_size                    = 7;

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi));
    spi_bus_inited = true;
}

void CustomLcdDisplay::spi_port_rx_init() {
    int              miso     = lcd_spi_data.mosi;
    int              scl      = lcd_spi_data.scl;
    int              spi_host = lcd_spi_data.spi_host;

    if (spi && spi_bus_inited) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_remove_device(spi));
        spi = nullptr;
    }
    if (spi_bus_inited) {
        esp_err_t free_ret = spi_bus_free((spi_host_device_t)spi_host);
        if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(free_ret);
        }
        spi_bus_inited = false;
    }

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num      = miso;
    buscfg.mosi_io_num      = -1;
    buscfg.sclk_io_num      = scl;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = lcd_spi_data.buffer_len;

    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num                  = -1;
    devcfg.clock_speed_hz                = 8 * 1000 * 1000;
    devcfg.mode                          = 0;
    devcfg.queue_size                    = 7;

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi));
    spi_bus_inited = true;
}

void CustomLcdDisplay::read_busy() {
    int busy = lcd_spi_data.busy;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(IsFourColorPanel() ? 120000 : 30000);
    TickType_t last_log = start;

    while (gpio_get_level((gpio_num_t)busy) == 0) {
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGW(TAG, "EPD busy wait: %u ms", (unsigned)((now - start) * portTICK_PERIOD_MS));
            last_log = now;
        }
        if ((now - start) >= timeout) {
            // The driver carries on so a wedged panel cannot hang the task
            // forever, but this is recorded: after this point the framebuffer
            // and the glass are not known to agree, and callers waiting for a
            // completion acknowledgement must be told the refresh failed.
            const uint32_t total = busy_timeout_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            ESP_LOGE(TAG, "EPD busy timeout after %u ms, continuing (total timeouts=%u)",
                     (unsigned)((now - start) * portTICK_PERIOD_MS),
                     (unsigned)total);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =======================================================
// SPI wrappers
// =======================================================
void CustomLcdDisplay::SPI_SendByte(uint8_t data) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &data;
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

uint8_t CustomLcdDisplay::SPI_RecvByte() {
    uint8_t rx = 0;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;        // 接收 8 bit
    t.rx_buffer = &rx;     // 只接收，不发送

    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);

    return rx;
}


uint8_t CustomLcdDisplay::EPD_RecvData() {
    unsigned char data = 0;
    spi_port_rx_init();
    set_dc_1();
    set_cs_0();
    data = SPI_RecvByte();
    set_cs_1();
    spi_port_init();

    return data;
}

void CustomLcdDisplay::EPD_SendData(uint8_t data) {
    set_dc_1();
    set_cs_0();
    SPI_SendByte(data);
    set_cs_1();
}

void CustomLcdDisplay::EPD_SendCommand(uint8_t command) {
    set_dc_0();
    set_cs_0();
    SPI_SendByte(command);
    set_cs_1();
}

void CustomLcdDisplay::writeBytes(uint8_t *buf, int len) {
    set_dc_1();
    set_cs_0();
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8 * len;
    t.tx_buffer = buf;
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
    set_cs_1();
}

void CustomLcdDisplay::writeBytes(const uint8_t *buf, int len) {
    set_dc_1();
    set_cs_0();
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8 * len;
    t.tx_buffer = buf;
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
    set_cs_1();
}

void CustomLcdDisplay::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x04); //power on
    read_busy();
    if (IsFourColorPanel()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    EPD_SendCommand(0x12);  // Display Refresh
    EPD_SendData(0x00);                
    if (IsFourColorPanel()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    read_busy(); 

    EPD_SendCommand(0x02);  // Power OFF
    EPD_SendData(0x00);                  
    read_busy(); 
    if (IsFourColorPanel()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        EPD_SendCommand(0x07);  // Deep sleep, same as vendor enterdeepsleep()
        EPD_SendData(0xA5);
    }
    EPD_PowerOff();
}


void CustomLcdDisplay::EPD_PowerOn() {
    gpio_hold_dis((gpio_num_t)lcd_spi_data.power);
    gpio_set_level((gpio_num_t)lcd_spi_data.power, 1);
    gpio_hold_en((gpio_num_t)lcd_spi_data.power);
}

void CustomLcdDisplay::EPD_PowerOff(){
    gpio_hold_dis((gpio_num_t)lcd_spi_data.power);
    gpio_set_level((gpio_num_t)lcd_spi_data.power, 0);
    gpio_hold_en((gpio_num_t)lcd_spi_data.power);
    
}

// =======================================================
// Init
// =======================================================
void CustomLcdDisplay::EPD_Init() {
    EPD_PowerOn();
    vTaskDelay(pdMS_TO_TICKS(10));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(10));
    set_rst_0();
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(10));

    read_busy();

    if (IsFourColorPanel()) {
        EPD_SendCommand(0xE9);
        EPD_SendData(0x01);
        return;
    }

    //ssd2683 init for otp
    EPD_SendCommand(0x00);
    EPD_SendData(0x2F);
    EPD_SendData(0x2E);

    EPD_SendCommand(0xE9);
    EPD_SendData(0x01);
    read_busy();
}

// =======================================================
// Framebuffer ops (full refresh)
// =======================================================
void CustomLcdDisplay::EPD_Clear() {
    memset(buffer, WhiteFillByte(), lcd_spi_data.buffer_len);
}

static inline void pack_1bpp_to_2683(uint8_t in, uint8_t& out0, uint8_t& out1)
{
    uint8_t b0 = 0, b1 = 0;

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t bit = (in >> (7 - i)) & 0x01;

        if (i < 4) {
            b0 |= bit << (8 - 2 * (i + 1));   // i=0..3 -> shift 6,4,2,0
        } else {
            b1 |= bit << (14 - 2 * i);        // i=4..7 -> shift 6,4,2,0
        }
    }

    out0 = b0;
    out1 = b1;
}

static inline uint8_t ssd2683_solid_4color_byte(uint8_t two_bit_color)
{
    two_bit_color &= 0x03;
    return (two_bit_color << 6) | (two_bit_color << 4) | (two_bit_color << 2) | two_bit_color;
}

void CustomLcdDisplay::EPD_DisplayFourColorTestPattern() {
    const int bytes_per_row_out = (Width * 2 + 7) >> 3; // 400px 2bpp -> 100 bytes
    std::vector<uint8_t> line(bytes_per_row_out);

    EPD_SendCommand(0x10);   // DTM1 Write, same as vendor dis_img()
    read_busy();

    const uint8_t black = ssd2683_solid_4color_byte(0x00);
    const uint8_t white = ssd2683_solid_4color_byte(0x01);
    const uint8_t yellow = ssd2683_solid_4color_byte(0x02);
    const uint8_t red = ssd2683_solid_4color_byte(0x03);

    for (int y = 0; y < Height; y++) {
        for (int xb = 0; xb < bytes_per_row_out; xb++) {
            if (xb < bytes_per_row_out / 4) {
                line[xb] = black;
            } else if (xb < bytes_per_row_out / 2) {
                line[xb] = white;
            } else if (xb < (bytes_per_row_out * 3) / 4) {
                line[xb] = red;
            } else {
                line[xb] = yellow;
            }
        }
        writeBytes(line.data(), bytes_per_row_out);
    }

    EPD_TurnOnDisplay();
}

void CustomLcdDisplay::EPD_Display() {
    unsigned char temp1, tempvalue;

    const int bytes_per_row_1bpp = (Width + 7) >> 3;       // 400 -> 50
    const int bytes_per_row_2bpp = (Width * 2 + 7) >> 3;    // 400 -> 100

    // 行缓冲：四彩屏直接发送 2bpp 数据，黑白屏保留原有 1bpp->2683 转换
    std::vector<uint8_t> line(IsFourColorPanel() ? bytes_per_row_2bpp : bytes_per_row_1bpp * 2);

    if (IsFourColorPanel()) {
        EPD_SendCommand(0x10);   // DTM1 Write, same as vendor dis_img()
        read_busy();

        for (int y = 0; y < Height; y++) {
            const uint8_t* src = tx_buf + y * bytes_per_row_2bpp;
            writeBytes(src, bytes_per_row_2bpp);
            if ((y % 16) == 15) {
                vTaskDelay(1);
            }
        }

        EPD_TurnOnDisplay();
        return;
    }

    EPD_SendCommand(0x40);
    read_busy();

    temp1=EPD_RecvData(); 
    ESP_LOGI(TAG, "[EPD_Display]temp1 %d", temp1);
  
    if(temp1<=5)
      tempvalue=232;  // -24


    else if(temp1<=10)
      tempvalue=235;   // -21


    else if(temp1<=20)
      tempvalue=238;   // -18


    else if(temp1<=30)
      tempvalue=241;   // -15


    else if(temp1<=127)
      tempvalue=244;    // -12
    
    else
      tempvalue=232;

    EPD_SendCommand(0xE0); 
    EPD_SendData(0x02);
    EPD_SendCommand(0xE6);  
    EPD_SendData(tempvalue);
    
    EPD_SendCommand(0xA5);      
    read_busy();
    vTaskDelay(pdMS_TO_TICKS(10));
  
    EPD_SendCommand(0x10);

     for (int y = 0; y < Height; y++) {
        const uint8_t* src = tx_buf + y * bytes_per_row_2bpp;
        uint8_t* dst = line.data();

        for (int xb = 0; xb < bytes_per_row_1bpp; ++xb) {
            uint8_t o0, o1;
            pack_1bpp_to_2683(Pack2bppRowTo1bppByte(src, xb * 8), o0, o1);
            *dst++ = o0;
            *dst++ = o1;
        }

        // 一行一发：只做一次 SPI transaction
        writeBytes(line.data(), bytes_per_row_1bpp * 2);
        if (IsFourColorPanel() && (y % 16) == 15) {
            vTaskDelay(1);
        }
    }

    EPD_TurnOnDisplay();
}

bool CustomLcdDisplay::DisplayRaw4ColorImage(const uint8_t* data, size_t len, int width, int height) {
    if (!IsFourColorPanel() || data == nullptr || width != Width || height != Height) {
        return false;
    }

    const int bytes_per_row = (Width * 2 + 7) >> 3;
    const size_t expected_len = static_cast<size_t>(bytes_per_row) * Height;
    if (len != expected_len) {
        ESP_LOGE(TAG, "Raw 4-color image size mismatch: got=%u expected=%u",
                 (unsigned)len, (unsigned)expected_len);
        return false;
    }

    if (dirty_mutex) {
        xSemaphoreTake(dirty_mutex, portMAX_DELAY);
        pending = false;
        urgent_refresh = false;
        force_full_refresh_ = false;
        refresh_in_progress = false;
        dirty = {0, 0, 0, 0};
        UpdateDisplayBusyLocked();
        xSemaphoreGive(dirty_mutex);
    }

    EPD_Init();
    EPD_SendCommand(0x10);   // DTM1 Write, raw SSD2683 2bpp stream
    read_busy();

    for (int y = 0; y < Height; ++y) {
        writeBytes(data + y * bytes_per_row, bytes_per_row);
        if ((y % 16) == 15) {
            vTaskDelay(1);
        }
    }

    EPD_TurnOnDisplay();
    return true;
}
#if 0
void bitInterleave(unsigned char bytes1, unsigned char bytes2) {
   
    unsigned short result=0;

    for (int i = 0; i < 8; i++) {
      
        result |= ((bytes1 >> (7 - i)) & 1) << (2 * (7-i)+1);
        result |= ((bytes2 >> (7 - i)) & 1) << (2 * (7-i));
       
        if(i == 3)
          EPD_SendData(result >> 8);
        
        //ESP_LOGI(TAG, "[bitInterleave] 0x%x",);
        if(i == 7)
          EPD_SendData(result);

        //ESP_LOGI(TAG, "[bitInterleave] 0x%x");
    }
}
#endif

#if 0
    //  prev_buffer / tx_buffer 是整屏 1bpp buffer，行优先：Height * bytes_per_row_1bpp
    // prev_buffer 对应旧图，tx_buffer 对应新图
    for (int i = 0; i < Height; i++) {
        const uint8_t* prev_row = prev_buffer + i * bytes_per_row_1bpp;
        const uint8_t* tx_row   = tx_buf   + i * bytes_per_row_1bpp;

        for (int j = 0; j < bytes_per_row_1bpp; j++) {
            uint8_t b1 = prev_row[j];
            uint8_t b2 = tx_row[j];

            // 等价 bitInterleave(b1, b2) 的 16-bit result（先发高字节再发低字节）
            uint16_t result = 0;
            for (int k = 0; k < 8; k++) {
                // 原代码：((bytes1 >> (7-k)) & 1) << (2*(7-k)+1)
                //         ((bytes2 >> (7-k)) & 1) << (2*(7-k))
                const int src_bit = 7 - k;
                const int dst_bit0 = 2 * src_bit;     // even: bytes2
                const int dst_bit1 = 2 * src_bit + 1; // odd : bytes1

                result |= ((uint16_t)((b1 >> src_bit) & 1u)) << dst_bit1;
                result |= ((uint16_t)((b2 >> src_bit) & 1u)) << dst_bit0;
            }

            // bitInterleave 在 i==3 时写高字节，在 i==7 时写低字节 -> 高字节先发
            line[2 * j + 0] = (uint8_t)(result >> 8);
            line[2 * j + 1] = (uint8_t)(result & 0xFF);
        }

        // 一行一发：只做一次 SPI transaction
        writeBytes(line.data(), bytes_per_row_out);
    }
#endif

void CustomLcdDisplay::bitInterleave(unsigned char bytes1, unsigned char bytes2) {
   
    unsigned short result=0;
    

    
    for (int i = 0; i < 8; i++) {
      
        result |= ((bytes1 >> (7 - i)) & 1) << (2 * (7-i)+1);
        result |= ((bytes2 >> (7 - i)) & 1) << (2 * (7-i));
       
        if(i == 3)
          EPD_SendData(result >> 8);
     
        if(i == 7)
          EPD_SendData(result);
    }
}



void CustomLcdDisplay::WRITE_WHITE_TO_HLINE()
{
  unsigned int i,j,pcnt,pcnt1;
  
    EPD_SendCommand(0x10);   // DTM1 Write
    read_busy();
                
  pcnt = 0;
 
  for(i=0; i <300; i++)
  {
    for(j=0; j<50; j++)
    {    
        if(j < 25)
        bitInterleave(0xFF,0x00);
        else 
        bitInterleave(0xFF,0xFF);
    }        
  }
}

void CustomLcdDisplay::WRITE_HLINE_TO_VLINE()
{
  unsigned int i,j,pcnt,pcnt1;
    EPD_SendCommand(0x10);   // DTM1 Write
    read_busy();
                
  pcnt = 0;
 
  for(i=0; i <300; i++)
  {  
          for(j=0; j<50; j++)
          {
             if(i<150 && j < 25)
             {
               bitInterleave(0x00,0x00);
             }
             else if(i >=150 && j <25 )
             {
               bitInterleave(0x00,0xFF);
             }
             else if(i <150 && j>= 25)
             {
                bitInterleave(0xFF,0x00);
             }
             else
             {
                bitInterleave(0xFF,0xFF);
             }
          }    
  }
  
 

}


void CustomLcdDisplay::WRITE_VLINE_TO_HLINE()
{
  unsigned int i,j,pcnt,pcnt1;

    EPD_SendCommand(0x10);   // DTM1 Write
    read_busy();              
  pcnt = 0;
 
  for(i=0; i <300; i++)
  {
    
          for(j=0; j<50; j++)
          {
            
             if(i<150 && j < 25)
             {
               bitInterleave(0x00,0x00);
             }
             else if(i >= 150 && j < 25 )
             {
               bitInterleave(0xFF,0x00);
             }
             else if(i < 150 && j>= 25)
             {
                bitInterleave(0x00,0xFF);
             }
             else
             {
                bitInterleave(0xFF,0xFF);
             }
             
          
          }   
          
         
  }
  
 

}

void CustomLcdDisplay::EPD_DisplayPart() {
    const int bytes_per_row_1bpp = (Width + 7) >> 3;       // 400 -> 50
    const int bytes_per_row_2bpp = (Width * 2 + 7) >> 3;    // 400 -> 100
    const int bytes_per_row_out  = bytes_per_row_1bpp * 2; // 100

    std::vector<uint8_t> line(bytes_per_row_out);

    EPD_SendCommand(0x10);
    read_busy();

    //  prev_buffer / tx_buffer 是整屏 1bpp buffer，行优先：Height * bytes_per_row_1bpp
    // prev_buffer 对应旧图，tx_buffer 对应新图
    for (int i = 0; i < Height; i++) {
        const uint8_t* prev_row = prev_buffer + i * bytes_per_row_2bpp;
        const uint8_t* tx_row   = tx_buf   + i * bytes_per_row_2bpp;

        for (int j = 0; j < bytes_per_row_1bpp; j++) {
            uint8_t b1 = Pack2bppRowTo1bppByte(prev_row, j * 8);
            uint8_t b2 = Pack2bppRowTo1bppByte(tx_row, j * 8);

            // 等价 bitInterleave(b1, b2) 的 16-bit result（先发高字节再发低字节）
            uint16_t result = 0;
            for (int k = 0; k < 8; k++) {
                // 原代码：((bytes1 >> (7-k)) & 1) << (2*(7-k)+1)
                //         ((bytes2 >> (7-k)) & 1) << (2*(7-k))
                const int src_bit = 7 - k;
                const int dst_bit0 = 2 * src_bit;     // even: bytes2
                const int dst_bit1 = 2 * src_bit + 1; // odd : bytes1

                result |= ((uint16_t)((b1 >> src_bit) & 1u)) << dst_bit1;
                result |= ((uint16_t)((b2 >> src_bit) & 1u)) << dst_bit0;
            }

            // bitInterleave 在 i==3 时写高字节，在 i==7 时写低字节 -> 高字节先发
            line[2 * j + 0] = (uint8_t)(result >> 8);
            line[2 * j + 1] = (uint8_t)(result & 0xFF);
        }

        // 一行一发：只做一次 SPI transaction
        writeBytes(line.data(), bytes_per_row_out);
        if (IsFourColorPanel() && (i % 16) == 15) {
            vTaskDelay(1);
        }
    }

    EPD_TurnOnDisplay();
}


void CustomLcdDisplay::EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= (uint16_t)Width || y >= (uint16_t)Height) return;

    uint16_t bytes_per_row = (Width + 7) >> 3;
    uint32_t index = (uint32_t)y * bytes_per_row + (uint32_t)(x >> 3);
    uint8_t  bit   = (uint8_t)(7 - (x & 0x07));
    uint8_t  mask  = (uint8_t)(1U << bit);

    if (color == DRIVER_COLOR_WHITE) buffer[index] |= mask;
    else                             buffer[index] &= (uint8_t)~mask;
}

// =======================================================
// 写入原始 1bpp 位图到帧缓冲区
// 输入格式: bit=1 表示黑色, bit=0 表示白色
// 帧缓冲格式: bit=1 表示白色, bit=0 表示黑色（需翻转）
// =======================================================
void CustomLcdDisplay::WriteRaw1bpp(int x, int y, int w, int h, const uint8_t* data, size_t len) {
    if (!data || !buffer || w <= 0 || h <= 0) return;

    const int src_bytes_per_row = (w + 7) >> 3;
    const size_t expected = (size_t)src_bytes_per_row * h;
    if (len < expected) {
        ESP_LOGW(TAG, "WriteRaw1bpp: data too short (%u < %u)", (unsigned)len, (unsigned)expected);
        return;
    }

    xSemaphoreTake(dirty_mutex, portMAX_DELAY);

    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= Height) continue;
        const uint8_t* src_row = data + row * src_bytes_per_row;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= Width) continue;
            // 读取源 bit（1=黑）
            bool black = (src_row[col >> 3] >> (7 - (col & 7))) & 1;
            rawdraw::set_pixel(buffer, Width, dx, dy, black ? rawdraw::BLACK : rawdraw::WHITE);
        }
    }

    // 标记脏区域并触发刷新
    Rect r = clamp_rect(align_x8({x, y, w, h}), Width, Height);
    if (rect_area(r) > 0) {
        dirty = rect_union(dirty, r);
        pending = true;
        refresh_in_progress = true;
        UpdateDisplayBusyLocked();
        sm_kick(kDisplayKickMs, "display_raw1bpp");
        if (refresh_task) {
            xTaskNotifyGive(refresh_task);
        }
    }

    xSemaphoreGive(dirty_mutex);
    ESP_LOGI(TAG, "WriteRaw1bpp: region x=%d y=%d w=%d h=%d, %u bytes", x, y, w, h, (unsigned)len);
}

// 对帧缓冲区的指定区域进行反色（XOR 操作）
void CustomLcdDisplay::InvertRegion(int x, int y, int w, int h) {
    if (!buffer || w <= 0 || h <= 0) return;

    xSemaphoreTake(dirty_mutex, portMAX_DELAY);

    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= Height) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= Width) continue;
            const rawdraw::Color c = rawdraw::get_pixel(buffer, Width, dx, dy);
            rawdraw::set_pixel(buffer, Width, dx, dy,
                               c == rawdraw::WHITE ? rawdraw::BLACK : rawdraw::WHITE);
        }
    }

    // 标记脏区域并触发刷新
    Rect r = clamp_rect(align_x8({x, y, w, h}), Width, Height);
    if (rect_area(r) > 0) {
        dirty = rect_union(dirty, r);
        pending = true;
        refresh_in_progress = true;
        UpdateDisplayBusyLocked();
        sm_kick(kDisplayKickMs, "display_invert");
        if (refresh_task) {
            xTaskNotifyGive(refresh_task);
        }
    }

    xSemaphoreGive(dirty_mutex);
    ESP_LOGI(TAG, "InvertRegion: region x=%d y=%d w=%d h=%d", x, y, w, h);
}

// utf8_next() 已在 rawdraw/font_engine.h 中定义，此处不重复定义

// =======================================================
// 文本渲染：用 LVGL 字体 API 逐字符写入 1bpp 帧缓冲
// =======================================================
void CustomLcdDisplay::render_text_to_buffer(const char* text, int start_x, int start_y, const lv_font_t* font) {
    int cursor_x = start_x;
    int cursor_y = start_y;
    const char* p = text;

    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        // 换行
        if (ch == '\n') {
            cursor_x = start_x;
            cursor_y += font->line_height;
            continue;
        }

        // 获取字形描述
        lv_font_glyph_dsc_t g = {};
        if (!lv_font_get_glyph_dsc(font, &g, ch, 0)) {
            cursor_x += font->line_height / 2;  // 未知字符跳过半宽
            continue;
        }

        // 获取字形位图（绕过 static_bitmap 检查，直接取 raw bitmap）
        g.req_raw_bitmap = 1;
        const uint8_t* bitmap = (const uint8_t*)font->get_glyph_bitmap(&g, nullptr);
        g.req_raw_bitmap = 0;
        if (!bitmap) {
            cursor_x += g.adv_w;
            continue;
        }

        // 字形在帧缓冲中的位置
        int gx = cursor_x + g.ofs_x;
        int gy = cursor_y + font->line_height - font->base_line - g.ofs_y - g.box_h;

        // 1bpp 字体位图：连续位流 / 或按 stride 对齐
        int row_bits = (g.stride > 0) ? (int)(g.stride * 8) : (int)g.box_w;

        for (int row = 0; row < g.box_h; row++) {
            for (int col = 0; col < g.box_w; col++) {
                int bit_idx = row * row_bits + col;
                bool pixel = (bitmap[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1;
                if (pixel) {
                    int px = gx + col;
                    int py = gy + row;
                    if (px >= 0 && px < Width && py >= 0 && py < Height) {
                        rawdraw::set_pixel(buffer, Width, px, py, rawdraw::BLACK);
                    }
                }
            }
        }

        cursor_x += g.adv_w;
    }
}

void CustomLcdDisplay::DrawTexts(const std::vector<TextItem>& texts, bool clear) {
    xSemaphoreTake(dirty_mutex, portMAX_DELAY);

    if (clear) {
        memset(buffer, WhiteFillByte(), lcd_spi_data.buffer_len);
    }

    for (const auto& item : texts) {
        const lv_font_t* font = nullptr;
        const char* text = item.content.c_str();

        // 检查图标字体编码类型
        if (item.content.size() >= 3) {
            // FontAwesome 编码: \xef\x8x\xxx (U+F0XX)
            if (item.content[0] == '\xef' &&
                (item.content[1] & 0xF0) == 0x80) {  // 0x80-0x8F
                // FontAwesome 天气图标，使用 weather_icons 字体
                font = (item.size >= 40) ? &weather_icons_48 : &weather_icons_16;
            }
            // IcoMoon 编码: \xee\xa4\xxx (U+E9XX)
            else if (item.content[0] == '\xee' &&
                     item.content[1] == '\xa4') {
                // IcoMoon 图标，使用 font_zectrix 字体
                font = (item.size >= 40) ? &font_zectrix_48_1 : &font_zectrix_16_1;
            }
        }

        // 普通文本使用默认字体
        if (font == nullptr) {
            if (item.size >= 20) {
                font = &SourceHanSansSC_Medium_slim;
            } else {
                font = &BUILTIN_TEXT_FONT;
            }
        }
        render_text_to_buffer(text, item.x, item.y, font);
    }

    // 标记全屏脏区并触发刷新
    Rect r = clamp_rect(align_x8({0, 0, Width, Height}), Width, Height);
    dirty = rect_union(dirty, r);
    pending = true;
    refresh_in_progress = true;
    UpdateDisplayBusyLocked();
    sm_kick(kDisplayKickMs, "display_text");
    if (refresh_task) {
        xTaskNotifyGive(refresh_task);
    }

    xSemaphoreGive(dirty_mutex);
    ESP_LOGI(TAG, "DrawTexts: %zu items, clear=%d", texts.size(), (int)clear);
}

// ============================================================
// P0: Rawdraw-backed drawing helpers
// These draw directly on the 1bpp framebuffer using rawdraw APIs,
// with proper mutex and dirty-rect tracking, WITHOUT triggering refresh.
// ============================================================

void CustomLcdDisplay::RawDrawRoundRect(int x, int y, int w, int h, int radius,
                                         bool filled, bool has_border) {
    if (!buffer || w <= 0 || h <= 0) return;

    rawdraw::Rect r = {x, y, w, h};
    rawdraw::Color fill = filled ? rawdraw::BLACK : rawdraw::WHITE;
    rawdraw::Color border = has_border ? rawdraw::BLACK : rawdraw::WHITE;
    int border_w = has_border ? 1 : 0;

    xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    rawdraw::DrawRoundRect(buffer, Width, Height, r, radius, fill, border, border_w);
    Rect dr = clamp_rect(align_x8({x, y, w, h}), Width, Height);
    if (rect_area(dr) > 0) {
        dirty = rect_union(dirty, dr);
        pending = true;
        refresh_in_progress = true;
        UpdateDisplayBusyLocked();
    }
    xSemaphoreGive(dirty_mutex);
}

void CustomLcdDisplay::RawDrawHLine(int y, int thickness) {
    if (!buffer || thickness <= 0) return;
    if (y < 0 || y >= Height) return;

    xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    for (int t = 0; t < thickness; t++) {
        int yy = y + t;
        if (yy >= 0 && yy < Height) {
            rawdraw::DrawHLine(buffer, Width, yy, 0, Width - 1, rawdraw::BLACK);
        }
    }
    Rect dr = clamp_rect(align_x8({0, y, Width, thickness}), Width, Height);
    if (rect_area(dr) > 0) {
        dirty = rect_union(dirty, dr);
        pending = true;
        refresh_in_progress = true;
        UpdateDisplayBusyLocked();
    }
    xSemaphoreGive(dirty_mutex);
}

void CustomLcdDisplay::RawInvertRegion(int x, int y, int w, int h) {
    if (!buffer || w <= 0 || h <= 0) return;

    xSemaphoreTake(dirty_mutex, portMAX_DELAY);
    rawdraw::InvertRegion(buffer, Width, {x, y, w, h});
    Rect dr = clamp_rect(align_x8({x, y, w, h}), Width, Height);
    if (rect_area(dr) > 0) {
        dirty = rect_union(dirty, dr);
        pending = true;
        refresh_in_progress = true;
        UpdateDisplayBusyLocked();
    }
    xSemaphoreGive(dirty_mutex);
}
