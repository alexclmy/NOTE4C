/**
 * @file modal.cc
 * @brief Shared centered modal container for rawdraw overlays
 */

#include "modal.h"
#include "rawdraw/layout_utils.h"  // FIX: 使用 InkCenteredTextTopYInBox 替代私有函数
#include "rawdraw/theme.h"

#include <algorithm>

namespace rawdraw {
namespace {

// ⚠️ DEPRECATED: 此函数用 line_height 居中，对中文紧凑控件文字偏上。
// 应改用 layout_utils.h 的 InkCenteredTextTopYInBox。
// 参见 wiki/projects/notellm-baseline-alignment.md
//
// int CalcBaselineY(const lv_font_t* font, int top, int height) {
//     if (!font) {
//         return top;
//     }
//     const int line_height = static_cast<int>(font->line_height);
//     const int base_line = static_cast<int>(font->base_line);
//     return top + std::max(0, (height - line_height) / 2) + base_line + Style::kVisualTextOffset;
// }
//
// int TopFromBaseline(const lv_font_t* font, int baseline_y) {
//     if (!font) {
//         return baseline_y;
//     }
//     return baseline_y - static_cast<int>(font->base_line);
// }

}  // namespace

Modal::Modal()
    : bounds_{Style::kModalInset, 44, Style::kScreenWidth - Style::kModalInset * 2, Style::kScreenHeight - 88},
      title_(nullptr),
      footer_(nullptr),
      title_font_(&BUILTIN_TEXT_FONT),
      radius_(Style::kBorderRadiusLG),
      border_width_(Style::kBorderThin) {}

void Modal::SetBounds(const Rect& bounds) {
    bounds_ = bounds;
}

void Modal::SetBounds(int x, int y, int w, int h) {
    bounds_ = {x, y, w, h};
}

void Modal::CenterInScreen(int screen_width, int screen_height, int inset) {
    bounds_ = {inset, inset + 8, screen_width - inset * 2, screen_height - inset * 2 - 16};
}

void Modal::SetTitle(const char* title) {
    title_ = title;
}

void Modal::SetTitleFont(const lv_font_t* font) {
    if (font) {
        title_font_ = font;
    }
}

void Modal::SetBodyFooter(const char* footer) {
    footer_ = footer;
}

void Modal::SetRadius(int radius) {
    radius_ = radius;
}

void Modal::SetBorderWidth(int border_width) {
    border_width_ = border_width;
}

Rect Modal::GetBounds() const {
    return bounds_;
}

Rect Modal::GetTitleBounds() const {
    return {bounds_.x, bounds_.y, bounds_.w, Style::kModalTitleHeight};
}

Rect Modal::GetContentBounds() const {
    const int title_h = title_ && title_[0] != '\0' ? Style::kModalTitleHeight : 0;
    const int footer_h = footer_ && footer_[0] != '\0' ? Style::kModalFooterHeight : 0;
    return {
        bounds_.x + Style::kCardPadding,
        bounds_.y + title_h + Style::kCardPadding,
        bounds_.w - Style::kCardPadding * 2,
        bounds_.h - title_h - footer_h - Style::kCardPadding * 2
    };
}

Rect Modal::GetFooterBounds() const {
    return {bounds_.x, bounds_.y + bounds_.h - Style::kModalFooterHeight, bounds_.w, Style::kModalFooterHeight};
}

void Modal::Draw(uint8_t* fb, int width, int height) const {
    if (!fb) {
        return;
    }

    const Rect bounds = clamp_rect(bounds_, width, height);
    if (bounds.w <= 0 || bounds.h <= 0) {
        return;
    }

    PaintStyle modal_style = ThemeManager::Get().Component(ComponentRole::Modal);
    modal_style.border_width = border_width_;
    DrawStyledRoundRect(fb, width, height, bounds, radius_, modal_style);

    if (title_ && title_[0] != '\0') {
        const Rect title_bounds = GetTitleBounds();
        DrawStyledRoundRect(fb, width, height, title_bounds, radius_, modal_style);
        DrawHLine(fb, width, title_bounds.y + title_bounds.h - 1, title_bounds.x, title_bounds.x + title_bounds.w - 1, modal_style.border);
        // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
        // 参见 wiki/projects/notellm-baseline-alignment.md
        const int text_y = InkCenteredTextTopYInBox(title_font_, title_, title_bounds.y, title_bounds.h, 0);
        const int text_w = MeasureTextWidth(title_, title_font_);
        const int text_x = title_bounds.x + std::max(0, (title_bounds.w - text_w) / 2);
        DrawStyledText(fb, width, text_x, text_y, title_, title_font_, modal_style, height);
    }

    if (footer_ && footer_[0] != '\0') {
        const Rect footer_bounds = GetFooterBounds();
        DrawHLine(fb, width, footer_bounds.y, footer_bounds.x, footer_bounds.x + footer_bounds.w - 1, modal_style.border);
        // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
        // 参见 wiki/projects/notellm-baseline-alignment.md
        const int text_y = InkCenteredTextTopYInBox(title_font_, footer_, footer_bounds.y, footer_bounds.h, 0);
        const int text_w = MeasureTextWidth(footer_, title_font_);
        const int text_x = footer_bounds.x + std::max(0, (footer_bounds.w - text_w) / 2);
        DrawStyledText(fb, width, text_x, text_y, footer_, title_font_, modal_style, height);
    }
}

void Modal::Draw(Framebuffer* fb, int width, int height) const {
    if (!fb) {
        return;
    }
    fb->SafeDraw([this, width, height](uint8_t* buffer) {
        Draw(buffer, width, height);
    });
}

}  // namespace rawdraw
