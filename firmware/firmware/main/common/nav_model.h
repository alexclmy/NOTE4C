/**
 * @file nav_model.h
 * @brief Portable navigation model: button events plus context to UI actions.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Like dashboard_slot and dashboard_service, this translation unit contains no
 * ESP-IDF, FreeRTOS or display headers, so the host tests exercise the exact
 * code that ships. The device glue in main/ui/rawdraw_ui_manager.cc and
 * main/application.cc decides *how* to carry out an action; this file decides
 * *which* action a gesture means.
 *
 * Why a separate model at all: before this, the mapping lived in three places
 * (main/application.cc:455-525 long presses, main/ui/rawdraw_ui_manager.cc:768-783
 * BOOT-long AP transfer, main/ui/rawdraw_ui_manager.cc:815 quick switch), each
 * with its own idea of what "back" means, and none of it host-testable. Two
 * consequences of that split were user-visible defects:
 *
 *  1. Gallery was unreachable by button. The quick-switch overlay only opened
 *     on kUpDoubleClick (rawdraw_ui_manager.cc:815) and OnDoubleClick is never
 *     registered by the board (zectrix-s3-epaper-4.2.cc:349-465), so the only
 *     paths into Gallery were HTTP and leaving AP transfer.
 *  2. A navigation click arriving during a panel refresh was silently dropped
 *     (rawdraw_ui_manager.cc:761-766). On a panel whose worst case is minutes,
 *     that reads as a dead button.
 *
 * Both are fixed here: BOOT click on the Dashboard opens the quick switch, and
 * a click during a refresh is latched as a depth-1 pending intent applied when
 * the refresh completes (the same coalescing philosophy as
 * dashboard::RefreshCoordinator).
 *
 * The single source of truth for the resulting table is
 * upstream/firmware/tests/nav-map.json. Every row there is asserted by
 * tests/host/test_nav_model.cc and required to appear in docs/MANUAL.md by
 * tests/test_manual.py.
 */

#ifndef COMMON_NAV_MODEL_H
#define COMMON_NAV_MODEL_H

#include <stddef.h>

namespace nav {

/// Pages that physical navigation can reach. This is deliberately a smaller
/// set than ui::RawDrawPageId: the legacy content pages are not compiled in the
/// shipped configuration (main/dashboard_build_config.h) and have no gestures.
enum class Page {
    Dashboard = 0,
    Gallery,
    Settings,
    Transfer,
};

/// Semantic button events, after the board-level gesture recogniser
/// (common/button_gestures.h) has resolved clicks, long presses and the combo.
enum class Event {
    kUpClick = 0,
    kDownClick,
    kBootClick,
    kUpLong,
    kDownLong,
    kBootLong,
    kComboLong,
};

enum class ActionType {
    /// Nothing to do and nothing to say. Not currently produced by Decide();
    /// it is the value of an empty pending intent.
    kNone = 0,
    /// Change the visible page. Action::page carries the target.
    kSwitchPage,
    /// Move a list selection by Action::delta (-1 or +1).
    kListMove,
    /// Activate the selected item on the current page.
    kSelect,
    /// Open the quick-switch overlay.
    kOpenQuickSwitch,
    /// Close the quick-switch overlay, leaving the underlying page alone.
    kCloseQuickSwitch,
    /// Move the quick-switch selection by Action::delta.
    kQuickSwitchMove,
    /// Activate the quick-switch selection.
    kQuickSwitchConfirm,
    /// Move a modal dialog selection by Action::delta.
    kDialogMove,
    /// Confirm a modal dialog.
    kDialogConfirm,
    /// Dismiss a modal dialog.
    kCloseDialog,
    /// Leave the gallery fullscreen photo view for the gallery grid.
    kCloseFullscreen,
    /// Leave AP photo transfer cleanly. Action::page carries where to land.
    kExitTransfer,
    /// Close every overlay, stop transfer, and show the Dashboard.
    kHome,
    /// Redraw the current page from what is already stored on the device.
    /// Never a fetch: the device has no route to the composer
    /// (main/application.cc:487-493).
    kRepaintPage,
    /// Enter the Wi-Fi configuration access point.
    kWifiConfigAp,
    /// Reserved gesture: hand BOOT-long to the audio state machine.
    /// With VOICE_PTT_ENABLED off this is a logged, earcon-only no-op.
    kPttArm,
    /// The gesture was understood but the state has nothing to change.
    /// Feedback is still owed to the user, hence an action rather than false.
    kFeedbackNoop,
    /// A refresh is in progress; the event is held as the pending intent and
    /// will be resolved when OnRefreshIdle() fires.
    kPendingLatched,
};

struct Action {
    ActionType type = ActionType::kNone;
    /// Target for kSwitchPage and kExitTransfer.
    Page page = Page::Dashboard;
    /// -1 or +1 for the kListMove / kQuickSwitchMove / kDialogMove family.
    int delta = 0;
    /// Whether the glue owes the user immediate physical feedback (the
    /// activity LED). True for everything the user initiated, including
    /// no-ops and latched intents: a panel that will not change for another
    /// minute must not also look like a dead button.
    bool led_feedback = true;
};

/// Context the model tracks. The glue mirrors its own state into these
/// setters; the model never reads device state directly.
struct Context {
    Page page = Page::Dashboard;
    bool quick_switch_open = false;
    /// Reserved. No component in this firmware opens a modal dialog yet; the
    /// rows exist so the contract is fixed before one does.
    bool dialog_open = false;
    /// Gallery fullscreen photo view (photo_gallery_renderer_->IsFullscreenMode()).
    bool fullscreen_photo = false;
    /// AP photo transfer session is running. Set independently of `page`
    /// because a background update can move the visible page while the
    /// transfer server is still up (the case rawdraw_ui_manager.cc:769-775
    /// handles today).
    bool transfer_active = false;
    /// Panel refresh in flight; navigation clicks latch instead of applying.
    bool refresh_busy = false;
};

/**
 * @brief Resolves gestures against context. No I/O, no allocation, no clock.
 *
 * Rules, in the order Decide() applies them:
 *
 *  1. kComboLong and kBootLong are never latched and never contextual.
 *     The combo is Wi-Fi config; BOOT-long is reserved exclusively for
 *     push-to-talk, so no page may bind it (PRODUCT-PLAN.md section 3.1).
 *  2. While `refresh_busy`, a click (UP/DOWN/BOOT) becomes a depth-1 pending
 *     intent. Long presses pass through, matching the pre-existing behaviour
 *     where only navigation clicks were gated (rawdraw_ui_manager.cc:762).
 *  3. UP-long is Back and pops the innermost context, in this order:
 *     dialog, quick switch, fullscreen photo, transfer, non-Dashboard page.
 *     On a bare Dashboard it is a feedback-only no-op.
 *  4. DOWN-long is Home from anywhere; on a bare Dashboard, a no-op.
 *  5. Clicks are per-page: ring movement on the Dashboard, list movement on
 *     Gallery and Settings, no-ops on the Transfer instructions page.
 */
class NavModel {
public:
    /// Ring order used by UP/DOWN clicks on the Dashboard, wrap-around.
    /// Transfer is deliberately absent: it is modal, entered from Settings.
    static constexpr size_t kRingSize = 3;

    NavModel() = default;

    const Context& context() const { return ctx_; }
    void SetContext(const Context& ctx) { ctx_ = ctx; }

    void SetPage(Page page) { ctx_.page = page; }
    void SetQuickSwitchOpen(bool open) { ctx_.quick_switch_open = open; }
    void SetDialogOpen(bool open) { ctx_.dialog_open = open; }
    void SetFullscreenPhoto(bool on) { ctx_.fullscreen_photo = on; }
    void SetTransferActive(bool on) { ctx_.transfer_active = on; }
    void SetRefreshBusy(bool busy) { ctx_.refresh_busy = busy; }

    /// Resolve one gesture. Pure with respect to everything except the
    /// pending-intent latch.
    Action Decide(Event event);

    /// Resolve a gesture ignoring the busy latch. Exposed so the glue and the
    /// tests can ask "what would this mean once the panel is free?".
    Action Resolve(Event event) const;

    /// True when a click is being held for the end of the current refresh.
    bool HasPendingIntent() const { return has_pending_; }
    Event PendingIntent() const { return pending_; }

    /// Drop a latched intent without applying it (Home and Back are immediate
    /// and must not be followed by a stale page change).
    void ClearPendingIntent() { has_pending_ = false; }

    /**
     * @brief The panel finished; resolve and hand back the latched intent.
     *
     * Returns kNone when nothing was latched. Clears `refresh_busy` as well,
     * because the single caller (the refresh-idle hook) is exactly the event
     * that unlocks input (rawdraw_ui_manager.cc:392-395).
     */
    Action OnRefreshIdle();

private:
    Action ResolveBack() const;
    Action ResolveClick(Event event) const;

    Context ctx_{};
    Event pending_ = Event::kUpClick;
    bool has_pending_ = false;
};

/// Ring neighbour, wrap-around. `delta` is -1 (previous) or +1 (next).
Page RingStep(Page page, int delta);

/**
 * @brief The one phrasing for "how do I get out of this screen".
 *
 * Every modal screen used to write its own exit hint, and three of them still
 * said "Hold BOOT to exit" after BOOT-long had been reserved exclusively for
 * push-to-talk. A user following that instruction held BOOT, got a logged
 * no-op, and stayed stuck on the page: the screen contradicted the manual,
 * which is the failure mode the honest-UI rule exists to prevent.
 *
 * Renderers use this constant rather than a literal so there is one place to
 * change, and tests/test_manual.py refuses any other BOOT-mentioning hint in
 * the renderer sources.
 */
constexpr const char* kExitHint = "Hold UP to go back, hold DOWN for the dashboard.";

/// How the Wi-Fi setup access point is actually reached: the two-button combo,
/// not BOOT. Same reasoning as kExitHint.
constexpr const char* kWifiSetupHint = "Hold UP+DOWN for Wi-Fi setup";

/// Stable names used by nav-map.json, the host tests and docs/MANUAL.md.
/// Keeping one spelling in one place is what lets tests/test_manual.py check
/// the manual against the table without a second vocabulary.
const char* PageName(Page page);
const char* EventName(Event event);

/// Renders an action as the exact string stored in nav-map.json, for example
/// "switch_page:Settings", "list_move:-1", "feedback_noop". Writes at most
/// `cap` bytes including the terminator and returns `out`.
const char* ActionString(const Action& action, char* out, size_t cap);

// -------------------------------------------------------- honest status --

/// What the device actually knows about itself. Every field is observed, none
/// is inferred: "paired" means a token is installed, not that a Mac is
/// listening, and "has_stored_frame" means bytes are on flash, not that they
/// are recent.
struct StatusInputs {
    bool wifi_connected = false;
    bool paired = false;
    bool has_stored_frame = false;
    bool lan_service_running = false;
    bool transfer_active = false;
};

/// Two English lines. `secondary` is "" when there is nothing to add.
struct StatusLines {
    const char* primary = "";
    const char* secondary = "";
};

/**
 * @brief The single source for the device's self-description.
 *
 * This exists because the same sentence was being written in two places with
 * two different meanings, and because the placeholder screen
 * (ui/renderers/rawdraw/dashboard_renderer.cc) had no host tests and no caller
 * for its own SetPlaceholderInfo(), so it claimed "Waiting for Wi-Fi" on a
 * connected, paired device. Strings are static storage, so this is safe to
 * call from a render path.
 *
 * Nothing here ever claims a fetch. The device cannot reach the composer
 * (main/application.cc:487-493); frames only arrive by push.
 */
StatusLines StatusLine(const StatusInputs& in);

// ------------------------------------------------------ slideshow liveness --

/// What has to be true for the gallery slideshow to be advancing pictures.
struct SlideshowInputs {
    /// The configured interval. Zero or less means the feature is switched off.
    int interval_minutes = 0;
    /// The gallery is the page currently on the panel.
    bool on_gallery_page = false;
    /// The gallery is showing one photo full-screen, which is the only mode the
    /// timer advances.
    bool fullscreen = false;
    /// The delete confirmation is up, which suspends advancing.
    bool dialog_open = false;
};

/**
 * @brief Is a slideshow actually running, as opposed to merely configured?
 *
 * power::WakeInputs::slideshow_active refuses deep sleep outright, ahead of the
 * mode, because a device that slept between slides would show one photo and
 * stop. That refusal is right — but it was being fed
 * `GetGallerySlideshowIntervalMinutes() > 0`, which is true on any device whose
 * interval has never been set to "Off", and the shipped default is five
 * minutes. An auto-saver device sitting on the dashboard therefore answered
 * "a slideshow is running" on every single tick and never entered deep sleep at
 * all: the hourly-refresh product ran its battery down instead, and then went
 * quiet with no explanation anywhere in the status route.
 *
 * The conditions below are exactly the ones RawDrawUiManager::
 * AdvanceGallerySlideshow() checks before it advances anything. If they do not
 * hold, the timer fires, does nothing, and re-arms — which is not a slideshow.
 *
 * Here rather than inline in the UI manager because it is the predicate that
 * decides whether this device ever sleeps, and a claim that big should be one a
 * host can call.
 */
bool SlideshowIsRunning(const SlideshowInputs& in);

}  // namespace nav

#endif  // COMMON_NAV_MODEL_H
