/**
 * @file progress_bar.h
 * @brief Progress bar component
 *
 * Horizontal progress bar with rounded corners, percentage value,
 * and optional label.
 */

#ifndef RAWDRAW_PROGRESS_BAR_H
#define RAWDRAW_PROGRESS_BAR_H

#include <stdint.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

// ============================================================
// Circular Gauge (standalone function + class)
// ============================================================

/**
 * @brief Draw circular progress gauge (ring style)
 *
 * Renders a filled arc from 12-o'clock position clockwise.
 * Uses Bresenham-based arc drawing for 1bpp efficiency.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param center Circle center point
 * @param radius Outer radius
 * @param thickness Ring thickness
 * @param value_pct Progress value (0-100)
 * @param bg_color Background ring color
 * @param fg_color Foreground (filled) color
 */
void DrawCircularProgress(uint8_t* fb, int width, const Point& center, int radius,
                          int thickness, int value_pct,
                          Color bg_color = WHITE, Color fg_color = BLACK);

/**
 * @brief Draw circular gauge with center text label
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param center Circle center
 * @param radius Outer radius
 * @param thickness Ring thickness
 * @param value_pct Progress value (0-100)
 * @param label Center text (e.g., "75%")
 * @param font Label font
 */
void DrawCircularProgressWithLabel(uint8_t* fb, int width, const Point& center, int radius,
                                   int thickness, int value_pct,
                                   const char* label, const lv_font_t* font);

/**
 * @brief Progress bar component
 *
 * Displays a horizontal bar showing progress from 0-100%.
 * Supports rounded corners, custom colors, and optional label.
 *
 * Usage:
 * 1. Create progress bar with bounds
 * 2. SetValue() to set progress percentage
 * 3. SetLabel() for optional text
 * 4. Draw() renders bar + label
 */
class ProgressBar {
public:
    /**
     * @brief Create progress bar
     *
     * @param x Position x
     * @param y Position y
     * @param w Width
     * @param h Height (default: 8)
     */
    ProgressBar(int x = 0, int y = 0, int w = 100, int h = 8);

    ~ProgressBar();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set bounds
     */
    void SetBounds(const Rect& r);
    void SetBounds(int x, int y, int w, int h);

    /**
     * @brief Set progress value (0-100)
     */
    void SetValue(int value);

    /**
     * @brief Get current value
     */
    int GetValue() const;

    /**
     * @brief Set label text (optional)
     */
    void SetLabel(const char* label);

    /**
     * @brief Set label font
     */
    void SetLabelFont(const lv_font_t* font);

    /**
     * @brief Set corner radius (default: h/2 for pill shape)
     */
    void SetRadius(int radius);

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw progress bar to framebuffer
     */
    void Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    /**
     * @brief Get bounds
     */
    Rect GetBounds() const;

    // ============================================================
    // Style
    // ============================================================

    /**
     * @brief Set background color (empty area)
     */
    void SetBgColor(Color color);

    /**
     * @brief Set foreground color (filled area)
     */
    void SetFgColor(Color color);

private:
    Rect bounds_;
    int value_;
    int radius_;

    const char* label_;
    const lv_font_t* label_font_;

    Color bg_color_;
    Color fg_color_;
};

// ============================================================
// Circular Gauge Component
// ============================================================

/**
 * @brief Circular progress gauge component
 *
 * Renders a ring-style progress indicator with optional center label.
 * Suitable for "year remaining", "life remaining" type visualizations.
 *
 * Usage:
 * 1. Create gauge with center position and radius
 * 2. SetValue() to set progress (0-100)
 * 3. SetLabel() for optional center text
 * 4. Draw() renders ring + label
 */
class CircularGauge {
public:
    /**
     * @brief Create circular gauge
     *
     * @param cx Center x position
     * @param cy Center y position
     * @param radius Outer radius
     * @param thickness Ring thickness (default: 6)
     */
    CircularGauge(int cx = 200, int cy = 150, int radius = 60, int thickness = 6);

    ~CircularGauge();

    // ============================================================
    // Configuration
    // ============================================================

    void SetCenter(int cx, int cy);
    void SetRadius(int radius);
    void SetThickness(int thickness);

    /**
     * @brief Set progress value (0-100)
     */
    void SetValue(int value);

    /**
     * @brief Get current value
     */
    int GetValue() const;

    /**
     * @brief Set center label text (optional)
     */
    void SetLabel(const char* label);

    /**
     * @brief Set label font
     */
    void SetLabelFont(const lv_font_t* font);

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw circular gauge to framebuffer
     */
    void Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    /**
     * @brief Get bounding box of the gauge (for dirty rect tracking)
     */
    Rect GetBounds() const;

    // ============================================================
    // Style
    // ============================================================

    void SetBgColor(Color color);
    void SetFgColor(Color color);

private:
    int cx_;
    int cy_;
    int radius_;
    int thickness_;
    int value_;

    const char* label_;
    const lv_font_t* label_font_;

    Color bg_color_;
    Color fg_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PROGRESS_BAR_H