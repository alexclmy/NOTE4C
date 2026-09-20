/**
 * @file test_settings_layout.cc
 * @brief Host tests for the real Settings page geometry.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The Settings page cannot be rendered on the host: it needs LVGL fonts, a
 * theme, a framebuffer and a panel. Its *arithmetic* can be, and the arithmetic
 * is where the bugs were. So settings_layout.h is free of every one of those
 * types and this file compiles it directly.
 *
 * What is asserted is invariants rather than numbers, because a number that a
 * test copies out of the implementation only checks that nobody typed it
 * twice. "The label box never crosses the divider" and "the cursor never
 * leaves its row" are properties that hold for every input, including the
 * inputs nobody thought of.
 *
 * The two shipped bugs these would have caught:
 *
 *   * A 90 px rail with the label column starting at x=43 gave "Dashboard" a
 *     47 px box, and nothing clipped it, so it was painted across the divider
 *     and over the settings rows.
 *   * A 16 px tall selection cursor against rows that compress to 28 px, and
 *     against a 34 px row, was the wrong height in both directions.
 */

#include "ui/renderers/rawdraw/settings_layout.h"
#include "ui/renderers/rawdraw/settings_menu_order.h"

#include <cstdio>
#include <cstring>

using namespace rawdraw::layout;
namespace smenu = rawdraw::settings_menu;

// ------------------------------------------------------------ mini harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current_test = "";

static void Check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", g_current_test, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define RUN(fn)                                \
    do {                                       \
        g_current_test = #fn;                  \
        const int before = g_failures;         \
        fn();                                  \
        std::printf("%-58s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

/// The panel this firmware runs on. Nothing here is allowed to exceed it.
constexpr int kPanelW = 400;
constexpr int kPanelH = 300;
constexpr int kStatusBarH = 28;
constexpr int kBodyBottom = kPanelH - 3;
constexpr int kTableTop = kStatusBarH + 8;

// ---------------------------------------------------------------- the rail --

static void test_the_rail_never_leaves_its_bounds() {
    // Every plausible measurement, and a few implausible ones.
    for (int label = -50; label <= 400; ++label) {
        const int rail = NavRailWidth(label);
        CHECK(rail >= kRailMinW);
        CHECK(rail <= kRailMaxW);
        // A third of the panel, so the settings rows always have most of it.
        CHECK(rail <= kPanelW / 3);
    }
}

static void test_the_rail_grows_with_the_label_until_it_is_capped() {
    CHECK(NavRailWidth(0) == kRailMinW);
    CHECK(NavRailWidth(10) == kRailMinW);
    // Somewhere in the middle it tracks the measurement exactly.
    const int tracked = kRailMargin * 2 + kRailPad * 2 + kRailIconW + kRailIconGap + 60;
    CHECK(NavRailWidth(60) == tracked);
    CHECK(NavRailWidth(1000) == kRailMaxW);
    // Monotonic: a longer label never produces a narrower rail.
    for (int label = 0; label < 300; ++label) {
        CHECK(NavRailWidth(label) <= NavRailWidth(label + 1));
    }
}

static void test_the_margins_are_equal_on_both_sides() {
    for (int label = 0; label <= 300; label += 7) {
        const int rail = NavRailWidth(label);
        const NavPill pill = NavPillFor(rail);
        const int left_margin = pill.x;
        const int right_margin = rail - pill.right();
        // The whole complaint about the old layout in one assertion: 16 on the
        // left and 10 on the right is not a margin, it is two numbers.
        CHECK(left_margin == right_margin);
        CHECK(left_margin == kRailMargin);
    }
}

static void test_the_label_box_never_crosses_the_divider() {
    for (int label = 0; label <= 300; ++label) {
        const int rail = NavRailWidth(label);
        const NavPill pill = NavPillFor(rail);
        // The label ends inside the pill, the pill ends inside the rail, and
        // the rail ends at the divider. Nothing is painted past it.
        CHECK(pill.label_x + pill.label_max_w <= pill.right() - kRailPad + 1);
        CHECK(pill.right() <= rail);
        CHECK(pill.label_x >= pill.icon_x + kRailIconW);
    }
}

static void test_a_label_that_fits_gets_the_room_it_needs() {
    // The point of measuring: a label short enough to be shown in full has a
    // box at least as wide as it is.
    for (int label = 0; label <= kRailMaxW - kRailMargin * 2 - kRailPad * 2 -
                                    kRailIconW - kRailIconGap;
         ++label) {
        const NavPill pill = NavPillFor(NavRailWidth(label));
        CHECK(pill.label_max_w >= label);
    }
}

static void test_a_label_too_long_for_the_cap_gets_a_positive_box() {
    // It will be truncated by the caller, and truncation needs somewhere to
    // draw. A zero or negative box would draw nothing at all.
    const NavPill pill = NavPillFor(NavRailWidth(1000));
    CHECK(pill.label_max_w > 0);
    CHECK(pill.label_x > 0);
}

static void test_the_icon_column_is_inside_the_pill() {
    for (int label = 0; label <= 300; label += 11) {
        const NavPill pill = NavPillFor(NavRailWidth(label));
        CHECK(pill.icon_x >= pill.x);
        CHECK(pill.icon_x + kRailIconW <= pill.right());
    }
}

static void test_category_rows_stack_without_overlapping() {
    const int nav_top = kStatusBarH + 8;
    for (int i = 0; i + 1 < 8; ++i) {
        const int a = NavPillY(nav_top, i);
        const int b = NavPillY(nav_top, i + 1);
        CHECK(b - a == kNavItemH);
        CHECK(a + kNavPillH <= b);
    }
    // Five sections is what this firmware ships. They fit the panel.
    CHECK(NavPillY(nav_top, 4) + kNavPillH <= kBodyBottom);
}

// -------------------------------------------------------------- the cursor --

static void test_the_selection_cursor_never_leaves_its_row() {
    const int content_left = 106;
    for (int row_h = 8; row_h <= 60; ++row_h) {
        const int center_y = 100;
        const RowCursor cursor = RowCursorFor(content_left, center_y, row_h);
        const int row_top = center_y - row_h / 2;
        const int row_bottom = row_top + row_h;
        CHECK(cursor.y >= row_top);
        CHECK(cursor.y + cursor.h <= row_bottom);
        CHECK(cursor.h >= kRowCursorMinH);
        CHECK(cursor.w == kRowCursorW);
    }
}

static void test_the_cursor_tracks_the_row_height() {
    const RowCursor tall = RowCursorFor(106, 100, 34);
    const RowCursor compressed = RowCursorFor(106, 100, 28);
    // Not the same fixed 16 px in both, which is what it used to be.
    CHECK(tall.h > compressed.h);
    CHECK(tall.h == 22);
    CHECK(compressed.h == 16);
}

static void test_the_cursor_is_never_drawn_off_the_left_edge() {
    // content_left is a divider plus a margin in the real page, but a caller
    // that passed something small must not produce a negative x.
    for (int content_left = 0; content_left <= 20; ++content_left) {
        const RowCursor cursor = RowCursorFor(content_left, 100, 34);
        CHECK(cursor.x >= 0);
    }
}

static void test_the_cursor_sits_between_the_divider_and_the_label() {
    const int rail = NavRailWidth(70);
    const int content_left = rail + 16;
    const RowCursor cursor = RowCursorFor(content_left, 100, 34);
    CHECK(cursor.x > rail);
    CHECK(cursor.x + cursor.w <= content_left);
}

// ------------------------------------------------------------ row heights --

static void test_rows_always_fit_the_pane() {
    const int available = kBodyBottom - kTableTop - 2;
    for (int rows = 1; rows <= 16; ++rows) {
        const int row_h = SharedRowHeight(available, rows, 34, 18);
        // Either they fit, or there are more rows than lines of text will fit
        // on a 300 px panel and the caller has to scroll. The boundary is the
        // minimum row height, and it is explicit rather than emergent.
        CHECK(row_h >= 18);
        CHECK(row_h <= 34);
        if (row_h > 18) {
            CHECK(ListHeight(rows, row_h) <= available);
        }
    }
}

static void test_the_ten_about_rows_fit_a_300_pixel_panel() {
    // This is the regression. The About section carried six rows at a fixed
    // 34 px and the product needs ten; at 34 px the last four were off the
    // bottom of the panel with nothing to say so.
    const int available = kBodyBottom - kTableTop - 2;
    const int row_h = SharedRowHeight(available, 10, 34, 18);
    CHECK(ListHeight(10, row_h) <= available);
    CHECK(kTableTop + ListHeight(10, row_h) <= kBodyBottom);
    CHECK(row_h >= 18);
}

static void test_few_rows_keep_the_comfortable_height() {
    const int available = kBodyBottom - kTableTop - 2;
    CHECK(SharedRowHeight(available, 1, 34, 18) == 34);
    CHECK(SharedRowHeight(available, 5, 34, 18) == 34);
}

static void test_a_row_count_of_zero_does_not_divide_by_zero() {
    CHECK(SharedRowHeight(200, 0, 34, 18) == 34);
}

// ----------------------------------------------------------- about dialog --

static void test_the_about_dialog_always_fits_the_panel() {
    const int dialog_y = kStatusBarH + 30;
    for (int rows = 1; rows <= 40; ++rows) {
        const AboutDialog dialog =
            AboutDialogFor(kPanelH, dialog_y, 28, 12, rows, 16, 24);
        CHECK(dialog_y + dialog.height <= kPanelH);
        CHECK(dialog.row_h >= 18);
        CHECK(dialog.rows_top > dialog_y);
        CHECK(dialog.visible_rows >= 1);
        CHECK(dialog.visible_rows <= rows);
    }
}

static void test_the_about_dialog_grows_with_its_contents() {
    const int dialog_y = kStatusBarH + 30;
    const AboutDialog six = AboutDialogFor(kPanelH, dialog_y, 28, 12, 6, 16, 24);
    const AboutDialog ten = AboutDialogFor(kPanelH, dialog_y, 28, 12, 10, 16, 24);
    CHECK(ten.height >= six.height);
    // And it compresses rather than overflowing once it cannot grow further.
    CHECK(ten.row_h <= six.row_h);
}

static void test_the_about_dialog_rows_end_inside_the_dialog() {
    const int dialog_y = kStatusBarH + 30;
    for (int rows = 1; rows <= 40; ++rows) {
        const AboutDialog dialog =
            AboutDialogFor(kPanelH, dialog_y, 28, 12, rows, 16, 24);
        const int last_row_bottom =
            dialog.rows_top + dialog.visible_rows * dialog.row_h;
        CHECK(last_row_bottom <= dialog_y + dialog.height + 1);
        CHECK(last_row_bottom <= kPanelH);
    }
}

static void test_the_about_dialog_reports_what_it_could_not_show() {
    const int dialog_y = kStatusBarH + 30;
    // The ten rows this firmware ships all fit, so nothing is dropped.
    const AboutDialog ten = AboutDialogFor(kPanelH, dialog_y, 28, 12, 10, 16, 24);
    CHECK(ten.visible_rows == 10);
    CHECK(!ten.truncated);

    // A row count that genuinely cannot fit says so instead of painting the
    // remainder off the bottom of the glass, which is what the fixed 210 px
    // dialog did once the list went from six rows to ten.
    const AboutDialog many = AboutDialogFor(kPanelH, dialog_y, 28, 12, 30, 16, 24);
    CHECK(many.truncated);
    CHECK(many.visible_rows < 30);
    CHECK(many.visible_rows >= 1);
}

// ---------------------------------------------------- the page as a whole --

static void test_the_real_settings_page_geometry_is_consistent() {
    // The five section labels this firmware ships, at a plausible width for
    // the page font. "Dashboard" is the long one, and it is the one that used
    // to be painted across the divider.
    const int dashboard_px = 66;
    const int rail = NavRailWidth(dashboard_px);
    const NavPill pill = NavPillFor(rail);

    CHECK(pill.label_max_w >= dashboard_px);
    CHECK(pill.right() + kRailMargin == rail);

    const int content_left = rail + 16;
    const int content_right = kPanelW - 20;
    CHECK(content_left < content_right);
    // The settings rows keep the larger share of the panel.
    CHECK(content_right - content_left > rail);

    const RowCursor cursor = RowCursorFor(content_left, kTableTop + 17, 34);
    CHECK(cursor.x >= rail);
    CHECK(cursor.x + cursor.w <= content_left);
}

// ------------------------------------------------------- the menu order --

// The reorg's headline promise: the actions the owner reaches for most sit near
// the top, so he is not paying a ~20 s e-paper repaint per row to scroll down
// to them. This asserts the promise as a property of the order table rather
// than trusting a hand count in application.cc.
static void test_frequent_actions_sit_near_the_top() {
    const smenu::Item frequent[] = {
        smenu::Item::Restart,
        smenu::Item::PowerSaving,
        smenu::Item::PairDashboard,
        smenu::Item::BlockLegacyWrites,
    };
    for (smenu::Item it : frequent) {
        CHECK(smenu::IsFrequentAction(it));
        // Strictly above the threshold row: reachable in the first two sections.
        CHECK(smenu::Index(it) < smenu::kFrequentActionRowThreshold);
    }
    // The threshold itself is inside the list, not a number past its end.
    CHECK(smenu::kFrequentActionRowThreshold < smenu::kItemCount);
    // Restart is the very first action (row 1, right under the System header).
    CHECK(smenu::Index(smenu::Item::Restart) == 1);
    // The dashboard actions are hoisted above the network rows they used to sit
    // below — that is the move that saved the scrolling.
    CHECK(smenu::Index(smenu::Item::PairDashboard) < smenu::Index(smenu::Item::Wifi));
    CHECK(smenu::Index(smenu::Item::BlockLegacyWrites) < smenu::Index(smenu::Item::Wifi));
}

// The order must stay coherent: each section header comes before the rows that
// belong to it, and there are exactly the five sections the geometry test also
// assumes. A row that jumped its section, or a sixth section, would break the
// rail geometry as well as the grouping.
static void test_the_menu_order_is_coherent() {
    // Section headers appear in this order and before their members.
    CHECK(smenu::Index(smenu::Item::SystemSection) <
          smenu::Index(smenu::Item::DashboardSection));
    CHECK(smenu::Index(smenu::Item::DashboardSection) <
          smenu::Index(smenu::Item::NetworkSection));
    CHECK(smenu::Index(smenu::Item::NetworkSection) <
          smenu::Index(smenu::Item::GallerySection));
    CHECK(smenu::Index(smenu::Item::GallerySection) <
          smenu::Index(smenu::Item::AboutSection));

    // Members sit under their own section header.
    CHECK(smenu::Index(smenu::Item::Restart) > smenu::Index(smenu::Item::SystemSection));
    CHECK(smenu::Index(smenu::Item::PairDashboard) >
          smenu::Index(smenu::Item::DashboardSection));
    CHECK(smenu::Index(smenu::Item::Wifi) > smenu::Index(smenu::Item::NetworkSection));
    CHECK(smenu::Index(smenu::Item::PhotoTransfer) >
          smenu::Index(smenu::Item::GallerySection));
    CHECK(smenu::Index(smenu::Item::Firmware) > smenu::Index(smenu::Item::AboutSection));

    // Count the section headers by hand and check it against the constant the
    // geometry test also relies on ("five sections is what this firmware ships").
    int sections = 0;
    for (int i = 0; i < smenu::kItemCount; ++i) {
        const smenu::Item it = static_cast<smenu::Item>(i);
        if (it == smenu::Item::SystemSection || it == smenu::Item::DashboardSection ||
            it == smenu::Item::NetworkSection || it == smenu::Item::GallerySection ||
            it == smenu::Item::AboutSection) {
            ++sections;
        }
    }
    CHECK(sections == smenu::kSectionCount);
    CHECK(smenu::kSectionCount == 5);
    // MuteVoice is the trailing row whose index application.cc reads from the
    // vector; it must be the last real row.
    CHECK(smenu::Index(smenu::Item::MuteVoice) == smenu::kItemCount - 1);
}

int main() {
    RUN(test_the_rail_never_leaves_its_bounds);
    RUN(test_the_rail_grows_with_the_label_until_it_is_capped);
    RUN(test_the_margins_are_equal_on_both_sides);
    RUN(test_the_label_box_never_crosses_the_divider);
    RUN(test_a_label_that_fits_gets_the_room_it_needs);
    RUN(test_a_label_too_long_for_the_cap_gets_a_positive_box);
    RUN(test_the_icon_column_is_inside_the_pill);
    RUN(test_category_rows_stack_without_overlapping);

    RUN(test_the_selection_cursor_never_leaves_its_row);
    RUN(test_the_cursor_tracks_the_row_height);
    RUN(test_the_cursor_is_never_drawn_off_the_left_edge);
    RUN(test_the_cursor_sits_between_the_divider_and_the_label);

    RUN(test_rows_always_fit_the_pane);
    RUN(test_the_ten_about_rows_fit_a_300_pixel_panel);
    RUN(test_few_rows_keep_the_comfortable_height);
    RUN(test_a_row_count_of_zero_does_not_divide_by_zero);

    RUN(test_the_about_dialog_always_fits_the_panel);
    RUN(test_the_about_dialog_grows_with_its_contents);
    RUN(test_the_about_dialog_rows_end_inside_the_dialog);
    RUN(test_the_about_dialog_reports_what_it_could_not_show);

    RUN(test_the_real_settings_page_geometry_is_consistent);

    RUN(test_frequent_actions_sit_near_the_top);
    RUN(test_the_menu_order_is_coherent);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
