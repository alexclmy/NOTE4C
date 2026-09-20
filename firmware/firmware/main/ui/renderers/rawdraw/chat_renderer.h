/**
 * @file chat_renderer.h
 * @brief Modernized chat page renderer for rawdraw mode
 *
 * Features: flat text messages with role prefixes, scroll indicator,
 * scroll indicator, proper spacing using Style constants.
 */

#ifndef RAWDRAW_CHAT_RENDERER_H
#define RAWDRAW_CHAT_RENDERER_H

#include "page_renderer.h"
#include <vector>
#include <string>

namespace rawdraw {

/**
 * @brief Chat message role
 */
enum class ChatRole {
    User,   ///< User message (right side, black fill, white text)
    AI,     ///< AI response (left side, white fill, black border)
    System  ///< System notification (centered, minimal)
};

/**
 * @brief Chat message data (flat text with layout info)
 */
struct ChatMessage {
    std::string text;
    ChatRole role;
    int y_pos = 0;     // Computed layout position
    int block_h = 0;   // Computed block height including gap
};

/**
 * @brief Modernized chat page renderer
 *
 * Displays conversation as flat text messages:
 * - User: "> prefix, right-aligned"
 * - AI: "[AI] prefix, left-aligned"
 * - System: centered
 * Includes streaming status indicator and scroll bar.
 * 
 * Volume adjustment: BOOT click shows volume dialog, UP/DN adjusts by 10 steps.
 */
class ChatRenderer : public PageRenderer {
public:
    ChatRenderer();
    ~ChatRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Streaming support
    bool AppendText(const char* chunk) override;
    void BeginStream() override;
    void EndStream() override;

    // Data interface
    void Clear();
    void AddMessage(const std::string& text, ChatRole role);
    void ShowStatus(const std::string& status, ChatRole role);
    void HideStatus();
    void SetListening(bool listening);
    void SetBottomStatus(const std::string& status);
    int GetMessageCount() const { return static_cast<int>(messages_.size()); }

    // Volume dialog support
    void ShowVolumeDialog(int volume);
    void SetVolumeDialogHandler(std::function<void(int, bool)> handler) {
        volume_dialog_handler_ = std::move(handler);
    }
    bool IsVolumeDialogShowing() const { return showing_volume_dialog_; }
    void HideVolumeDialog() { showing_volume_dialog_ = false; needs_full_refresh_ = true; }

private:
    // Layout uses Style:: constants

    // Draw scroll indicator
    void DrawScrollIndicator(uint8_t* fb, int width, int content_y, int content_height);

    // Draw streaming status indicator
    void DrawStreamingIndicator(uint8_t* fb, int width, int content_bottom);

    // Draw bottom status bar
    void DrawBottomBar(uint8_t* fb, int width, int height);

    // Layout and position all messages
    void LayoutMessages();

    // Volume dialog rendering and update
    void RenderVolumeDialog(uint8_t* fb, int width, int height);
    void UpdateVolumeValue(int delta, bool commit);

    std::vector<ChatMessage> messages_;

    bool is_streaming_ = false;
    bool is_listening_ = false;
    bool follow_latest_ = true;
    int scroll_offset_ = 0;
    int max_scroll_offset_ = 0;

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;

    // Animation frame for streaming dots
    int stream_frame_ = 0;

    // Bottom status bar for chat page (15-20px)
    static constexpr int kBottomBarH = 18;
    std::string bottom_status_text_;

    // Volume dialog state
    bool showing_volume_dialog_ = false;
    int volume_dialog_value_ = 70;
    std::function<void(int, bool)> volume_dialog_handler_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_CHAT_RENDERER_H
