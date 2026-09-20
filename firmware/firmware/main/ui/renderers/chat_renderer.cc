#include "chat_renderer.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

namespace ui {

static constexpr char kTag[] = "ChatRenderer";

ChatRenderer::ChatRenderer() = default;
ChatRenderer::~ChatRenderer() = default;

void ChatRenderer::Create(lv_obj_t* parent) {
    parent_ = parent;
    chat_page_ = std::make_unique<ChatPage>(parent);
    ESP_LOGI(kTag, "ChatRenderer created");
}

void ChatRenderer::Destroy() {
    if (chat_page_) {
        chat_page_->Clear();
        chat_page_.reset();
    }
    parent_ = nullptr;
}

void ChatRenderer::Update() {
    if (chat_page_) {
        chat_page_->Refresh();
    }
}

bool ChatRenderer::HandleInput(const ButtonEvent& event) {
    // Chat page 不直接处理按钮事件，由 LanMicApp orchestrator 处理
    (void)event;
    return false;
}

lv_obj_t* ChatRenderer::root() const {
    if (!chat_page_) return nullptr;
    return chat_page_->container();
}

bool ChatRenderer::AppendText(const char* chunk) {
    if (!chat_page_ || !chunk) return false;
    chat_page_->AppendText(chunk);
    return true;
}

void ChatRenderer::BeginStream() {
    if (chat_page_) {
        chat_page_->BeginStream();
    }
}

void ChatRenderer::EndStream() {
    if (chat_page_) {
        chat_page_->EndStream();
    }
}

void ChatRenderer::AddMessage(const std::string& text, ChatRole role) {
    if (chat_page_) {
        chat_page_->AddMessage(text, role);
    }
}

void ChatRenderer::ShowStatus(const std::string& status, ChatRole role) {
    if (chat_page_) {
        chat_page_->ShowStatus(status, role);
    }
}

void ChatRenderer::HideStatus() {
    if (chat_page_) {
        chat_page_->HideStatus();
    }
}

void ChatRenderer::Clear() {
    if (chat_page_) {
        chat_page_->Clear();
    }
}

}  // namespace ui
