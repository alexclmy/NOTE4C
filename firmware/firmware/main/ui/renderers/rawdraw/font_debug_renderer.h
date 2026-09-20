/**
 * @file font_debug_renderer.h
 * @brief Text alignment test page — rect/round-rect with centered text + bY/tY markers
 */

#ifndef RAWDRAW_FONT_DEBUG_RENDERER_H
#define RAWDRAW_FONT_DEBUG_RENDERER_H

#include "page_renderer.h"

namespace rawdraw {

class FontDebugRenderer : public PageRenderer {
public:
    FontDebugRenderer();
    ~FontDebugRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

private:
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_FONT_DEBUG_RENDERER_H