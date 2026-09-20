/**
 * @file slider.h
 * @brief Horizontal slider component for value selection
 *
 * A horizontal track with a draggable thumb indicator.
 * Supports min/max value range and optional label display.
 */

#ifndef RAWDRAW_SLIDER_H
#define RAWDRAW_SLIDER_H

#include <stdint.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Slider value change callback type
 */
using SliderCallback = std::function<void(int)>;

/**
 * @brief Horizontal slider component
 *
 * Renders a horizontal track with a draggable thumb (diamond or circle).
 * Shows current value position along the track.
 *
 * Layout:
 * - Track: horizontal line or thin rounded rectangle
 * - Thumb: filled diamond/circle at current value position
 * - Optional min/max labels at track ends
 * - Optional value label above thumb
 *
 * Usage:
 * 1. Create slider with position, size, and value range
 * 2. SetValue() to set current value
 * 3. SetLabels() for optional min/max/value text
 * 4. Draw() renders slider
 * 5. HandleDrag() updates value based on x position
 */
class Slider {
public:
    /**
     * @brief Create slider
     *
     * @param x Position x (left edge)
     * @param y Position y (top edge)
     * @param w Track width
     * @param h Track height (default 24)
     * @param min_val Minimum value (default 0)
     * @param max_val Maximum value (default 100)
     */
    Slider(int x = 0, int y = 0, int w = 200, int h = 24,
           int min_val = 0, int max_val = 100);

    ~Slider();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set slider position
     */
    void SetPosition(int x, int y);

    /**
     * @brief Set slider size
     */
    void SetSize(int w, int h);

    /**
     * @brief Set value range
     */
    void SetRange(int min_val, int max_val);

    /**
     * @brief Set current value (clamped to range)
     */
    void SetValue(int value);

    /**
     * @brief Get current value
     */
    int GetValue() const;

    /**
     * @brief Get current value as percentage (0-100)
     */
    int GetValuePercent() const;

    /**
     * @brief Set optional labels
     * @param min_label Text at left end (e.g., "0")
     * @param max_label Text at right end (e.g., "100")
     * @param value_label Text shown above thumb (nullptr = auto format)
     */
    void SetLabels(const char* min_label, const char* max_label, const char* value_label = nullptr);

    /**
     * @brief Set font for labels
     */
    void SetFont(const lv_font_t* font);

    /**
     * @brief Set value change callback
     */
    void SetCallback(SliderCallback callback);

    // ============================================================
    // Interaction
    // ============================================================

    /**
     * @brief Check if point is inside slider bounds
     */
    bool Contains(int px, int py) const;

    /**
     * @brief Handle drag event (update value based on x position)
     * @param px Touch x coordinate
     * @return true if value changed
     */
    bool HandleDrag(int px);

    // ============================================================
    // Layout
    // ============================================================

    /**
     * @brief Get slider bounds
     */
    Rect GetBounds() const;

    /**
     * @brief Get track bounds (just the bar area)
     */
    Rect GetTrackBounds() const;

    /**
     * @brief Get thumb center position
     */
    Point GetThumbCenter() const;

    /**
     * @brief Calculate value from x position
     */
    int XToValue(int px) const;

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw slider to framebuffer
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
     */
    void SetColors(Color track_bg, Color track_fill, Color thumb, Color text);

private:
    int x_, y_, w_, h_;
    int min_val_, max_val_;
    int value_;

    const char* min_label_;
    const char* max_label_;
    const char* value_label_;
    const lv_font_t* font_;

    SliderCallback callback_;

    Color track_bg_color_;
    Color track_fill_color_;
    Color thumb_color_;
    Color text_color_;
    Color border_color_;

    // Default label storage
    char min_label_buf_[12];
    char max_label_buf_[12];
};

}  // namespace rawdraw

#endif  // RAWDRAW_SLIDER_H
