#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"

#ifdef HAVE_LVGL

#include "lvgl_image.h"
#include <lvgl.h>

class LvglDisplay : public Display {
public:
    LvglDisplay();
    ~LvglDisplay() override;

    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image);
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);

    // 获取 LVGL display 对象
    lv_display_t* GetLvDisplay() override { return display_; }

protected:
    lv_display_t* display_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#else  // !HAVE_LVGL — rawdraw mode, no LVGL runtime

/**
 * LvglDisplay stub: exists for inheritance compatibility but returns nullptr.
 * When CONFIG_USE_EMOTE_MESSAGE_STYLE=y, the firmware uses rawdraw
 * rendering directly from the application layer.
 */
class LvglDisplay : public Display {
public:
    LvglDisplay() = default;
    ~LvglDisplay() override = default;

    // Always returns nullptr — triggers rawdraw fallback in Initialize()
#ifdef HAVE_LVGL
    lv_display_t* GetLvDisplay() override { return nullptr; }
#endif

    // Stub declaration for Lock/Unlock (required by DisplayLockGuard)
    friend class DisplayLockGuard;
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
};

#endif  // HAVE_LVGL

#endif  // LVGL_DISPLAY_H
