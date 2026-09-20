/**
 * @file footer_bar.h
 * @brief Shared bottom footer / hint bar for rawdraw pages
 */

#ifndef RAWDRAW_FOOTER_BAR_H
#define RAWDRAW_FOOTER_BAR_H

#include <stdint.h>
#include "font_engine.h"
#include "framebuffer.h"
#include "rawdraw.h"
#include "style.h"

namespace rawdraw {

class FooterBar {
public:
    FooterBar();

    void SetBounds(int screen_width, int screen_height);
    void SetText(const char* left, const char* center = nullptr, const char* right = nullptr);
    void SetFont(const lv_font_t* font);
    void SetInverted(bool inverted);

    Rect GetBounds() const;
    void Draw(uint8_t* fb, int width, int height) const;
    void Draw(Framebuffer* fb, int width, int height) const;

private:
    Rect bounds_;
    const lv_font_t* font_;
    const char* left_text_;
    const char* center_text_;
    const char* right_text_;
    bool inverted_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_FOOTER_BAR_H
