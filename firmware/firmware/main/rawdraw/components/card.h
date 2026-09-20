/**
 * @file card.h
 * @brief Card container component with optional title bar and shadow
 *
 * A rounded rectangle container for grouping related content.
 * Supports optional title bar, shadow effect (offset rect), and padding.
 */

#ifndef RAWDRAW_CARD_H
#define RAWDRAW_CARD_H

#include <stdint.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Card container component
 *
 * A versatile rounded container for grouping UI elements.
 * Features:
 * - Rounded rectangle with configurable border
 * - Optional shadow effect (draws offset filled rect behind)
 * - Optional title bar with separator line
 * - Configurable content padding
 *
 * Layout:
 * - Shadow: offset by shadow_offset pixels down-right
 * - Title bar: top section with background and text
 * - Content area: remaining area inside card
 *
 * Usage:
 * 1. Create card with bounds
 * 2. SetTitle() for optional title bar
 * 3. Enable/disable shadow with SetShadowEnabled()
 * 4. Draw() renders card + title
 * 5. GetContentBounds() for placing child components
 */
class Card {
public:
    /**
     * @brief Create card container
     *
     * @param x Position x
     * @param y Position y
     * @param w Width
     * @param h Height
     * @param radius Corner radius (default: 8)
     */
    Card(int x = 0, int y = 0, int w = 0, int h = 0, int radius = 8);

    ~Card();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set card bounds
     */
    void SetBounds(const Rect& r);
    void SetBounds(int x, int y, int w, int h);

    /**
     * @brief Set corner radius
     */
    void SetRadius(int radius);

    /**
     * @brief Set border thickness
     */
    void SetBorderWidth(int width);

    /**
     * @brief Set content padding
     */
    void SetPadding(int padding);

    /**
     * @brief Set title text
     */
    void SetTitle(const char* title);

    /**
     * @brief Set title font
     */
    void SetTitleFont(const lv_font_t* font);

    /**
     * @brief Set title bar height (0 = auto based on font height + padding)
     */
    void SetTitleHeight(int height);

    /**
     * @brief Enable/disable title bar
     */
    void SetTitleEnabled(bool enabled);

    /**
     * @brief Enable/disable shadow effect
     *
     * Shadow is drawn as a black offset rectangle behind the card.
     * Works well for 1bpp ePaper to create depth.
     */
    void SetShadowEnabled(bool enabled);

    /**
     * @brief Set shadow offset (default: 2 pixels)
     */
    void SetShadowOffset(int offset);

    // ============================================================
    // Layout
    // ============================================================

    /**
     * @brief Get card bounds (full area including shadow offset)
     */
    Rect GetBounds() const;

    /**
     * @brief Get title bar bounds
     */
    Rect GetTitleBounds() const;

    /**
     * @brief Get content area bounds (inside card, below title bar if present)
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
     * @brief Draw card to framebuffer
     *
     * Renders in order:
     * 1. Shadow (offset black rect)
     * 2. Card background with border
     * 3. Title bar (if enabled, with separator line)
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
     * @brief Set card colors
     */
    void SetColors(Color bg, Color border);

    /**
     * @brief Set title bar colors
     */
    void SetTitleColors(Color bg, Color text);

    /**
     * @brief Set shadow color
     */
    void SetShadowColor(Color color);

private:
    Rect bounds_;
    int radius_;
    int border_width_;
    int padding_;

    const char* title_;
    const lv_font_t* title_font_;
    int title_height_;
    bool title_enabled_;

    bool shadow_enabled_;
    int shadow_offset_;

    Color bg_color_;
    Color border_color_;
    Color title_bg_color_;
    Color title_text_color_;
    Color shadow_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_CARD_H
