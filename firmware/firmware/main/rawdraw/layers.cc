/**
 * @file layers.cc
 * @brief Layer manager implementation
 */

#include "layers.h"
#include <cstring>
#include <algorithm>
#include <esp_heap_caps.h>

namespace rawdraw {

LayerManager::LayerManager(int width, int height, int max_layers)
    : width_(width)
    , height_(height)
    , bytes_per_row_((width + 7) >> 3)
    , layer_size_((size_t)bytes_per_row_ * height)
    , layer_count_(0) {

    max_layers = max_layers < kMaxLayers ? max_layers : kMaxLayers;

    for (int i = 0; i < max_layers; i++) {
        LayerType types[] = { LAYER_BACKGROUND, LAYER_FOREGROUND, LAYER_OVERLAY };
        layers_[i].type = types[i];
        layers_[i].bounds = { 0, 0, width, height };
        layers_[i].buffer = (uint8_t*)heap_caps_malloc(layer_size_, MALLOC_CAP_SPIRAM);
        if (layers_[i].buffer) {
            memset(layers_[i].buffer, 0xFF, layer_size_);  // All white
        }
        layers_[i].dirty = false;
        layers_[i].visible = true;
        layer_count_++;
    }
}

LayerManager::~LayerManager() {
    for (int i = 0; i < layer_count_; i++) {
        if (layers_[i].buffer) {
            heap_caps_free(layers_[i].buffer);
            layers_[i].buffer = nullptr;
        }
    }
}

int LayerManager::FindLayer(LayerType type) const {
    for (int i = 0; i < layer_count_; i++) {
        if (layers_[i].type == type) return i;
    }
    return -1;
}

Layer* LayerManager::GetLayer(LayerType type) {
    int idx = FindLayer(type);
    return (idx >= 0) ? &layers_[idx] : nullptr;
}

uint8_t* LayerManager::GetBuffer(LayerType type) {
    Layer* l = GetLayer(type);
    return (l && l->buffer) ? l->buffer : nullptr;
}

void LayerManager::MarkDirty(LayerType type) {
    Layer* l = GetLayer(type);
    if (l) l->dirty = true;
}

void LayerManager::MarkClean(LayerType type) {
    Layer* l = GetLayer(type);
    if (l) l->dirty = false;
}

void LayerManager::SetVisible(LayerType type, bool visible) {
    Layer* l = GetLayer(type);
    if (l) l->visible = visible;
}

bool LayerManager::HasDirty() const {
    for (int i = 0; i < layer_count_; i++) {
        if (layers_[i].dirty && layers_[i].visible) return true;
    }
    return false;
}

void LayerManager::ClearLayer(LayerType type) {
    uint8_t* buf = GetBuffer(type);
    if (buf) {
        memset(buf, 0xFF, layer_size_);
        MarkDirty(type);
    }
}

void LayerManager::XorMerge(uint8_t* dst, const uint8_t* src, int bytes_per_row, const Rect& r) {
    if (!dst || !src || r.w <= 0 || r.h <= 0) return;

    int src_bpr = bytes_per_row;  // Same layout as dst
    int dst_bpr = bytes_per_row;

    for (int y = 0; y < r.h; y++) {
        uint8_t* dst_row = dst + (r.y + y) * dst_bpr;
        const uint8_t* src_row = src + (r.y + y) * src_bpr;

        // XOR only the columns within the region
        int start_byte = r.x >> 3;
        int end_byte = (r.x + r.w + 7) >> 3;
        if (end_byte > dst_bpr) end_byte = dst_bpr;
        if (start_byte >= end_byte) continue;

        for (int xb = start_byte; xb < end_byte; xb++) {
            dst_row[xb] ^= src_row[xb];
        }
    }
}

void LayerManager::Composite(uint8_t* fb, int fb_width) {
    if (!fb) return;

    // Composite in order: background → foreground → overlay
    for (int i = 0; i < layer_count_; i++) {
        if (layers_[i].dirty && layers_[i].visible && layers_[i].buffer) {
            CompositeLayer(fb, fb_width, layers_[i].type);
        }
    }
}

void LayerManager::CompositeLayer(uint8_t* fb, int fb_width, LayerType type) {
    int idx = FindLayer(type);
    if (idx < 0 || !layers_[idx].buffer || !fb) return;

    // Clamp layer bounds to framebuffer
    Rect r = clamp_rect(layers_[idx].bounds, fb_width, height_);
    r = align_x8(r);
    if (rect_area(r) <= 0) {
        MarkClean(type);
        return;
    }

    // For background layer: clear region white first, then XOR
    // For foreground/overlay: just XOR on top
    if (type == LAYER_BACKGROUND) {
        // Clear region to white
        DrawRect(fb, fb_width, r, WHITE);
    }

    // XOR-merge layer buffer into framebuffer
    XorMerge(fb, layers_[idx].buffer, bytes_per_row_, r);
    MarkClean(type);
}

}  // namespace rawdraw
