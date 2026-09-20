/**
 * @file button.h
 * @brief Button component with icon and text
 *
 * Rounded button supporting icon font characters and optional text label.
 * Visual feedback for press state (inverted colors).
 */

#ifndef RAWDRAW_BUTTON_H
#define RAWDRAW_BUTTON_H

#include <stdint.h>
#include <functional>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Button click callback type
 */
using ButtonCallback = std::function<void()>;

/**
 * @brief Button component with icon and optional text
 *
 * Displays a rounded rectangle containing an icon (from icon font)
 * and optional text label below.
 *
 * States:
 * - Normal: Default colors
 * - Pressed: Inverted colors (highlight)
 *
 * Usage:
 * 1. Create button with position, size, and icon code
 * 2. SetText() for optional label
 * 3. SetCallback() for click handler
 * 4. Draw() renders current state
 * 5. HandlePress() toggles pressed state and invokes callback
 */
class Button {
public:
    /**
     * @brief Create button
     *
     * @param x Position x
     * @param y Position y
     * @param w Button width
     * @param h Button height
     * @param icon_code UTF-8 icon character (from font_zectrix or weather_icons)
     * @param icon_font Icon font (default: font_zectrix_16_1)
     */
    Button(int x = 0, int y = 0, int w = 40, int h = 40,
           const char* icon_code = nullptr,
           const lv_font_t* icon_font = nullptr);

    ~Button();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set button position
     */
    void SetPosition(int x, int y);

    /**
     * @brief Set button size
     */
    void SetSize(int w, int h);

    /**
     * @brief Set icon code (UTF-8 from icon font)
     */
    void SetIcon(const char* icon_code);

    /**
     * @brief Set icon font
     */
    void SetIconFont(const lv_font_t* font);

    /**
     * @brief Set text label (displayed below icon)
     */
    void SetText(const char* text);

    /**
     * @brief Set text font
     */
    void SetTextFont(const lv_font_t* font);

    /**
     * @brief Set corner radius
     */
    void SetRadius(int radius);

    /**
     * @brief Set click callback
     */
    void SetCallback(ButtonCallback callback);

    // ============================================================
    // State
    // ============================================================

    /**
     * @brief Check if point is inside button
     *
     * @param px Point x
     * @param py Point y
     * @return true if point is inside button bounds
     */
    bool Contains(int px, int py) const;

    /**
     * @brief Set pressed state (visual feedback)
     */
    void SetPressed(bool pressed);

    /**
     * @brief Check if button is pressed
     */
    bool IsPressed() const;

    /**
     * @brief Handle press event (toggle state + invoke callback)
     *
     * Called when touch/click is detected on button.
     */
    void HandlePress();

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw button to framebuffer
     *
     * Renders background, border, icon, and text.
     * Uses inverted colors if pressed.
     */
    void Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    /**
     * @brief Get button bounds
     */
    Rect GetBounds() const;

    // ============================================================
    // Style
    // ============================================================

    /**
     * @brief Set custom colors
     */
    void SetColors(Color bg, Color fg, Color border);

private:
    int x_, y_, w_, h_;
    int radius_;

    const char* icon_code_;
    const lv_font_t* icon_font_;

    const char* text_;
    const lv_font_t* text_font_;

    bool pressed_;
    ButtonCallback callback_;

    Color bg_color_;
    Color fg_color_;
    Color border_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUTTON_H