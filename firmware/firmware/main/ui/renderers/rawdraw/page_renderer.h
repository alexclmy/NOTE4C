/**
 * @file page_renderer.h
 * @brief Rawdraw page renderer base class (no LVGL dependency)
 */

#ifndef RAWDRAW_PAGE_RENDERER_H
#define RAWDRAW_PAGE_RENDERER_H

#include "rawdraw/framebuffer.h"
#include "rawdraw/font_engine.h"
#include <functional>
#include <string>

namespace rawdraw {

/**
 * @brief Button event types
 */
struct ButtonEvent {
    enum Type {
        kUpClick,
        kDownClick,
        kUpDoubleClick,
        kDownDoubleClick,
        kUpLongPress,
        kDownLongPress,
        kBootClick,
        kBootDoubleClick,
        kBootLongPress,
    };
    Type type;
};

/**
 * @brief Page renderer base class for rawdraw mode
 *
 * All page renderers must implement this interface.
 * Renders directly to 1bpp framebuffer, no LVGL dependency.
 */
class PageRenderer {
public:
    virtual ~PageRenderer() = default;

    /**
     * @brief Initialize page resources
     *
     * Called once when page becomes active. Set up fonts, initial state.
     */
    virtual void Init(int width, int height) = 0;

    /**
     * @brief Render page to framebuffer
     *
     * Called on each display update. Draw all page content.
     *
     * @param fb Framebuffer to render to
     * @param width Framebuffer width
     * @param height Framebuffer height
     */
    virtual void Render(uint8_t* fb, int width, int height) = 0;

    /**
     * @brief Handle button input
     *
     * @param event Button event
     * @return true if event was consumed
     */
    virtual bool HandleInput(const ButtonEvent& event) = 0;

    /**
     * @brief Get dirty rect for partial refresh
     *
     * @return Rect that needs refresh, or {0,0,0,0} for full refresh
     */
    virtual Rect GetDirtyRect() const { return {0, 0, 0, 0}; }

    /**
     * @brief Check if page needs full refresh
     *
     * @return true if full refresh needed (e.g., page switch)
     */
    virtual bool NeedsFullRefresh() const { return needs_full_refresh_; }

    /**
     * @brief Mark page as needing full refresh
     */
    void MarkFullRefresh() { needs_full_refresh_ = true; }

    /**
     * @brief Clear full refresh flag
     */
    void ClearFullRefreshFlag() { needs_full_refresh_ = false; }

    // Streaming support (for chat pages)
    virtual bool AppendText(const char* chunk) { (void)chunk; return false; }
    virtual void BeginStream() {}
    virtual void EndStream() {}

protected:
    int width_ = 0;
    int height_ = 0;
    bool needs_full_refresh_ = true;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PAGE_RENDERER_H
