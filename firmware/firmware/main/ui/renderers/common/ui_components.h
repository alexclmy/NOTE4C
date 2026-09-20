#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include <lvgl.h>
#include <string>

namespace ui {

// ============================================================
// 通用 UI 组件库 (Spec §2)
// 所有页面共用，确保视觉一致性
// ============================================================

// 字体声明（extern C 防止 C++ name mangling）
extern "C" {
    extern const lv_font_t font_zectrix_16_1;
    extern const lv_font_t SourceHanSansSC_Regular_slim;
}

// 颜色常量
namespace Colors {
    inline lv_color_t White() { return lv_color_white(); }
    inline lv_color_t Black() { return lv_color_black(); }
    inline lv_color_t Gray() { return lv_color_hex(0x888888); }
    inline lv_color_t LightGray() { return lv_color_hex(0xCCCCCC); }
    inline lv_color_t DarkGray() { return lv_color_hex(0x333333); }
}

// ============================================================
// Panel: 带边框、圆角、标题栏的区域容器
// ============================================================
class Panel {
public:
    Panel();
    ~Panel();

    // 创建面板
    void Create(lv_obj_t* parent, int x, int y, int w, int h);
    void Create(lv_obj_t* parent, lv_coord_t x_pct, lv_coord_t y_pct,
                lv_coord_t w_pct, lv_coord_t h_pct);

    // 设置标题
    void SetTitle(const char* title);

    // 获取容器对象（用于放置子控件）
    lv_obj_t* content() const { return content_; }
    lv_obj_t* root() const { return panel_; }

    // 显示/隐藏
    void Show();
    void Hide();

    // 设置样式
    void SetBorder(bool enabled, lv_color_t color = lv_color_black(), lv_coord_t width = 1);
    void SetBackground(lv_color_t color, lv_opa_t opa = LV_OPA_COVER);
    void SetRadius(lv_coord_t radius);

private:
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* title_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
};

// ============================================================
// ScrollView: 可滚动内容区域，带滚动条指示器
// ============================================================
class ScrollView {
public:
    ScrollView();
    ~ScrollView();

    // 创建滚动容器
    void Create(lv_obj_t* parent, int x, int y, int w, int h);

    // 获取内容对象
    lv_obj_t* content() const { return scroll_; }
    lv_obj_t* root() const { return scroll_; }

    // 滚动到底部
    void ScrollToEnd(bool anim = false);

    // 显示/隐藏滚动条
    void ShowScrollbar(bool show);

    // 显示/隐藏
    void Show();
    void Hide();

private:
    lv_obj_t* scroll_ = nullptr;
};

// ============================================================
// IconButton: 带 icon font 的按钮
// ============================================================
class IconButton {
public:
    IconButton();
    ~IconButton();

    // 创建图标按钮
    void Create(lv_obj_t* parent, const char* icon_text, int w = 40, int h = 40);

    // 设置点击回调
    void SetClickCallback(void (*callback)(void*), void* user_data);

    // 设置样式
    void SetIconColor(lv_color_t color);
    void SetBgColor(lv_color_t color, lv_opa_t opa = LV_OPA_COVER);
    void SetBorder(bool enabled, lv_color_t color = lv_color_black(), lv_coord_t width = 1);

    // 获取对象
    lv_obj_t* root() const { return btn_; }

    // 显示/隐藏
    void Show();
    void Hide();

private:
    lv_obj_t* btn_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
};

// ============================================================
// Bubble: 对话气泡（左/右对齐，圆角，支持流式追加文字）
// ============================================================
class Bubble {
public:
    enum class Align {
        Left,   // AI 回复（左侧，白底黑框）
        Right,  // 用户消息（右侧，黑底白字）
        Center  // 系统提示（居中，透明背景）
    };

    Bubble();
    ~Bubble();

    // 创建气泡
    void Create(lv_obj_t* parent, Align align = Align::Left);

    // 设置文本
    void SetText(const char* text);

    // 流式追加文本（Spec §3 关键功能）
    void AppendText(const char* chunk);

    // 获取当前文本
    std::string GetText() const;

    // 获取对象
    lv_obj_t* root() const { return bubble_; }
    lv_obj_t* label() const { return label_; }

    // 显示/隐藏
    void Show();
    void Hide();

    // 滚动到可视区域
    void ScrollToView(bool anim = false);

private:
    void ApplyStyle(Align align);

    lv_obj_t* bubble_ = nullptr;
    lv_obj_t* label_ = nullptr;
    std::string text_;
    Align align_ = Align::Left;
};

// ============================================================
// ProgressBar: 进度条
// ============================================================
class ProgressBar {
public:
    ProgressBar();
    ~ProgressBar();

    // 创建进度条
    void Create(lv_obj_t* parent, int x, int y, int w, int h = 8);

    // 设置进度 (0-100)
    void SetValue(int value);

    // 获取对象
    lv_obj_t* root() const { return bar_; }

    // 显示/隐藏
    void Show();
    void Hide();

    // 设置样式
    void SetBgColor(lv_color_t color);
    void SetIndicColor(lv_color_t color);

private:
    lv_obj_t* bar_ = nullptr;
};

// ============================================================
// 工具函数
// ============================================================

// 安全设置 label 长模式（自动换行）
inline void SetLabelWrap(lv_obj_t* label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

// 获取字体高度
inline int GetFontHeight(const lv_font_t* font) {
    return font ? font->line_height : 16;
}

// 测量文本宽度
inline int GetTextWidth(const char* text, const lv_font_t* font) {
    if (!text || !font) return 0;
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)size.x;
}

}  // namespace ui

#endif  // UI_COMPONENTS_H
