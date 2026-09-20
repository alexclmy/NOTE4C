#ifndef SETTINGS_RENDERER_H
#define SETTINGS_RENDERER_H

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace ui {

// 设置项类型
enum class SettingsItemType {
    Normal,    // 普通项，显示 >
    Checkbox,  // 可勾选项
    Action,    // 操作项
};

// 设置项结构
struct SettingsItemDef {
    std::string label;
    std::string value;
    const char* icon;                   // font_zectrix icon
    SettingsItemType type = SettingsItemType::Normal;
    bool checked = false;
    std::function<void()> on_click;
};

// 设置页面渲染器 (Spec §6)
// 所有菜单项使用 font_zectrix icon，现代 UI 风格
class SettingsRenderer {
public:
    SettingsRenderer();
    ~SettingsRenderer();

    // 创建设置页面
    void Create(lv_obj_t* parent);

    // 设置设置项列表
    void SetItems(const std::vector<SettingsItemDef>& items);

    // 更新单项显示值
    void UpdateItem(int index, const std::string& value);

    // 更新 Checkbox 项的选中状态
    void UpdateChecked(int index, bool checked);

    // 获取根对象
    lv_obj_t* root() const { return container_; }

    // 显示/隐藏
    void Show();
    void Hide();

private:
    // 创建单个设置项
    lv_obj_t* CreateItem(lv_obj_t* parent, const SettingsItemDef& def, int index);

    // 获取 checkbox 图标
    const char* GetCheckboxIcon(bool checked) const;

    lv_obj_t* container_ = nullptr;
    std::vector<lv_obj_t*> item_buttons_;
    std::vector<SettingsItemDef> item_data_;
    std::vector<std::function<void()>*> callbacks_;
};

}  // namespace ui

#endif  // SETTINGS_RENDERER_H
