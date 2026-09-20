/**
 * @file bubble.h
 * @brief Chat bubble component for dialogue UI
 *
 * Displays conversation messages in rounded rectangles with text.
 * Supports left/right alignment (AI/user), streaming text append,
 * and automatic sizing based on content.
 */

#ifndef RAWDRAW_BUBBLE_H
#define RAWDRAW_BUBBLE_H

#include <stdint.h>
#include <string>
#include <functional>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Chat bubble alignment position
 */
enum class BubbleAlign {
    Left,    ///< AI/system messages (left side, white background with black border)
    Right,   ///< User messages (right side, black background with white text)
    Center,  ///< System notifications (centered, transparent/minimal background)
};

/**
 * @brief Chat bubble component
 *
 * Renders a rounded rectangle containing text, positioned according to alignment.
 * Supports streaming text append for real-time chat display.
 *
 * Layout:
 * - Left bubble: starts at left margin (x=margin), white fill, black border
 * - Right bubble: ends at right margin (x=width-margin-w), black fill, white text
 * - Center bubble: centered horizontally, minimal styling
 *
 * Usage:
 * 1. Create bubble with alignment and position
 * 2. SetText() for static text, or AppendText() for streaming
 * 3. Draw() renders to framebuffer
 * 4. GetBounds() returns current occupied area
 */
class Bubble {
public:
    /**
     * @brief Create bubble with alignment
     *
     * @param align Position alignment (Left, Right, Center)
     * @param margin Horizontal margin from screen edge (default 8)
     * @param max_width Maximum bubble width (0 = auto, typically screen_width - 2*margin)
     * @param radius Corner radius (default 8)
     */
    Bubble(BubbleAlign align = BubbleAlign::Left,
           int margin = 8,
           int max_width = 0,
           int radius = 8);

    ~Bubble();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set alignment position
     */
    void SetAlign(BubbleAlign align);

    /**
     * @brief Set horizontal margin from screen edge
     */
    void SetMargin(int margin);

    /**
     * @brief Set maximum bubble width
     */
    void SetMaxWidth(int max_width);

    /**
     * @brief Set corner radius
     */
    void SetRadius(int radius);

    /**
     * @brief Set font for text rendering
     */
    void SetFont(const lv_font_t* font);

    /**
     * @brief Set line spacing (extra pixels between lines)
     */
    void SetLineSpacing(int spacing);

    /**
     * @brief Set padding (internal margin between border and text)
     */
    void SetPadding(int padding);

    // ============================================================
    // Content
    // ============================================================

    /**
     * @brief Set bubble text (replaces existing content)
     *
     * @param text UTF-8 encoded text (may contain '\n' for multiple lines)
     */
    void SetText(const char* text);
    void SetText(const std::string& text);

    /**
     * @brief Append text to bubble (for streaming display)
     *
     * Text is added to existing content. Use for real-time chat
     * where messages arrive in chunks.
     *
     * @param chunk Text chunk to append
     */
    void AppendText(const char* chunk);
    void AppendText(const std::string& chunk);

    /**
     * @brief Clear bubble content
     */
    void Clear();

    /**
     * @brief Get current text content
     */
    const std::string& GetText() const;

    /**
     * @brief Check if bubble has content
     */
    bool HasContent() const;

    // ============================================================
    // Layout
    // ============================================================

    /**
     * @brief Set vertical position (top edge)
     *
     * @param y Y coordinate of bubble top edge
     */
    void SetY(int y);

    /**
     * @brief Get current bounds (position and size)
     *
     * @param screen_width Screen width for alignment calculation
     * @return Rect with bubble bounds
     */
    Rect GetBounds(int screen_width) const;

    /**
     * @brief Calculate bubble height based on text content
     *
     * Height includes: padding (top+bottom) + text height
     */
    int CalculateHeight() const;

    /**
     * @brief Calculate bubble width based on text content
     *
     * Width is clamped to max_width, or auto-sized if max_width=0
     */
    int CalculateWidth() const;

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw bubble to framebuffer
     *
     * Renders the bubble background, border, and text.
     * Marks the bubble region as dirty in framebuffer.
     *
     * @param fb Framebuffer pointer
     * @param width Framebuffer width
     * @param height Framebuffer height (for bounds clipping)
     */
    void Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw bubble using Framebuffer wrapper
     *
     * Convenience method using Framebuffer::SafeDraw.
     */
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    // ============================================================
    // Style
    // ============================================================

    /**
     * @brief Set custom colors (overrides defaults based on alignment)
     *
     * @param fill_color Background fill color
     * @param text_color Text color
     * @param border_color Border color
     * @param border_width Border thickness (0 = no border)
     */
    void SetColors(Color fill, Color text, Color border, int border_width);

private:
    void ApplyDefaultStyle();
    Rect CalculateTextBounds() const;

    // Configuration
    BubbleAlign align_;
    int margin_;
    int max_width_;
    int radius_;
    const lv_font_t* font_;
    int line_spacing_;
    int padding_;

    // Content
    std::string text_;
    int y_;

    // Style (may override defaults)
    Color fill_color_;
    Color text_color_;
    Color border_color_;
    int border_width_;
    bool custom_colors_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUBBLE_H