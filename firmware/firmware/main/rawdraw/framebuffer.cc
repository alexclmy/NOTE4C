/**
 * @file framebuffer.cc
 * @brief Framebuffer manager implementation
 */

#include "framebuffer.h"
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace rawdraw {

Framebuffer::Framebuffer(uint8_t* buffer, int width, int height, SemaphoreHandle_t mutex)
    : buffer_(buffer)
    , width_(width)
    , height_(height)
    , mutex_(mutex)
    , dirty_{0, 0, 0, 0}
    , pending_(false)
    , next_kick_ms_(0) {
}

Framebuffer::~Framebuffer() {
    // Buffer is externally owned, don't free
}

void Framebuffer::Clear(Color fill) {
    Lock();
    rawdraw::Clear(buffer_, width_, height_, fill);
    Unlock();
}

void Framebuffer::InvalidateRect(int x, int y, int w, int h) {
    InvalidateRect({x, y, w, h});
}

void Framebuffer::InvalidateRect(const Rect& r) {
    if (rect_area(r) <= 0) return;

    Lock();
    InvalidateRect_NoLock(r);
    Unlock();
}

void Framebuffer::InvalidateRect_NoLock(const Rect& r) {
    if (rect_area(r) <= 0) return;

    // Clamp and align to 8-byte boundary (EPD requirement)
    Rect aligned = align_x8(clamp_rect(r, width_, height_));
    if (rect_area(aligned) > 0) {
        dirty_ = rect_union(dirty_, aligned);
        pending_ = true;
    }
}

void Framebuffer::InvalidateAll() {
    Lock();
    dirty_ = align_x8({0, 0, width_, height_});
    pending_ = true;
    Unlock();
}

Rect Framebuffer::GetDirtyRect() const {
    return dirty_;
}

bool Framebuffer::HasDirtyRegions() const {
    return pending_ && rect_area(dirty_) > 0;
}

void Framebuffer::ClearDirty() {
    Lock();
    dirty_ = {0, 0, 0, 0};
    pending_ = false;
    Unlock();
}

void Framebuffer::RequestRefresh(bool urgent) {
    Lock();
    if (pending_ && rect_area(dirty_) > 0) {
        Rect r = dirty_;
        // Note: dirty is cleared by refresh task after EPD update
        Unlock();

        if (refresh_cb_) {
            refresh_cb_(r, urgent);
        }
    } else {
        Unlock();
    }
}

void Framebuffer::SetRefreshCallback(RefreshCallback callback) {
    Lock();
    refresh_cb_ = std::move(callback);
    Unlock();
}

void Framebuffer::SetNextKickMs(uint32_t kick_ms) {
    Lock();
    next_kick_ms_ = kick_ms;
    Unlock();
}

void Framebuffer::Lock() {
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
}

void Framebuffer::Unlock() {
    if (mutex_) {
        xSemaphoreGive(mutex_);
    }
}

void Framebuffer::DrawRect(const Rect& r, Color color) {
    Lock();
    DrawRect_NoLock(r, color);
    Unlock();
}

void Framebuffer::DrawRect_NoLock(const Rect& r, Color color) {
    rawdraw::DrawRect(buffer_, width_, r, color);
    InvalidateRect_NoLock(r);
}

void Framebuffer::DrawText(int x, int y, const char* text, const lv_font_t* font, Color color) {
    if (!text || !font) return;

    Lock();
    DrawText_NoLock(x, y, text, font, color);
    Unlock();
}

void Framebuffer::DrawText_NoLock(int x, int y, const char* text, const lv_font_t* font, Color color) {
    if (!text || !font) return;

    // Measure text bounds for dirty rect
    Rect bounds = MeasureTextBounds(text, font);
    bounds.x = x;
    bounds.y = y;

    rawdraw::DrawText(buffer_, width_, x, y, text, font, color, height_);
    InvalidateRect_NoLock(bounds);
}

void Framebuffer::DrawRoundRect(const Rect& r, int radius, Color fill, Color border, int border_w) {
    Lock();
    DrawRoundRect_NoLock(r, radius, fill, border, border_w);
    Unlock();
}

void Framebuffer::DrawRoundRect_NoLock(const Rect& r, int radius, Color fill, Color border, int border_w) {
    rawdraw::DrawRoundRect(buffer_, width_, height_, r, radius, fill, border, border_w);
    InvalidateRect_NoLock(r);
}

}  // namespace rawdraw
