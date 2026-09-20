/**
 * @file list_item.cc
 * @brief List item implementation
 */

#include "list_item.h"
#include "rawdraw/theme.h"
#include "style.h"
#include <algorithm>

namespace rawdraw {

ListItem::ListItem(int x, int y, int w, int h)
    : x_(x)
    , y_(y)
    , w_(w)
    , h_(h)
    , padding_(Style::kSpacingSM)
    , label_(nullptr)
    , label_font_(nullptr)
    , value_(nullptr)
    , value_font_(nullptr)
    , icon_code_(nullptr)
    , icon_font_(nullptr)
    , show_chevron_(false)
    , show_separator_(true)
    , pressed_(false)
    , callback_(nullptr)
    , bg_color_(WHITE)
    , text_color_(BLACK)
    , value_text_color_(BLACK)
    , separator_color_(BLACK) {
}

ListItem::~ListItem() {}

void ListItem::SetBounds(const Rect& r) { x_ = r.x; y_ = r.y; w_ = r.w; h_ = r.h; }
void ListItem::SetBounds(int x, int y, int w, int h) { x_ = x; y_ = y; w_ = w; h_ = h; }

void ListItem::SetLabel(const char* label) { label_ = label; }
void ListItem::SetLabelFont(const lv_font_t* font) { label_font_ = font; }
void ListItem::SetValue(const char* value) { value_ = value; }
void ListItem::SetValueFont(const lv_font_t* font) { value_font_ = font; }
void ListItem::SetIcon(const char* icon_code) { icon_code_ = icon_code; }
void ListItem::SetIconFont(const lv_font_t* font) { icon_font_ = font; }
void ListItem::SetShowChevron(bool show) { show_chevron_ = show; }
void ListItem::SetShowSeparator(bool show) { show_separator_ = show; }
void ListItem::SetPadding(int padding) { padding_ = padding; }
void ListItem::SetCallback(ListItemCallback callback) { callback_ = callback; }

bool ListItem::Contains(int px, int py) const {
    return px >= x_ && px < x_ + w_ && py >= y_ && py < y_ + h_;
}

void ListItem::SetPressed(bool pressed) { pressed_ = pressed; }
bool ListItem::IsPressed() const { return pressed_; }

void ListItem::HandleTap() {
    pressed_ = true;
    if (callback_) {
        callback_();
    }
}

Rect ListItem::GetBounds() const {
    return {x_, y_, w_, h_};
}

void ListItem::SetColors(Color bg, Color text, Color value_text, Color separator) {
    bg_color_ = bg;
    text_color_ = text;
    value_text_color_ = value_text;
    separator_color_ = separator;
}

void ListItem::DrawChevron(uint8_t* fb, int width, int x, int y, Color color) const {
    // Draw a simple right-pointing chevron using lines
    int size = 5;
    // Chevron: >
    Point p1 = {x, y - size};      // top-left
    Point p2 = {x + size, y};      // center-right (tip)
    Point p3 = {x, y + size};      // bottom-left

    DrawLine(fb, width, p1, p2, color);
    DrawLine(fb, width, p2, p3, color);
}

void ListItem::Draw(uint8_t* fb, int width, int height) {
    if (!fb || w_ <= 0 || h_ <= 0) return;

    Rect bounds = clamp_rect({x_, y_, w_, h_}, width, height);
    if (rect_area(bounds) <= 0) return;

    const auto& theme = ThemeManager::Get();
    const bool default_colors = bg_color_ == WHITE && text_color_ == BLACK &&
                                value_text_color_ == BLACK && separator_color_ == BLACK;
    PaintStyle row_style = theme.Component(pressed_ ? ComponentRole::SettingsSelected
                                                    : ComponentRole::SettingsRow);

    Color bg = pressed_ ? BLACK : bg_color_;
    Color fg = pressed_ ? WHITE : text_color_;
    Color val_fg = pressed_ ? WHITE : value_text_color_;
    Color sep = pressed_ ? WHITE : separator_color_;
    if (default_colors) {
        bg = row_style.bg;
        fg = row_style.fg;
        val_fg = pressed_ ? row_style.fg : theme.ColorFor(ThemeToken::TextSecondary);
        sep = row_style.border;
    }

    // Draw background fill
    if (default_colors) {
        DrawStyledRect(fb, width, bounds, row_style);
    } else {
        DrawRect(fb, width, bounds, bg);
    }

    // Calculate layout positions
    int cur_x = x_ + padding_;
    int center_y = y_ + h_ / 2;

    // 1. Draw icon (if set)
    int icon_w = 0;
    if (icon_code_ && icon_font_) {
        int icon_size = icon_font_->line_height;
        int icon_y = center_y - icon_size / 2;
        DrawIcon(fb, width, cur_x, icon_y, icon_code_, icon_font_, fg);
        icon_w = icon_size + Style::kSpacingSM;
        cur_x += icon_w;
    }

    // 2. Draw label text
    int label_w = 0;
    if (label_ && label_font_) {
        label_w = MeasureTextWidth(label_, label_font_);
        int text_y = center_y - label_font_->line_height / 2;
        DrawText(fb, width, cur_x, text_y, label_, label_font_, fg);
        cur_x += label_w + Style::kSpacingSM;
    }

    // 3. Draw chevron (right side, reserve space)
    int chevron_w = 0;
    if (show_chevron_) {
        chevron_w = Style::kSpacingMD + Style::kSpacingSM;  // 8 + 5 + padding
    }

    // 4. Draw value text (right-aligned, before chevron)
    if (value_ && value_font_) {
        int val_w = MeasureTextWidth(value_, value_font_);
        int val_x = x_ + w_ - padding_ - val_w - chevron_w;
        int text_y = center_y - value_font_->line_height / 2;
        DrawText(fb, width, val_x, text_y, value_, value_font_, val_fg);
    }

    // 5. Draw chevron
    if (show_chevron_) {
        int chev_x = x_ + w_ - padding_ - 5;
        DrawChevron(fb, width, chev_x, center_y, fg);
    }

    // 6. Draw separator line at bottom
    if (show_separator_) {
        int sep_y = y_ + h_ - 1;
        if (sep_y >= y_ && sep_y < height) {
            DrawHLine(fb, width, sep_y, x_, x_ + w_ - 1, sep);
        }
    }
}

void ListItem::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
