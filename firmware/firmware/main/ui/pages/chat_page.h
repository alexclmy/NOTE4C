#ifndef CHAT_PAGE_H
#define CHAT_PAGE_H

#include <lvgl.h>
#include <string>
#include <vector>
#include <memory>

#include "renderers/common/ui_components.h"

namespace ui {

// 聊天消息角色
enum class ChatRole {
    User,   // 用户消息（右侧，黑底白字）
    AI,     // AI 回复（左侧，白底黑框）
    System  // 系统提示（居中，透明）
};

// 聊天消息
struct ChatMessage {
    std::string text;
    ChatRole role;
};

// AI 对话页 - 修复版（Spec §3）
// 修复：文字不显示、流式追加、自动换行
class ChatPage {
public:
    ChatPage(lv_obj_t* parent);
    ~ChatPage();

    // 清空消息列表
    void Clear();

    // 添加消息（自动滚动到底部）
    void AddMessage(const std::string& text, ChatRole role);

    // 显示临时状态提示（录音/识别/思考）
    void ShowStatus(const std::string& status, ChatRole role);

    // 隐藏状态提示
    void HideStatus();

    // 流式追加文本到最后一个气泡（用于 LLM streaming）
    void AppendText(const std::string& chunk);

    // 开始新的流式响应（创建新的 AI 气泡）
    void BeginStream();

    // 结束流式响应
    void EndStream();

    // 获取最后一个气泡用于流式追加
    Bubble* GetLastBubble() const;

    // 获取容器对象（供渲染器使用）
    lv_obj_t* container() const { return container_; }

    // 刷新显示
    void Refresh();

private:
    // 气泡容器管理
    struct BubbleEntry {
        std::unique_ptr<Bubble> bubble;
        ChatRole role;
    };

    lv_obj_t* container_ = nullptr;       // Flex 滚动容器
    lv_obj_t* status_bubble_ = nullptr;   // 临时状态气泡
    Bubble* status_bubble_wrapper_ = nullptr;
    std::vector<BubbleEntry> bubbles_;    // 消息气泡列表
    bool is_streaming_ = false;           // 是否正在流式接收
};

}  // namespace ui

#endif  // CHAT_PAGE_H
