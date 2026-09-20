#include "status_bar.h"
#include "esp_log.h"

namespace ui {

static const char* TAG = "StatusBar";

StatusBar::StatusBar(lv_obj_t* parent) {
    ESP_LOGI(TAG, "Creating Status Bar...");
    if (!parent) {
        ESP_LOGE(TAG, "Parent is NULL");
        return;
    }

    // Create container with visible style
    container_ = lv_obj_create(parent);
    if (!container_) {
        ESP_LOGE(TAG, "Failed to create container");
        return;
    }
    lv_obj_set_size(container_, LV_PCT(100), 32); // Height 32
    lv_obj_align(container_, LV_ALIGN_TOP_MID, 0, 0);
    
    // Style: Visible border and white background
    lv_obj_set_style_border_width(container_, 2, 0);
    lv_obj_set_style_border_color(container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container_, lv_color_white(), 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

    // 1. WiFi Status (Left) - Use text instead of symbol
    wifi_label_ = lv_label_create(container_);
    if (wifi_label_) {
        lv_obj_set_style_text_font(wifi_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(wifi_label_, lv_color_black(), 0);
        lv_label_set_text(wifi_label_, "WiFi: --");
        lv_obj_align(wifi_label_, LV_ALIGN_LEFT_MID, 5, 0);
    }

    // 2. Server Status (Left-Center)
    server_label_ = lv_label_create(container_);
    if (server_label_) {
        lv_obj_set_style_text_font(server_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(server_label_, lv_color_black(), 0);
        lv_label_set_text(server_label_, "Srv: --");
        lv_obj_align_to(server_label_, wifi_label_, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    }

    // 3. Page Title (Center)
    title_label_ = lv_label_create(container_);
    if (title_label_) {
        lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title_label_, lv_color_black(), 0);
        lv_label_set_text(title_label_, "Loading...");
        lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, 0);
    }

    // 4. Battery Status (Right) - Use text label instead of canvas
    battery_label_ = lv_label_create(container_);
    if (battery_label_) {
        lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(battery_label_, lv_color_black(), 0);
        lv_label_set_text(battery_label_, "Bat: --");
        lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -5, 0);
    }

    ESP_LOGI(TAG, "Status bar created (Text-only, visible style)");
}

void StatusBar::Update(const StatusBarData& data) {
    if (!container_) return;

    // Safe update WiFi
    if (wifi_label_) {
        if (data.wifi_connected) {
            lv_label_set_text(wifi_label_, "WiFi: OK");
        } else {
            lv_label_set_text(wifi_label_, "WiFi: Off");
        }
    }

    // Safe update Server
    if (server_label_) {
        if (data.server_connected) {
            lv_label_set_text(server_label_, "Srv: Online");
        } else if (data.wifi_connected) {
            lv_label_set_text(server_label_, "Srv: Offline");
        } else {
            lv_label_set_text(server_label_, "Srv: --");
        }
    }

    // Safe update Title
    if (title_label_ && !data.page_title.empty()) {
        lv_label_set_text(title_label_, data.page_title.c_str());
    }

    // Safe update Battery
    if (battery_label_) {
        char buf[20];
        if (data.battery_level >= 0) {
            if (data.battery_charging) {
                snprintf(buf, sizeof(buf), "Bat: %d%%*", data.battery_level);
            } else {
                snprintf(buf, sizeof(buf), "Bat: %d%%", data.battery_level);
            }
        } else {
            snprintf(buf, sizeof(buf), "Bat: --");
        }
        lv_label_set_text(battery_label_, buf);
    }
}

}  // namespace ui
