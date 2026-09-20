/**
 * @file rawdraw_ui_manager.h
 * @brief RawDraw-based UI manager (no LVGL dependency)
 *
 * Manages rawdraw page renderers, status bar drawing, page switching,
 * and button event routing. Renders directly to the 1bpp framebuffer.
 */

#ifndef RAWDRAW_UI_MANAGER_H
#define RAWDRAW_UI_MANAGER_H

// Include LVGL header FIRST so font_engine.h detects LVGL types and skips redefining them
#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"

#include "ui/renderers/rawdraw/page_renderer.h"
#include "common/nav_model.h"
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/chat_renderer.h"
#endif
#include "ui/renderers/rawdraw/settings_renderer.h"
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/ebook_renderer.h"
#endif
#include "ui/renderers/rawdraw/wifi_renderer.h"
#include "ui/renderers/rawdraw/photo_gallery.h"
#include "ui/renderers/rawdraw/photo_detail_renderer.h"
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/weather_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/weather_detail_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/news_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/lifebar_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/almanac_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/log_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/yearprogress_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/font_debug_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/font_metrics_renderer.h"
#endif
#if !DASHBOARD_MINIMAL_UI
#include "ui/renderers/rawdraw/calendar_renderer.h"
#endif
#include "common/dashboard_service.h"
#include "dashboard_build_config.h"
#include "ui/renderers/rawdraw/ap_transfer_renderer.h"
#include "ui/renderers/rawdraw/dashboard_renderer.h"
#include "ui/renderers/rawdraw/ap_transfer_server.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/theme.h"
#include "rawdraw/style.h"
#include "rawdraw/framebuffer.h"
#include "rawdraw/clock.h"
#include "rawdraw/components/voice_wakeup.h"

#include <memory>
#include <string>
#include <functional>
#include <cstdint>
#include <array>
#include <atomic>
#include <vector>
#include <mutex>

// Forward declaration
class CustomLcdDisplay;

namespace ui {

/**
 * @brief Page identifiers for the rawdraw UI
 *
 * Only pages with rawdraw renderers are included here.
 * Separate from ui::PageId to avoid conflict with LVGL UiManager.
 */
enum class RawDrawPageId {
    Chat = 0,
    Ebook = 2,
    Wifi = 3,
    Settings = 4,
    Gallery = 5,
    Weather = 6,
    News = 7,
    WeatherDetail = 8,
    PhotoDetail = 9,
    LifeBar = 10,
    Almanac = 11,
    Log = 12,
    YearProgress = 13,
    Calendar = 14,
    FontDebug = 15,
    FontMetrics = 16,
    APTransfer = 17,
    // Full-screen poulailler dashboard. Appended rather than inserted so the
    // persisted last_page value in NVS keeps its meaning across the upgrade.
    Dashboard = 18,
    Count,
};

/**
 * @brief Status bar data for rawdraw rendering
 */
struct RawDrawStatusBarData {
    std::string page_title;
    std::string central_text;  // Overrides page_title when non-empty (e.g. "录音中...", "file.txt 1/3")
    bool wifi_connected = false;
    bool server_connected = false;
    bool bluetooth_enabled = false;
    int battery_level = -1;       // -1 = unknown
    bool battery_charging = false;
    bool battery_vertical = false;
    std::string date_format;      // "" = default (M月D日), "iso" = yyyy-mm-dd, "hidden" = hide date
    std::string server_date;      // date from WSS (yyyy-mm-dd), fallback for RTC
    std::string server_weekday;   // weekday from WSS (周一~周日)
};

/**
 * @brief Refresh callback type
 *
 * Called after RenderAll to trigger EPD update.
 * The caller (typically CustomLcdDisplay) provides this callback
 * to handle the actual EPD refresh timing.
 */
using RefreshCallback = std::function<void(const rawdraw::Rect& dirty_rect, bool urgent)>;
using PageSwitchCallback = std::function<void(RawDrawPageId page)>;

/**
 * @brief RawDraw UI Manager
 *
 * Central UI manager that:
 * - Owns all rawdraw page renderers
 * - Manages page switching with full framebuffer clear + re-render
 * - Routes button events to the active page renderer
 * - Draws the status bar + active page content
 * - Provides data update methods for each page
 *
 * Unlike the LVGL UiManager, this operates directly on the 1bpp framebuffer.
 * No LVGL objects, no tabview, no display driver — just raw pixels.
 */
class RawDrawUiManager {
public:
    RawDrawUiManager();
    ~RawDrawUiManager();

    /**
     * @brief Initialize the UI manager
     *
     * @param lcd Pointer to CustomLcdDisplay for framebuffer access
     * @param refresh_cb Optional callback to trigger EPD refresh after rendering
     */
    void Init(CustomLcdDisplay* lcd, RefreshCallback refresh_cb = nullptr);
    void SetPageSwitchCallback(PageSwitchCallback callback) { page_switch_cb_ = std::move(callback); }

    /**
     * @brief Switch to a different page
     *
     * Clears the entire framebuffer and re-renders the new page.
     *
     * @param page Target page ID
     */
    void SwitchPage(RawDrawPageId page);

    /**
     * @brief Set current page without rendering
     *
     * Updates page state (current_page_, title, renderer init) but
     * does NOT clear framebuffer or trigger refresh. Use when the
     * caller will handle rendering in a single pass.
     *
     * @param page Target page ID
     */
    void SetCurrentPageWithoutRender(RawDrawPageId page);

    /**
     * @brief Get the current active page
     */
    RawDrawPageId GetCurrentPage() const { return current_page_; }
    bool IsDisplayRefreshPending() const;

    /**
     * @brief Get the active page renderer (may be null)
     */
    rawdraw::PageRenderer* GetActiveRenderer() const;

    /**
     * @brief Handle a button event
     *
     * Routes the event to the active page renderer's HandleInput().
     * If the renderer handles it, marks the framebuffer dirty and
     * requests a refresh.
     *
     * @param event Button event
     * @return true if the event was consumed
     */
    bool HandleInput(const rawdraw::ButtonEvent& event);

    /**
     * @brief Device-side services a navigation action may need.
     *
     * These live in Application, not here: entering the Wi-Fi AP tears down
     * the LAN server and the sleep timer, and push-to-talk belongs to the
     * audio state machine. Injecting them keeps the UI manager from reaching
     * back into the application singleton for policy.
     */
    struct NavHooks {
        /// UP+DOWN long: open the Wi-Fi configuration access point.
        std::function<void()> enter_wifi_config_ap;
        /// Back and Home also leave the Wi-Fi setup access point, which shares
        /// the transfer page. A no-op when the AP is not running.
        std::function<void()> exit_wifi_config_ap;
        /// BOOT long: reserved gesture, handed to the audio state machine.
        std::function<void()> ptt_arm;
        /// Redraw the stored dashboard frame. Never a fetch.
        std::function<void()> repaint_dashboard_frame;
        /// Any button activity, for the sleep timer and the activity LED.
        std::function<void()> note_activity;
    };
    void SetNavHooks(NavHooks hooks) { nav_hooks_ = std::move(hooks); }

    /**
     * @brief The single entry point for physical navigation.
     *
     * Resolves @p event against nav::NavModel and carries out the result.
     * Replaces the gesture bindings that used to be spread across
     * Application::On*LongPress and HandleInput.
     *
     * @return true when the event produced an action (including a deliberate
     *         no-op or a latched intent), which is every event today.
     */
    bool HandleNavEvent(nav::Event event);

    /// Current model context, for logging and tests of the glue.
    const nav::Context& GetNavContext() const { return nav_model_.context(); }

    /**
     * @brief Whether the global UP-double quick switch modal is open.
     *
     * LanMicApp checks this before page-local input handling so UP/DN/BOOT
     * are consumed by the modal and do not leak into Todo/Gallery/etc.
     */
    bool IsQuickSwitchOpen() const { return quick_switch_open_; }
    bool IsApTransferRunning() const {
        return ap_transfer_server_ && ap_transfer_server_->IsRunning();
    }
    bool IsApTransferModeRunning() const {
        return ap_transfer_server_ && ap_transfer_server_->IsRunning() && ap_transfer_server_->IsApMode();
    }
    bool IsLanHttpServerRunning() const {
        return ap_transfer_server_ && ap_transfer_server_->IsRunning() && ap_transfer_server_->IsLanMode();
    }
    bool IsHttpServerRunning() const {
        return ap_transfer_server_ && ap_transfer_server_->IsRunning();
    }
    void StartApTransferMode();
    void StopApTransferMode();
    void ShowWifiConfigPage(const std::string& ssid,
                            const std::string& password,
                            const std::string& url);
    bool StartLanHttpServer(const std::string& ip_address);
    void StopLanHttpServer();

    /**
     * @brief Render everything to the framebuffer
     *
     * Draws the status bar at the top, then calls the active page
     * renderer's Render() for the content area.
     *
     * @param fb Framebuffer pointer (1bpp)
     * @param width Framebuffer width
     * @param height Framebuffer height
     */
    void RenderAll(uint8_t* fb, int width, int height);

    /**
     * @brief Update the status bar data
     *
     * Does NOT trigger a render — call RenderAll() or HandleInput()
     * to display the changes.
     */
    void UpdateStatusBar(const RawDrawStatusBarData& data);

    /**
     * @brief Get current status bar data (non-const copy)
     */
    RawDrawStatusBarData GetStatusBarData() const {
        std::lock_guard<std::mutex> lock(ui_state_mutex_);
        return status_bar_data_;
    }

    // ============================================================
    // Page data update methods
    // ============================================================

    /**
     * @brief Add a chat message to the chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void AddChatMessage(const std::string& text, rawdraw::ChatRole role);
#endif

    /**
     * @brief Clear all chat messages
     */
#if !DASHBOARD_MINIMAL_UI
    void ClearChat();
#endif

    /**
     * @brief Begin streaming text to chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void BeginChatStream();
#endif

    /**
     * @brief Append streaming text chunk to chat page
     */
#if !DASHBOARD_MINIMAL_UI
    bool AppendChatText(const char* chunk);
#endif

    /**
     * @brief End streaming text on chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void EndChatStream();
#endif

    /**
     * @brief Show/hide status bubble on chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void ShowChatStatus(const std::string& status, rawdraw::ChatRole role);
#endif
#if !DASHBOARD_MINIMAL_UI
    void HideChatStatus();
#endif

    /**
     * @brief Set listening state on chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void SetChatListening(bool listening);
#endif

    /**
     * @brief Set bottom status text on chat page
     */
#if !DASHBOARD_MINIMAL_UI
    void SetChatBottomStatus(const std::string& status);
#endif

    /**
     * @brief Set settings page items
     */
    void SetSettingsItems(const std::vector<rawdraw::SettingsItemDef>& items);

    /**
     * @brief Update a settings item value
     */
    void UpdateSettingsItem(int index, const std::string& value);

    /**
     * @brief Update a settings item checkbox state
     */
    void UpdateSettingsChecked(int index, bool checked);

    /**
     * @brief Switch and persist the global RawDraw theme.
     */
    void SetRawDrawTheme(rawdraw::ThemeId theme_id);
    rawdraw::ThemeId GetRawDrawTheme() const;

    /**
     * @brief Update WiFi status page data
     */
    void UpdateWifiStatus(const rawdraw::WifiStatus& status);

    /**
     * @brief Get WiFi status
     */
    rawdraw::WifiStatus GetWifiStatus() const;

    /**
     * @brief Set WiFi blinking animation state
     */
    void SetWifiBlinking(bool blinking);

    /**
     * @brief Toggle lifebar page visibility (controlled via settings)
     */
    void SetLifeBarVisible(bool visible);
    bool IsLifeBarVisible() const;

    // ============================================================
    // Page renderer access (for advanced usage)
    // ============================================================

#if !DASHBOARD_MINIMAL_UI
    rawdraw::ChatRenderer* GetChatRenderer() { return chat_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::EbookRenderer* GetEbookRenderer() { return ebook_renderer_.get(); }
#endif
    rawdraw::WifiRenderer* GetWifiRenderer() { return wifi_renderer_.get(); }
    rawdraw::SettingsRenderer* GetSettingsRenderer() { return settings_renderer_.get(); }
    rawdraw::PhotoGalleryRenderer* GetPhotoGalleryRenderer() { return photo_gallery_renderer_.get(); }
    rawdraw::PhotoDetailRenderer* GetPhotoDetailRenderer() { return photo_detail_renderer_.get(); }
    rawdraw::DashboardRenderer* GetDashboardRenderer() { return dashboard_renderer_.get(); }
#if !DASHBOARD_MINIMAL_UI
    rawdraw::WeatherRenderer* GetWeatherRenderer() { return weather_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::WeatherDetailRenderer* GetWeatherDetailRenderer() { return weather_detail_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::NewsRenderer* GetNewsRenderer() { return news_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::LifeBarRenderer* GetLifeBarRenderer() { return lifebar_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::AlmanacRenderer* GetAlmanacRenderer() { return almanac_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::LogRenderer* GetLogRenderer() { return log_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::YearProgressRenderer* GetYearProgressRenderer() { return yearprogress_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::CalendarRenderer* GetCalendarRenderer() { return calendar_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::FontDebugRenderer* GetFontDebugRenderer() { return font_debug_renderer_.get(); }
#endif
#if !DASHBOARD_MINIMAL_UI
    rawdraw::FontMetricsRenderer* GetFontMetricsRenderer() { return font_metrics_renderer_.get(); }
#endif
    rawdraw::ApTransferRenderer* GetApTransferRenderer() { return ap_transfer_renderer_.get(); }

    // ============================================================
    // Display dimensions
    // ============================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    /**
     * @brief Request a full EPD refresh (for clearing ghosting)
     * Only sets a flag — does NOT actually push fb to EPD.
     * Use TriggerRefresh() for immediate EPD update.
     */
    void RequestFullRefresh();

    /**
     * @brief Queue a re-render of the current page on the main/UI loop.
     *
     * Use this from callbacks that may run outside LanMicApp::Run().
     */
    void RequestActivePageRefresh();

    /**
     * @brief Paint a dashboard frame and wait for the panel to settle.
     *
     * Called from the dashboard render task. Blocks for the whole panel
     * refresh, so it must not be called from the HTTP task.
     *
     * @param frame          kFrameBytes of 2bpp data in the panel's own layout.
     * @param timeout_us     upper bound on the wait for the panel to report done.
     * @param panel_ms_out   receives the measured panel time, may be nullptr.
     * @return dashboard::RenderOutcome::kDrawn when the frame reached the glass,
     *         kDeferred when the user is currently looking at a different page
     *         (benign — the frame is stored and shown when the dashboard is next
     *         opened), and kFailed on any fault. The frame is already stored in
     *         every case.
     */
    dashboard::RenderOutcome PaintDashboardFrame(const uint8_t* frame,
                                                 int64_t timeout_us,
                                                 uint32_t* panel_ms_out);
    void SetGallerySlideshowIntervalMinutes(int minutes);
    int GetGallerySlideshowIntervalMinutes() const { return gallery_slideshow_interval_minutes_; }
    /**
     * @brief True only while the slideshow is actually advancing pictures.
     *
     * Not the same question as "is an interval configured", which is what
     * GetGallerySlideshowIntervalMinutes() answers and what the power policy
     * used to be given. The rule is nav::SlideshowIsRunning(), host tested,
     * and it matches what AdvanceGallerySlideshow() checks before it acts.
     */
    bool IsGallerySlideshowRunning() const;
    bool ShowPhotoById(const std::string& photo_id);

    /**
     * @brief Trigger EPD refresh immediately
     *
     * Marks entire framebuffer dirty and calls refresh_cb_ to push
     * fb content to the EPD. This is the method that actually makes
     * pixels appear on the screen.
     *
     * @param urgent If true, forces immediate full refresh (no throttling)
     */
    void TriggerRefresh(bool urgent = false);
    bool TryDisplayCurrentPhotoRaw4Color();

    /**
     * @brief Get page title for a given RawDrawPageId
     */
    static const char* GetPageTitle(RawDrawPageId page);

    // ============================================================
    // Clock and voice wakeup integration
    // ============================================================

    /**
     * @brief Tick voice wakeup state machine (call from main loop)
     */
    void VoiceWakeupTick();

    /**
     * @brief Trigger voice recording (called on BOOT long-press)
     */
    void VoiceWakeupTrigger(bool network_available);

    /**
     * @brief Signal voice recording completed
     */
    void VoiceWakeupDone();

    /**
     * @brief Check if voice wakeup overlay is currently active
     */
    bool VoiceWakeupIsActive() const;

    /**
     * @brief Process pending minute-clock refresh requests from esp_timer
     *
     * The esp_timer callback only marks a pending flag. Actual framebuffer
     * rendering and EPD refresh happen here on the main/UI loop thread.
     */
    void PumpClockRefresh();

    /**
     * @brief Apply a navigation intent latched during a panel refresh.
     *
     * Called from PumpClockRefresh() on the main loop. The refresh-idle hook
     * only sets the flag, because it runs on the panel refresh task.
     */
    void PumpPendingNavIntent();

private:
    struct QuickSwitchItem {
        RawDrawPageId page;
        const char* label;
        const char* icon;  // UTF-8 icon string (FontAwesome codepoint)
        /// Action row rather than a destination: redraw the stored frame.
        /// Labelled as a repaint, never as an update or a refresh from source,
        /// because the device cannot fetch anything (application.cc:487-493).
        bool repaint = false;
    };

    // Display state
    CustomLcdDisplay* lcd_ = nullptr;
    int width_ = 400;
    int height_ = 300;

    // Current page
    // Boot straight to the persisted dashboard: no menu, no splash.
    RawDrawPageId current_page_ = RawDrawPageId::Dashboard;

    // Status bar
    RawDrawStatusBarData status_bar_data_;
    mutable std::mutex ui_state_mutex_;

    // Page renderers (owned)
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::ChatRenderer> chat_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::EbookRenderer> ebook_renderer_;
#endif
    std::unique_ptr<rawdraw::WifiRenderer> wifi_renderer_;
    std::unique_ptr<rawdraw::SettingsRenderer> settings_renderer_;
    std::unique_ptr<rawdraw::PhotoGalleryRenderer> photo_gallery_renderer_;
    std::unique_ptr<rawdraw::PhotoDetailRenderer> photo_detail_renderer_;
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::WeatherRenderer> weather_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::WeatherDetailRenderer> weather_detail_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::NewsRenderer> news_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::LifeBarRenderer> lifebar_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::AlmanacRenderer> almanac_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::LogRenderer> log_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::YearProgressRenderer> yearprogress_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::CalendarRenderer> calendar_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::FontDebugRenderer> font_debug_renderer_;
#endif
#if !DASHBOARD_MINIMAL_UI
    std::unique_ptr<rawdraw::FontMetricsRenderer> font_metrics_renderer_;
#endif
    std::unique_ptr<rawdraw::ApTransferRenderer> ap_transfer_renderer_;
    std::unique_ptr<rawdraw::DashboardRenderer> dashboard_renderer_;
    // Enforces drain -> trigger -> bounded wait -> verify for panel refreshes.
    dashboard::RenderHandshake render_handshake_;
    std::unique_ptr<rawdraw::ApTransferServer> ap_transfer_server_;

    // Refresh callback (provided by CustomLcdDisplay)
    RefreshCallback refresh_cb_;
    PageSwitchCallback page_switch_cb_;

    // Full refresh flag
    bool full_refresh_pending_ = false;

    // Clock component (persistent, drawn on every RenderAll)
    rawdraw::Clock clock_;
    esp_timer_handle_t clock_refresh_timer_ = nullptr;
    esp_timer_handle_t transient_refresh_timer_ = nullptr;
    esp_timer_handle_t gallery_slideshow_timer_ = nullptr;
    std::atomic<bool> clock_refresh_pending_{false};
    std::atomic<bool> transient_refresh_pending_{false};
    std::atomic<bool> active_page_refresh_pending_{false};
    /// Set by the refresh-idle hook, consumed by PumpPendingNavIntent().
    std::atomic<bool> pending_nav_intent_{false};
    std::atomic<bool> gallery_slideshow_pending_{false};
    std::atomic<bool> input_refresh_locked_{false};
    int last_clock_minute_key_ = -1;
    int gallery_slideshow_interval_minutes_ = 0;

    // Voice wakeup overlay state
    rawdraw::VoiceWakeupState voice_wakeup_state_;

    // Global quick switch overlay
    bool quick_switch_open_ = false;
    int quick_switch_index_ = 0;
    int quick_switch_first_visible_ = 0;
    std::vector<uint8_t> quick_switch_backing_;

    // Internal helpers
    rawdraw::PageRenderer* GetRendererForPage(RawDrawPageId page) const;
    void InitRenderer(RawDrawPageId page);
    void RefreshActivePage(bool urgent = false);
    void RefreshActivePageRect(const rawdraw::Rect& rect, bool urgent = false);
    void DrawStatusBar(uint8_t* fb, int width, int height);
    void ArmClockRefreshTimer();
    void ArmTransientRefreshTimer(int delay_ms = 2000);
    static void OnClockRefreshTimer(void* arg);
    static void OnTransientRefreshTimer(void* arg);
    static void OnGallerySlideshowTimer(void* arg);
    void ArmGallerySlideshowTimer();
    bool AdvanceGallerySlideshow();
    void DrawGlobalPageFrame(uint8_t* fb, int width, int height);
    void DrawQuickSwitchOverlay(uint8_t* fb, int width, int height);
    rawdraw::Rect GetQuickSwitchBounds() const;
    void SnapshotQuickSwitchBacking(uint8_t* fb);
    void RestoreQuickSwitchBacking(uint8_t* fb);
    void RedrawQuickSwitchOnly(uint8_t* fb);
    void RefreshRect(const rawdraw::Rect& rect, bool urgent = false);
    static const std::array<QuickSwitchItem, 4>& GetQuickSwitchItems();
    void MarkAllRenderersFullRefresh();

    // ---------------------------------------------------------- navigation --
    /// Refresh the model's view of page, overlays and busy state before a
    /// decision. The model owns the mapping; the UI manager owns the truth
    /// about what is on screen.
    void SyncNavContext();
    void ApplyNavAction(const nav::Action& action);
    /// Feed one already-resolved event to the active page renderer and
    /// re-render, which is what list movement and select amount to.
    bool ForwardToRenderer(rawdraw::ButtonEvent::Type type);
    void OpenQuickSwitch();
    void CloseQuickSwitch();
    void MoveQuickSwitch(int delta);
    void ConfirmQuickSwitch();
    void RedrawQuickSwitchOverlayNow();
    void GoHome();
    static nav::Page ToNavPage(RawDrawPageId page);
    static RawDrawPageId ToPageId(nav::Page page);

    nav::NavModel nav_model_;
    NavHooks nav_hooks_;
};

}  // namespace ui

#endif  // RAWDRAW_UI_MANAGER_H
