/**
 * @file layers.h
 * @brief Background/foreground layer management for rawdraw EPD rendering
 *
 * Separates static UI elements (background layer) from dynamic content
 * (foreground layer). When compositing:
 *   - Background layer: borders, labels, icons (rarely changes)
 *   - Foreground layer: text updates, progress values, clock (frequent)
 *   - Composite: foreground XOR background → framebuffer
 *
 * This reduces unnecessary redraws: only the foreground layer needs
 * re-rendering when dynamic values change.
 */

#ifndef RAWDRAW_LAYERS_H
#define RAWDRAW_LAYERS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rawdraw.h"

namespace rawdraw {

/**
 * @brief Maximum number of layers supported
 */
constexpr int kMaxLayers = 4;

/**
 * @brief Layer type identifier
 */
enum LayerType {
    LAYER_BACKGROUND = 0,  ///< Static UI (borders, labels, icons)
    LAYER_FOREGROUND = 1,  ///< Dynamic content (text, progress, clock)
    LAYER_OVERLAY    = 2,  ///< Temporary floating elements (status bar, dialogs)
};

/**
 * @brief Single layer descriptor
 */
struct Layer {
    LayerType type;          ///< Layer type
    Rect bounds;             ///< Layer region (within framebuffer)
    uint8_t* buffer;         ///< 1bpp pixel data (owned by LayerManager)
    bool dirty;              ///< Needs re-composite
    bool visible;            ///< Should be rendered
};

/**
 * @brief Layer manager for background/foreground separation
 *
 * Each layer is a separate 1bpp buffer. Drawing to a layer does NOT
 * immediately affect the framebuffer. Call Composite() to merge
 * all dirty layers into the framebuffer.
 *
 * Composite strategy:
 * - Background layer: XOR-clear + redraw (full region)
 * - Foreground layer: XOR into framebuffer on top of background
 * - Overlay layer: XOR into framebuffer (highest priority)
 *
 * Usage:
 * 1. Create LayerManager with framebuffer dimensions
 * 2. Get layer buffers via GetLayer(LAYER_BACKGROUND) etc.
 * 3. Draw directly into layer buffers using rawdraw API
 * 4. Call MarkDirty(layer) to flag for compositing
 * 5. Call Composite(fb, width) to merge into main framebuffer
 */
class LayerManager {
public:
    /**
     * @brief Create layer manager
     *
     * @param width Framebuffer width
     * @param height Framebuffer height
     * @param max_layers Max layers to allocate (default: 3)
     */
    LayerManager(int width, int height, int max_layers = 3);

    ~LayerManager();

    /**
     * @brief Get a layer by type
     * @return Layer pointer, or nullptr if not found
     */
    Layer* GetLayer(LayerType type);

    /**
     * @brief Get layer buffer pointer for direct drawing
     * @return 1bpp buffer, or nullptr if layer not found
     */
    uint8_t* GetBuffer(LayerType type);

    /**
     * @brief Mark layer as dirty (needs compositing)
     */
    void MarkDirty(LayerType type);

    /**
     * @brief Mark layer as clean (after compositing)
     */
    void MarkClean(LayerType type);

    /**
     * @brief Set layer visibility
     */
    void SetVisible(LayerType type, bool visible);

    /**
     * @brief Check if any layer is dirty
     */
    bool HasDirty() const;

    /**
     * @brief Clear a layer to white
     */
    void ClearLayer(LayerType type);

    /**
     * @brief Composite all dirty layers into the main framebuffer
     *
     * Order: background → foreground → overlay.
     * Each dirty layer's region is XOR-merged into the framebuffer.
     *
     * @param fb Main framebuffer pointer
     * @param fb_width Framebuffer width in pixels
     */
    void Composite(uint8_t* fb, int fb_width);

    /**
     * @brief Composite only a specific layer
     */
    void CompositeLayer(uint8_t* fb, int fb_width, LayerType type);

    /**
     * @brief Get bytes per row in layer buffers
     */
    int bytes_per_row() const { return bytes_per_row_; }

    /**
     * @brief Get layer buffer size in bytes
     */
    size_t layer_size() const { return layer_size_; }

    /**
     * @brief Get framebuffer dimensions
     */
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_;
    int height_;
    int bytes_per_row_;
    size_t layer_size_;

    Layer layers_[kMaxLayers];
    int layer_count_;

    /**
     * @brief Find layer index by type
     */
    int FindLayer(LayerType type) const;

    /**
     * @brief XOR-merge source buffer region into destination buffer
     */
    static void XorMerge(uint8_t* dst, const uint8_t* src, int bytes_per_row, const Rect& r);
};

}  // namespace rawdraw

#endif  // RAWDRAW_LAYERS_H
