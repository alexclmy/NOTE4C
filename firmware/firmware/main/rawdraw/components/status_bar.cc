/**
 * @file status_bar.cc
 * @brief Bottom status bar implementation
 *
 * Renders a thin status bar at the bottom of the screen with auto-hide timer.
 * Uses independent RegionRefresh counter to avoid interfering with other UI elements.
 */

#include "status_bar.h"
#include "rawdraw/theme.h"
#include <cstring>
#include <cstdio>
#include "esp_timer.h"

namespace rawdraw {

void StatusBarInit(StatusBarState* state, const lv_font_t* font) {
    if (!state) return;
    state->visible = false;
    state->text[0] = '\0';
    state->show_time_us = 0;
    state->auto_hide_ms = 0;
    state->font = font;
    refresh_tracker_init(&state->refresh);
}

void StatusBarShow(StatusBarState* state, const char* text, int64_t auto_hide_ms) {
    if (!state || !text) return;

    // Truncate to fit buffer
    strncpy(state->text, text, sizeof(state->text) - 1);
    state->text[sizeof(state->text) - 1] = '\0';

    state->visible = true;
    state->show_time_us = esp_timer_get_time();
    state->auto_hide_ms = auto_hide_ms;

    // Mark dirty for refresh
    refresh_mark_dirty(&state->refresh);
}

void StatusBarHide(StatusBarState* state) {
    if (!state) return;
    state->visible = false;
    state->text[0] = '\0';
}

bool StatusBarIsVisible(const StatusBarState* state) {
    return state && state->visible;
}

bool StatusBarShouldAutoHide(const StatusBarState* state, int64_t now_us) {
    if (!state || !state->visible || state->auto_hide_ms == 0) {
        return false;
    }
    int64_t elapsed_ms = (now_us - state->show_time_us) / 1000;
    return elapsed_ms >= state->auto_hide_ms;
}

Rect StatusBarGetBounds(const StatusBarState* state) {
    (void)state;  // Bounds are fixed regardless of state
    return { 0, kStatusBarBottomY, Style::kScreenWidth, kStatusBarBottomHeight };
}

bool StatusBarDraw(uint8_t* fb, int width, int height, StatusBarState* state) {
    if (!fb || !state || !state->visible) return false;

    Rect bounds = StatusBarGetBounds(state);
    bounds = clamp_rect(bounds, width, height);
    if (rect_area(bounds) <= 0) return false;

    const auto& theme = ThemeManager::Get();
    const PaintStyle bar_style = theme.Component(ComponentRole::StatusBar);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color text = bar_style.fg;

    // Clear background
    DrawStyledRect(fb, width, bounds, bar_style);

    // Draw thin top border line
    DrawHLine(fb, width, bounds.y, bounds.x, bounds.x + bounds.w - 1, border);

    // Draw text centered vertically within the bar
    if (state->font && state->text[0] != '\0') {
        int text_h = state->font->line_height;
        int text_y = bounds.y + (bounds.h - text_h) / 2;
        if (text_y < bounds.y) text_y = bounds.y;
        int text_x = bounds.x + Style::kSpacingMD;
        DrawText(fb, width, text_x, text_y, state->text, state->font, text, height);
    }

    // Update refresh counter
    refresh_update_counter(&state->refresh, esp_timer_get_time());

    return true;
}

bool StatusBarDrawFb(Framebuffer* fb, int screen_width, int screen_height, StatusBarState* state) {
    if (!fb) return false;
    bool drawn = false;
    fb->SafeDraw([&drawn, screen_width, screen_height, state](uint8_t* buffer) {
        drawn = StatusBarDraw(buffer, screen_width, screen_height, state);
    });
    return drawn;
}

}  // namespace rawdraw
