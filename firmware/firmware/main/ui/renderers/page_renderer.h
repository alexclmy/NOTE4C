#ifndef PAGE_RENDERER_H
#define PAGE_RENDERER_H

#include <lvgl.h>

namespace ui {

// 按钮事件结构
struct ButtonEvent {
    enum Type {
        kUpClick,
        kDownClick,
        kUpDoubleClick,
        kDownDoubleClick,
        kUpLongPress,
        kDownLongPress,
        kBootClick,
        kBootDoubleClick,
        kBootLongPress,
    };
    Type type;
};

// PageRenderer 基类接口 (Spec §1)
// 所有页面渲染器必须实现此接口
class PageRenderer {
public:
    virtual ~PageRenderer() = default;

    // 创建页面 UI（在 LVGL 锁内调用）
    virtual void Create(lv_obj_t* parent) = 0;

    // 销毁页面 UI（在 LVGL 锁内调用）
    virtual void Destroy() = 0;

    // 更新页面数据（在 LVGL 锁内调用）
    virtual void Update() = 0;

    // 处理输入事件
    // 返回 true 表示消费了事件
    virtual bool HandleInput(const ButtonEvent& event) = 0;

    // 获取根对象
    virtual lv_obj_t* root() const = 0;

    // 流式追加文本到当前气泡（用于 LLM streaming）
    // 返回 true 表示成功追加
    virtual bool AppendText(const char* chunk) { (void)chunk; return false; }

    // 开始新的流式响应（清空当前气泡或创建新气泡）
    virtual void BeginStream() {}

    // 结束流式响应
    virtual void EndStream() {}
};

}  // namespace ui

#endif  // PAGE_RENDERER_H
