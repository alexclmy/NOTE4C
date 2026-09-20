#pragma once

#include <lvgl.h>
#include <string>

namespace ui {

struct StatusBarData {
    std::string page_title;
    bool wifi_connected = false;
    bool server_connected = false;
    int battery_level = -1;
    bool battery_charging = false;
};

class StatusBar {
public:
    static constexpr int kHeight = 32;
    static constexpr int GetHeight() { return kHeight; }

    StatusBar(lv_obj_t* parent);
    void Update(const StatusBarData& data);

private:
    lv_obj_t* container_ = nullptr;
    lv_obj_t* wifi_label_ = nullptr;
    lv_obj_t* server_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr; // Replaced canvas with label
};

}  // namespace ui
