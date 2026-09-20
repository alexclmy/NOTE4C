/**
 * @file modal.h
 * @brief Shared centered modal container for rawdraw overlays
 */

#ifndef RAWDRAW_MODAL_H
#define RAWDRAW_MODAL_H

#include <stdint.h>
#include "font_engine.h"
#include "framebuffer.h"
#include "rawdraw.h"
#include "style.h"

namespace rawdraw {

class Modal {
public:
    Modal();

    void SetBounds(const Rect& bounds);
    void SetBounds(int x, int y, int w, int h);
    void CenterInScreen(int screen_width, int screen_height, int inset = Style::kModalInset);
    void SetTitle(const char* title);
    void SetTitleFont(const lv_font_t* font);
    void SetBodyFooter(const char* footer);
    void SetRadius(int radius);
    void SetBorderWidth(int border_width);

    Rect GetBounds() const;
    Rect GetTitleBounds() const;
    Rect GetContentBounds() const;
    Rect GetFooterBounds() const;

    void Draw(uint8_t* fb, int width, int height) const;
    void Draw(Framebuffer* fb, int width, int height) const;

private:
    Rect bounds_;
    const char* title_;
    const char* footer_;
    const lv_font_t* title_font_;
    int radius_;
    int border_width_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_MODAL_H
