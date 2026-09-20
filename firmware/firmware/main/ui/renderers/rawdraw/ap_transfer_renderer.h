/**
 * @file ap_transfer_renderer.h
 * @brief AP Transfer mode renderer for WiFi image upload
 *
 * Display when user enters AP transfer mode from photo gallery:
 * - Shows WiFi AP connection instructions
 * - Displays 192.168.4.1 URL
 * - Shows status during image upload
 */

#ifndef RAWDRAW_AP_TRANSFER_RENDERER_H
#define RAWDRAW_AP_TRANSFER_RENDERER_H

#include "common/nav_model.h"
#include "page_renderer.h"
#include "rawdraw/style.h"
#include <string>

namespace rawdraw {

/**
 * @brief AP Transfer mode renderer
 * 
 * Shows instructions for WiFi AP image upload:
 * 1. "WiFi 已开启"
 * 2. "连接 InkScreen-AP (密码 12345678)"
 * 3. "浏览器访问 192.168.4.1"
 * 4. Upload progress/status
 */
class ApTransferRenderer : public PageRenderer {
public:
    ApTransferRenderer();
    ~ApTransferRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Status updates
    enum TransferState {
        kWaitingForConnection,  // AP started, waiting for client
        kClientConnected,       // Client connected to AP
        kUploading,             // Image being uploaded
        kProcessing,            // Floyd-Steinberg dithering
        kComplete,              // Upload complete, image saved
        kError,                 // Error occurred
    };

    void SetState(TransferState state, const std::string& message = "");
    TransferState GetState() const { return state_; }
    void UseDefaultTransferInstructions();
    void SetInstructionContent(const std::string& title,
                               const std::string& ssid,
                               const std::string& password,
                               const std::string& url,
                               const std::string& hint,
                               const std::string& exit_hint);

    // Exit callback
    void SetExitCallback(std::function<void()> callback);

private:
    void RenderInstructions(uint8_t* fb, int width, int height);
    void RenderStatus(uint8_t* fb, int width, int height);

    // State
    TransferState state_ = kWaitingForConnection;
    std::string status_message_;
    std::string title_text_ = "Wi-Fi image transfer";
    std::string ssid_text_ = "InkScreen-AP";
    std::string password_text_ = "12345678";
    std::string url_text_ = "http://192.168.4.1";
    std::string hint_text_;
    // Default seeded from the one shared phrasing (common/nav_model.h). BOOT
    // long is push-to-talk and exits nothing, so no screen may say otherwise.
    std::string exit_hint_text_ = nav::kExitHint;
    std::function<void()> exit_callback_;

    // Fonts
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;

    int width_ = 400;
    int height_ = 300;
};

}  // namespace rawdraw

#endif  // RAWDRAW_AP_TRANSFER_RENDERER_H
