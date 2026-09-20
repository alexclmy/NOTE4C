/**
 * @file scrollview.cc
 * @brief Scrollable container implementation
 */

#include "scrollview.h"
#include "rawdraw/theme.h"
#include <algorithm>

namespace rawdraw {

ScrollView::ScrollView(int x, int y, int w, int h, int content_height)
    : bounds_{x, y, w, h}
    , content_height_(content_height)
    , scroll_offset_(0)
    , scrollbar_width_(4)
    , scrollbar_enabled_(true) {
}

ScrollView::~ScrollView() {}

void ScrollView::SetBounds(const Rect& r) { bounds_ = r; }
void ScrollView::SetContentHeight(int height) {
    content_height_ = height;
    // Clamp scroll offset to valid range
    scroll_offset_ = std::min(scroll_offset_, GetMaxScrollOffset());
}
void ScrollView::SetScrollbarWidth(int width) { scrollbar_width_ = width; }
void ScrollView::SetScrollbarEnabled(bool enabled) { scrollbar_enabled_ = enabled; }

void ScrollView::SetScrollOffset(int offset) {
    scroll_offset_ = std::max(0, std::min(offset, GetMaxScrollOffset()));
}

int ScrollView::GetScrollOffset() const { return scroll_offset_; }

void ScrollView::ScrollToEnd() {
    scroll_offset_ = GetMaxScrollOffset();
}

void ScrollView::ScrollBy(int delta) {
    SetScrollOffset(scroll_offset_ + delta);
}

int ScrollView::GetMaxScrollOffset() const {
    return std::max(0, content_height_ - bounds_.h);
}

bool ScrollView::CanScrollUp() const {
    return scroll_offset_ > 0;
}

bool ScrollView::CanScrollDown() const {
    return scroll_offset_ < GetMaxScrollOffset();
}

Rect ScrollView::GetVisibleContentRect() const {
    return {bounds_.x, scroll_offset_, bounds_.w - scrollbar_width_, bounds_.h};
}

Rect ScrollView::GetBounds() const { return bounds_; }

void ScrollView::DrawScrollbar(uint8_t* fb, int width) {
    if (!scrollbar_enabled_ || content_height_ <= bounds_.h) return;

    // Calculate scrollbar position and size
    int sb_x = bounds_.x + bounds_.w - scrollbar_width_;

    // Scrollbar height proportional to visible content ratio
    float visible_ratio = (float)bounds_.h / content_height_;
    int sb_height = std::max(8, (int)(bounds_.h * visible_ratio));

    // Scrollbar position based on scroll offset
    float scroll_ratio = (float)scroll_offset_ / GetMaxScrollOffset();
    int sb_y = bounds_.y + (int)((bounds_.h - sb_height) * scroll_ratio);

    const auto& theme = ThemeManager::Get();
    const PaintStyle track = theme.Style(ThemeToken::BackgroundSecondary);
    const Color thumb = theme.ColorFor(ThemeToken::Selected);

    // Draw scrollbar background (light)
    DrawStyledRect(fb, width, {sb_x, bounds_.y, scrollbar_width_, bounds_.h}, track);

    // Draw scrollbar indicator (dark)
    DrawRect(fb, width, {sb_x, sb_y, scrollbar_width_, sb_height}, thumb);
}

void ScrollView::Draw(uint8_t* fb, int width, ContentDrawCallback draw_cb) {
    if (!fb || rect_area(bounds_) <= 0) return;

    // Draw content at scroll offset
    if (draw_cb) {
        Rect visible = GetVisibleContentRect();
        Rect clip = bounds_;
        draw_cb(fb, width, visible, clip);
    }

    // Draw scrollbar
    DrawScrollbar(fb, width);
}

void ScrollView::Draw(Framebuffer* fb, ContentDrawCallback draw_cb) {
    if (!fb) return;
    int width = fb->width();
    fb->SafeDraw([this, width, draw_cb](uint8_t* buffer) {
        this->Draw(buffer, width, draw_cb);
    });
}

}  // namespace rawdraw
