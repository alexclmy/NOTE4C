/**
 * @file calendar.cc
 * @brief Calendar grid component implementation
 *
 * Renders a 7x6 monthly calendar with solar term and lunar date labels.
 * Solar terms use fixed-date approximation (±1 day accuracy).
 * Lunar month/day names are shown when enabled.
 */

#include "calendar.h"
#include "holiday_fetcher.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace rawdraw {

// ============================================================
// Solar term lookup table (approximate fixed dates, ±1 day)
// ============================================================

struct SolarTermEntry {
    int month;
    int day;
    const char* name;
};

// Approximate solar term dates (fixed-day approximation)
// In reality these shift by ±1 day depending on the year
static const SolarTermEntry kSolarTerms[] = {
    { 1,  5, "小寒" }, { 1, 20, "大寒" },
    { 2,  4, "立春" }, { 2, 19, "雨水" },
    { 3,  5, "惊蛰" }, { 3, 20, "春分" },
    { 4,  4, "清明" }, { 4, 20, "谷雨" },
    { 5,  5, "立夏" }, { 5, 21, "小满" },
    { 6,  5, "芒种" }, { 6, 21, "夏至" },
    { 7,  7, "小暑" }, { 7, 23, "大暑" },
    { 8,  7, "立秋" }, { 8, 23, "处暑" },
    { 9,  7, "白露" }, { 9, 23, "秋分" },
    {10,  8, "寒露" }, {10, 23, "霜降" },
    {11,  7, "立冬" }, {11, 22, "小雪" },
    {12,  7, "大雪" }, {12, 22, "冬至" },
};

static constexpr int kSolarTermCount = sizeof(kSolarTerms) / sizeof(kSolarTerms[0]);

// Lunar month names (lunar calendar months 1-12)
static const char* kLunarMonths[] = {
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "腊月"
};

// Lunar day names
static const char* kLunarDays[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
};

// Weekday header characters
static const char* kWeekdayChars[] = {"日", "一", "二", "三", "四", "五", "六"};
static const char* kWeekdayFull[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

// Holidays (fixed-date solar holidays in Gregorian calendar)
struct HolidayEntry {
    int month;
    int day;
    const char* name;
};

static const HolidayEntry kHolidays[] = {
    { 1,  1, "元旦" },
    { 2, 14, "情人节" },
    { 3,  8, "妇女节" },
    { 3, 12, "植树节" },
    { 4,  1, "愚人节" },
    { 5,  1, "劳动节" },
    { 5,  4, "青年节" },
    { 6,  1, "儿童节" },
    { 7,  1, "建党节" },
    { 8,  1, "建军节" },
    { 9, 10, "教师节" },
    {10,  1, "国庆节" },
    {10, 31, "万圣节" },
    {12, 25, "圣诞节" },
};

static constexpr int kHolidayCount = sizeof(kHolidays) / sizeof(kHolidays[0]);

// ============================================================
// Static helpers
// ============================================================

bool Calendar::IsLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int Calendar::DaysInMonth(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && IsLeap(year)) return 29;
    return d[month - 1];
}

int Calendar::WeekdayOfDate(int year, int month, int day) {
    // Sakamoto's algorithm: returns 0=Sun, 1=Mon, ..., 6=Sat
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

int Calendar::FirstDayOfMonth() const {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year_;
    int m = month_;
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + 1) % 7;
}

const char* Calendar::GetLunarMonthName(int month) {
    if (month < 1 || month > 12) return "";
    return kLunarMonths[month - 1];
}

const char* Calendar::GetLunarDayName(int day) {
    if (day < 1 || day > 30) return "";
    return kLunarDays[day - 1];
}

const char* Calendar::GetSolarTerm(int month, int day) {
    for (int i = 0; i < kSolarTermCount; i++) {
        if (kSolarTerms[i].month == month && kSolarTerms[i].day == day) {
            return kSolarTerms[i].name;
        }
    }
    return nullptr;
}

// ============================================================
// Proper lunar calendar algorithm (2000-2050)
// Uses embedded Chinese New Year dates + alternating month sizes.
// Accuracy: ±1-2 days for lunar day, ±1 month in leap years.
// ============================================================

// Chinese New Year dates for 2000-2050
// Format: (month << 8) | day  (day in decimal, NOT hex!)
static const uint16_t kSpringInfo[] = {
    0x0205, 0x0118, 0x020C, 0x0201, 0x0116,  // 2000-2004
    0x0209, 0x011D, 0x0212, 0x0207, 0x011A,  // 2005-2009
    0x020E, 0x0203, 0x0117, 0x020A, 0x011F,  // 2010-2014
    0x0213, 0x0208, 0x011C, 0x0210, 0x0205,  // 2015-2019
    0x0119, 0x020C, 0x0201, 0x0116, 0x020A,  // 2020-2024
    0x011D, 0x0211, 0x0206, 0x011A, 0x020D,  // 2025-2029
    0x0203, 0x0117, 0x020B, 0x011F, 0x0213,  // 2030-2034
    0x0208, 0x011C, 0x020F, 0x0204, 0x0118,  // 2035-2039
    0x020C, 0x0201, 0x0116, 0x020A, 0x011E,  // 2040-2044
    0x0211, 0x0206, 0x011A, 0x020E, 0x0202,  // 2045-2049
    0x0117,                                     // 2050
};

static constexpr int kLunarMinYear = 2000;
static constexpr int kLunarMaxYear = 2050;
static constexpr int kLunarYearCount = kLunarMaxYear - kLunarMinYear + 1;  // 51

// Tian Gan (天干) and Di Zhi (地支) for year names
static const char* kTianGan[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
static const char* kDiZhi[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};

// Leap month info for 2000-2050 (0 = no leap, 1-12 = which month has leap)
// Derived from lunardate library validation
static const uint8_t kLeapMonths[] = {
    0, 4, 0, 0, 2, 0, 7, 0, 0, 5,  // 2000-2009
    0, 0, 4, 0, 9, 0, 0, 6, 0, 0,  // 2010-2019
    4, 0, 0, 2, 0, 6, 0, 0, 5, 0,  // 2020-2029
    0, 3, 0, 11, 0, 0, 6, 0, 0, 5,  // 2030-2039
    0, 0, 2, 0, 7, 0, 0, 5, 0, 0,  // 2040-2049
    3,                                // 2050
};

static bool IsGregorianLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int DaysInGregorianMonth(int year, int month) {
    static const int d[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsGregorianLeap(year)) return 29;
    return d[month];
}

// Day-of-year for a Gregorian date
static int DayOfYear(int year, int month, int day) {
    int doy = 0;
    for (int m = 1; m < month; m++) {
        doy += DaysInGregorianMonth(year, m);
    }
    return doy + day;
}

// Total days in a Gregorian year
static int DaysInGregorianYear(int year) {
    return IsGregorianLeap(year) ? 366 : 365;
}

Calendar::LunarDate Calendar::ToLunarDate(int year, int month, int day) {
    if (year < kLunarMinYear || year > kLunarMaxYear) return {0, 0, 0, false};

    int idx = year - kLunarMinYear;
    uint16_t cny = kSpringInfo[idx];
    int cny_m = (cny >> 8) & 0xff;
    int cny_d = cny & 0xff;

    int doy = DayOfYear(year, month, day);
    int cny_doy = DayOfYear(year, cny_m, cny_d);

    int lunar_year = year;
    int days_since_cny;

    if (doy < cny_doy) {
        // Before CNY, belongs to previous lunar year
        if (year <= kLunarMinYear) return {0, 0, 0, false};
        lunar_year = year - 1;
        uint16_t prev_cny = kSpringInfo[idx - 1];
        int prev_cny_m = (prev_cny >> 8) & 0xff;
        int prev_cny_d = prev_cny & 0xff;
        int prev_cny_doy = DayOfYear(year - 1, prev_cny_m, prev_cny_d);
        int prev_year_days = DaysInGregorianYear(year - 1);
        days_since_cny = (prev_year_days - prev_cny_doy) + doy;
    } else {
        days_since_cny = doy - cny_doy;
    }

    // Walk through lunar months using alternating 30/29 pattern
    int lunar_month = 1;
    int leap_month = (lunar_year >= kLunarMinYear && lunar_year <= kLunarMaxYear)
        ? kLeapMonths[lunar_year - kLunarMinYear] : 0;

    while (lunar_month <= 12) {
        // Alternating: odd months = 30, even = 29
        int days_in_month = (lunar_month % 2 == 1) ? 30 : 29;

        if (days_since_cny < days_in_month) {
            return {lunar_year, lunar_month, days_since_cny + 1, false};
        }
        days_since_cny -= days_in_month;

        // Check for leap month
        if (leap_month == lunar_month) {
            int leap_days = 29;  // Default 29 days for leap month
            if (days_since_cny < leap_days) {
                return {lunar_year, lunar_month, days_since_cny + 1, true};
            }
            days_since_cny -= leap_days;
        }

        lunar_month++;
    }

    // If we still have days left (shouldn't happen with correct data), clamp
    if (lunar_month > 12) {
        return {lunar_year, 12, 1, false};
    }

    return {lunar_year, lunar_month, days_since_cny + 1, false};
}

const char* Calendar::GetLunarYearName(int year) {
    static char buf[8];
    if (year < 1900 || year > 2100) return "";
    int tg = (year - 4) % 10;
    int dz = (year - 4) % 12;
    snprintf(buf, sizeof(buf), "%s%s", kTianGan[tg], kDiZhi[dz]);
    return buf;
}

// ============================================================
// Constructor
// ============================================================

Calendar::Calendar(int x, int y, int w, int h)
    : x_(x), y_(y), w_(w), h_(h)
    , year_(2026), month_(1)
    , today_year_(2026), today_month_(1), today_day_(1)
    , title_font_(nullptr), body_font_(nullptr), small_font_(nullptr)
    , cell_w_(0), cell_h_(0)
    , show_lunar_(false)
    , show_overflow_(false)
    , show_header_(true)
    , needs_full_refresh_(true)
    , selection_mode_(false)
    , sel_row_(-1), sel_col_(-1)
    , selected_day_(0) {

    // Initialize today's date
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    today_year_ = tm_buf.tm_year + 1900;
    today_month_ = tm_buf.tm_mon + 1;
    today_day_ = tm_buf.tm_mday;
    year_ = today_year_;
    month_ = today_month_;
}

Calendar::~Calendar() {}

// ============================================================
// Configuration
// ============================================================

void Calendar::SetBounds(int x, int y, int w, int h) {
    x_ = x; y_ = y; w_ = w; h_ = h;
    needs_full_refresh_ = true;
}

void Calendar::SetBounds(const Rect& r) {
    x_ = r.x; y_ = r.y; w_ = r.w; h_ = r.h;
    needs_full_refresh_ = true;
}

Rect Calendar::GetBounds() const {
    return {x_, y_, w_, h_};
}

void Calendar::SetDate(int year, int month) {
    if (month < 1) {
        month = 12;
        year--;
    } else if (month > 12) {
        month = 1;
        year++;
    }
    year_ = year;
    month_ = month;
    needs_full_refresh_ = true;
}

void Calendar::SetShowLunar(bool show) { show_lunar_ = show; }
void Calendar::SetShowOverflowDays(bool show) { show_overflow_ = show; }
void Calendar::SetShowHeader(bool show) { show_header_ = show; needs_full_refresh_ = true; }

void Calendar::SetTitleFont(const lv_font_t* font) { title_font_ = font; needs_full_refresh_ = true; }
void Calendar::SetBodyFont(const lv_font_t* font) { body_font_ = font; needs_full_refresh_ = true; }
void Calendar::SetSmallFont(const lv_font_t* font) { small_font_ = font; needs_full_refresh_ = true; }

// ============================================================
// Navigation
// ============================================================

bool Calendar::PrevMonth() {
    int old_year = year_, old_month = month_;
    month_--;
    if (month_ < 1) { month_ = 12; year_--; }
    needs_full_refresh_ = (year_ != old_year || month_ != old_month);
    return needs_full_refresh_;
}

bool Calendar::NextMonth() {
    int old_year = year_, old_month = month_;
    month_++;
    if (month_ > 12) { month_ = 1; year_++; }
    needs_full_refresh_ = (year_ != old_year || month_ != old_month);
    return needs_full_refresh_;
}

bool Calendar::JumpToToday() {
    int old_year = year_, old_month = month_;
    year_ = today_year_;
    month_ = today_month_;
    needs_full_refresh_ = (year_ != old_year || month_ != old_month);
    return needs_full_refresh_;
}

// ============================================================
// Date selection cursor
// ============================================================

void Calendar::EnterSelectionMode() {
    selection_mode_ = true;
    selected_day_ = 0;
    needs_full_refresh_ = true;

    // Place cursor on today's date if in current month, otherwise 1st
    int first_dow = FirstDayOfMonth();

    if (year_ == today_year_ && month_ == today_month_) {
        // Cursor on today
        int today_cell = first_dow + today_day_ - 1;
        sel_row_ = today_cell / kCols;
        sel_col_ = today_cell % kCols;
    } else {
        // Cursor on 1st of month
        sel_row_ = first_dow / kCols;
        sel_col_ = first_dow % kCols;
    }
}

void Calendar::ExitSelectionMode() {
    selection_mode_ = false;
    sel_row_ = -1;
    sel_col_ = -1;
    needs_full_refresh_ = true;
}

void Calendar::NavigateSelection(int direction) {
    if (!selection_mode_) return;

    int first_dow = FirstDayOfMonth();
    int dim = DaysInMonth(year_, month_);
    int prev_dim = DaysInMonth(
        month_ == 1 ? year_ - 1 : year_,
        month_ == 1 ? 12 : month_ - 1
    );

    // Try moving row by row
    int new_row = sel_row_ + direction;

    // Clamp to grid rows
    if (new_row < 0) new_row = 0;
    if (new_row >= kRows) new_row = kRows - 1;

    // Find valid day at (new_row, sel_col_)
    int cell = new_row * kCols + sel_col_;
    int day = 0;
    bool valid = false;

    if (cell < first_dow) {
        // Previous month overflow
        day = prev_dim - first_dow + cell + 1;
        if (show_overflow_) valid = true;
    } else if (cell >= first_dow + dim) {
        // Next month overflow
        day = cell - first_dow - dim + 1;
        if (show_overflow_) valid = true;
    } else {
        // Current month
        day = cell - first_dow + 1;
        valid = true;
    }

    if (valid && day > 0) {
        sel_row_ = new_row;
    } else {
        // Try to find nearest valid day in same column
        for (int try_row = new_row; try_row >= 0 && try_row < kRows; try_row += direction) {
            int try_cell = try_row * kCols + sel_col_;
            int try_day = 0;
            bool try_valid = false;
            if (try_cell < first_dow) {
                try_day = prev_dim - first_dow + try_cell + 1;
                if (show_overflow_) try_valid = true;
            } else if (try_cell >= first_dow + dim) {
                try_day = try_cell - first_dow - dim + 1;
                if (show_overflow_) try_valid = true;
            } else {
                try_day = try_cell - first_dow + 1;
                try_valid = true;
            }
            if (try_valid && try_day > 0) {
                sel_row_ = try_row;
                return;
            }
        }
        // No valid cell found in this column, stay put
        return;
    }
}

bool Calendar::ConfirmSelection() {
    if (!selection_mode_) return false;

    int first_dow = FirstDayOfMonth();
    int dim = DaysInMonth(year_, month_);
    int cell = sel_row_ * kCols + sel_col_;
    int day = 0;

    if (cell < first_dow) {
        // Previous month overflow
        int prev_dim = DaysInMonth(
            month_ == 1 ? year_ - 1 : year_,
            month_ == 1 ? 12 : month_ - 1
        );
        day = prev_dim - first_dow + cell + 1;
    } else if (cell >= first_dow + dim) {
        // Next month overflow
        day = cell - first_dow - dim + 1;
    } else {
        // Current month
        day = cell - first_dow + 1;
    }

    if (day > 0 && day <= 31) {
        selected_day_ = day;
        ExitSelectionMode();
        return true;
    }
    return false;
}

// ============================================================
// Rendering
// ============================================================

void Calendar::Draw(uint8_t* fb, int width, int height) {
    if (!fb) return;

    const lv_font_t* tfont = title_font_;
    const lv_font_t* bfont = body_font_;
    const lv_font_t* sfont = small_font_;
    if (!tfont) tfont = &SourceHanSansSC_Medium_slim;
    if (!bfont) bfont = &SourceHanSansSC_Regular_slim;
    if (!sfont) sfont = &SourceHanSansSC_Regular_slim;

    // Layout calculation
    int title_bar_h = show_header_ ? Style::kPanelTitleHeight : 0;  // 28px or 0
    int weekday_h = 22;
    const bool show_bottom_info = show_header_;
    int bottom_reserve = show_bottom_info ? 26 : 0;
    int grid_total_h = h_ - title_bar_h - weekday_h - bottom_reserve;

    cell_h_ = grid_total_h / kRows;
    if (cell_h_ < 34) cell_h_ = 34;
    if (cell_h_ > 40) cell_h_ = 40;

    cell_w_ = w_ / kCols;  // ~57 for 400px width

    int base_y = y_;

    // Header (skip if show_header_ is false — global status bar provides title)
    if (show_header_) {
        DrawHeader(fb, width);
    }
    base_y += title_bar_h;

    // Weekday row
    DrawWeekdayRow(fb, width, base_y);
    base_y += weekday_h;

    // Grid
    int grid_y = base_y;
    DrawGrid(fb, width, base_y);
    base_y += cell_h_ * kRows;

    // Selection cursor (overlay on grid)
    if (selection_mode_) {
        DrawSelectionCursor(fb, width, grid_y);
    }

    // Bottom info is only used by standalone calendar widgets with their own
    // header. The page renderer already has a global status title and needs
    // the lower area for the 6x7 grid.
    if (show_bottom_info) {
        DrawBottomInfo(fb, width, base_y);
    }

    needs_full_refresh_ = false;
}

void Calendar::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

void Calendar::DrawHeader(uint8_t* fb, int width) const {
    const int title_bar_h = Style::kPanelTitleHeight;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg = theme.Component(ComponentRole::Panel);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // Background
    DrawStyledRect(fb, width, {x_, y_, w_, title_bar_h}, bg);

    // Divider line (2px)
    int line_y = y_ + title_bar_h - 2;
    DrawHLine(fb, width, line_y, x_, x_ + w_ - 1, border);
    DrawHLine(fb, width, line_y + 1, x_, x_ + w_ - 1, border);

    // Title: "2026年 4月" centered
    char title[32];
    snprintf(title, sizeof(title), "%d年 %d月", year_, month_);
    int title_w = MeasureTextWidth(title, title_font_);
    int title_x = x_ + (w_ - title_w) / 2;
    int title_text_y = y_ + (title_bar_h - title_font_->line_height) / 2;
    title_x = (title_x + 7) & ~7;
    DrawText(fb, width, title_x, title_text_y, title, title_font_, text, y_ + title_bar_h);

    // Left arrow
    const char* left_arrow = "<";
    int left_x = x_ + Style::kSpacingLG;
    left_x = (left_x + 7) & ~7;
    DrawText(fb, width, left_x, title_text_y, left_arrow, title_font_, text, y_ + title_bar_h);

    // Right arrow
    const char* right_arrow = ">";
    int right_w = MeasureTextWidth(right_arrow, title_font_);
    int right_x = x_ + w_ - Style::kSpacingLG - right_w;
    right_x = (right_x + 7) & ~7;
    DrawText(fb, width, right_x, title_text_y, right_arrow, title_font_, text, y_ + title_bar_h);
}

void Calendar::DrawWeekdayRow(uint8_t* fb, int width, int y) const {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg = theme.Style(ThemeToken::BackgroundSecondary);
    const Color text = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // Subtle background
    int bg_h = 22;
    DrawStyledRect(fb, width, {x_, y, w_, bg_h}, bg);

    for (int i = 0; i < kCols; i++) {
        const char* ch = kWeekdayChars[i];
        int ch_w = MeasureTextWidth(ch, body_font_);
        int cx = x_ + i * cell_w_;
        int text_x = cx + (cell_w_ - ch_w) / 2;

        // Weekend emphasis: draw 日 and 六 slightly differently could go here
        DrawText(fb, width, text_x,
                 InkCenteredTextTopYInBox(body_font_, ch, y, bg_h, 0),
                 ch, body_font_, text, y + bg_h);
    }

    // Divider below weekday row
    int line_y = y + bg_h - 2;
    DrawHLine(fb, width, line_y, x_, x_ + w_ - 1, border);
    DrawHLine(fb, width, line_y + 1, x_, x_ + w_ - 1, border);
}

void Calendar::DrawGrid(uint8_t* fb, int width, int y) const {
    const auto& theme = ThemeManager::Get();
    const PaintStyle today_style = theme.Style(ThemeToken::Selected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color dim_text = theme.ColorFor(ThemeToken::Disabled);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    int first_dow = FirstDayOfMonth();  // 0=Sun
    int dim = DaysInMonth(year_, month_);
    int prev_dim = DaysInMonth(
        month_ == 1 ? year_ - 1 : year_,
        month_ == 1 ? 12 : month_ - 1
    );

    int total_cells = kRows * kCols;  // 42
    int start_offset = first_dow;

    for (int cell = 0; cell < total_cells; cell++) {
        int row = cell / kCols;
        int col = cell % kCols;
        int cx = x_ + col * cell_w_;
        int cy = y + row * cell_h_;

        int display_day = 0;
        bool is_today = false;
        bool is_current_month = true;

        if (cell < start_offset) {
            // Previous month
            display_day = prev_dim - start_offset + cell + 1;
            is_current_month = false;
        } else if (cell >= start_offset + dim) {
            // Next month
            display_day = cell - start_offset - dim + 1;
            is_current_month = false;
        } else {
            // Current month
            display_day = cell - start_offset + 1;

            // Check if today
            if (year_ == today_year_ && month_ == today_month_ && display_day == today_day_) {
                is_today = true;
            }
        }

        // Skip overflow days if hidden
        if (!is_current_month && !show_overflow_) continue;

        // Check for solar term or holiday annotation
        const char* solar_term = nullptr;
        const char* holiday = nullptr;
        const char* makeup_label = nullptr;
        if (is_current_month) {
            solar_term = GetSolarTerm(month_, display_day);
            for (int h = 0; h < kHolidayCount; h++) {
                if (kHolidays[h].month == month_ && kHolidays[h].day == display_day) {
                    holiday = kHolidays[h].name;
                    break;
                }
            }
            // Official State Council holiday / makeup workday
            int q_year = year_, q_month = month_, q_day = display_day;
            if (holiday_fetcher::IsHoliday(q_year, q_month, q_day)) {
                holiday = holiday_fetcher::GetHolidayName(q_year, q_month, q_day);
                if (!holiday) holiday = "休";
            } else if (holiday_fetcher::IsMakeupWorkday(q_year, q_month, q_day)) {
                makeup_label = holiday_fetcher::GetMakeupLabel(q_year, q_month, q_day);
            }
        }

        // Draw day number
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", display_day);
        int num_w = MeasureTextWidth(buf, body_font_);
        constexpr int kDateBoxH = 18;
        constexpr int kDateTopPad = 2;
        constexpr int kDateLunarGap = 2;
        constexpr int kLunarBoxH = 16;
        int num_x = cx + (cell_w_ - num_w) / 2;
        const int num_box_y = cy + kDateTopPad;
        int num_y = InkCenteredTextTopYInBox(body_font_, buf, num_box_y, kDateBoxH, 0);

        // Clamp to screen
        if (num_x + num_w > x_ + w_) continue;
        if (num_y + kDateBoxH > y_ + h_) continue;

        if (is_today) {
            // Highlight the whole date+lunar stack, not just the day number.
            // The old tiny day-only pill made the lower lunar text appear as a
            // stray one-third black block on e-paper.
            Rect hl = {cx + 5, cy + 1, cell_w_ - 10, cell_h_ + 2};
            if (hl.x + hl.w > x_ + w_) hl.w = x_ + w_ - hl.x;
            if (hl.y + hl.h > y_ + h_) hl.h = y_ + h_ - hl.y;
            DrawStyledRoundRect(fb, width, y_ + h_, hl, Style::kBorderRadiusSM, today_style);
            DrawStyledText(fb, width, num_x, num_y, buf, body_font_, today_style, y_ + h_);
        } else if (!is_current_month) {
            // Dim overflow days
            DrawText(fb, width, num_x, num_y, buf, body_font_, dim_text, y_ + h_);
        } else {
            DrawText(fb, width, num_x, num_y, buf, body_font_, text, y_ + h_);
        }

        // Lunar date or solar term / holiday / makeup sub-label
        if (show_lunar_ || solar_term || holiday || makeup_label) {
            const char* label = nullptr;
            bool is_solar_term = false;

            if (solar_term) {
                label = solar_term;
                is_solar_term = true;
            } else if (holiday) {
                label = holiday;
            } else if (makeup_label) {
                label = makeup_label;
            } else if (show_lunar_ && is_current_month) {
                LunarDate ld = ToLunarDate(year_, month_, display_day);
                if (ld.lunar_day == 1) {
                    // Show lunar month name on 1st of lunar month
                    label = GetLunarMonthName(ld.lunar_month);
                } else {
                    label = GetLunarDayName(ld.lunar_day);
                }
            }

            if (label && *label) {
                int label_w = MeasureTextWidth(label, small_font_);
                int label_x = cx + (cell_w_ - label_w) / 2;
                int label_box_y = num_box_y + kDateBoxH + kDateLunarGap;
                int label_y = InkCenteredTextTopYInBox(small_font_, label, label_box_y, kLunarBoxH, 0);

                if (label_x + label_w <= x_ + w_ && label_box_y + kLunarBoxH <= y_ + h_) {
                    Color label_color = is_today ? today_style.fg :
                        (is_solar_term ? accent : text);
                    DrawText(fb, width, label_x, label_y, label, small_font_,
                             label_color, y_ + h_);
                }
            }
        }
    }
}

void Calendar::DrawBottomInfo(uint8_t* fb, int width, int y) const {
    if (y + small_font_->line_height > y_ + h_ - Style::kSpacingSM) return;

    // Bottom line: current view info
    char buf[80];
    if (year_ == today_year_ && month_ == today_month_) {
        int weekday_idx = WeekdayOfDate(today_year_, today_month_, today_day_);
        snprintf(buf, sizeof(buf), "今天 %d月%d日 %s",
                 today_month_, today_day_, kWeekdayFull[weekday_idx]);

        // Add lunar date
        LunarDate ld = ToLunarDate(today_year_, today_month_, today_day_);
        const char* year_name = GetLunarYearName(today_year_);
        int extra_len = snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
                                 " %s年%s%s", year_name, GetLunarMonthName(ld.lunar_month), GetLunarDayName(ld.lunar_day));
        (void)extra_len;
    } else {
        int dim = DaysInMonth(year_, month_);
        snprintf(buf, sizeof(buf), "%d年%d月 共%d天", year_, month_, dim);
    }

    int text_w = MeasureTextWidth(buf, small_font_);
    int text_x = x_ + (w_ - text_w) / 2;
    text_x = (text_x + 7) & ~7;
    DrawText(fb, width, text_x, y, buf, small_font_,
             ThemeManager::Get().ColorFor(ThemeToken::TextSecondary), y_ + h_);
}

void Calendar::DrawSelectionCursor(uint8_t* fb, int width, int grid_y) const {
    if (!selection_mode_ || sel_row_ < 0 || sel_col_ < 0) return;

    int cx = x_ + sel_col_ * cell_w_;
    int cy = grid_y + sel_row_ * cell_h_;

    const Color focus = ThemeManager::Get().ColorFor(ThemeToken::Focus);

    // Draw a thick focus rectangle border around the selected cell
    // This is visually distinct from the today pill (filled black)
    int border_w = 3;  // 3px thick border

    // Top and bottom borders (filled strip)
    DrawRect(fb, width, {cx, cy, cell_w_, border_w}, focus);
    DrawRect(fb, width, {cx, cy + cell_h_ - border_w, cell_w_, border_w}, focus);

    // Left and right borders
    DrawRect(fb, width, {cx, cy, border_w, cell_h_}, focus);
    DrawRect(fb, width, {cx + cell_w_ - border_w, cy, border_w, cell_h_}, focus);

    // Also draw small arrow indicator at top-left corner: "▶" or ">"
    const char* cursor_mark = ">";
    if (small_font_) {
        int mark_x = cx + Style::kSpacingXS;
        int mark_y = cy + 1;
        mark_x = (mark_x + 7) & ~7;
        DrawText(fb, width, mark_x, mark_y, cursor_mark, small_font_, focus, grid_y + h_);
    }
}

}  // namespace rawdraw
