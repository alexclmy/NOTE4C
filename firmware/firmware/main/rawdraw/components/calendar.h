/**
 * @file calendar.h
 * @brief Reusable calendar grid component for rawdraw UI
 *
 * Renders a 7x6 monthly calendar grid with:
 * - Year/month title bar
 * - Weekday header row (日 一 二 三 四 五 六)
 * - Date cells with optional lunar date sub-labels
 * - Current day highlighted as black pill
 * - Previous/next month overflow days (dimmed or hidden)
 * - Solar term and holiday annotations
 *
 * Usage:
 * 1. Create Calendar with bounds
 * 2. SetDate() to set displayed month/year
 * 3. SetShowLunar(true) to enable lunar date sub-labels
 * 4. Call Draw() each frame
 * 5. Handle UP/DOWN clicks for month navigation
 */

#ifndef RAWDRAW_CALENDAR_H
#define RAWDRAW_CALENDAR_H

#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"
#include "rawdraw/style.h"

#include <cstdint>
#include <ctime>

namespace rawdraw {

/**
 * @brief Calendar grid component
 */
class Calendar {
public:
    static constexpr int kCols = 7;
    static constexpr int kRows = 6;

    Calendar(int x = 0, int y = 0, int w = 400, int h = 240);
    ~Calendar();

    // ============================================================
    // Lunar date types (public for use by calendar_renderer)
    // ============================================================

    /**
     * @brief Lunar date result from Gregorian conversion
     */
    struct LunarDate {
        int lunar_year;    // e.g. 2026
        int lunar_month;   // 1-12 (1=正月)
        int lunar_day;     // 1-30
        bool is_leap_month; // true if this is a leap month (闰月)
    };

    /**
     * @brief Convert Gregorian date to lunar date
     * @return LunarDate, or {0,0,0,false} if year out of range
     */
    static LunarDate ToLunarDate(int year, int month, int day);

    /**
     * @brief Get lunar year string for display (e.g. "丙午年")
     * Returns empty string if year out of range (1900-2100)
     */
    static const char* GetLunarYearName(int year);

    static const char* GetLunarMonthName(int month);    // 正月..腊月
    static const char* GetLunarDayName(int day);         // 初一..三十
    static const char* GetSolarTerm(int month, int day); // 节气名称

    // ============================================================
    // Configuration
    // ============================================================

    void SetBounds(int x, int y, int w, int h);
    void SetBounds(const Rect& r);
    Rect GetBounds() const;

    /**
     * @brief Set the displayed month/year
     */
    void SetDate(int year, int month);

    /**
     * @brief Enable/disable lunar date sub-labels in cells
     */
    void SetShowLunar(bool show);

    /**
     * @brief Enable/disable showing previous/next month overflow days
     */
    void SetShowOverflowDays(bool show);

    /**
     * @brief Enable/disable drawing the title header bar
     * When embedded in a page with a global status bar, set false
     * and use the status bar central_text for the year/month title.
     */
    void SetShowHeader(bool show);

    /**
     * @brief Set fonts
     */
    void SetTitleFont(const lv_font_t* font);
    void SetBodyFont(const lv_font_t* font);
    void SetSmallFont(const lv_font_t* font);

    // ============================================================
    // Navigation
    // ============================================================

    /**
     * @brief Go to previous month
     * @return true if month changed
     */
    bool PrevMonth();

    /**
     * @brief Go to next month
     * @return true if month changed
     */
    bool NextMonth();

    /**
     * @brief Jump to current month (today)
     * @return true if month changed
     */
    bool JumpToToday();

    // ============================================================
    // Date selection cursor
    // ============================================================

    /**
     * @brief Enter selection mode with cursor on today (or 1st if not in current month)
     */
    void EnterSelectionMode();

    /**
     * @brief Exit selection mode, hide cursor
     */
    void ExitSelectionMode();

    /**
     * @brief Whether currently in selection mode
     */
    bool InSelectionMode() const { return selection_mode_; }

    /**
     * @brief Navigate cursor within grid (row-based, 7 columns)
     * @param direction -1 for up/prev, +1 for down/next
     */
    void NavigateSelection(int direction);

    /**
     * @brief Confirm current selection
     * @return true if a valid day was selected
     */
    bool ConfirmSelection();

    /**
     * @brief Get the currently selected day (0 if none selected)
     */
    int GetSelectedDay() const { return selected_day_; }

    /**
     * @brief Get cursor row/column (0-based, valid when InSelectionMode)
     */
    int GetCursorRow() const { return sel_row_; }
    int GetCursorCol() const { return sel_col_; }

    // ============================================================
    // Rendering
    // ============================================================

    void Draw(uint8_t* fb, int width, int height);
    void Draw(Framebuffer* fb, int screen_width, int screen_height);

    // ============================================================
    // Getters
    // ============================================================

    int GetYear() const { return year_; }
    int GetMonth() const { return month_; }
    bool NeedsFullRefresh() const { return needs_full_refresh_; }
    void SetNeedsFullRefresh(bool v) { needs_full_refresh_ = v; }

private:
    // Calendar math
    static bool IsLeap(int year);
    static int DaysInMonth(int year, int month);
    static int WeekdayOfDate(int year, int month, int day);  // 0=Sun
    int FirstDayOfMonth() const;  // 0=Sun

    // Rendering sub-methods
    void DrawHeader(uint8_t* fb, int width) const;
    void DrawWeekdayRow(uint8_t* fb, int width, int y) const;
    void DrawGrid(uint8_t* fb, int width, int y) const;
    void DrawBottomInfo(uint8_t* fb, int width, int y) const;
    void DrawSelectionCursor(uint8_t* fb, int width, int grid_y) const;

    // Layout state (computed in Draw)
    int CellWidth() const { return cell_w_; }
    int CellHeight() const { return cell_h_; }

    // Bounds
    int x_, y_, w_, h_;

    // Displayed month/year
    int year_;
    int month_;  // 1-based

    // Today's date (for highlight)
    int today_year_;
    int today_month_;
    int today_day_;

    // Fonts
    const lv_font_t* title_font_;
    const lv_font_t* body_font_;
    const lv_font_t* small_font_;

    // Computed layout
    int cell_w_;
    int cell_h_;

    // Flags
    bool show_lunar_;
    bool show_overflow_;
    bool show_header_;
    bool needs_full_refresh_;

    // Selection cursor state
    bool selection_mode_;
    int sel_row_;    // 0-based grid row (0..5)
    int sel_col_;    // 0-based grid col (0..6, Sun..Sat)
    int selected_day_; // confirmed selected day, 0 if none
};

}  // namespace rawdraw

#endif  // RAWDRAW_CALENDAR_H
