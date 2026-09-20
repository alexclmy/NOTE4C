/**
 * @file news_renderer.h
 * @brief Daily news feed page renderer for rawdraw mode
 */

#ifndef RAWDRAW_NEWS_RENDERER_H
#define RAWDRAW_NEWS_RENDERER_H

#include "page_renderer.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rawdraw {

struct NewsItem {
    std::string title;
    std::string summary;
    std::string source;
    std::string time_label;
};

class NewsRenderer : public PageRenderer {
public:
    NewsRenderer();
    ~NewsRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void Clear();
    void SetItems(const std::vector<NewsItem>& items);
    void AddItem(const NewsItem& item);

    using TtsRequestCallback = std::function<void(const std::string& text)>;
    void SetTtsRequestCallback(TtsRequestCallback cb) { tts_request_cb_ = std::move(cb); }

private:
    void RenderItem(uint8_t* fb, int width, int y, int index, bool selected);
    void DrawPreviewModal(uint8_t* fb, int width, int height);
    void ClampSelection();
    void ClampScrollOffset();

    std::vector<NewsItem> items_;
    int selected_index_ = 0;
    int scroll_offset_ = 0;
    bool preview_open_ = false;
    int footer_focus_ = 0;  // 0=关闭, 1=朗读
    int preview_scroll_ = 0;  // scroll line offset in preview modal

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    TtsRequestCallback tts_request_cb_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_NEWS_RENDERER_H
