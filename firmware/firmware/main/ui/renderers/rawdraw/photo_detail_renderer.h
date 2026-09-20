/**
 * @file photo_detail_renderer.h
 * @brief Photo detail page renderer with metadata modal
 */

#ifndef RAWDRAW_PHOTO_DETAIL_RENDERER_H
#define RAWDRAW_PHOTO_DETAIL_RENDERER_H

#include "common/photo_storage.h"
#include "page_renderer.h"
#include <vector>

namespace rawdraw {

class PhotoDetailRenderer : public PageRenderer {
public:
    PhotoDetailRenderer();
    ~PhotoDetailRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void RefreshPhotoList();
    void SetSelection(int index);
    bool IsMetadataOpen() const { return metadata_open_; }
    bool IsCurrentPhotoBwry2bpp() const;
    const uint8_t* GetCurrentPhotoData() const { return current_photo_data_; }
    uint32_t GetCurrentPhotoSize() const { return current_photo_size_; }
    int GetCurrentPhotoWidth() const { return current_photo_width_; }
    int GetCurrentPhotoHeight() const { return current_photo_height_; }

private:
    void LoadPhotoData(int index);
    void ClampSelection();
    void DrawMetadataModal(uint8_t* fb, int width, int height);

    std::vector<PhotoInfo> photos_;
    int selected_index_ = 0;
    bool metadata_open_ = false;

    uint8_t* current_photo_data_ = nullptr;
    uint32_t current_photo_size_ = 0;
    int current_photo_width_ = 400;
    int current_photo_height_ = 300;

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PHOTO_DETAIL_RENDERER_H
