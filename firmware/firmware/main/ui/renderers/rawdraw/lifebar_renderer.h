/**
 * @file lifebar_renderer.h
 * @brief Life progress page renderer for rawdraw mode
 *
 * Shows a large circular gauge with age, days elapsed/remaining,
 * remaining weekends count, and a motivational quote.
 * Assumes birthdate 1990-01-01, 80-year lifespan.
 */

#ifndef RAWDRAW_LIFEBAR_RENDERER_H
#define RAWDRAW_LIFEBAR_RENDERER_H

#include "page_renderer.h"
#include "rawdraw/style.h"

namespace rawdraw {

class LifeBarRenderer : public PageRenderer {
public:
    LifeBarRenderer();
    ~LifeBarRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Visibility toggle (controlled via settings)
    void SetVisible(bool visible) { visible_ = visible; }
    bool IsVisible() const { return visible_; }

private:
    // Data calculation
    void UpdateStats();

    // Rendering sections
    void RenderHeader(uint8_t* fb, int width, int y) const;
    void RenderGauge(uint8_t* fb, int width, int y, int height) const;
    void RenderStats(uint8_t* fb, int width, int y) const;
    void RenderQuote(uint8_t* fb, int width, int y) const;

    // Fonts
    const lv_font_t* title_font_;
    const lv_font_t* body_font_;
    const lv_font_t* small_font_;

    // Cached stats
    int age_years_;
    int age_months_;
    int days_elapsed_;
    int days_remaining_;
    int weekends_remaining_;
    int life_pct_;

    // Visibility
    bool visible_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_LIFEBAR_RENDERER_H
