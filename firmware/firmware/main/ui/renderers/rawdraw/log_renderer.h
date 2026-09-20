/**
 * @file log_renderer.h
 * @brief Log page renderer for rawdraw mode
 *
 * Shows system events, memory stats, and diagnostics.
 * Scrollable list with UP/DOWN navigation.
 */

#ifndef RAWDRAW_LOG_RENDERER_H
#define RAWDRAW_LOG_RENDERER_H

#include "page_renderer.h"
#include <ctime>
#include <algorithm>

#ifndef PROJECT_VER
#define PROJECT_VER "3.8.0"
#endif

namespace rawdraw {

struct LogEntry {
    time_t time;
    char tag[8];
    char message[64];
};

class LogRenderer : public PageRenderer {
public:
    static constexpr int kTitleBarH = 28;

    LogRenderer();
    ~LogRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

private:
    void DrawTitleBar(uint8_t* fb, int width);
    void CollectLogEntries();
    void AddLogEntry(const char* tag, const char* message);
    void ClampScrollOffset();

    int selected_index_;
    int scroll_offset_;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
    const lv_font_t* icon_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_LOG_RENDERER_H
