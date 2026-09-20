/**
 * @file clock.h
 * @brief Persistent clock component for top-right corner
 *
 * Displays HH:MM format clock at a fixed position (X=320, Y=4, W=80, H=32).
 * This is a floating overlay that persists across page changes.
 * All other UI elements must avoid this 80x32px reserved zone.
 *
 * Usage:
 * 1. Create clock component
 * 2. Call Draw() each frame (caller decides when to redraw, e.g. every minute)
 * 3. The clock reads RTC time directly via esp_sntp functions
 */

#ifndef RAWDRAW_CLOCK_H
#define RAWDRAW_CLOCK_H

#include <stdint.h>
#include <string>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Clock position constants
 */
constexpr int kClockX = 320;
constexpr int kClockY = 4;  // Inside status bar area (y=0~24), top-right corner
constexpr int kClockW = 80;
constexpr int kClockH = 32;

/**
 * @brief Persistent clock component
 *
 * Renders "HH:MM" text at top-right corner.
 * Caller should check time changed before redrawing to avoid unnecessary EPD refresh.
 */
class Clock {
public:
    /**
     * @brief Create clock component
     *
     * @param x Position x (default: kClockX)
     * @param y Position y (default: kClockY)
     * @param font Font to use (default: BUILTIN_TEXT_FONT)
     */
    Clock(int x = kClockX, int y = kClockY, const lv_font_t* font = &BUILTIN_TEXT_FONT);

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw clock to raw framebuffer
     *
     * @return true if time changed and was redrawn, false if time unchanged
     */
    bool Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     * @return true if time changed and was redrawn
     */
    bool Draw(Framebuffer* fb, int screen_width, int screen_height);

    /**
     * @brief Draw clock with custom background fill first
     * Useful for clearing previous frame before redrawing.
     */
    bool DrawWithClear(uint8_t* fb, int width, int height, Color bg_color = WHITE);

    // ============================================================
    // Configuration
    // ============================================================

    void SetPosition(int x, int y);
    void SetFont(const lv_font_t* font);
    void SetColor(Color color);

    Rect GetBounds() const;

    /**
     * @brief Get the current time string in "HH:MM" format
     */
    static const char* GetTimeString();

    /**
     * @brief Get date string in short format (default: "M月D日")
     * Returns "4月27日" style. If date_format is "iso", returns "04-27".
     */
    static std::string GetDateString(const char* date_format = nullptr);

    /**
     * @brief Get last drawn minute value (for change detection)
     */
    int LastMinute() const { return last_minute_; }

    /**
     * @brief Get reserved zone bounds (all pages must avoid this area)
     */
    static Rect ReservedZone();

private:
    int x_;
    int y_;
    const lv_font_t* font_;
    Color color_;
    int last_minute_;
    char time_buf_[6];  // "HH:MM\0"
};

}  // namespace rawdraw

#endif  // RAWDRAW_CLOCK_H
