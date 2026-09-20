/**
 * @file list_item.h
 * @brief List item component for settings menus and lists
 *
 * A horizontal row with optional icon, label, value, and chevron arrow.
 * Designed for settings pages and menu lists.
 */

#ifndef RAWDRAW_LIST_ITEM_H
#define RAWDRAW_LIST_ITEM_H

#include <stdint.h>
#include <functional>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief List item click callback type
 */
using ListItemCallback = std::function<void()>;

/**
 * @brief List item component
 *
 * Renders a horizontal row with the following optional elements (left to right):
 * 1. Icon (from icon font)
 * 2. Label (main text)
 * 3. Value (secondary text, right-aligned)
 * 4. Chevron/arrow (indicates navigable item)
 *
 * Supports pressed state with inverted colors for touch feedback.
 * Separator line at bottom for multi-item lists.
 *
 * Layout:
 * [Icon] Label            Value  [Chevron]
 * ─────────────────────────────────────
 *
 * Usage:
 * 1. Create list item with position and size
 * 2. SetLabel() and/or SetValue()
 * 3. SetIcon() for optional leading icon
 * 4. SetShowChevron() to show navigation arrow
 * 5. SetCallback() for click handler
 * 6. Draw() renders the item
 */
class ListItem {
public:
    /**
     * @brief Create list item
     *
     * @param x Position x
     * @param y Position y
     * @param w Width
     * @param h Height (default 36)
     */
    ListItem(int x = 0, int y = 0, int w = 0, int h = 36);

    ~ListItem();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set item bounds
     */
    void SetBounds(const Rect& r);
    void SetBounds(int x, int y, int w, int h);

    /**
     * @brief Set label text (main text, left side)
     */
    void SetLabel(const char* label);

    /**
     * @brief Set label font
     */
    void SetLabelFont(const lv_font_t* font);

    /**
     * @brief Set value text (right-aligned, before chevron)
     */
    void SetValue(const char* value);

    /**
     * @brief Set value font
     */
    void SetValueFont(const lv_font_t* font);

    /**
     * @brief Set icon (UTF-8 from icon font)
     */
    void SetIcon(const char* icon_code);

    /**
     * @brief Set icon font
     */
    void SetIconFont(const lv_font_t* font);

    /**
     * @brief Show/hide chevron arrow on right side
     */
    void SetShowChevron(bool show);

    /**
     * @brief Show/hide separator line at bottom
     */
    void SetShowSeparator(bool show);

    /**
     * @brief Set internal padding
     */
    void SetPadding(int padding);

    /**
     * @brief Set click callback
     */
    void SetCallback(ListItemCallback callback);

    // ============================================================
    // State
    // ============================================================

    /**
     * @brief Check if point is inside item bounds
     */
    bool Contains(int px, int py) const;

    /**
     * @brief Set pressed state (inverted colors)
     */
    void SetPressed(bool pressed);

    /**
     * @brief Get pressed state
     */
    bool IsPressed() const;

    /**
     * @brief Handle tap event (toggle pressed + invoke callback)
     */
    void HandleTap();

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw list item to framebuffer
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
     * @brief Set custom colors
     */
    void SetColors(Color bg, Color text, Color value_text, Color separator);

private:
    /**
     * @brief Draw chevron arrow at position
     */
    void DrawChevron(uint8_t* fb, int width, int x, int y, Color color) const;

    int x_, y_, w_, h_;
    int padding_;

    const char* label_;
    const lv_font_t* label_font_;

    const char* value_;
    const lv_font_t* value_font_;

    const char* icon_code_;
    const lv_font_t* icon_font_;

    bool show_chevron_;
    bool show_separator_;
    bool pressed_;

    ListItemCallback callback_;

    Color bg_color_;
    Color text_color_;
    Color value_text_color_;
    Color separator_color_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_LIST_ITEM_H
