#include "ui_components.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

namespace ui {

static constexpr char kTag[] = "UiComponents";

// ============================================================
// Panel 实现
// ============================================================

Panel::Panel() = default;
Panel::~Panel() = default;

void Panel::Create(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "Panel::Create: Failed to acquire LVGL lock");
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
    lv_obj_set_style_pad_all(panel_, 0, 0);

    // 标题栏
    title_bar_ = lv_obj_create(panel_);
    lv_obj_set_size(title_bar_, LV_PCT(100), 24);
    lv_obj_set_pos(title_bar_, 0, 0);
    lv_obj_set_style_radius(title_bar_, 8, 0);
    lv_obj_set_style_bg_color(title_bar_, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(title_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar_, 0, 0);

    // 内容区域
    content_ = lv_obj_create(panel_);
    lv_obj_set_size(content_, LV_PCT(100), LV_PCT(100) - 24);
    lv_obj_set_pos(content_, 0, 24);
    lv_obj_set_style_bg_color(content_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);

    lvgl_port_unlock();
}

void Panel::Create(lv_obj_t* parent, lv_coord_t x_pct, lv_coord_t y_pct,
                   lv_coord_t w_pct, lv_coord_t h_pct) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "Panel::Create: Failed to acquire LVGL lock");
        return;
    }

    panel_ = lv_obj_create(parent);
    lv_obj_set_size(panel_, w_pct, h_pct);
    lv_obj_set_pos(panel_, x_pct, y_pct);
    lv_obj_set_style_radius(panel_, 8, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel_, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel_, 1, 0);
    lv_obj_set_style_pad_all(panel_, 0, 0);

    title_bar_ = lv_obj_create(panel_);
    lv_obj_set_size(title_bar_, LV_PCT(100), 24);
    lv_obj_set_pos(title_bar_, 0, 0);
    lv_obj_set_style_radius(title_bar_, 8, 0);
    lv_obj_set_style_bg_color(title_bar_, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(title_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar_, 0, 0);

    content_ = lv_obj_create(panel_);
    lv_obj_set_size(content_, LV_PCT(100), LV_PCT(100) - 24);
    lv_obj_set_pos(content_, 0, 24);
    lv_obj_set_style_bg_color(content_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);

    lvgl_port_unlock();
}

void Panel::SetTitle(const char* title) {
    if (!title_bar_) return;

    if (!lvgl_port_lock(0)) return;

    // 删除旧的标题 label
    uint32_t child_count = lv_obj_get_child_count(title_bar_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(title_bar_, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_delete(child);
            break;
        }
    }

    lv_obj_t* label = lv_label_create(title_bar_);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &SourceHanSansSC_Regular_slim, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

    lvgl_port_unlock();
}

void Panel::Show() {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void Panel::Hide() {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void Panel::SetBorder(bool enabled, lv_color_t color, lv_coord_t width) {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_border_width(panel_, enabled ? width : 0, 0);
    lv_obj_set_style_border_color(panel_, color, 0);
    lvgl_port_unlock();
}

void Panel::SetBackground(lv_color_t color, lv_opa_t opa) {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_bg_color(panel_, color, 0);
    lv_obj_set_style_bg_opa(panel_, opa, 0);
    lvgl_port_unlock();
}

void Panel::SetRadius(lv_coord_t radius) {
    if (!panel_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_radius(panel_, radius, 0);
    lvgl_port_unlock();
}

// ============================================================
// ScrollView 实现
// ============================================================

ScrollView::ScrollView() = default;
ScrollView::~ScrollView() = default;

void ScrollView::Create(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "ScrollView::Create: Failed to acquire LVGL lock");
        return;
    }

    scroll_ = lv_obj_create(parent);
    lv_obj_set_pos(scroll_, x, y);
    lv_obj_set_size(scroll_, w, h);
    lv_obj_set_style_bg_color(scroll_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scroll_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_, 0, 0);
    lv_obj_set_style_pad_all(scroll_, 4, 0);

    // 启用垂直滚动
    lv_obj_set_scroll_dir(scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_, LV_SCROLLBAR_MODE_AUTO);

    // 设置滚动条样式
    lv_obj_set_style_bg_color(scroll_, lv_color_hex(0x666666), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll_, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(scroll_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll_, 2, LV_PART_SCROLLBAR);

    // 使用 flex 布局管理子控件
    lv_obj_set_flex_flow(scroll_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lvgl_port_unlock();
}

void ScrollView::ScrollToEnd(bool anim) {
    if (!scroll_ || !lvgl_port_lock(0)) return;
    lv_obj_scroll_to_view_recursive(lv_obj_get_child(scroll_, lv_obj_get_child_count(scroll_) - 1),
                                     anim ? LV_ANIM_ON : LV_ANIM_OFF);
    lvgl_port_unlock();
}

void ScrollView::ShowScrollbar(bool show) {
    if (!scroll_ || !lvgl_port_lock(0)) return;
    lv_obj_set_scrollbar_mode(scroll_, show ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
    lvgl_port_unlock();
}

void ScrollView::Show() {
    if (!scroll_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(scroll_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ScrollView::Hide() {
    if (!scroll_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(scroll_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

// ============================================================
// IconButton 实现
// ============================================================

IconButton::IconButton() = default;
IconButton::~IconButton() = default;

void IconButton::Create(lv_obj_t* parent, const char* icon_text, int w, int h) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "IconButton::Create: Failed to acquire LVGL lock");
        return;
    }

    btn_ = lv_obj_create(parent);
    lv_obj_set_size(btn_, w, h);
    lv_obj_set_style_radius(btn_, 6, 0);
    lv_obj_set_style_bg_color(btn_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_, 1, 0);
    lv_obj_set_style_pad_all(btn_, 0, 0);
    lv_obj_add_flag(btn_, LV_OBJ_FLAG_CLICKABLE);

    // 居中 icon label
    icon_label_ = lv_label_create(btn_);
    lv_label_set_text(icon_label_, icon_text);
    lv_obj_set_style_text_font(icon_label_, &font_zectrix_16_1, 0);
    lv_obj_set_style_text_color(icon_label_, lv_color_black(), 0);
    lv_obj_align(icon_label_, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();
}

void IconButton::SetClickCallback(void (*callback)(void*), void* user_data) {
    if (!btn_ || !lvgl_port_lock(0)) return;

    struct CallbackData {
        void (*func)(void*);
        void* data;
    };

    auto* cb_data = new CallbackData{callback, user_data};
    lv_obj_add_event_cb(btn_, [](lv_event_t* e) {
        auto* cb = static_cast<CallbackData*>(lv_event_get_user_data(e));
        if (cb && cb->func) {
            cb->func(cb->data);
        }
    }, LV_EVENT_CLICKED, cb_data);

    lvgl_port_unlock();
}

void IconButton::SetIconColor(lv_color_t color) {
    if (!icon_label_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_text_color(icon_label_, color, 0);
    lvgl_port_unlock();
}

void IconButton::SetBgColor(lv_color_t color, lv_opa_t opa) {
    if (!btn_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_bg_color(btn_, color, 0);
    lv_obj_set_style_bg_opa(btn_, opa, 0);
    lvgl_port_unlock();
}

void IconButton::SetBorder(bool enabled, lv_color_t color, lv_coord_t width) {
    if (!btn_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_border_width(btn_, enabled ? width : 0, 0);
    lv_obj_set_style_border_color(btn_, color, 0);
    lvgl_port_unlock();
}

void IconButton::Show() {
    if (!btn_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(btn_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void IconButton::Hide() {
    if (!btn_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(btn_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

// ============================================================
// Bubble 实现
// ============================================================

Bubble::Bubble() = default;
Bubble::~Bubble() = default;

void Bubble::Create(lv_obj_t* parent, Align align) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "Bubble::Create: Failed to acquire LVGL lock");
        return;
    }

    align_ = align;

    // 气泡容器
    bubble_ = lv_obj_create(parent);
    lv_obj_set_width(bubble_, LV_PCT(80));  // 限制最大宽度 (Spec §3)
    lv_obj_set_style_min_width(bubble_, 60, 0);
    lv_obj_set_style_radius(bubble_, 8, 0);
    lv_obj_set_style_pad_all(bubble_, 8, 0);
    lv_obj_set_style_border_width(bubble_, 0, 0);

    // 文本标签 - 关键修复点
    label_ = lv_label_create(bubble_);
    lv_label_set_text(label_, "");
    lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);  // 自动换行 (Spec §3)
    lv_obj_set_style_text_font(label_, &SourceHanSansSC_Regular_slim, 0);  // 使用中文字体
    lv_obj_set_width(label_, LV_PCT(100));  // label 填满气泡宽度

    // 应用样式
    ApplyStyle(align);

    lvgl_port_unlock();
}

void Bubble::SetText(const char* text) {
    if (!label_ || !lvgl_port_lock(0)) return;

    text_ = text ? text : "";
    lv_label_set_text(label_, text_.c_str());

    lvgl_port_unlock();
}

void Bubble::AppendText(const char* chunk) {
    if (!chunk || !label_ || !lvgl_port_lock(0)) return;

    text_ += chunk;
    lv_label_set_text(label_, text_.c_str());

    // 自动滚动到底部
    lv_obj_scroll_to_view(bubble_, LV_ANIM_OFF);

    lvgl_port_unlock();
}

std::string Bubble::GetText() const {
    return text_;
}

void Bubble::Show() {
    if (!bubble_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(bubble_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void Bubble::Hide() {
    if (!bubble_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(bubble_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void Bubble::ScrollToView(bool anim) {
    if (!bubble_ || !lvgl_port_lock(0)) return;
    lv_obj_scroll_to_view(bubble_, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    lvgl_port_unlock();
}

void Bubble::ApplyStyle(Align align) {
    if (!bubble_ || !label_ || !lvgl_port_lock(0)) return;

    switch (align) {
        case Align::Right:  // 用户消息：黑底白字，右对齐
            lv_obj_set_style_bg_color(bubble_, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bubble_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bubble_, 0, 0);
            lv_obj_set_style_text_color(label_, lv_color_white(), 0);
            lv_obj_set_style_align(bubble_, LV_ALIGN_RIGHT_MID, 0);
            break;

        case Align::Left:  // AI 回复：白底黑框，左对齐
            lv_obj_set_style_bg_color(bubble_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bubble_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bubble_, lv_color_black(), 0);
            lv_obj_set_style_border_width(bubble_, 1, 0);
            lv_obj_set_style_text_color(label_, lv_color_black(), 0);
            lv_obj_set_style_align(bubble_, LV_ALIGN_LEFT_MID, 0);
            break;

        case Align::Center:  // 系统提示：透明背景，居中
            lv_obj_set_style_bg_color(bubble_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bubble_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bubble_, 0, 0);
            lv_obj_set_style_text_color(label_, lv_color_hex(0x666666), 0);
            lv_obj_set_style_align(bubble_, LV_ALIGN_CENTER, 0);
            break;
    }

    lvgl_port_unlock();
}

// ============================================================
// ProgressBar 实现
// ============================================================

ProgressBar::ProgressBar() = default;
ProgressBar::~ProgressBar() = default;

void ProgressBar::Create(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(kTag, "ProgressBar::Create: Failed to acquire LVGL lock");
        return;
    }

    bar_ = lv_bar_create(parent);
    lv_obj_set_pos(bar_, x, y);
    lv_obj_set_size(bar_, w, h);
    lv_bar_set_range(bar_, 0, 100);
    lv_bar_set_value(bar_, 0, LV_ANIM_OFF);

    // 样式
    lv_obj_set_style_bg_color(bar_, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_, 4, 0);
    lv_obj_set_style_radius(bar_, 4, LV_PART_INDICATOR);

    lvgl_port_unlock();
}

void ProgressBar::SetValue(int value) {
    if (!bar_ || !lvgl_port_lock(0)) return;
    value = (value < 0) ? 0 : (value > 100) ? 100 : value;
    lv_bar_set_value(bar_, value, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void ProgressBar::Show() {
    if (!bar_ || !lvgl_port_lock(0)) return;
    lv_obj_remove_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ProgressBar::Hide() {
    if (!bar_ || !lvgl_port_lock(0)) return;
    lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ProgressBar::SetBgColor(lv_color_t color) {
    if (!bar_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_bg_color(bar_, color, 0);
    lvgl_port_unlock();
}

void ProgressBar::SetIndicColor(lv_color_t color) {
    if (!bar_ || !lvgl_port_lock(0)) return;
    lv_obj_set_style_bg_color(bar_, color, LV_PART_INDICATOR);
    lvgl_port_unlock();
}

}  // namespace ui
