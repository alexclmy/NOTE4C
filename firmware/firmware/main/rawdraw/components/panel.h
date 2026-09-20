/**
 * @file panel.h
 * @brief Panel component with title bar and content area
 *
 * A bordered container with optional title bar, rounded corners,
 * and separate content area for child components.
 */

#ifndef RAWDRAW_PANEL_H
#define RAWDRAW_PANEL_H

#include <stdint.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Panel component with title bar
 *
 * A bordered region with optional title bar at top and
 * content area below. Supports rounded corners.
 *
 * Layout:
 * - Title bar: height = title_font.line_height + padding
 * - Content area: remaining height below title bar
 *
 * Usage:
 * 1. Create panel with bounds
 * 2. SetTitle() for optional title
 * 3. Draw() renders panel + title
 * 4. GetContentBounds() for placing child components
 */
class Panel {
public:
    /**
     * @brief Create panel
     *
     * @param x Position x
     * @param y Position y
     * @param w Width
     * @param h Height
     * @param radius Corner radius (default: 8)
     */
    Panel(int x = 0, int y = 0, int w = 0, int h = 0, int radius = 8);

    ~Panel();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set bounds
     */
    void SetBounds(const Rect& r);
    void SetBounds(int x, int y, int w, int h);

    /**
     * @brief Set corner radius
     */
    void SetRadius(int radius);

    /**
     * @brief Set title text
     */
    void SetTitle(const char* title);

    /**
     * @brief Set title font
     */
    void SetTitleFont(const lv_font_t* font);

    /**
     * @brief Set title bar height (0 = auto based on font)
     */
    void SetTitleHeight(int height);

    /**
     * @brief Set content padding (margin inside content area)
     */
    void SetPadding(int padding);

    /**
     * @brief Set border thickness
     */
    void SetBorderWidth(int width);

    /**
     * @brief Enable/disable title bar
     */
    void SetTitleEnabled(bool enabled);

    // ============================================================
    // Layout
    // ============================================================

    /**
     * @brief Get panel bounds (full area)
     */
    Rect GetBounds() const;

    /**
     * @brief Get title bar bounds
     */
    Rect GetTitleBounds() const;

    /**
     * @brief Get content area bounds (inside panel, below title bar)
     */
    Rect GetContentBounds() const;

    /**
     * @brief Calculate title bar height
     */
    int CalculateTitleHeight() const;

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw panel to framebuffer
     *
     * Renders panel background, border, title bar, and title text.
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
     * @brief Set panel colors
     */
    void SetColors(Color bg, Color border);

    /**
     * @brief Set title bar colors
     */
    void SetTitleColors(Color bg, Color text);

private:
    Rect bounds_;
    int radius_;

    const char* title_;
    const lv_font_t* title_font_;
    int title_height_;
    int padding_;
    int border_width_;
    bool title_enabled_;

    Color bg_color_;
    Color border_color_;
    Color title_bg_color_;
    Color title_text_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PANEL_H