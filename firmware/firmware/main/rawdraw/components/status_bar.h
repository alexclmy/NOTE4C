/**
 * @file status_bar.h
 * @brief Bottom status bar for rawdraw EPD UI
 *
 * Displays short status text at the bottom of the screen (Y = 280).
 * Auto-hides after 3 seconds via esp_timer.
 * Uses independent RegionRefresh counter for partial refresh.
 *
 * Design constraints:
 * - Height: 15-20px (Style::kStatusBarBottomHeight)
 * - Position: bottom of screen (Y = 300 - 20 = 280)
 * - Avoids top-right clock zone (no conflict, it is at top-right)
 * - Auto-hide: 3 second countdown after Show() call
 * - Independent refresh counter (no interference with clock or page content)
 *
 * Usage:
 * 1. Create status bar: StatusBarCreate()
 * 2. Show text: StatusBarShow("已保存", 3000)  // 3s auto-hide
 * 3. Hide manually: StatusBarHide()
 * 4. Draw when needed: StatusBarDraw(fb, width, height)
 */

#ifndef RAWDRAW_STATUS_BAR_H
#define RAWDRAW_STATUS_BAR_H

#include <stdint.h>
#include <stdbool.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"
#include "refresh.h"
#include "style.h"

namespace rawdraw {

/**
 * @brief Bottom status bar constants
 */
constexpr int kStatusBarBottomHeight = 18;           ///< Height in pixels
constexpr int kStatusBarBottomY = Style::kScreenHeight - kStatusBarBottomHeight;  ///< Y = 282
constexpr int kStatusBarBottomAutoHideMs = 3000;     ///< Auto-hide delay (3s)

/**
 * @brief Bottom status bar state
 *
 * Maintains visibility, text, and auto-hide timer state.
 * Thread-safe: all operations should be called from the same task.
 */
struct StatusBarState {
    bool visible;                                    ///< Currently visible
    char text[64];                                   ///< Status text (UTF-8)
    int64_t show_time_us;                            ///< Timestamp when shown (microseconds)
    int64_t auto_hide_ms;                            ///< Auto-hide delay in ms (0 = never)
    RegionRefresh refresh;                           ///< Independent refresh counter
    const lv_font_t* font;                           ///< Text font
};

/**
 * @brief Initialize status bar state
 *
 * @param state Status bar state pointer
 * @param font Font for text rendering (default: BUILTIN_TEXT_FONT)
 */
void StatusBarInit(StatusBarState* state, const lv_font_t* font = &BUILTIN_TEXT_FONT);

/**
 * @brief Show status text with auto-hide timer
 *
 * @param state Status bar state pointer
 * @param text UTF-8 status text (max 63 chars)
 * @param auto_hide_ms Auto-hide delay in milliseconds (default: 3000)
 *                     Set to 0 to disable auto-hide
 */
void StatusBarShow(StatusBarState* state, const char* text, int64_t auto_hide_ms = kStatusBarBottomAutoHideMs);

/**
 * @brief Hide status bar immediately
 */
void StatusBarHide(StatusBarState* state);

/**
 * @brief Check if status bar is currently visible
 */
bool StatusBarIsVisible(const StatusBarState* state);

/**
 * @brief Check if auto-hide timer has expired
 *
 * @param state Status bar state pointer
 * @param now_us Current timestamp in microseconds
 * @return true if auto-hide has expired and bar should be hidden
 */
bool StatusBarShouldAutoHide(const StatusBarState* state, int64_t now_us);

/**
 * @brief Draw status bar to framebuffer
 *
 * Only draws if visible. Call StatusBarShouldAutoHide() first
 * to handle auto-hide, then call this if still visible.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param height Framebuffer height
 * @return true if status bar was drawn (dirty), false if hidden
 */
bool StatusBarDraw(uint8_t* fb, int width, int height, StatusBarState* state);

/**
 * @brief Draw using Framebuffer wrapper
 */
bool StatusBarDrawFb(Framebuffer* fb, int screen_width, int screen_height, StatusBarState* state);

/**
 * @brief Get the bounding rectangle of the status bar
 */
Rect StatusBarGetBounds(const StatusBarState* state);

}  // namespace rawdraw

#endif  // RAWDRAW_STATUS_BAR_H
