/**
 * @file scrollview.h
 * @brief Scrollable container component
 *
 * Manages scrollable content area with scrollbar indicator.
 * Supports content offset tracking and scroll-to-end functionality.
 */

#ifndef RAWDRAW_SCROLLVIEW_H
#define RAWDRAW_SCROLLVIEW_H

#include <stdint.h>
#include <functional>
#include "rawdraw.h"
#include "framebuffer.h"

namespace rawdraw {

/**
 * @brief Scrollable container component
 *
 * Provides a scrollable content area with visual scrollbar.
 * Content is drawn at offset position based on scroll_offset_.
 *
 * Usage:
 * 1. Create scrollview with bounds and content height
 * 2. SetScrollOffset() to control visible content position
 * 3. DrawContent() callback draws visible portion
 * 4. Draw() renders content + scrollbar indicator
 */
class ScrollView {
public:
    /**
     * @brief Content drawing callback type
     *
     * @param fb Framebuffer
     * @param width Framebuffer width
     * @param visible_rect Currently visible content region (relative to content)
     * @param clip_rect Clipping region (screen coordinates)
     */
    using ContentDrawCallback = std::function<void(uint8_t* fb, int width,
                                                    const Rect& visible_rect,
                                                    const Rect& clip_rect)>;

    /**
     * @brief Create scrollview
     *
     * @param x Position x
     * @param y Position y
     * @param w Width
     * @param h Height (visible area)
     * @param content_height Total content height (scrollable range)
     */
    ScrollView(int x = 0, int y = 0, int w = 0, int h = 0, int content_height = 0);

    ~ScrollView();

    // ============================================================
    // Configuration
    // ============================================================

    /**
     * @brief Set bounds (visible area)
     */
    void SetBounds(const Rect& r);

    /**
     * @brief Set content height (scrollable range)
     */
    void SetContentHeight(int height);

    /**
     * @brief Set scrollbar width (default: 4)
     */
    void SetScrollbarWidth(int width);

    /**
     * @brief Enable/disable scrollbar
     */
    void SetScrollbarEnabled(bool enabled);

    // ============================================================
    // Scroll Control
    // ============================================================

    /**
     * @brief Set scroll offset (vertical scroll position)
     *
     * @param offset Y offset in content (0 = top, max = content_height - visible_height)
     */
    void SetScrollOffset(int offset);

    /**
     * @brief Get current scroll offset
     */
    int GetScrollOffset() const;

    /**
     * @brief Scroll to end (bottom of content)
     */
    void ScrollToEnd();

    /**
     * @brief Scroll by delta (positive = down, negative = up)
     */
    void ScrollBy(int delta);

    /**
     * @brief Get maximum scroll offset
     */
    int GetMaxScrollOffset() const;

    /**
     * @brief Check if can scroll up
     */
    bool CanScrollUp() const;

    /**
     * @brief Check if can scroll down
     */
    bool CanScrollDown() const;

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw scrollview with content callback
     *
     * Renders content at scroll offset + scrollbar indicator.
     *
     * @param fb Framebuffer
     * @param width Framebuffer width
     * @param draw_cb Content drawing callback
     */
    void Draw(uint8_t* fb, int width, ContentDrawCallback draw_cb);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    void Draw(Framebuffer* fb, ContentDrawCallback draw_cb);

    /**
     * @brief Get visible content rect (relative to content, accounting for scroll)
     */
    Rect GetVisibleContentRect() const;

    /**
     * @brief Get screen bounds
     */
    Rect GetBounds() const;

private:
    void DrawScrollbar(uint8_t* fb, int width);

    Rect bounds_;           ///< Visible area on screen
    int content_height_;    ///< Total scrollable content height
    int scroll_offset_;     ///< Current scroll position (Y offset)
    int scrollbar_width_;   ///< Scrollbar indicator width
    bool scrollbar_enabled_; ///< Show scrollbar
};

}  // namespace rawdraw

#endif  // RAWDRAW_SCROLLVIEW_H