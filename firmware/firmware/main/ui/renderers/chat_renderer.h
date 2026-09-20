#ifndef CHAT_RENDERER_H
#define CHAT_RENDERER_H

#include "renderers/page_renderer.h"
#include "pages/chat_page.h"
#include <memory>

namespace ui {

// ChatRenderer: 对话页面渲染器
// 包装 ChatPage 并实现 PageRenderer 接口
class ChatRenderer : public PageRenderer {
public:
    ChatRenderer();
    ~ChatRenderer() override;

    // PageRenderer 接口实现
    void Create(lv_obj_t* parent) override;
    void Destroy() override;
    void Update() override;
    bool HandleInput(const ButtonEvent& event) override;
    lv_obj_t* root() const override;

    // 流式文本支持
    bool AppendText(const char* chunk) override;
    void BeginStream() override;
    void EndStream() override;

    // 数据接口
    void AddMessage(const std::string& text, ChatRole role);
    void ShowStatus(const std::string& status, ChatRole role);
    void HideStatus();
    void Clear();

private:
    std::unique_ptr<ChatPage> chat_page_;
    lv_obj_t* parent_ = nullptr;
};

}  // namespace ui

#endif  // CHAT_RENDERER_H
