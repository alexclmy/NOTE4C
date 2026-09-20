/**
 * @file footer_bar.cc
 * @brief Shared bottom footer / hint bar for rawdraw pages
 */

#include "footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/theme.h"

#include <algorithm>

namespace rawdraw {

FooterBar::FooterBar()
    : bounds_{0, Style::kScreenHeight - Style::kFooterBarHeight, Style::kScreenWidth, Style::kFooterBarHeight},
      font_(&BUILTIN_TEXT_FONT),
      left_text_(nullptr),
      center_text_(nullptr),
      right_text_(nullptr),
      inverted_(false) {}

void FooterBar::SetBounds(int screen_width, int screen_height) {
    bounds_ = {0, screen_height - Style::kFooterBarHeight, screen_width, Style::kFooterBarHeight};
}

void FooterBar::SetText(const char* left, const char* center, const char* right) {
    left_text_ = left;
    center_text_ = center;
    right_text_ = right;
}

void FooterBar::SetFont(const lv_font_t* font) {
    if (font) {
        font_ = font;
    }
}

void FooterBar::SetInverted(bool inverted) {
    inverted_ = inverted;
}

Rect FooterBar::GetBounds() const {
    return bounds_;
}

void FooterBar::Draw(uint8_t* fb, int width, int height) const {
    if (!fb || !font_) {
        return;
    }

    const Rect bounds = clamp_rect(bounds_, width, height);
    if (bounds.w <= 0 || bounds.h <= 0) {
        return;
    }

    const auto& theme = ThemeManager::Get();
    const PaintStyle normal = theme.Component(ComponentRole::StatusBar);
    const PaintStyle selected = theme.Style(ThemeToken::Selected);
    const Color bg = inverted_ ? selected.bg : normal.bg;
    const Color fg = inverted_ ? selected.fg : normal.fg;
    const Color border = inverted_ ? selected.border : theme.ColorFor(ThemeToken::Border);
    DrawStyledRect(fb, width, bounds,
                   MakePaint(fg, bg, border, inverted_ ? selected.dither : normal.dither,
                             0, RefreshCost::StaticSafe));
    DrawRect(fb, width, {bounds.x, bounds.y, bounds.w, Style::kShellDividerThickness}, border);

    const int center_y = bounds.y + bounds.h / 2;
    const int text_y = InkCenteredTextTopY(font_, left_text_ ? left_text_ : (center_text_ ? center_text_ : (right_text_ ? right_text_ : "")), center_y, 0);
    const int pad = Style::kFooterBarPadding;

    if (left_text_ && left_text_[0] != '\0') {
        DrawText(fb, width, bounds.x + pad, text_y, left_text_, font_, fg, height);
    }

    if (center_text_ && center_text_[0] != '\0') {
        const int center_w = MeasureTextWidth(center_text_, font_);
        const int center_x = bounds.x + std::max(0, (bounds.w - center_w) / 2);
        DrawText(fb, width, center_x, text_y, center_text_, font_, fg, height);
    }

    if (right_text_ && right_text_[0] != '\0') {
        const int right_w = MeasureTextWidth(right_text_, font_);
        const int right_x = bounds.x + bounds.w - pad - right_w;
        DrawText(fb, width, std::max(bounds.x + pad, right_x), text_y, right_text_, font_, fg, height);
    }
}

void FooterBar::Draw(Framebuffer* fb, int width, int height) const {
    if (!fb) {
        return;
    }
    fb->SafeDraw([this, width, height](uint8_t* buffer) {
        Draw(buffer, width, height);
    });
}

}  // namespace rawdraw
