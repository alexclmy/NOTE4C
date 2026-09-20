/**
 * @file toggle.h
 * @brief Toggle switch component for on/off settings
 *
 * Visual toggle switch with pill-shaped track and circular indicator.
 * Designed for 1bpp ePaper: uses filled/unfilled circle and contrasting
 * backgrounds to clearly show on/off state.
 */

#ifndef RAWDRAW_TOGGLE_H
#define RAWDRAW_TOGGLE_H

#include <stdint.h>
#include <functional>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Toggle callback type
 */
using ToggleCallback = std::function<void(bool)>;

/**
 * @brief Toggle switch component
 *
 * Renders a pill-shaped track with a circular thumb indicator.
 * On state: filled black circle, black track fill, white thumb.
 * Off state: outlined circle, white track fill, black thumb outline.
 *
 * Layout:
 * - Track: pill-shaped rectangle (w >= 2*h for proper appearance)
 * - Thumb: circle centered on left (off) or right (on) side of track
 * - Optional label text displayed to the right of the toggle
 *
 * Usage:
 * 1. Create toggle with position and size
 * 2. SetState() to set on/off
 * 3. SetLabel() for optional text label
 * 4. Draw() renders toggle + label
 * 5. HandleTap() toggles state and invokes callback
 */
class Toggle {
public:
    /**
     * @brief Create toggle switch
     *
     * @param x Position x (left edge of track)
     * @param y Position y (top edge of track)
     * @param w Track width (recommended >= 2*height, default 48)
     * @param h Track height (determines thumb size, default 24)
     */
    Toggle(int x = 0, int y = 0, int w = 48, int h = 24);

    ~Toggle();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set toggle position
     */
    void SetPosition(int x, int y);

    /**
     * @brief Set toggle size
     */
    void SetSize(int w, int h);

    /**
     * @brief Set toggle state
     * @param on true = on (thumb right, filled), false = off (thumb left)
     */
    void SetState(bool on);

    /**
     * @brief Get current state
     */
    bool GetState() const;

    /**
     * @brief Set optional text label (displayed to the right of toggle)
     */
    void SetLabel(const char* label);

    /**
     * @brief Set label font
     */
    void SetFont(const lv_font_t* font);

    /**
     * @brief Set change callback
     */
    void SetCallback(ToggleCallback callback);

    // ============================================================
    // Interaction
    // ============================================================

    /**
     * @brief Check if point is inside toggle bounds
     */
    bool Contains(int px, int py) const;

    /**
     * @brief Handle tap event (toggle state + invoke callback)
     */
    void HandleTap();

    // ============================================================
    // Layout
    // ============================================================

    /**
     * @brief Get track bounds
     */
    Rect GetTrackBounds() const;

    /**
     * @brief Get total bounds (track + label if present)
     * @param screen_width Screen width for label measurement
     */
    Rect GetBounds(int screen_width = 0) const;

    /**
     * @brief Get thumb center position
     */
    Point GetThumbCenter() const;

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw toggle to framebuffer
     *
     * For 1bpp ePaper:
     * - ON: black filled track with white circle thumb on right
     * - OFF: white track with black outline and black circle thumb on left
     */
    void Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    // ============================================================
    // Style
    // ============================================================

    /**
     * @brief Set custom colors
     * @param track_on_color Track fill color when ON
     * @param track_off_color Track fill color when OFF
     * @param thumb_color Thumb fill color
     * @param border_color Border/outline color
     */
    void SetColors(Color track_on, Color track_off, Color thumb, Color border);

private:
    int x_, y_, w_, h_;
    bool state_;

    const char* label_;
    const lv_font_t* font_;

    ToggleCallback callback_;

    Color track_on_color_;
    Color track_off_color_;
    Color thumb_color_;
    Color border_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_TOGGLE_H
