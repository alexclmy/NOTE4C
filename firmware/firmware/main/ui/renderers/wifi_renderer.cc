#include "wifi_renderer.h"
#include "renderers/common/ui_components.h"
#include "components/78__xiaozhi-fonts/include/font_zectrix.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

// Icon font
extern "C" {
    extern const lv_font_t font_zectrix_16_1;
    extern const lv_font_t SourceHanSansSC_Regular_slim;
}

namespace ui {

static constexpr char kTag[] = "WifiRenderer";

WifiRenderer::WifiRenderer() = default;
WifiRenderer::~WifiRenderer() = default;

void WifiRenderer::Create(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "Create: Failed to acquire LVGL lock");
        return;
    }

    // 主面板
    panel_ = lv_obj_create(parent);
    lv_obj_set_pos(panel_, x, y);
    lv_obj_set_size(panel_, w, h);
    lv_obj_set_style_radius(panel_, 8, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel_, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel_, 1, 0);
    lv_obj_set_style_pad_all(panel_, 12, 0);

    // WiFi 图标（左侧）
    wifi_icon_ = lv_label_create(panel_);
    lv_label_set_text(wifi_icon_, "");
    lv_obj_set_style_text_font(wifi_icon_, &font_zectrix_16_1, 0);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_black(), 0);
    lv_obj_align(wifi_icon_, LV_ALIGN_LEFT_MID, 0, 0);

    // 状态文本（右上）
    status_label_ = lv_label_create(panel_);
    lv_label_set_text(status_label_, "");
    lv_obj_set_style_text_font(status_label_, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 20, 4);

    // SSID 标签（状态下方）
    ssid_label_ = lv_label_create(panel_);
    lv_label_set_text(ssid_label_, "");
    lv_obj_set_style_text_font(ssid_label_, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_set_style_text_color(ssid_label_, lv_color_hex(0x666666), 0);
    lv_obj_align(ssid_label_, LV_ALIGN_LEFT_MID, 24, 8);

    // 进度条（底部）
    progress_bar_ = lv_bar_create(panel_);
    lv_obj_set_size(progress_bar_, LV_PCT(80), 8);
    lv_obj_align(progress_bar_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(progress_bar_, 0, 100);
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(progress_bar_, 4, 0);
    lv_obj_set_style_radius(progress_bar_, 4, LV_PART_INDICATOR);

    // 提示文本
    hint_label_ = lv_label_create(panel_);
    lv_label_set_text(hint_label_, "");
    lv_obj_set_style_text_font(hint_label_, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_set_style_text_color(hint_label_, lv_color_hex(0x999999), 0);
    lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -16);

    // 服务状态
    server_label_ = lv_label_create(panel_);
    lv_label_set_text(server_label_, "");
    lv_obj_set_style_text_font(server_label_, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_set_style_text_color(server_label_, lv_color_hex(0x666666), 0);
    lv_obj_align(server_label_, LV_ALIGN_BOTTOM_LEFT, 4, -30);

    // 默认显示断开状态
    RenderDisconnected({});

    lvgl_port_unlock();

    ESP_LOGI(kTag, "WiFi renderer created");
}

void WifiRenderer::Update(const WifiStatus& status) {
    if (!panel_ || !lvgl_port_lock(0)) return;

    if (status.state != current_state_) {
        current_state_ = status.state;
        // 状态切换时重新渲染
        switch (status.state) {
            case WifiState::Connecting:
                RenderConnecting(status);
                break;
            case WifiState::Connected:
                RenderConnected(status);
                break;
            case WifiState::Disconnected:
                RenderDisconnected(status);
                break;
        }
    } else {
        // 同状态下更新数据
        switch (status.state) {
            case WifiState::Connecting:
                // 更新进度
                if (progress_bar_) {
                    lv_bar_set_value(progress_bar_, status.progress, LV_ANIM_OFF);
                }
                break;
            case WifiState::Connected:
                // 更新信号强度图标
                if (wifi_icon_) {
                    lv_label_set_text(wifi_icon_, GetWifiIcon(status.signal_strength));
                }
                if (ssid_label_) {
                    lv_label_set_text(ssid_label_, status.ssid.c_str());
                }
                // 更新服务状态
                if (server_label_) {
                    if (status.server_connected) {
                        lv_label_set_text(server_label_, "服务: 在线");
                        lv_obj_set_style_text_color(server_label_, lv_color_hex(0x006600), 0);
                    } else if (!status.server_uri.empty()) {
                        std::string uri = "服务: " + status.server_uri;
                        lv_label_set_text(server_label_, uri.c_str());
                        lv_obj_set_style_text_color(server_label_, lv_color_hex(0x996600), 0);
                    } else {
                        lv_label_set_text(server_label_, "服务: 离线");
                        lv_obj_set_style_text_color(server_label_, lv_color_hex(0x990000), 0);
                    }
                }
                break;
            case WifiState::Disconnected:
                // 无需额外更新
                break;
        }
    }

    lvgl_port_unlock();
}

void WifiRenderer::RenderConnecting(const WifiStatus& status) {
    if (!lvgl_port_lock(0)) return;

    // WiFi icon 闪烁
    if (wifi_icon_) {
        lv_label_set_text(wifi_icon_, FONT_ZECTRIX_WIFI_FAIR);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0x996600), 0);
    }

    // 状态文本
    if (status_label_) {
        lv_label_set_text(status_label_, "连接中...");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0x996600), 0);
    }

    // 隐藏 SSID
    if (ssid_label_) {
        lv_obj_add_flag(ssid_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示进度条
    if (progress_bar_) {
        lv_obj_remove_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progress_bar_, status.progress, LV_ANIM_OFF);
    }

    // 隐藏提示
    if (hint_label_) {
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 服务状态
    if (server_label_) {
        lv_label_set_text(server_label_, "正在发现服务...");
        lv_obj_set_style_text_color(server_label_, lv_color_hex(0x996600), 0);
        lv_obj_remove_flag(server_label_, LV_OBJ_FLAG_HIDDEN);
    }

    is_blinking_ = true;
    lvgl_port_unlock();
}

void WifiRenderer::RenderConnected(const WifiStatus& status) {
    if (!lvgl_port_lock(0)) return;

    // 实心 WiFi icon
    if (wifi_icon_) {
        const char* icon = GetWifiIcon(status.signal_strength);
        lv_label_set_text(wifi_icon_, icon);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_black(), 0);
    }

    // 状态文本
    if (status_label_) {
        lv_label_set_text(status_label_, "已连接");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0x006600), 0);
    }

    // 显示 SSID
    if (ssid_label_) {
        lv_obj_remove_flag(ssid_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ssid_label_, status.ssid.c_str());
    }

    // 隐藏进度条
    if (progress_bar_) {
        lv_obj_add_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    // 隐藏提示
    if (hint_label_) {
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 服务状态
    if (server_label_) {
        lv_obj_remove_flag(server_label_, LV_OBJ_FLAG_HIDDEN);
        if (status.server_connected) {
            lv_label_set_text(server_label_, "服务: 在线");
            lv_obj_set_style_text_color(server_label_, lv_color_hex(0x006600), 0);
        } else if (!status.server_uri.empty()) {
            std::string uri = "服务: " + status.server_uri;
            lv_label_set_text(server_label_, uri.c_str());
            lv_obj_set_style_text_color(server_label_, lv_color_hex(0x996600), 0);
        } else {
            lv_label_set_text(server_label_, "服务: 离线");
            lv_obj_set_style_text_color(server_label_, lv_color_hex(0x990000), 0);
        }
    }

    is_blinking_ = false;
    lvgl_port_unlock();
}

void WifiRenderer::RenderDisconnected(const WifiStatus& status) {
    if (!lvgl_port_lock(0)) return;

    // 叉号 WiFi icon
    if (wifi_icon_) {
        lv_label_set_text(wifi_icon_, FONT_ZECTRIX_WIFI_SLASH);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0x990000), 0);
    }

    // 状态文本
    if (status_label_) {
        lv_label_set_text(status_label_, "已断开");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0x990000), 0);
    }

    // 隐藏 SSID
    if (ssid_label_) {
        lv_obj_add_flag(ssid_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 隐藏进度条
    if (progress_bar_) {
        lv_obj_add_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示提示
    if (hint_label_) {
        lv_obj_remove_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hint_label_, "按 BOOT 重连");
    }

    // 服务状态
    if (server_label_) {
        lv_label_set_text(server_label_, "服务: 未连接");
        lv_obj_set_style_text_color(server_label_, lv_color_hex(0x990000), 0);
        lv_obj_remove_flag(server_label_, LV_OBJ_FLAG_HIDDEN);
    }

    is_blinking_ = false;
    lvgl_port_unlock();
}

const char* WifiRenderer::GetWifiIcon(int signal_dbm) {
    // 将信号强度转为图标
    if (signal_dbm >= -50) {
        return FONT_ZECTRIX_WIFI_FULL;
    } else if (signal_dbm >= -65) {
        return FONT_ZECTRIX_WIFI_FAIR;
    } else if (signal_dbm >= -80) {
        return FONT_ZECTRIX_WIFI_WEAK;
    } else {
        return FONT_ZECTRIX_WIFI_SLASH;
    }
}

int WifiRenderer::SignalToPercent(int dbm) const {
    // dBm 范围: -30 (最强) 到 -90 (最弱)
    if (dbm >= -30) return 100;
    if (dbm <= -90) return 0;
    return static_cast<int>((dbm + 90) * 100.0f / 60.0f);
}

void WifiRenderer::Show() {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void WifiRenderer::Hide() {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void WifiRenderer::StopBlinking() {
    is_blinking_ = false;
}

}  // namespace ui
