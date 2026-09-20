/**
 * @file font_metrics_renderer.h
 * @brief Font metrics & baseline formula display page
 */

#ifndef RAWDRAW_FONT_METRICS_RENDERER_H
#define RAWDRAW_FONT_METRICS_RENDERER_H

#include "page_renderer.h"

namespace rawdraw {

class FontMetricsRenderer : public PageRenderer {
public:
    FontMetricsRenderer();
    ~FontMetricsRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

private:
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_FONT_METRICS_RENDERER_H