/**
 * @file photo_gallery.h
 * @brief Photo gallery page renderer for rawdraw mode
 *
 * Two display modes:
 * - Memory card mode: left narrative + right image
 * - Full-screen mode: Single photo at full resolution
 *
 * Navigation:
 * - UP/DOWN: Previous/next memory
 * - BOOT: Enter full-screen / return to memory card
 */

#ifndef RAWDRAW_PHOTO_GALLERY_H
#define RAWDRAW_PHOTO_GALLERY_H

#include "common/photo_storage.h"
#include "page_renderer.h"
#include "rawdraw/style.h"
#include <vector>
#include <string>

namespace rawdraw {

/**
 * @brief Photo gallery page renderer
 */
class PhotoGalleryRenderer : public PageRenderer {
public:
    PhotoGalleryRenderer();
    ~PhotoGalleryRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Display modes
    enum DisplayMode {
        kMemoryCardMode,  // Left text + right image
        kFullscreenMode,  // Single photo
    };

    // Data interface
    void RefreshPhotoList();  // Reload from photo_storage
    int GetPhotoCount() const { return static_cast<int>(photo_ids_.size()); }
    int GetSelectedIndex() const { return selected_index_; }
    void SetSelectedIndex(int index);
    bool SetSelectedById(const char* id);
    void EnterFullscreenMode();
    /// Leave the fullscreen photo for the memory-card grid. This is what the
    /// universal Back gesture pops; the BOOT click toggle keeps working too.
    void ExitFullscreenMode();
    /// Dismiss the delete confirmation without deleting anything. Back and
    /// Home use this so a modal can never be left on screen by a page change.
    void CloseDeleteDialog();
    bool SelectNext(bool wrap);
    bool IsFullscreenMode() const { return mode_ == kFullscreenMode; }
    bool IsDeleteDialogOpen() const { return showing_delete_dialog_; }
    bool IsCurrentPhotoBwry2bpp() const;
    const uint8_t* GetCurrentPhotoData() const { return current_photo_data_; }
    uint32_t GetCurrentPhotoSize() const { return current_photo_size_; }
    int GetCurrentPhotoWidth() const { return current_photo_width_; }
    int GetCurrentPhotoHeight() const { return current_photo_height_; }

private:
    struct PhotoEntry {
        char id[16];
        char title[64];
        char date[PHOTO_DATE_LEN];
        char location[PHOTO_LOCATION_LEN];
        char body[PHOTO_BODY_LEN];
        uint16_t width;
        uint16_t height;
        uint32_t file_size;
    };

    void RenderMemoryCardMode(uint8_t* fb, int width, int height);
    void RenderPhotoInRect(uint8_t* fb, int fb_width, const PhotoEntry& entry,
                           int x, int y, int w, int h, bool invert = false);
    void RenderDeleteDialog(uint8_t* fb, int width, int height);

    // Full-screen mode rendering
    void RenderFullscreenMode(uint8_t* fb, int width, int height);

    // Photo data loading
    void LoadPhotoData(int index);
    void DeleteSelectedPhoto();

    // Helpers
    void ClampSelection();

    // State
    DisplayMode mode_ = kMemoryCardMode;
    int selected_index_ = 0;
    bool showing_delete_dialog_ = false;
    int delete_dialog_selected_ = 1;  // 0=delete, 1=cancel

    std::vector<PhotoEntry> photo_ids_;

    // Cached photo data for current selection (1bpp or BWRY 2bpp)
    uint8_t* current_photo_data_ = nullptr;
    uint32_t current_photo_size_ = 0;
    int current_photo_width_ = 400;
    int current_photo_height_ = 300;

    // Fonts
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PHOTO_GALLERY_H
