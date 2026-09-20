/**
 * @file calendar_renderer.h
 * @brief Monthly calendar grid renderer for rawdraw mode
 *
 * Displays a 7x6 calendar grid with:
 * - Title bar with year/month and 2px divider
 * - Weekday header row (日 一 二 三 四 五 六)
 * - Date cells with today highlighted as black pill
 * - Bottom date info line
 * - Selection cursor for picking specific dates
 *
 * Navigation: UP/DOWN=翻月, BOOT=选中日期/进入选择模式
 * In selection mode: UP/DOWN=移动光标, BOOT=确认选择
 */

#ifndef RAWDRAW_CALENDAR_RENDERER_H
#define RAWDRAW_CALENDAR_RENDERER_H

#include "page_renderer.h"
#include "rawdraw/components/calendar.h"
#include "rawdraw/style.h"

namespace rawdraw {

class CalendarRenderer : public PageRenderer {
public:
    CalendarRenderer();
    ~CalendarRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /**
     * @brief Get the last confirmed selected date (year, month, day)
     * Returns {0,0,0} if no selection made.
     */
    struct SelectedDate {
        int year;
        int month;
        int day;
    };
    SelectedDate GetSelectedDate() const;

    int GetYear() const { return year_; }
    int GetMonth() const { return month_; }

    /**
     * @brief Get formatted voice query context string.
     * Returns formatted date like "2026年4月15日 丙午年二月初一" if a date
     * has been selected, or empty string otherwise.
     * The parent should call this when voice collection starts to include
     * the calendar date context in the LLM prompt.
     */
    std::string GetVoiceQueryContext() const;

private:
    // Calendar component (owns selection cursor state)
    Calendar cal_;

    // Fonts
    const lv_font_t* title_font_;
    const lv_font_t* body_font_;
    const lv_font_t* small_font_;

    // Calendar state (mirrored from cal_)
    int year_;
    int month_;
    int today_day_;
    int today_month_;
    int today_year_;

    // Last confirmed selection
    SelectedDate selected_date_;

    // Layout constants
    static constexpr int kTitleBarH = 28;
};

}  // namespace rawdraw

#endif  // RAWDRAW_CALENDAR_RENDERER_H
