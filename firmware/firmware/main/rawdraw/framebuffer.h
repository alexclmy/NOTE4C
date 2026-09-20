/**
 * @file framebuffer.h
 * @brief Framebuffer manager for 2bpp EPD display
 *
 * Encapsulates the 2bpp framebuffer, dirty rect tracking, and refresh triggering.
 * Works with CustomLcdDisplay's existing infrastructure.
 */

#ifndef RAWDRAW_FRAMEBUFFER_H
#define RAWDRAW_FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <functional>
#include "rawdraw.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace rawdraw {

/**
 * @brief Framebuffer manager for 1bpp EPD display
 *
 * This class wraps the raw framebuffer buffer and provides:
 * - Dirty rectangle tracking and merging
 * - Refresh request callback
 * - Thread-safe access via mutex
 *
 * Usage:
 * 1. Create with existing buffer pointer (from CustomLcdDisplay)
 * 2. Call drawing functions via GetBuffer() + rawdraw API
 * 3. Call InvalidateRect() to mark dirty regions
 * 4. Call RequestRefresh() to trigger EPD update
 */
class Framebuffer {
public:
    /**
     * @brief Refresh callback type
     * Called when refresh is requested, allowing CustomLcdDisplay to handle EPD timing
     */
    using RefreshCallback = std::function<void(const Rect& dirty_rect, bool urgent)>;

    /**
     * @brief Create framebuffer manager
     *
 * @param buffer Pointer to 2bpp framebuffer (owned externally, e.g., by CustomLcdDisplay)
     * @param width Display width in pixels
     * @param height Display height in pixels
     * @param mutex Optional mutex for thread-safe access (from CustomLcdDisplay::dirty_mutex)
     */
    Framebuffer(uint8_t* buffer, int width, int height, SemaphoreHandle_t mutex = nullptr);

    ~Framebuffer();

    // ============================================================
    // Basic Operations
    // ============================================================

    /**
     * @brief Clear framebuffer to the requested color
     */
    void Clear(Color fill = WHITE);

    /**
     * @brief Get framebuffer pointer for direct drawing
     *
     * WARNING: Caller must manually call InvalidateRect() after drawing!
     * For thread-safe access, use Lock()/Unlock() around drawing operations.
     *
     * @return Pointer to 2bpp framebuffer
     */
    uint8_t* data() { return buffer_; }
    const uint8_t* data() const { return buffer_; }

    /**
     * @brief Get framebuffer dimensions
     */
    int width() const { return width_; }
    int height() const { return height_; }

    /**
     * @brief Get bytes per row in framebuffer
     */
    int bytes_per_row() const { return (width_ * 2 + 7) >> 3; }

    /**
     * @brief Get total buffer size in bytes
     */
    size_t buffer_size() const { return (size_t)bytes_per_row() * height_; }

    // ============================================================
    // Dirty Rectangle Tracking
    // ============================================================

    /**
     * @brief Mark region as dirty (needs refresh)
     *
     * Regions are merged into a single dirty rectangle.
     * Call RequestRefresh() to trigger EPD update.
     *
     * @param x Region x coordinate
     * @param y Region y coordinate
     * @param w Region width
     * @param h Region height
     */
    void InvalidateRect(int x, int y, int w, int h);

    /**
     * @brief Mark region as dirty using Rect struct
     */
    void InvalidateRect(const Rect& r);

    /**
     * @brief Mark entire framebuffer as dirty
     */
    void InvalidateAll();

    /**
     * @brief Get current dirty rectangle
     *
     * @return Current merged dirty region, or empty rect if clean
     */
    Rect GetDirtyRect() const;

    /**
     * @brief Check if there are pending dirty regions
     */
    bool HasDirtyRegions() const;

    /**
     * @brief Clear dirty tracking (reset to empty)
     *
     * Called by refresh task after EPD update completes.
     */
    void ClearDirty();

    // ============================================================
    // Refresh Control
    // ============================================================

    /**
     * @brief Request EPD refresh
     *
     * Triggers the refresh callback with current dirty rect.
     * The callback should handle timing and EPD update.
     *
     * @param urgent If true, bypass debounce delay
     */
    void RequestRefresh(bool urgent = false);

    /**
     * @brief Set refresh callback
     *
     * Typically set to CustomLcdDisplay's refresh trigger.
     */
    void SetRefreshCallback(RefreshCallback callback);

    /**
     * @brief Set next kick delay (for debounce control)
     *
     * Controls minimum time before next EPD refresh.
     * Typically 300-1000ms for EPD stability.
     */
    void SetNextKickMs(uint32_t kick_ms);

    // ============================================================
    // Thread Safety
    // ============================================================

    /**
     * @brief Lock framebuffer mutex
     *
     * Must be called before direct drawing if using shared buffer.
     * Automatically called by InvalidateRect().
     */
    void Lock();

    /**
     * @brief Unlock framebuffer mutex
     */
    void Unlock();

    /**
     * @brief Execute drawing operation with mutex protection
     *
     * Helper template for safe drawing operations.
     *
     * @param draw_func Drawing function to execute (receives buffer pointer)
     */
    template<typename Func>
    void SafeDraw(Func draw_func) {
        Lock();
        draw_func(buffer_);
        Unlock();
    }

    // ============================================================
    // Internal (non-locking) drawing helpers
    // Use these when already holding the lock (e.g., inside SafeDraw)
    // ============================================================

    void DrawRect_NoLock(const Rect& r, Color color);
    void DrawText_NoLock(int x, int y, const char* text, const lv_font_t* font, Color color);
    void DrawRoundRect_NoLock(const Rect& r, int radius, Color fill, Color border = BLACK, int border_w = 1);
    void InvalidateRect_NoLock(const Rect& r);

    // ============================================================
    // Convenience Drawing Methods
    // ============================================================

    /**
     * @brief Draw rect with automatic dirty tracking
     */
    void DrawRect(const Rect& r, Color color);

    /**
     * @brief Draw text with automatic dirty tracking
     */
    void DrawText(int x, int y, const char* text, const lv_font_t* font, Color color = BLACK);

    /**
     * @brief Draw rounded rect with automatic dirty tracking
     */
    void DrawRoundRect(const Rect& r, int radius, Color fill, Color border = BLACK, int border_w = 1);

private:
    uint8_t* buffer_;           ///< Pointer to 2bpp framebuffer (external ownership)
    int width_;                 ///< Display width
    int height_;                ///< Display height
    SemaphoreHandle_t mutex_;   ///< Optional mutex for thread safety

    Rect dirty_;                ///< Current merged dirty rectangle
    bool pending_;              ///< Has pending dirty regions

    RefreshCallback refresh_cb_; ///< Refresh request callback
    uint32_t next_kick_ms_;     ///< Next kick delay override
};

}  // namespace rawdraw

#endif  // RAWDRAW_FRAMEBUFFER_H
