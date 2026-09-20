/**
 * @file settings_layout.h
 * @brief The Settings page geometry, as arithmetic rather than as offsets.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Header-only and free of every rawdraw, LVGL and ESP-IDF type on purpose:
 * that is what lets tests/host/test_settings_layout.cc compile these exact
 * functions and assert the invariants directly.
 *
 * Why this file exists
 * --------------------
 * The Settings page was laid out with numbers that happened to line up: a
 * divider at x=90, a pill at x=16 with a width of `divider - 26`, and a label
 * column starting at x=43 with no width limit. That is 16 px of margin on one
 * side and 10 on the other, and a label box of 47 px for section names that
 * measure wider than that, so "Dashboard" was painted across the divider and
 * into the settings rows. The selected-row cursor had the matching problem in
 * the other axis: a fixed 16 px tall against rows whose height varies with how
 * many of them there are.
 *
 * Numbers that happen to line up cannot be tested, because there is nothing to
 * test except the numbers. Numbers derived from a grid can be: the invariants
 * below are properties of the arithmetic, and a future change that breaks one
 * fails a test on the build machine rather than on the glass.
 */

#ifndef RAWDRAW_SETTINGS_LAYOUT_H
#define RAWDRAW_SETTINGS_LAYOUT_H

namespace rawdraw {
namespace layout {

// --------------------------------------------------------------- the grid --

/// Outside the category pill, equal on both sides.
constexpr int kRailMargin = 10;
/// Inside the pill, equal on both sides.
constexpr int kRailPad = 8;
constexpr int kRailIconW = 16;
constexpr int kRailIconGap = 6;
/// The historical rail width, kept as a floor so short label sets look unchanged.
constexpr int kRailMinW = 90;
/// A third of a 400 px panel, kept as a ceiling so the rail can never crowd out
/// the settings rows it is a table of contents for.
constexpr int kRailMaxW = 132;

constexpr int kNavItemH = 44;
constexpr int kNavPillH = 28;

/// Gap between the content column and the selected-row cursor, and its width.
constexpr int kRowCursorGap = 8;
constexpr int kRowCursorW = 3;
/// Shortest cursor worth drawing. Below this it reads as a dot.
constexpr int kRowCursorMinH = 8;

constexpr int Clamp(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

constexpr int Max(int a, int b) { return a > b ? a : b; }
constexpr int Min(int a, int b) { return a < b ? a : b; }

/**
 * @brief Rail width for a given longest section label.
 *
 * @param widest_label_px width of the widest section label at the page font.
 *
 * Derived from the measurement rather than fixed, which is what makes a
 * clipped label impossible instead of merely unlikely. Clamped at both ends so
 * a pathological measurement cannot produce a rail wider than the page.
 */
constexpr int NavRailWidth(int widest_label_px) {
    return Clamp(kRailMargin * 2 + kRailPad * 2 + kRailIconW + kRailIconGap +
                     Max(0, widest_label_px),
                 kRailMinW, kRailMaxW);
}

/// Where the parts of one category row sit, given the rail width.
struct NavPill {
    int x;
    int w;
    int icon_x;
    int label_x;
    int label_max_w;
    /// Right edge of the pill. Never past the divider.
    int right() const { return x + w; }
};

constexpr NavPill NavPillFor(int rail_w) {
    return NavPill{
        kRailMargin,
        rail_w - kRailMargin * 2,
        kRailMargin + kRailPad,
        kRailMargin + kRailPad + kRailIconW + kRailIconGap,
        Max(0, rail_w - kRailMargin * 2 - kRailPad * 2 - kRailIconW - kRailIconGap),
    };
}

/// Vertical placement of one category row inside the rail.
constexpr int NavPillY(int nav_top, int index) {
    return nav_top + index * kNavItemH + (kNavItemH - kNavPillH) / 2;
}

// ------------------------------------------------------------ row cursor ---

/// The solid ink bar marking the selected settings row.
struct RowCursor {
    int x;
    int y;
    int w;
    int h;
};

/**
 * @brief Cursor geometry for a row of height @p row_h centred on @p center_y.
 *
 * The height tracks the row instead of being fixed, and is clamped so the bar
 * never touches the separator above or below it however short the row gets.
 */
constexpr RowCursor RowCursorFor(int content_left, int center_y, int row_h) {
    return RowCursor{
        Max(0, content_left - kRowCursorGap),
        center_y - Clamp(row_h - 12, kRowCursorMinH, Max(kRowCursorMinH, row_h - 4)) / 2,
        kRowCursorW,
        Clamp(row_h - 12, kRowCursorMinH, Max(kRowCursorMinH, row_h - 4)),
    };
}

// -------------------------------------------------------------- row heights --

/**
 * @brief Share @p available_h between @p rows rows, never below one line.
 *
 * Returns @p preferred when the rows fit at their preferred height, and a
 * smaller shared height when they do not. This is what stops a list from
 * running off the bottom of a 300 px panel when a row is added to it.
 */
constexpr int SharedRowHeight(int available_h, int rows, int preferred,
                              int min_row_h) {
    return Min(preferred, Max(min_row_h, available_h / Max(1, rows)));
}

/// Total height a list of @p rows occupies at @p row_h.
constexpr int ListHeight(int rows, int row_h) { return rows * row_h; }

// ---------------------------------------------------------- about dialog ---

/**
 * @brief The About dialog, sized from its contents rather than from a constant.
 *
 * `visible_rows` is the part that matters. A row height has a floor, because
 * below it the text is not readable, and the panel has a height, so there is a
 * number of rows past which they cannot all be shown. Returning that number
 * lets the caller draw what fits and say how many it left out. The alternative
 * is a dialog that quietly paints its last rows off the bottom of the glass,
 * which is exactly what the fixed 210 px version did once the row count went
 * from six to ten.
 */
struct AboutDialog {
    int row_h;
    int height;
    int rows_top;
    int visible_rows;
    /// True when the dialog could not show everything it was given.
    bool truncated;
};

/// Room available for rows, before the dialog's own bottom padding.
constexpr int AboutRowsSpace(int panel_h, int dialog_y, int titlebar_h,
                             int rows_top_gap) {
    return Max(0, panel_h - dialog_y - titlebar_h - rows_top_gap - 10);
}

constexpr int AboutRowHeight(int panel_h, int dialog_y, int titlebar_h,
                             int rows_top_gap, int rows, int line_height,
                             int preferred_row_h) {
    return Max(line_height + 2,
               Min(preferred_row_h,
                   AboutRowsSpace(panel_h, dialog_y, titlebar_h, rows_top_gap) /
                       Max(1, rows)));
}

constexpr AboutDialog AboutDialogFor(int panel_h, int dialog_y, int titlebar_h,
                                     int rows_top_gap, int rows, int line_height,
                                     int preferred_row_h) {
    // Written as one expression per member with the shared sub-expressions
    // repeated, because this has to stay a constant expression in C++17 and a
    // constexpr function cannot hold a local until C++14 relaxations that the
    // ESP-IDF toolchain applies but the host sanitiser build reads strictly.
    return AboutDialog{
        AboutRowHeight(panel_h, dialog_y, titlebar_h, rows_top_gap, rows,
                       line_height, preferred_row_h),
        titlebar_h + rows_top_gap +
            Min(rows,
                AboutRowsSpace(panel_h, dialog_y, titlebar_h, rows_top_gap) /
                    Max(1, AboutRowHeight(panel_h, dialog_y, titlebar_h,
                                          rows_top_gap, rows, line_height,
                                          preferred_row_h))) *
                AboutRowHeight(panel_h, dialog_y, titlebar_h, rows_top_gap, rows,
                               line_height, preferred_row_h) +
            6,
        dialog_y + titlebar_h + rows_top_gap,
        Max(1, Min(rows,
                   AboutRowsSpace(panel_h, dialog_y, titlebar_h, rows_top_gap) /
                       Max(1, AboutRowHeight(panel_h, dialog_y, titlebar_h,
                                             rows_top_gap, rows, line_height,
                                             preferred_row_h)))),
        Min(rows, AboutRowsSpace(panel_h, dialog_y, titlebar_h, rows_top_gap) /
                      Max(1, AboutRowHeight(panel_h, dialog_y, titlebar_h,
                                            rows_top_gap, rows, line_height,
                                            preferred_row_h))) < rows,
    };
}

}  // namespace layout
}  // namespace rawdraw

#endif  // RAWDRAW_SETTINGS_LAYOUT_H
