/**
 * @file yearprogress_renderer.h
 * @brief Year progress page renderer for rawdraw mode
 *
 * Displays:
 * - Current date (YYYY/MM/DD)
 * - Year progress percentage
 * - Horizontal progress bar (80% screen width)
 * - 12-month overview with UP/DOWN navigation
 */

#ifndef RAWDRAW_YEARPROGRESS_RENDERER_H
#define RAWDRAW_YEARPROGRESS_RENDERER_H

#include "page_renderer.h"
#include <cstdint>

namespace rawdraw {

/**
 * @brief Year progress page renderer
 */
class YearProgressRenderer : public PageRenderer {
public:
    YearProgressRenderer();
    ~YearProgressRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

private:
    // Date calculation helpers
    int GetDaysInYear(int year) const;
    int GetDaysInMonth(int year, int month) const;

    // Formatting
    void FormatDate(char* buf, int len) const;
    const char* GetMonthName(int month) const;
    const char* GetWeekdayName(int wday) const;

    // Update cached time data from RTC
    void UpdateTime();

    // Layout
    void RenderHeader(uint8_t* fb, int width, int y_start) const;
    void RenderMonthGrid(uint8_t* fb, int width, int height, int y_start) const;
    void RenderMonthRow(uint8_t* fb, int width, int y, int month,
                        bool is_past, bool is_current, bool is_selected) const;

    // Font references
    const lv_font_t* title_font_;
    const lv_font_t* body_font_;
    const lv_font_t* small_font_;
    const lv_font_t* icon_font_;

    // Cached time data
    int year_;
    int month_;          // 0-based
    int day_;
    int wday_;           // 0=Sun...6=Sat
    int day_of_year_;    // 1-based
    int total_days_;
    int progress_pct_;

    // State
    int selected_month_; // -1 = overview, 0-11 = selected month
};

}  // namespace rawdraw

#endif  // RAWDRAW_YEARPROGRESS_RENDERER_H
