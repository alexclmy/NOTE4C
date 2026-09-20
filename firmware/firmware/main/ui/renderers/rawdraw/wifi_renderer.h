/**
 * @file wifi_renderer.h
 * @brief Modernized WiFi status page renderer for rawdraw mode
 *
 * Features: visual connection status with large icon, signal strength
 * bars visualization, server status card, connection progress.
 */

#ifndef RAWDRAW_WIFI_RENDERER_H
#define RAWDRAW_WIFI_RENDERER_H

#include "page_renderer.h"
#include <string>

namespace rawdraw {

/**
 * @brief WiFi connection state
 */
enum class WifiState {
    Connecting,   ///< Blinking WiFi icon + progress bar
    Connected,    ///< Solid WiFi icon + SSID + signal bars
    Disconnected, ///< Cross icon + disconnected message
};

/**
 * @brief WiFi status data
 */
struct WifiStatus {
    WifiState state = WifiState::Disconnected;
    std::string ssid;
    int signal_strength = 0;    ///< dBm (typically -30 to -90)
    int progress = 0;           ///< Connection progress (0-100)
    bool server_connected = false;
    std::string server_uri;
};

/**
 * @brief Modernized WiFi status page renderer
 *
 * Design for 400x300 1bpp ePaper:
 * - Large central icon representing connection state
 * - Signal strength bars (5 bars, like phone UI)
 * - Server status card with icon + text
 * - Connection progress with progress bar
 * - Clean card-based layout
 */
class WifiRenderer : public PageRenderer {
public:
    WifiRenderer();
    ~WifiRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    void Update(const WifiStatus& status);
    WifiStatus GetStatus() const { return status_; }

    // Animation control
    void SetBlinking(bool blinking);
    bool IsBlinking() const { return is_blinking_; }

private:
    // Render each state
    void RenderConnecting(uint8_t* fb, int width, int height);
    void RenderConnected(uint8_t* fb, int width, int height);
    void RenderDisconnected(uint8_t* fb, int width, int height);

    // Draw signal strength bars (5-bar visualization)
    void DrawSignalBars(uint8_t* fb, int width, int x, int y,
                         int bar_count, int signal_pct);

    // Draw WiFi icon at given position with size
    void DrawWifiIcon(uint8_t* fb, int width, int x, int y, int size,
                       Color color);

    // Draw server status card
    void DrawServerCard(uint8_t* fb, int width, int x, int y, int w,
                         bool connected, const std::string& uri);

    // Get WiFi icon code for signal level
    const char* GetWifiIcon(int signal_dbm) const;

    // Convert dBm to percentage
    int SignalToPercent(int dbm) const;

    WifiStatus status_;
    bool is_blinking_ = false;
    int blink_frame_ = 0;

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;
    const lv_font_t* large_icon_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_WIFI_RENDERER_H
