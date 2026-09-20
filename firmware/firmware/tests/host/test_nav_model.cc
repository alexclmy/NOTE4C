/**
 * @file test_nav_model.cc
 * @brief Host tests for the real nav_model translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This links main/common/nav_model.cc itself, so a change to the shipped
 * mapping either shows up here or stops compiling.
 *
 * The matrix section below has one test function per row of
 * tests/nav-map.json (6 contexts x 7 gestures = 42). That is deliberately
 * repetitive: a loop over a table would let a whole context disappear from the
 * table and from the run without a single failure, and run.sh counts RUN()
 * registrations precisely so silent absences are caught. tests/test_manual.py
 * checks that every nav-map.json row has a matching test function here and a
 * matching line in docs/MANUAL.md.
 */

#include "common/nav_model.h"

#include <cstdio>
#include <cstring>

using namespace nav;

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
        std::printf("%-46s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

// ------------------------------------------------------------- test helpers --

static const char* Str(const Action& a, char* buf, size_t cap) {
    return ActionString(a, buf, cap);
}

/// Assert that @p ctx plus @p event produces exactly @p expected, and that the
/// user is owed feedback for it. Every row owes feedback: a gesture that does
/// nothing and says nothing is indistinguishable from a broken button.
static void ExpectAction(const Context& ctx, Event event, const char* expected, int line) {
    NavModel model;
    model.SetContext(ctx);
    const Action action = model.Decide(event);
    char buf[64];
    Str(action, buf, sizeof(buf));
    ++g_checks;
    if (std::strcmp(buf, expected) != 0) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s + %s: expected \"%s\", got \"%s\"\n",
                    g_current_test, line, PageName(ctx.page), EventName(event),
                    expected, buf);
    }
    Check(action.led_feedback, "action.led_feedback", line);
}

#define EXPECT(ctx, event, expected) ExpectAction((ctx), (event), (expected), __LINE__)

static Context CtxDashboard() {
    Context c;
    c.page = Page::Dashboard;
    return c;
}

static Context CtxGallery() {
    Context c;
    c.page = Page::Gallery;
    return c;
}

static Context CtxSettings() {
    Context c;
    c.page = Page::Settings;
    return c;
}

static Context CtxTransfer() {
    Context c;
    c.page = Page::Transfer;
    c.transfer_active = true;
    return c;
}

static Context CtxQuickSwitchOpen() {
    Context c;
    c.page = Page::Dashboard;
    c.quick_switch_open = true;
    return c;
}

static Context CtxDialogOpen() {
    Context c;
    c.page = Page::Dashboard;
    c.dialog_open = true;
    return c;
}

// =========================================================== matrix: Dashboard

static void test_nav_dashboard_upclick() {
    EXPECT(CtxDashboard(), Event::kUpClick, "switch_page:Settings");
}

static void test_nav_dashboard_downclick() {
    EXPECT(CtxDashboard(), Event::kDownClick, "switch_page:Gallery");
}

static void test_nav_dashboard_bootclick() {
    EXPECT(CtxDashboard(), Event::kBootClick, "open_quick_switch");
}

static void test_nav_dashboard_uplong() {
    EXPECT(CtxDashboard(), Event::kUpLong, "feedback_noop");
}

static void test_nav_dashboard_downlong() {
    EXPECT(CtxDashboard(), Event::kDownLong, "feedback_noop");
}

static void test_nav_dashboard_bootlong() {
    EXPECT(CtxDashboard(), Event::kBootLong, "ptt_arm");
}

static void test_nav_dashboard_combolong() {
    EXPECT(CtxDashboard(), Event::kComboLong, "wifi_config_ap");
}

// ============================================================= matrix: Gallery

static void test_nav_gallery_upclick() {
    EXPECT(CtxGallery(), Event::kUpClick, "list_move:-1");
}

static void test_nav_gallery_downclick() {
    EXPECT(CtxGallery(), Event::kDownClick, "list_move:+1");
}

static void test_nav_gallery_bootclick() {
    EXPECT(CtxGallery(), Event::kBootClick, "select");
}

static void test_nav_gallery_uplong() {
    EXPECT(CtxGallery(), Event::kUpLong, "switch_page:Dashboard");
}

static void test_nav_gallery_downlong() {
    EXPECT(CtxGallery(), Event::kDownLong, "home");
}

static void test_nav_gallery_bootlong() {
    EXPECT(CtxGallery(), Event::kBootLong, "ptt_arm");
}

static void test_nav_gallery_combolong() {
    EXPECT(CtxGallery(), Event::kComboLong, "wifi_config_ap");
}

// ============================================================ matrix: Settings

static void test_nav_settings_upclick() {
    EXPECT(CtxSettings(), Event::kUpClick, "list_move:-1");
}

static void test_nav_settings_downclick() {
    EXPECT(CtxSettings(), Event::kDownClick, "list_move:+1");
}

static void test_nav_settings_bootclick() {
    EXPECT(CtxSettings(), Event::kBootClick, "select");
}

static void test_nav_settings_uplong() {
    EXPECT(CtxSettings(), Event::kUpLong, "switch_page:Dashboard");
}

static void test_nav_settings_downlong() {
    EXPECT(CtxSettings(), Event::kDownLong, "home");
}

static void test_nav_settings_bootlong() {
    EXPECT(CtxSettings(), Event::kBootLong, "ptt_arm");
}

static void test_nav_settings_combolong() {
    EXPECT(CtxSettings(), Event::kComboLong, "wifi_config_ap");
}

// ============================================================ matrix: Transfer

static void test_nav_transfer_upclick() {
    EXPECT(CtxTransfer(), Event::kUpClick, "feedback_noop");
}

static void test_nav_transfer_downclick() {
    EXPECT(CtxTransfer(), Event::kDownClick, "feedback_noop");
}

static void test_nav_transfer_bootclick() {
    EXPECT(CtxTransfer(), Event::kBootClick, "repaint_page");
}

static void test_nav_transfer_uplong() {
    EXPECT(CtxTransfer(), Event::kUpLong, "exit_transfer:Settings");
}

static void test_nav_transfer_downlong() {
    EXPECT(CtxTransfer(), Event::kDownLong, "home");
}

static void test_nav_transfer_bootlong() {
    // The gesture that used to leave AP transfer (rawdraw_ui_manager.cc:768-775)
    // is now reserved. Back and Home are the documented exits.
    EXPECT(CtxTransfer(), Event::kBootLong, "ptt_arm");
}

static void test_nav_transfer_combolong() {
    EXPECT(CtxTransfer(), Event::kComboLong, "wifi_config_ap");
}

// ===================================================== matrix: QuickSwitchOpen

static void test_nav_quickswitchopen_upclick() {
    EXPECT(CtxQuickSwitchOpen(), Event::kUpClick, "quick_switch_move:-1");
}

static void test_nav_quickswitchopen_downclick() {
    EXPECT(CtxQuickSwitchOpen(), Event::kDownClick, "quick_switch_move:+1");
}

static void test_nav_quickswitchopen_bootclick() {
    EXPECT(CtxQuickSwitchOpen(), Event::kBootClick, "quick_switch_confirm");
}

static void test_nav_quickswitchopen_uplong() {
    EXPECT(CtxQuickSwitchOpen(), Event::kUpLong, "close_quick_switch");
}

static void test_nav_quickswitchopen_downlong() {
    EXPECT(CtxQuickSwitchOpen(), Event::kDownLong, "home");
}

static void test_nav_quickswitchopen_bootlong() {
    EXPECT(CtxQuickSwitchOpen(), Event::kBootLong, "ptt_arm");
}

static void test_nav_quickswitchopen_combolong() {
    EXPECT(CtxQuickSwitchOpen(), Event::kComboLong, "wifi_config_ap");
}

// ========================================================== matrix: DialogOpen

static void test_nav_dialogopen_upclick() {
    EXPECT(CtxDialogOpen(), Event::kUpClick, "dialog_move:-1");
}

static void test_nav_dialogopen_downclick() {
    EXPECT(CtxDialogOpen(), Event::kDownClick, "dialog_move:+1");
}

static void test_nav_dialogopen_bootclick() {
    EXPECT(CtxDialogOpen(), Event::kBootClick, "dialog_confirm");
}

static void test_nav_dialogopen_uplong() {
    EXPECT(CtxDialogOpen(), Event::kUpLong, "close_dialog");
}

static void test_nav_dialogopen_downlong() {
    EXPECT(CtxDialogOpen(), Event::kDownLong, "home");
}

static void test_nav_dialogopen_bootlong() {
    EXPECT(CtxDialogOpen(), Event::kBootLong, "ptt_arm");
}

static void test_nav_dialogopen_combolong() {
    EXPECT(CtxDialogOpen(), Event::kComboLong, "wifi_config_ap");
}

// ------------------------------------------------------------------- the ring

static void test_ring_wraps_in_both_directions() {
    CHECK(RingStep(Page::Dashboard, +1) == Page::Gallery);
    CHECK(RingStep(Page::Gallery, +1) == Page::Settings);
    CHECK(RingStep(Page::Settings, +1) == Page::Dashboard);
    CHECK(RingStep(Page::Dashboard, -1) == Page::Settings);
    CHECK(RingStep(Page::Settings, -1) == Page::Gallery);
    CHECK(RingStep(Page::Gallery, -1) == Page::Dashboard);
}

static void test_ring_excludes_transfer() {
    // Transfer is modal, not a ring member. Asking for its neighbour must not
    // read outside the table.
    CHECK(RingStep(Page::Transfer, +1) == Page::Gallery);
    CHECK(RingStep(Page::Transfer, -1) == Page::Settings);
}

static void test_gallery_is_reachable_by_button_from_dashboard() {
    // The defect this milestone fixes: before it, no button sequence reached
    // Gallery at all (quick switch opened only on a double click the board
    // never delivers).
    NavModel model;
    model.SetPage(Page::Dashboard);
    const Action a = model.Decide(Event::kDownClick);
    CHECK(a.type == ActionType::kSwitchPage);
    CHECK(a.page == Page::Gallery);
}

// ------------------------------------------------------------- back and home

static void test_back_pops_dialog_before_quick_switch() {
    Context c = CtxDashboard();
    c.dialog_open = true;
    c.quick_switch_open = true;
    c.fullscreen_photo = true;
    c.transfer_active = true;
    EXPECT(c, Event::kUpLong, "close_dialog");
}

static void test_back_pops_quick_switch_before_fullscreen() {
    Context c = CtxGallery();
    c.quick_switch_open = true;
    c.fullscreen_photo = true;
    c.transfer_active = true;
    EXPECT(c, Event::kUpLong, "close_quick_switch");
}

static void test_back_pops_fullscreen_before_transfer() {
    Context c = CtxGallery();
    c.fullscreen_photo = true;
    c.transfer_active = true;
    EXPECT(c, Event::kUpLong, "close_fullscreen");
}

static void test_back_pops_transfer_before_page() {
    Context c = CtxGallery();
    c.transfer_active = true;
    EXPECT(c, Event::kUpLong, "exit_transfer:Settings");
}

static void test_back_from_plain_page_goes_to_dashboard() {
    EXPECT(CtxSettings(), Event::kUpLong, "switch_page:Dashboard");
}

static void test_back_on_bare_dashboard_is_feedback_only() {
    EXPECT(CtxDashboard(), Event::kUpLong, "feedback_noop");
}

static void test_home_works_from_every_context() {
    EXPECT(CtxGallery(), Event::kDownLong, "home");
    EXPECT(CtxSettings(), Event::kDownLong, "home");
    EXPECT(CtxTransfer(), Event::kDownLong, "home");
    EXPECT(CtxQuickSwitchOpen(), Event::kDownLong, "home");
    EXPECT(CtxDialogOpen(), Event::kDownLong, "home");
    Context fullscreen = CtxGallery();
    fullscreen.fullscreen_photo = true;
    EXPECT(fullscreen, Event::kDownLong, "home");
}

static void test_home_on_dashboard_with_overlay_is_not_a_noop() {
    // Bare Dashboard is already home, but an open overlay means there is
    // still something to close.
    EXPECT(CtxQuickSwitchOpen(), Event::kDownLong, "home");
}

static void test_transfer_running_off_page_still_backs_out_of_transfer() {
    // A background update can move the visible page while the transfer server
    // is still up; Back must still stop the transfer rather than page-hop.
    Context c = CtxDashboard();
    c.transfer_active = true;
    EXPECT(c, Event::kUpLong, "exit_transfer:Settings");
}

// ------------------------------------------------------- the busy-refresh latch

static void test_busy_click_is_latched_not_dropped() {
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    const Action a = model.Decide(Event::kDownClick);
    CHECK(a.type == ActionType::kPendingLatched);
    CHECK(a.led_feedback);
    CHECK(model.HasPendingIntent());
}

static void test_busy_latch_applies_on_refresh_idle() {
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kDownClick);
    const Action applied = model.OnRefreshIdle();
    CHECK(applied.type == ActionType::kSwitchPage);
    CHECK(applied.page == Page::Gallery);
    CHECK(!model.HasPendingIntent());
    CHECK(!model.context().refresh_busy);
}

static void test_busy_latch_coalesces_to_the_newest_click() {
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kDownClick);
    model.Decide(Event::kDownClick);
    model.Decide(Event::kUpClick);
    CHECK(model.PendingIntent() == Event::kUpClick);
    const Action applied = model.OnRefreshIdle();
    CHECK(applied.type == ActionType::kSwitchPage);
    CHECK(applied.page == Page::Settings);
    // Depth 1: one intent in, one action out, nothing queued behind it.
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

static void test_refresh_idle_without_pending_does_nothing() {
    NavModel model;
    const Action a = model.OnRefreshIdle();
    CHECK(a.type == ActionType::kNone);
}

static void test_busy_does_not_latch_long_presses() {
    NavModel model;
    Context c = CtxSettings();
    c.refresh_busy = true;
    model.SetContext(c);
    EXPECT(c, Event::kUpLong, "switch_page:Dashboard");
    const Action a = model.Decide(Event::kUpLong);
    CHECK(a.type == ActionType::kSwitchPage);
    CHECK(!model.HasPendingIntent());
}

static void test_busy_never_delays_ptt() {
    // Voice feedback must never wait for the panel (PRODUCT-PLAN.md 1.2).
    Context c = CtxDashboard();
    c.refresh_busy = true;
    EXPECT(c, Event::kBootLong, "ptt_arm");
}

static void test_busy_never_delays_the_wifi_combo() {
    Context c = CtxDashboard();
    c.refresh_busy = true;
    EXPECT(c, Event::kComboLong, "wifi_config_ap");
}

static void test_back_discards_a_latched_click() {
    NavModel model;
    Context c = CtxSettings();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kDownClick);
    CHECK(model.HasPendingIntent());
    model.Decide(Event::kUpLong);
    CHECK(!model.HasPendingIntent());
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

static void test_home_discards_a_latched_click() {
    NavModel model;
    Context c = CtxGallery();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kUpClick);
    CHECK(model.HasPendingIntent());
    model.Decide(Event::kDownLong);
    CHECK(!model.HasPendingIntent());
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

static void test_a_click_handled_outside_the_refresh_discards_the_latch() {
    // The regression. Decide() used to clear the latch only for Back and Home,
    // so a click latched during a refresh survived a later click that ran
    // immediately, and was then replayed at the next refresh-idle: two page
    // changes for one press, in the wrong order.
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kDownClick);
    CHECK(model.HasPendingIntent());

    // The panel is free again and the user clicks once more.
    c.refresh_busy = false;
    model.SetContext(c);
    const Action immediate = model.Decide(Event::kUpClick);
    CHECK(immediate.type == ActionType::kSwitchPage);
    CHECK(!model.HasPendingIntent());
    // Nothing left over to replay.
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

static void test_a_long_press_outside_the_refresh_still_discards_the_latch() {
    NavModel model;
    Context c = CtxSettings();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kBootClick);
    CHECK(model.HasPendingIntent());
    c.refresh_busy = false;
    model.SetContext(c);
    model.Decide(Event::kBootLong);
    CHECK(!model.HasPendingIntent());
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

static void test_depth_one_holds_while_the_panel_stays_busy() {
    // Clearing the latch on every non-busy Decide() must not weaken the latch
    // itself: while the refresh is still running, the newest click still wins
    // and nothing is dropped.
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kUpClick);
    model.Decide(Event::kDownClick);
    CHECK(model.HasPendingIntent());
    CHECK(model.PendingIntent() == Event::kDownClick);
    const Action applied = model.OnRefreshIdle();
    CHECK(applied.type == ActionType::kSwitchPage);
    CHECK(applied.page == Page::Gallery);
    CHECK(!model.HasPendingIntent());
}

static void test_pending_intent_resolves_against_state_at_apply_time() {
    // The latch stores the gesture, not the conclusion. If the context moved
    // while the panel was busy, the gesture means what it means then.
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kDownClick);
    model.SetPage(Page::Gallery);
    const Action applied = model.OnRefreshIdle();
    CHECK(applied.type == ActionType::kListMove);
    CHECK(applied.delta == +1);
}

static void test_clear_pending_intent_is_explicit() {
    NavModel model;
    Context c = CtxDashboard();
    c.refresh_busy = true;
    model.SetContext(c);
    model.Decide(Event::kUpClick);
    model.ClearPendingIntent();
    CHECK(!model.HasPendingIntent());
    CHECK(model.OnRefreshIdle().type == ActionType::kNone);
}

// ------------------------------------------------------------- action strings

static void test_action_strings_match_the_nav_map_vocabulary() {
    char buf[64];
    Action a;
    a.type = ActionType::kSwitchPage;
    a.page = Page::Settings;
    CHECK(std::strcmp(Str(a, buf, sizeof(buf)), "switch_page:Settings") == 0);
    a = Action{};
    a.type = ActionType::kListMove;
    a.delta = -1;
    CHECK(std::strcmp(Str(a, buf, sizeof(buf)), "list_move:-1") == 0);
    a.delta = +1;
    CHECK(std::strcmp(Str(a, buf, sizeof(buf)), "list_move:+1") == 0);
    a = Action{};
    a.type = ActionType::kHome;
    CHECK(std::strcmp(Str(a, buf, sizeof(buf)), "home") == 0);
    a.type = ActionType::kNone;
    CHECK(std::strcmp(Str(a, buf, sizeof(buf)), "none") == 0);
}

static void test_action_string_truncates_rather_than_overflows() {
    char buf[8];
    Action a;
    a.type = ActionType::kSwitchPage;
    a.page = Page::Dashboard;
    Str(a, buf, sizeof(buf));
    CHECK(std::strlen(buf) < sizeof(buf));
    // A zero capacity must not be written to at all.
    char guard[2] = {'\x7f', '\x7f'};
    Str(a, guard, 0);
    CHECK(guard[0] == '\x7f');
}

static void test_page_and_event_names_are_total() {
    CHECK(std::strcmp(PageName(Page::Dashboard), "Dashboard") == 0);
    CHECK(std::strcmp(PageName(Page::Transfer), "Transfer") == 0);
    CHECK(std::strcmp(EventName(Event::kComboLong), "ComboLong") == 0);
    CHECK(std::strcmp(EventName(Event::kBootLong), "BootLong") == 0);
}

// ------------------------------------------------------------- offline status

static void test_status_reports_wifi_down_first() {
    StatusInputs in;
    in.wifi_connected = false;
    in.paired = true;
    in.has_stored_frame = true;
    const StatusLines s = StatusLine(in);
    CHECK(std::strcmp(s.primary, "Waiting for Wi-Fi.") == 0);
    CHECK(std::strlen(s.secondary) > 0);
}

static void test_status_reports_unpaired_before_missing_frame() {
    StatusInputs in;
    in.wifi_connected = true;
    in.paired = false;
    in.has_stored_frame = false;
    CHECK(std::strcmp(StatusLine(in).primary, "Not paired yet.") == 0);
}

static void test_status_reports_waiting_for_the_first_frame() {
    StatusInputs in;
    in.wifi_connected = true;
    in.paired = true;
    in.has_stored_frame = false;
    const StatusLines s = StatusLine(in);
    CHECK(std::strcmp(s.primary, "Paired. Waiting for the first frame.") == 0);
    // Never claim the device can fetch.
    CHECK(std::strstr(s.secondary, "never fetches") != nullptr);
}

static void test_status_says_lan_service_off_when_it_is_off() {
    StatusInputs in;
    in.wifi_connected = true;
    in.paired = true;
    in.has_stored_frame = true;
    in.lan_service_running = false;
    const StatusLines s = StatusLine(in);
    CHECK(std::strstr(s.secondary, "LAN service is off") != nullptr);
    in.lan_service_running = true;
    CHECK(std::strstr(StatusLine(in).secondary, "LAN service is on") != nullptr);
}

static void test_status_reports_transfer_before_anything_else() {
    StatusInputs in;
    in.transfer_active = true;
    in.wifi_connected = false;
    in.paired = false;
    CHECK(std::strcmp(StatusLine(in).primary, "Photo transfer is running.") == 0);
}

static void test_the_shared_hints_never_name_boot() {
    // BOOT long is reserved for push-to-talk and exits nothing, so a hint that
    // tells the user to hold BOOT sends them to a dead button. Three modal
    // screens said exactly that until these constants replaced their literals;
    // tests/test_manual.py enforces the same rule on the renderer sources.
    CHECK(std::strstr(kExitHint, "BOOT") == nullptr);
    CHECK(std::strstr(kWifiSetupHint, "BOOT") == nullptr);
    // The way out really is Back and Home, so the hint has to name both.
    CHECK(std::strstr(kExitHint, "UP") != nullptr);
    CHECK(std::strstr(kExitHint, "DOWN") != nullptr);
}

static void test_the_transfer_status_line_uses_the_shared_exit_hint() {
    StatusInputs in;
    in.transfer_active = true;
    CHECK(std::strcmp(StatusLine(in).secondary, kExitHint) == 0);
}

static void test_status_never_claims_a_refresh_from_source() {
    // The honest-UI rule, asserted on the strings themselves rather than left
    // to review: nothing here may suggest the device pulled anything.
    const char* forbidden[] = {"Updating", "Fetching", "Downloading", "Refreshed from"};
    StatusInputs in;
    for (int wifi = 0; wifi < 2; ++wifi) {
        for (int paired = 0; paired < 2; ++paired) {
            for (int frame = 0; frame < 2; ++frame) {
                for (int lan = 0; lan < 2; ++lan) {
                    for (int transfer = 0; transfer < 2; ++transfer) {
                        in.wifi_connected = wifi != 0;
                        in.paired = paired != 0;
                        in.has_stored_frame = frame != 0;
                        in.lan_service_running = lan != 0;
                        in.transfer_active = transfer != 0;
                        const StatusLines s = StatusLine(in);
                        CHECK(std::strlen(s.primary) > 0);
                        for (const char* bad : forbidden) {
                            CHECK(std::strstr(s.primary, bad) == nullptr);
                            CHECK(std::strstr(s.secondary, bad) == nullptr);
                        }
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------- slideshow liveness --

/**
 * The defect this predicate was extracted for.
 *
 * power::WakeInputs::slideshow_active refuses deep sleep ahead of the mode, and
 * it was being fed "is an interval configured" — which is true on every device
 * that has not had the slideshow turned off, and the shipped default is five
 * minutes. An auto-saver device sitting on the dashboard therefore never slept
 * at all, and a product whose promise is an hourly refresh on battery ran
 * itself flat instead.
 */
static void test_a_configured_interval_alone_is_not_a_running_slideshow() {
    SlideshowInputs in;
    in.interval_minutes = 5;           // the shipped default
    in.on_gallery_page = false;        // the dashboard is on the panel
    in.fullscreen = false;
    CHECK(!SlideshowIsRunning(in));
}

static void test_a_slideshow_is_running_only_in_gallery_fullscreen() {
    SlideshowInputs in;
    in.interval_minutes = 5;
    in.on_gallery_page = true;
    in.fullscreen = true;
    CHECK(SlideshowIsRunning(in));

    // Every one of these is a condition AdvanceGallerySlideshow() checks before
    // it advances anything. If it would not advance, it is not a slideshow.
    SlideshowInputs off = in;
    off.interval_minutes = 0;
    CHECK(!SlideshowIsRunning(off));

    SlideshowInputs grid = in;
    grid.fullscreen = false;
    CHECK(!SlideshowIsRunning(grid));

    SlideshowInputs dialog = in;
    dialog.dialog_open = true;
    CHECK(!SlideshowIsRunning(dialog));

    SlideshowInputs elsewhere = in;
    elsewhere.on_gallery_page = false;
    CHECK(!SlideshowIsRunning(elsewhere));
}

static void test_a_negative_interval_is_off_rather_than_forever() {
    SlideshowInputs in;
    in.interval_minutes = -5;
    in.on_gallery_page = true;
    in.fullscreen = true;
    CHECK(!SlideshowIsRunning(in));
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("nav_model host tests (real firmware translation unit)\n\n");

    RUN(test_nav_dashboard_upclick);
    RUN(test_nav_dashboard_downclick);
    RUN(test_nav_dashboard_bootclick);
    RUN(test_nav_dashboard_uplong);
    RUN(test_nav_dashboard_downlong);
    RUN(test_nav_dashboard_bootlong);
    RUN(test_nav_dashboard_combolong);

    RUN(test_nav_gallery_upclick);
    RUN(test_nav_gallery_downclick);
    RUN(test_nav_gallery_bootclick);
    RUN(test_nav_gallery_uplong);
    RUN(test_nav_gallery_downlong);
    RUN(test_nav_gallery_bootlong);
    RUN(test_nav_gallery_combolong);

    RUN(test_nav_settings_upclick);
    RUN(test_nav_settings_downclick);
    RUN(test_nav_settings_bootclick);
    RUN(test_nav_settings_uplong);
    RUN(test_nav_settings_downlong);
    RUN(test_nav_settings_bootlong);
    RUN(test_nav_settings_combolong);

    RUN(test_nav_transfer_upclick);
    RUN(test_nav_transfer_downclick);
    RUN(test_nav_transfer_bootclick);
    RUN(test_nav_transfer_uplong);
    RUN(test_nav_transfer_downlong);
    RUN(test_nav_transfer_bootlong);
    RUN(test_nav_transfer_combolong);

    RUN(test_nav_quickswitchopen_upclick);
    RUN(test_nav_quickswitchopen_downclick);
    RUN(test_nav_quickswitchopen_bootclick);
    RUN(test_nav_quickswitchopen_uplong);
    RUN(test_nav_quickswitchopen_downlong);
    RUN(test_nav_quickswitchopen_bootlong);
    RUN(test_nav_quickswitchopen_combolong);

    RUN(test_nav_dialogopen_upclick);
    RUN(test_nav_dialogopen_downclick);
    RUN(test_nav_dialogopen_bootclick);
    RUN(test_nav_dialogopen_uplong);
    RUN(test_nav_dialogopen_downlong);
    RUN(test_nav_dialogopen_bootlong);
    RUN(test_nav_dialogopen_combolong);

    RUN(test_ring_wraps_in_both_directions);
    RUN(test_ring_excludes_transfer);
    RUN(test_gallery_is_reachable_by_button_from_dashboard);

    RUN(test_back_pops_dialog_before_quick_switch);
    RUN(test_back_pops_quick_switch_before_fullscreen);
    RUN(test_back_pops_fullscreen_before_transfer);
    RUN(test_back_pops_transfer_before_page);
    RUN(test_back_from_plain_page_goes_to_dashboard);
    RUN(test_back_on_bare_dashboard_is_feedback_only);
    RUN(test_home_works_from_every_context);
    RUN(test_home_on_dashboard_with_overlay_is_not_a_noop);
    RUN(test_transfer_running_off_page_still_backs_out_of_transfer);

    RUN(test_busy_click_is_latched_not_dropped);
    RUN(test_busy_latch_applies_on_refresh_idle);
    RUN(test_busy_latch_coalesces_to_the_newest_click);
    RUN(test_refresh_idle_without_pending_does_nothing);
    RUN(test_busy_does_not_latch_long_presses);
    RUN(test_busy_never_delays_ptt);
    RUN(test_busy_never_delays_the_wifi_combo);
    RUN(test_back_discards_a_latched_click);
    RUN(test_home_discards_a_latched_click);
    RUN(test_a_click_handled_outside_the_refresh_discards_the_latch);
    RUN(test_a_long_press_outside_the_refresh_still_discards_the_latch);
    RUN(test_depth_one_holds_while_the_panel_stays_busy);
    RUN(test_pending_intent_resolves_against_state_at_apply_time);
    RUN(test_clear_pending_intent_is_explicit);

    RUN(test_action_strings_match_the_nav_map_vocabulary);
    RUN(test_action_string_truncates_rather_than_overflows);
    RUN(test_page_and_event_names_are_total);

    RUN(test_status_reports_wifi_down_first);
    RUN(test_status_reports_unpaired_before_missing_frame);
    RUN(test_status_reports_waiting_for_the_first_frame);
    RUN(test_status_says_lan_service_off_when_it_is_off);
    RUN(test_status_reports_transfer_before_anything_else);
    RUN(test_the_shared_hints_never_name_boot);
    RUN(test_the_transfer_status_line_uses_the_shared_exit_hint);
    RUN(test_status_never_claims_a_refresh_from_source);

    RUN(test_a_configured_interval_alone_is_not_a_running_slideshow);
    RUN(test_a_slideshow_is_running_only_in_gallery_fullscreen);
    RUN(test_a_negative_interval_is_off_rather_than_forever);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
