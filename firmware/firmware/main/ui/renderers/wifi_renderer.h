#ifndef WIFI_RENDERER_H
#define WIFI_RENDERER_H

#include <lvgl.h>
#include <string>

namespace ui {

// WiFi 状态可视化 (Spec §5)
// 三种状态：连接中、已连接、断开

enum class WifiState {
    Connecting,   // 连接中：WiFi icon 闪烁 + "连接中..." + 进度条
    Connected,    // 已连接：实心 WiFi icon + SSID + 信号强度
    Disconnected, // 断开：叉号 WiFi icon + "已断开" + "按 BOOT 重连"
};

struct WifiStatus {
    WifiState state = WifiState::Disconnected;
    std::string ssid;
    int signal_strength = 0;   // dBm (通常 -30 到 -90)
    int progress = 0;          // 连接进度 (0-100)
    bool server_connected = false;
    std::string server_uri;
};

class WifiRenderer {
public:
    WifiRenderer();
    ~WifiRenderer();

    // 创建 WiFi 状态面板
    void Create(lv_obj_t* parent, int x, int y, int w, int h);

    // 更新显示
    void Update(const WifiStatus& status);

    // 获取根对象
    lv_obj_t* root() const { return panel_; }

    // 显示/隐藏
    void Show();
    void Hide();

    // 停止闪烁动画
    void StopBlinking();

private:
    void RenderConnecting(const WifiStatus& status);
    void RenderConnected(const WifiStatus& status);
    void RenderDisconnected(const WifiStatus& status);

    // 获取 WiFi 信号强度 icon
    const char* GetWifiIcon(int signal_dbm);

    // 将 dBm 转为百分比
    int SignalToPercent(int dbm) const;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* ssid_label_ = nullptr;
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* server_label_ = nullptr;

    WifiState current_state_ = WifiState::Disconnected;
    bool is_blinking_ = false;
};

}  // namespace ui

#endif  // WIFI_RENDERER_H
