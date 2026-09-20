#include "settings_renderer.h"
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

static constexpr char kTag[] = "SettingsRenderer";

SettingsRenderer::SettingsRenderer() = default;
SettingsRenderer::~SettingsRenderer() {
    // 释放回调内存
    for (auto* cb : callbacks_) {
        delete cb;
    }
    callbacks_.clear();
}

void SettingsRenderer::Create(lv_obj_t* parent) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "Create: Failed to acquire LVGL lock");
        return;
    }

    // 使用 flex 容器（垂直布局）
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(container_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 4, 0);
    lv_obj_set_style_pad_row(container_, 4, 0);

    // 垂直 flex 布局
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 启用滚动
    lv_obj_set_scroll_dir(container_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x888888), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(container_, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(container_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(container_, 2, LV_PART_SCROLLBAR);

    lvgl_port_unlock();

    ESP_LOGI(kTag, "Settings renderer created with modern UI");
}

void SettingsRenderer::SetItems(const std::vector<SettingsItemDef>& items) {
    if (!lvgl_port_lock(0)) return;

    // 清除旧项和回调
    for (auto* cb : callbacks_) {
        delete cb;
    }
    callbacks_.clear();
    for (lv_obj_t* btn : item_buttons_) {
        lv_obj_delete(btn);
    }
    item_buttons_.clear();
    item_data_ = items;

    // 创建新列表项
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        lv_obj_t* btn = CreateItem(container_, items[i], i);
        item_buttons_.push_back(btn);
    }

    lvgl_port_unlock();

    ESP_LOGI(kTag, "Settings items updated: %zu", items.size());
}

lv_obj_t* SettingsRenderer::CreateItem(lv_obj_t* parent, const SettingsItemDef& def, int index) {
    // 创建按钮容器
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_set_style_pad_column(btn, 4, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    // 水平 flex 布局：icon | label | value
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Icon 标签
    lv_obj_t* icon = lv_label_create(btn);
    const char* icon_text = def.icon ? def.icon : FONT_ZECTRIX_ICON_CHECKBOX;
    lv_label_set_text(icon, icon_text);
    lv_obj_set_style_text_font(icon, &font_zectrix_16_1, 0);
    lv_obj_set_style_text_color(icon, lv_color_black(), 0);
    lv_obj_set_style_pad_right(icon, 6, 0);

    // 如果是 checkbox 类型，使用 checkbox 图标
    if (def.type == SettingsItemType::Checkbox) {
        lv_label_set_text(icon, GetCheckboxIcon(def.checked));
    }

    // 文本标签
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, def.label.c_str());
    lv_obj_set_style_text_font(label, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_flex_grow(label, 1);  // 占据剩余空间

    // 值标签（右侧）
    if (!def.value.empty()) {
        lv_obj_t* value_label = lv_label_create(btn);
        lv_label_set_text(value_label, def.value.c_str());
        lv_obj_set_style_text_font(value_label, &SourceHanSansSC_Regular_slim, 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0x666666), 0);
        lv_obj_set_style_pad_right(value_label, 4, 0);
    }

    // 箭头（仅 Normal 和 Action 类型）
    if (def.type != SettingsItemType::Checkbox) {
        lv_obj_t* arrow = lv_label_create(btn);
        lv_label_set_text(arrow, ">");
        lv_obj_set_style_text_font(arrow, &SourceHanSansSC_Regular_slim, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x999999), 0);
    }

    // 设置点击回调
    if (def.on_click) {
        auto* callback_ptr = new std::function<void()>(def.on_click);
        callbacks_.push_back(callback_ptr);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* callback = static_cast<std::function<void()>*>(lv_event_get_user_data(e));
            if (callback && *callback) {
                (*callback)();
            }
        }, LV_EVENT_CLICKED, callback_ptr);
    }

    return btn;
}

void SettingsRenderer::UpdateItem(int index, const std::string& value) {
    if (index < 0 || index >= static_cast<int>(item_buttons_.size())) return;

    if (!lvgl_port_lock(0)) return;

    item_data_[index].value = value;

    lv_obj_t* btn = item_buttons_[index];
    // 查找值标签（第三个子控件：icon=0, label=1, value=2）
    lv_obj_t* value_label = lv_obj_get_child(btn, 2);
    if (value_label && lv_obj_check_type(value_label, &lv_label_class)) {
        lv_label_set_text(value_label, value.c_str());
    } else if (!value.empty()) {
        // 创建新的值标签
        value_label = lv_label_create(btn);
        lv_label_set_text(value_label, value.c_str());
        lv_obj_set_style_text_font(value_label, &SourceHanSansSC_Regular_slim, 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0x666666), 0);
        lv_obj_set_style_pad_right(value_label, 4, 0);
    }

    lvgl_port_unlock();
}

void SettingsRenderer::UpdateChecked(int index, bool checked) {
    if (index < 0 || index >= static_cast<int>(item_buttons_.size())) return;

    if (!lvgl_port_lock(0)) return;

    item_data_[index].checked = checked;

    lv_obj_t* btn = item_buttons_[index];
    // 更新 checkbox 图标（第一个子控件）
    lv_obj_t* icon = lv_obj_get_child(btn, 0);
    if (icon && lv_obj_check_type(icon, &lv_label_class)) {
        lv_label_set_text(icon, GetCheckboxIcon(checked));
    }

    lvgl_port_unlock();
}

const char* SettingsRenderer::GetCheckboxIcon(bool checked) const {
    return checked ? FONT_ZECTRIX_ICON_CHECKBOX_OK : FONT_ZECTRIX_ICON_CHECKBOX;
}

void SettingsRenderer::Show() {
    if (!container_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void SettingsRenderer::Hide() {
    if (!container_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

}  // namespace ui
