/**
 * @file rawdraw_ui_manager.cc
 * @brief RawDraw-based UI manager implementation
 */

#include "rawdraw_ui_manager.h"
// Include LVGL-containing header FIRST so font_engine.h detects LVGL types
#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "rawdraw/style.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/theme.h"
#include "rawdraw/clock.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/components/voice_wakeup.h"
#include "rawdraw/font_engine.h"  // for fa_settings_16 font
#include "components/78__xiaozhi-fonts/include/fa_settings.h"  // for icon macros
#include "lvgl.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>

static const char* kTag = "RawDrawUiManager";
static constexpr const char* kRawDrawThemeNvsKey = "rawdraw_theme";
// Four-color e-paper cannot reliably partial-refresh the status bar in this
// driver path; even a one-minute clock change becomes a full 400x300 refresh
// and keeps the panel busy for 10s+. Keep the timer code in place, but leave
// it disabled for now. To restore minute-by-minute clock updates later, flip
// this to true after a safe status-bar-only refresh path exists.
static constexpr bool kEnableMinuteClockRefresh = false;

namespace ui {

namespace {

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (rawdraw::MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    static const std::string kEllipsis = "...";
    const int ellipsis_w = rawdraw::MeasureTextWidth(kEllipsis.c_str(), font);
    if (ellipsis_w >= max_width) return "";

    std::string fitted;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        std::string next = fitted;
        next.append(start, p - start);
        next += kEllipsis;
        if (rawdraw::MeasureTextWidth(next.c_str(), font) > max_width) break;
        fitted.append(start, p - start);
    }

    if (fitted.empty()) return "";
    fitted += kEllipsis;
    return fitted;
}

int CurrentLocalMinuteKey() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    return tm_buf.tm_year * 366 * 24 * 60 + tm_buf.tm_yday * 24 * 60 +
           tm_buf.tm_hour * 60 + tm_buf.tm_min;
}

int64_t MsUntilNextMinuteBoundary() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    const int seconds_to_next = 60 - tm_buf.tm_sec;
    return static_cast<int64_t>(std::max(1, seconds_to_next)) * 1000;
}

void DrawBatteryIcon(uint8_t* fb, int width, int x, int y, int level, bool vertical) {
    level = std::max(0, std::min(100, level));
    const rawdraw::Color battery_color = level <= 15
        ? rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Danger).border
        : rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::TextPrimary).fg;
    if (vertical) {
        const int body_w = 9;
        const int body_h = 14;
        rawdraw::DrawRectBorder(fb, width, {x, y + 2, body_w, body_h}, 1, battery_color);
        rawdraw::DrawRect(fb, width, {x + 3, y, 3, 2}, battery_color);
        const int seg_h = 3;
        const int gap = 1;
        const int filled = level >= 90 ? 3 : (level >= 50 ? 2 : (level > 10 ? 1 : 0));
        for (int i = 0; i < 3; ++i) {
            if (i >= 3 - filled) {
                const int sy = y + 3 + i * (seg_h + gap);
                rawdraw::DrawRect(fb, width, {x + 2, sy, body_w - 4, seg_h}, battery_color);
            }
        }
    } else {
        const int body_w = 24;
        const int body_h = 12;
        rawdraw::DrawRectBorder(fb, width, {x, y, body_w, body_h}, 1, battery_color);
        rawdraw::DrawRect(fb, width, {x + body_w, y + 3, 2, 6}, battery_color);
        const int seg_w = 6;
        const int gap = 1;
        const int filled = level >= 90 ? 3 : (level >= 50 ? 2 : (level > 10 ? 1 : 0));
        for (int i = 0; i < filled; ++i) {
            const int sx = x + 2 + i * (seg_w + gap);
            rawdraw::DrawRect(fb, width, {sx, y + 2, seg_w, body_h - 4}, battery_color);
        }
    }
}

void DrawServerStatusMarker(uint8_t* fb, int width, int x, int center_y, bool server_connected, bool wifi_connected) {
    if (!fb || width <= 0) return;
    const rawdraw::Color success = rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::SuccessLike).fg;
    const rawdraw::Color warning = rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Warning).border;
    if (server_connected) {
        // Draw the service-online marker as pixels instead of a font glyph.
        // This keeps the independent "*" visible on the 1bpp status bar even
        // when font baselines or partial refreshes shift by a pixel.
        rawdraw::DrawLine(fb, width, {x - 4, center_y}, {x + 4, center_y}, success);
        rawdraw::DrawLine(fb, width, {x, center_y - 4}, {x, center_y + 4}, success);
        rawdraw::DrawLine(fb, width, {x - 3, center_y - 3}, {x + 3, center_y + 3}, success);
        rawdraw::DrawLine(fb, width, {x - 3, center_y + 3}, {x + 3, center_y - 3}, success);
        return;
    }
    if (wifi_connected) {
        rawdraw::DrawRectBorder(fb, width, {x - 3, center_y - 3, 7, 7}, 1, warning);
    }
}

void DrawMiniTimeDigit(uint8_t* fb, int width, int x, int y, char digit, rawdraw::Color color) {
    if (digit < '0' || digit > '9') return;
    // Seven-segment bits: A B C D E F G.
    static constexpr uint8_t kSegments[10] = {
        0b1111110,  // 0
        0b0110000,  // 1
        0b1101101,  // 2
        0b1111001,  // 3
        0b0110011,  // 4
        0b1011011,  // 5
        0b1011111,  // 6
        0b1110000,  // 7
        0b1111111,  // 8
        0b1111011,  // 9
    };
    const uint8_t seg = kSegments[digit - '0'];
    if (seg & 0b1000000) rawdraw::DrawHLine(fb, width, y, x + 1, x + 4, color);       // A
    if (seg & 0b0100000) rawdraw::DrawVLine(fb, width, x + 5, y + 1, y + 4, color);   // B
    if (seg & 0b0010000) rawdraw::DrawVLine(fb, width, x + 5, y + 6, y + 9, color);   // C
    if (seg & 0b0001000) rawdraw::DrawHLine(fb, width, y + 10, x + 1, x + 4, color);  // D
    if (seg & 0b0000100) rawdraw::DrawVLine(fb, width, x, y + 6, y + 9, color);       // E
    if (seg & 0b0000010) rawdraw::DrawVLine(fb, width, x, y + 1, y + 4, color);       // F
    if (seg & 0b0000001) rawdraw::DrawHLine(fb, width, y + 5, x + 1, x + 4, color);   // G
}

int MiniTimeWidth(const char* text) {
    if (!text || std::strlen(text) < 5) return 32;
    return 32;  // "HH:MM": 4 digits * 6px + colon 2px + gaps.
}

void DrawMiniTimeText(uint8_t* fb, int width, int x, int y, const char* text, rawdraw::Color color) {
    if (!fb || !text || std::strlen(text) < 5) return;
    int cursor_x = x;
    for (int i = 0; i < 5; ++i) {
        if (text[i] == ':') {
            rawdraw::set_pixel(fb, width, cursor_x, y + 3, color);
            rawdraw::set_pixel(fb, width, cursor_x, y + 7, color);
            cursor_x += 4;
        } else if (text[i] == '-') {
            rawdraw::DrawHLine(fb, width, y + 5, cursor_x + 1, cursor_x + 4, color);
            cursor_x += 7;
        } else {
            DrawMiniTimeDigit(fb, width, cursor_x, y, text[i], color);
            cursor_x += 7;
        }
    }
}

}  // namespace

// ============================================================
// Page titles
// ============================================================

const char* RawDrawUiManager::GetPageTitle(RawDrawPageId page) {
    switch (page) {
        case RawDrawPageId::Chat:     return "Chat";
        case RawDrawPageId::Ebook:    return "E-book";
        case RawDrawPageId::Wifi:     return "Wi-Fi status";
        case RawDrawPageId::Settings: return "Settings";
        case RawDrawPageId::Gallery:  return "Gallery";
        case RawDrawPageId::Weather:  return "Weather";
        case RawDrawPageId::News:     return "News";
        case RawDrawPageId::WeatherDetail: return "Weather detail";
        case RawDrawPageId::PhotoDetail: return "Photo detail";
        case RawDrawPageId::LifeBar:  return "Life progress";
        case RawDrawPageId::Almanac:  return "Almanac";
        case RawDrawPageId::Log:      return "Log";
        case RawDrawPageId::YearProgress: return "Year progress";
        case RawDrawPageId::Calendar:   return "Calendar";
        case RawDrawPageId::FontDebug:  return "Alignment test";
        case RawDrawPageId::FontMetrics: return "Font metrics";
        case RawDrawPageId::APTransfer: return "Image transfer";
        case RawDrawPageId::Dashboard:  return "Dashboard";
        default:               return "Unknown";
    }
}

// ============================================================
// Construction / Destruction
// ============================================================

RawDrawUiManager::RawDrawUiManager()
    : lcd_(nullptr)
    , width_(Style::kScreenWidth)
    , height_(Style::kScreenHeight)
    , current_page_(RawDrawPageId::Dashboard)
    , refresh_cb_(nullptr)
    , full_refresh_pending_(false)
    , clock_(rawdraw::kClockX, rawdraw::kClockY, &font_zectrix_16_1)
    , voice_wakeup_state_() {
    // Create renderers
    clock_.SetColor(rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Accent).fg);
#if !DASHBOARD_MINIMAL_UI
    chat_renderer_ = std::make_unique<rawdraw::ChatRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    ebook_renderer_ = std::make_unique<rawdraw::EbookRenderer>();
#endif
    wifi_renderer_ = std::make_unique<rawdraw::WifiRenderer>();
    settings_renderer_ = std::make_unique<rawdraw::SettingsRenderer>();
    photo_gallery_renderer_ = std::make_unique<rawdraw::PhotoGalleryRenderer>();
    photo_detail_renderer_ = std::make_unique<rawdraw::PhotoDetailRenderer>();
#if !DASHBOARD_MINIMAL_UI
    weather_renderer_ = std::make_unique<rawdraw::WeatherRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    weather_detail_renderer_ = std::make_unique<rawdraw::WeatherDetailRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    news_renderer_ = std::make_unique<rawdraw::NewsRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    lifebar_renderer_ = std::make_unique<rawdraw::LifeBarRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    almanac_renderer_ = std::make_unique<rawdraw::AlmanacRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    log_renderer_ = std::make_unique<rawdraw::LogRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    yearprogress_renderer_ = std::make_unique<rawdraw::YearProgressRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    calendar_renderer_ = std::make_unique<rawdraw::CalendarRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    font_debug_renderer_ = std::make_unique<rawdraw::FontDebugRenderer>();
#endif
#if !DASHBOARD_MINIMAL_UI
    font_metrics_renderer_ = std::make_unique<rawdraw::FontMetricsRenderer>();
#endif
    ap_transfer_renderer_ = std::make_unique<rawdraw::ApTransferRenderer>();
    dashboard_renderer_ = std::make_unique<rawdraw::DashboardRenderer>();
    ap_transfer_server_ = std::make_unique<rawdraw::ApTransferServer>();
    ap_transfer_server_->SetStateCallback(
        [this](rawdraw::ApTransferServer::ServerState state, const std::string& message) {
            if (!ap_transfer_renderer_) return;
            bool should_refresh = false;
            switch (state) {
                case rawdraw::ApTransferServer::kApStarted:
                    ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kWaitingForConnection, message);
                    should_refresh = true;
                    break;
                case rawdraw::ApTransferServer::kClientConnected:
                    ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kClientConnected, message);
                    break;
                case rawdraw::ApTransferServer::kReceivingImage:
                    // Uploads from the phone finish much faster than a
                    // four-color EPD refresh. Keep the existing screen until
                    // the final saved/error state so transient upload text
                    // cannot become stale on glass.
                    break;
                case rawdraw::ApTransferServer::kProcessingImage:
                    // Same as Receiving: this state is useful for logs, but
                    // not worth a slow panel refresh.
                    break;
                case rawdraw::ApTransferServer::kImageSaved:
                    ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kComplete, message);
                    should_refresh = true;
                    break;
                case rawdraw::ApTransferServer::kError:
                    ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kError, message);
                    should_refresh = true;
                    break;
                case rawdraw::ApTransferServer::kStopped:
                default:
                    ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kWaitingForConnection, message);
                    should_refresh = true;
                    break;
            }
            if (should_refresh && current_page_ == RawDrawPageId::APTransfer) {
                // AP/HTTP handlers run on their own task. Queue the actual EPD
                // render for the main loop, and skip transient Receiving /
                // Processing states so a fast upload does not leave the panel
                // stuck showing an obsolete slow-refresh status.
                RequestActivePageRefresh();
            }
        });
    ap_transfer_server_->SetImageReceivedCallback([this](const char*) {
        if (photo_gallery_renderer_) {
            photo_gallery_renderer_->RefreshPhotoList();
            const int count = photo_gallery_renderer_->GetPhotoCount();
            if (count > 0) {
                photo_gallery_renderer_->SetSelectedIndex(count - 1);
            }
        }
    });
    ap_transfer_server_->SetSettingsChangedCallback([this](int slideshow_interval_minutes) {
        SetGallerySlideshowIntervalMinutes(slideshow_interval_minutes);
        UpdateSettingsItem(3, slideshow_interval_minutes <= 0
            ? std::string("Off")
            : std::to_string(slideshow_interval_minutes) + "min");
        RequestActivePageRefresh();
    });
    ap_transfer_server_->SetPhotosChangedCallback([this]() {
        if (photo_gallery_renderer_) {
            photo_gallery_renderer_->RefreshPhotoList();
        }
    });
    ap_transfer_server_->SetShowPhotoCallback([this](const std::string& photo_id) {
        return ShowPhotoById(photo_id);
    });

    // Initialize status bar defaults
    status_bar_data_.page_title = GetPageTitle(RawDrawPageId::Gallery);
    status_bar_data_.wifi_connected = false;
    status_bar_data_.server_connected = false;
    status_bar_data_.battery_level = -1;
    status_bar_data_.battery_charging = false;
    status_bar_data_.battery_vertical = false;

    ESP_LOGI(kTag, "RawDraw UI Manager created (no LVGL)");
}

RawDrawUiManager::~RawDrawUiManager() {
    if (clock_refresh_timer_ != nullptr) {
        esp_timer_stop(clock_refresh_timer_);
        esp_timer_delete(clock_refresh_timer_);
        clock_refresh_timer_ = nullptr;
    }
    if (transient_refresh_timer_ != nullptr) {
        esp_timer_stop(transient_refresh_timer_);
        esp_timer_delete(transient_refresh_timer_);
        transient_refresh_timer_ = nullptr;
    }
    if (gallery_slideshow_timer_ != nullptr) {
        esp_timer_stop(gallery_slideshow_timer_);
        esp_timer_delete(gallery_slideshow_timer_);
        gallery_slideshow_timer_ = nullptr;
    }
    ESP_LOGI(kTag, "RawDraw UI Manager destroyed");
}

// ============================================================
// Initialization
// ============================================================

void RawDrawUiManager::Init(CustomLcdDisplay* lcd, RefreshCallback refresh_cb) {
    if (!lcd) {
        ESP_LOGE(kTag, "LCD display pointer is null");
        return;
    }

    lcd_ = lcd;
    width_ = lcd_->GetFBWidth();
    height_ = lcd_->GetFBHeight();
    refresh_cb_ = refresh_cb;
    lcd_->SetOnRefreshIdle([this]() {
        input_refresh_locked_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "Display refresh idle; input unlocked");
        // This runs on the panel refresh task. Two things follow from that,
        // and both used to be got wrong here:
        //
        //  - Applying a page change from this callback would re-enter the
        //    render path that is calling us, so only a flag is raised and
        //    PumpPendingNavIntent() does the work on the main loop. That was
        //    already the case.
        //  - nav_model_ belongs to the main loop and is not read here at all.
        //    The old code asked it HasPendingIntent() from this task, which is
        //    an unsynchronised cross-task read of the latch, and it also lost
        //    the intent outright when the idle landed between SyncNavContext()
        //    and the latch being set. The flag is now unconditional and the
        //    pump re-checks the latch itself.
        pending_nav_intent_.store(true, std::memory_order_release);
    });

    {
        Settings settings("display", false);
        const std::string key = settings.GetString(kRawDrawThemeNvsKey, "industrial");
        rawdraw::ThemeManager::Get().SetThemeByKey(key.c_str());
        clock_.SetColor(rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Accent).fg);
    }

    // Initialize the current page renderer
    InitRenderer(current_page_);

    // Initialize voice wakeup overlay
    rawdraw::VoiceWakeupInit(&voice_wakeup_state_, &SourceHanSansSC_Regular_slim);
    last_clock_minute_key_ = CurrentLocalMinuteKey();

    if (kEnableMinuteClockRefresh && clock_refresh_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &RawDrawUiManager::OnClockRefreshTimer;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "rawdraw_clock";
        esp_err_t ret = esp_timer_create(&timer_args, &clock_refresh_timer_);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "Failed to create clock refresh timer: %s", esp_err_to_name(ret));
        }
    }
    ArmClockRefreshTimer();

    if (transient_refresh_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &RawDrawUiManager::OnTransientRefreshTimer;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "rawdraw_transient";
        esp_err_t ret = esp_timer_create(&timer_args, &transient_refresh_timer_);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "Failed to create transient refresh timer: %s", esp_err_to_name(ret));
        }
    }

    if (gallery_slideshow_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &RawDrawUiManager::OnGallerySlideshowTimer;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "gallery_slideshow";
        esp_err_t ret = esp_timer_create(&timer_args, &gallery_slideshow_timer_);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "Failed to create slideshow timer: %s", esp_err_to_name(ret));
        }
    }
    ArmGallerySlideshowTimer();

    // Clear and render initial frame
    auto* fb = lcd_->GetFramebuffer();
    if (fb) {
        // Lock for thread safety
        auto* mutex = lcd_->GetMutex();
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

        rawdraw::Clear(fb, width_, height_);
        RenderAll(fb, width_, height_);

        if (mutex) xSemaphoreGive(mutex);

        // Trigger initial full refresh
        TriggerRefresh(true);
    }

    ESP_LOGI(kTag, "RawDraw UI Manager initialized: %dx%d, page=%s",
             width_, height_, GetPageTitle(current_page_));
}

void RawDrawUiManager::MarkAllRenderersFullRefresh() {
    for (int i = 0; i < static_cast<int>(RawDrawPageId::Count); ++i) {
        auto* renderer = GetRendererForPage(static_cast<RawDrawPageId>(i));
        if (renderer) renderer->MarkFullRefresh();
    }
}

void RawDrawUiManager::SetRawDrawTheme(rawdraw::ThemeId theme_id) {
    rawdraw::ThemeManager::Get().SetTheme(theme_id);
    clock_.SetColor(rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Accent).fg);
    {
        Settings settings("display", true);
        settings.SetString(kRawDrawThemeNvsKey, rawdraw::ThemeManager::Key(theme_id));
    }
    MarkAllRenderersFullRefresh();
    full_refresh_pending_ = true;
    RefreshActivePage(true);
}

rawdraw::ThemeId RawDrawUiManager::GetRawDrawTheme() const {
    return rawdraw::ThemeManager::Get().CurrentId();
}

// ============================================================
// Page switching
// ============================================================

void RawDrawUiManager::SwitchPage(RawDrawPageId page) {
    if (page == current_page_) {
        return;  // No change
    }

    ESP_LOGI(kTag, "Switching page: %s -> %s",
             GetPageTitle(current_page_), GetPageTitle(page));

    // Initialize the new page renderer
    InitRenderer(page);

    // Switch current page
    current_page_ = page;
    if (page_switch_cb_) {
        page_switch_cb_(page);
    }

    // Update status bar title
    {
        std::lock_guard<std::mutex> lock(ui_state_mutex_);
        status_bar_data_.page_title = GetPageTitle(page);
    }
    // Full clear + re-render of the entire framebuffer
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (fb) {
        auto* mutex = lcd_->GetMutex();
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

        // Clear entire framebuffer to white
        rawdraw::Clear(fb, width_, height_);

        // Render everything
        RenderAll(fb, width_, height_);

        if (mutex) xSemaphoreGive(mutex);

        if (!TryDisplayCurrentPhotoRaw4Color()) {
            TriggerRefresh(true);
        }
    }

    // Mark for full EPD refresh to clear ghosting
    full_refresh_pending_ = true;
}

void RawDrawUiManager::InitRenderer(RawDrawPageId page) {
    auto* renderer = GetRendererForPage(page);
    if (renderer) {
        renderer->Init(width_, height_);
        renderer->MarkFullRefresh();
    }
}

void RawDrawUiManager::SetCurrentPageWithoutRender(RawDrawPageId page) {
    if (page == current_page_) {
        return;
    }
    // Initialize the new page renderer
    InitRenderer(page);
    // Switch current page
    current_page_ = page;
    // Update status bar title
    {
        std::lock_guard<std::mutex> lock(ui_state_mutex_);
        status_bar_data_.page_title = GetPageTitle(page);
    }
}

rawdraw::PageRenderer* RawDrawUiManager::GetRendererForPage(RawDrawPageId page) const {
    switch (page) {
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Chat:     return chat_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Ebook:    return ebook_renderer_.get();
#endif
        case RawDrawPageId::Wifi:     return wifi_renderer_.get();
        case RawDrawPageId::Settings: return settings_renderer_.get();
        case RawDrawPageId::Gallery:  return photo_gallery_renderer_.get();
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Weather:  return weather_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::News:     return news_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::WeatherDetail: return weather_detail_renderer_.get();
#endif
        case RawDrawPageId::PhotoDetail: return photo_detail_renderer_.get();
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::LifeBar:  return lifebar_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Almanac:  return almanac_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Log:      return log_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::YearProgress: return yearprogress_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::Calendar:   return calendar_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::FontDebug:  return font_debug_renderer_.get();
#endif
#if !DASHBOARD_MINIMAL_UI
        case RawDrawPageId::FontMetrics: return font_metrics_renderer_.get();
#endif
        case RawDrawPageId::APTransfer: return ap_transfer_renderer_.get();
        case RawDrawPageId::Dashboard:  return dashboard_renderer_.get();
        default:               return nullptr;
    }
}

rawdraw::PageRenderer* RawDrawUiManager::GetActiveRenderer() const {
    return GetRendererForPage(current_page_);
}

bool RawDrawUiManager::IsDisplayRefreshPending() const {
    return lcd_ != nullptr && const_cast<CustomLcdDisplay*>(lcd_)->IsRefreshPending();
}

void RawDrawUiManager::RefreshActivePage(bool urgent) {
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (!fb) return;

    auto* mutex = lcd_->GetMutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    rawdraw::Clear(fb, width_, height_);
    RenderAll(fb, width_, height_);
    if (mutex) xSemaphoreGive(mutex);

    if (!TryDisplayCurrentPhotoRaw4Color()) {
        TriggerRefresh(urgent);
    }
}

void RawDrawUiManager::RefreshActivePageRect(const rawdraw::Rect& rect, bool urgent) {
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (!fb) return;

    auto* mutex = lcd_->GetMutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    rawdraw::Clear(fb, width_, height_);
    RenderAll(fb, width_, height_);
    if (mutex) xSemaphoreGive(mutex);

    RefreshRect(rect, urgent);
}

bool RawDrawUiManager::TryDisplayCurrentPhotoRaw4Color() {
    // The direct raw 2bpp photo path can leave this four-color panel busy for
    // minutes on real hardware, which blocks the normal RawDraw refresh queue
    // and makes page switching appear frozen. Photo renderers already preserve
    // BLACK/WHITE/RED/YELLOW pixels in the shared framebuffer, so keep photos
    // on the standard UI refresh path until the panel mode switch is proven
    // safe.
    static constexpr bool kEnableDirectRawPhotoRefresh = false;
    if (!kEnableDirectRawPhotoRefresh) {
        return false;
    }

    if (!lcd_) return false;

    const uint8_t* data = nullptr;
    uint32_t size = 0;
    int photo_width = 0;
    int photo_height = 0;

    if (current_page_ == RawDrawPageId::Gallery && photo_gallery_renderer_ &&
        photo_gallery_renderer_->IsFullscreenMode() &&
        !photo_gallery_renderer_->IsDeleteDialogOpen() &&
        photo_gallery_renderer_->IsCurrentPhotoBwry2bpp()) {
        data = photo_gallery_renderer_->GetCurrentPhotoData();
        size = photo_gallery_renderer_->GetCurrentPhotoSize();
        photo_width = photo_gallery_renderer_->GetCurrentPhotoWidth();
        photo_height = photo_gallery_renderer_->GetCurrentPhotoHeight();
    } else if (current_page_ == RawDrawPageId::PhotoDetail && photo_detail_renderer_ &&
               !photo_detail_renderer_->IsMetadataOpen() &&
               photo_detail_renderer_->IsCurrentPhotoBwry2bpp()) {
        data = photo_detail_renderer_->GetCurrentPhotoData();
        size = photo_detail_renderer_->GetCurrentPhotoSize();
        photo_width = photo_detail_renderer_->GetCurrentPhotoWidth();
        photo_height = photo_detail_renderer_->GetCurrentPhotoHeight();
    }

    if (!data || size == 0 || photo_width <= 0 || photo_height <= 0) {
        return false;
    }

    const bool shown = lcd_->DisplayRaw4ColorImage(data, size, photo_width, photo_height);
    ESP_LOGI(kTag, "raw 4-color photo display: shown=%d size=%lu %dx%d page=%s",
             shown ? 1 : 0,
             static_cast<unsigned long>(size),
             photo_width,
             photo_height,
             GetPageTitle(current_page_));
    return shown;
}

const std::array<RawDrawUiManager::QuickSwitchItem, 4>& RawDrawUiManager::GetQuickSwitchItems() {
    static const std::array<QuickSwitchItem, 4> kItems = {{
        {RawDrawPageId::Dashboard, "Dashboard", FA_SETTINGS_SERVER, false},
        {RawDrawPageId::Gallery, "Gallery", FA_SETTINGS_IMAGE, false},
        {RawDrawPageId::Settings, "Settings", FA_SETTINGS_GEAR, false},
        // Deliberate wording. This redraws the frame already on the device; it
        // does not ask the Mac for a new one, because the device has no route
        // to the composer (application.cc:487-493).
        {RawDrawPageId::Dashboard, "Repaint screen", FA_SETTINGS_SYNC, true},
#if 0
        // Hardware-only alignment pages are intentionally hidden from the
        // user-facing quick switch. Keep the renderer code for calibration,
        // but do not expose them in normal navigation.
        {RawDrawPageId::FontDebug, "Alignment test", nullptr},
        {RawDrawPageId::FontMetrics, "Font metrics", nullptr},
#endif
    }};
    return kItems;
}

void RawDrawUiManager::StartApTransferMode() {
    if (ap_transfer_renderer_) {
        ap_transfer_renderer_->UseDefaultTransferInstructions();
    }
    if (ap_transfer_server_) {
        ap_transfer_server_->Start();
    }
    SwitchPage(RawDrawPageId::APTransfer);
}

void RawDrawUiManager::StopApTransferMode() {
    if (ap_transfer_server_) {
        ap_transfer_server_->Stop();
    }
    // Land where transfer is now entered from, so leaving it by any route ends
    // in the same place the user started.
    SwitchPage(RawDrawPageId::Settings);
}

void RawDrawUiManager::ShowWifiConfigPage(const std::string& ssid,
                                          const std::string& password,
                                          const std::string& url) {
    if (ap_transfer_renderer_) {
        ap_transfer_renderer_->SetInstructionContent("Wi-Fi setup",
                                                     ssid.empty() ? "ZecTrix" : ssid,
                                                     password,
                                                     url.empty() ? "http://192.168.4.1" : url,
                                                     "Join the hotspot, then open the page to set up Wi-Fi",
                                                     nav::kExitHint);
        ap_transfer_renderer_->SetState(rawdraw::ApTransferRenderer::kWaitingForConnection,
                                        "192.168.4.1");
    }
    SwitchPage(RawDrawPageId::APTransfer);
}

bool RawDrawUiManager::StartLanHttpServer(const std::string& ip_address) {
    if (!ap_transfer_server_) return false;
    return ap_transfer_server_->StartLan(ip_address);
}

void RawDrawUiManager::StopLanHttpServer() {
    if (ap_transfer_server_ && ap_transfer_server_->IsLanMode()) {
        ap_transfer_server_->Stop();
    }
}

// ============================================================
// Input handling
// ============================================================

bool RawDrawUiManager::HandleInput(const rawdraw::ButtonEvent& event) {
    // Gesture *policy* is no longer decided here. HandleNavEvent() resolves a
    // physical gesture against common/nav_model.cc and calls back into this
    // function only for the part a page renderer owns: moving its own
    // selection and activating it. What used to live here and does not any
    // more:
    //
    //   - BOOT-long entering AP transfer from Gallery and leaving it again.
    //     BOOT-long is reserved for push-to-talk; transfer is entered from the
    //     Settings row "Photo transfer" and left with Back or Home.
    //   - The quick-switch toggle on kUpDoubleClick. The board never registers
    //     OnDoubleClick (zectrix-s3-epaper-4.2.cc:349-465), so that branch could
    //     not fire and Gallery was unreachable by button. BOOT click on the
    //     Dashboard opens it now.
    //   - Silently dropping navigation clicks during a refresh. They are
    //     latched by the model and applied on refresh-idle.
    auto* renderer = GetActiveRenderer();
    if (!renderer) {
        return false;
    }

    // Route event to active page renderer
    bool handled = renderer->HandleInput(event);

    if (handled) {
        // The input lock is no longer set here. TriggerRefresh() sets it for
        // every path that makes the panel busy, this one included, which is
        // what stops the nav-initiated refreshes from escaping it.
        // Re-render the framebuffer with updated state
        auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
        if (fb) {
            auto* mutex = lcd_->GetMutex();
            if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

            // Clear and re-render everything
            rawdraw::Clear(fb, width_, height_);
            RenderAll(fb, width_, height_);

            if (mutex) xSemaphoreGive(mutex);

            if (!TryDisplayCurrentPhotoRaw4Color()) {
                TriggerRefresh(false);
            }
        }
    }

    return handled;
}

// ============================================================
// Navigation (common/nav_model.cc decides, this section carries out)
// ============================================================

nav::Page RawDrawUiManager::ToNavPage(RawDrawPageId page) {
    switch (page) {
        case RawDrawPageId::Gallery:    return nav::Page::Gallery;
        case RawDrawPageId::Settings:   return nav::Page::Settings;
        case RawDrawPageId::APTransfer: return nav::Page::Transfer;
        default:                        return nav::Page::Dashboard;
    }
}

ui::RawDrawPageId RawDrawUiManager::ToPageId(nav::Page page) {
    switch (page) {
        case nav::Page::Gallery:  return RawDrawPageId::Gallery;
        case nav::Page::Settings: return RawDrawPageId::Settings;
        case nav::Page::Transfer: return RawDrawPageId::APTransfer;
        case nav::Page::Dashboard: break;
    }
    return RawDrawPageId::Dashboard;
}

void RawDrawUiManager::SyncNavContext() {
    nav::Context ctx;
    ctx.page = ToNavPage(current_page_);
    ctx.quick_switch_open = quick_switch_open_;
    ctx.dialog_open = photo_gallery_renderer_ && photo_gallery_renderer_->IsDeleteDialogOpen();
    ctx.fullscreen_photo = current_page_ == RawDrawPageId::Gallery &&
                           photo_gallery_renderer_ &&
                           photo_gallery_renderer_->IsFullscreenMode();
    // Independent of the visible page on purpose: a background update can move
    // the page while the transfer server is still up.
    ctx.transfer_active = ap_transfer_server_ && ap_transfer_server_->IsRunning() &&
                          ap_transfer_server_->IsApMode();
    ctx.refresh_busy = input_refresh_locked_.load(std::memory_order_acquire);
    nav_model_.SetContext(ctx);
}

bool RawDrawUiManager::ForwardToRenderer(rawdraw::ButtonEvent::Type type) {
    return HandleInput(rawdraw::ButtonEvent{type});
}

void RawDrawUiManager::RedrawQuickSwitchOverlayNow() {
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    auto* mutex = lcd_ ? lcd_->GetMutex() : nullptr;
    if (!fb) {
        return;
    }
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    RedrawQuickSwitchOnly(fb);
    if (mutex) xSemaphoreGive(mutex);
    // Partial-window refresh on this BWRY panel is unverified on hardware
    // (PRODUCT-PLAN.md HG6). It is what the overlay has always used; the
    // fallback if HG6 fails is a full refresh here.
    RefreshRect(GetQuickSwitchBounds(), false);
}

void RawDrawUiManager::OpenQuickSwitch() {
    if (quick_switch_open_) return;
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    auto* mutex = lcd_ ? lcd_->GetMutex() : nullptr;
    quick_switch_open_ = true;
    quick_switch_index_ = 0;
    quick_switch_first_visible_ = 0;
    const auto& items = GetQuickSwitchItems();
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].repaint && items[i].page == current_page_) {
            quick_switch_index_ = static_cast<int>(i);
            break;
        }
    }
    const int kVisibleCount = 5;
    const int total = static_cast<int>(items.size());
    if (quick_switch_index_ >= kVisibleCount) {
        quick_switch_first_visible_ = quick_switch_index_ - kVisibleCount + 1;
    }
    if (quick_switch_first_visible_ + kVisibleCount > total) {
        quick_switch_first_visible_ = std::max(0, total - kVisibleCount);
    }
    if (fb) {
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
        SnapshotQuickSwitchBacking(fb);
        RedrawQuickSwitchOnly(fb);
        if (mutex) xSemaphoreGive(mutex);
        RefreshRect(GetQuickSwitchBounds(), false);
    }
    nav_model_.SetQuickSwitchOpen(true);
}

void RawDrawUiManager::CloseQuickSwitch() {
    if (!quick_switch_open_) return;
    quick_switch_open_ = false;
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    auto* mutex = lcd_ ? lcd_->GetMutex() : nullptr;
    const bool had_backing = !quick_switch_backing_.empty();
    if (fb) {
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
        if (had_backing) RestoreQuickSwitchBacking(fb);
        if (mutex) xSemaphoreGive(mutex);
        if (had_backing) RefreshRect(GetQuickSwitchBounds(), false);
    }
    quick_switch_backing_.clear();
    nav_model_.SetQuickSwitchOpen(false);
}

void RawDrawUiManager::MoveQuickSwitch(int delta) {
    if (!quick_switch_open_) return;
    const int total = static_cast<int>(GetQuickSwitchItems().size());
    if (total <= 0) return;
    const int kVisibleCount = 5;
    quick_switch_index_ = (quick_switch_index_ + total + (delta < 0 ? -1 : 1)) % total;
    if (quick_switch_index_ < quick_switch_first_visible_) {
        quick_switch_first_visible_ = quick_switch_index_;
    }
    if (quick_switch_index_ >= quick_switch_first_visible_ + kVisibleCount) {
        quick_switch_first_visible_ = quick_switch_index_ - kVisibleCount + 1;
    }
    if (quick_switch_index_ == total - 1) {
        quick_switch_first_visible_ = std::max(0, total - kVisibleCount);
    }
    if (quick_switch_index_ == 0) {
        quick_switch_first_visible_ = 0;
    }
    RedrawQuickSwitchOverlayNow();
}

void RawDrawUiManager::ConfirmQuickSwitch() {
    if (!quick_switch_open_) return;
    const auto& items = GetQuickSwitchItems();
    const int index = std::max(0, std::min(quick_switch_index_,
                                           static_cast<int>(items.size()) - 1));
    const QuickSwitchItem item = items[index];
    CloseQuickSwitch();
    if (item.repaint) {
        ESP_LOGI(kTag, "Quick switch: repaint of the stored frame requested");
        if (nav_hooks_.repaint_dashboard_frame) {
            nav_hooks_.repaint_dashboard_frame();
        }
        return;
    }
    SwitchPage(item.page);
}

void RawDrawUiManager::GoHome() {
    // Home means "leave everything and show the dashboard", so every context
    // is torn down explicitly rather than relying on a page change to do it.
    if (photo_gallery_renderer_) {
        photo_gallery_renderer_->CloseDeleteDialog();
        photo_gallery_renderer_->ExitFullscreenMode();
    }
    CloseQuickSwitch();
    if (ap_transfer_server_ && ap_transfer_server_->IsRunning() &&
        ap_transfer_server_->IsApMode()) {
        ESP_LOGI(kTag, "Home: stopping AP transfer");
        ap_transfer_server_->Stop();
    }
    if (nav_hooks_.exit_wifi_config_ap) {
        nav_hooks_.exit_wifi_config_ap();
    }
    SwitchPage(RawDrawPageId::Dashboard);
}

void RawDrawUiManager::ApplyNavAction(const nav::Action& action) {
    switch (action.type) {
        case nav::ActionType::kNone:
            break;

        case nav::ActionType::kSwitchPage:
            SwitchPage(ToPageId(action.page));
            break;

        case nav::ActionType::kListMove:
            ForwardToRenderer(action.delta < 0 ? rawdraw::ButtonEvent::kUpClick
                                               : rawdraw::ButtonEvent::kDownClick);
            break;

        case nav::ActionType::kSelect:
            ForwardToRenderer(rawdraw::ButtonEvent::kBootClick);
            break;

        case nav::ActionType::kOpenQuickSwitch:
            OpenQuickSwitch();
            break;

        case nav::ActionType::kCloseQuickSwitch:
            CloseQuickSwitch();
            break;

        case nav::ActionType::kQuickSwitchMove:
            MoveQuickSwitch(action.delta);
            break;

        case nav::ActionType::kQuickSwitchConfirm:
            ConfirmQuickSwitch();
            break;

        case nav::ActionType::kDialogMove:
            ForwardToRenderer(action.delta < 0 ? rawdraw::ButtonEvent::kUpClick
                                               : rawdraw::ButtonEvent::kDownClick);
            break;

        case nav::ActionType::kDialogConfirm:
            ForwardToRenderer(rawdraw::ButtonEvent::kBootClick);
            break;

        case nav::ActionType::kCloseDialog:
            if (photo_gallery_renderer_) {
                photo_gallery_renderer_->CloseDeleteDialog();
                RefreshActivePage(false);
            }
            break;

        case nav::ActionType::kCloseFullscreen:
            if (photo_gallery_renderer_) {
                photo_gallery_renderer_->ExitFullscreenMode();
                RefreshActivePage(false);
            }
            break;

        case nav::ActionType::kExitTransfer:
            ESP_LOGI(kTag, "Leaving AP transfer for %s", nav::PageName(action.page));
            if (ap_transfer_server_) {
                ap_transfer_server_->Stop();
            }
            if (nav_hooks_.exit_wifi_config_ap) {
                nav_hooks_.exit_wifi_config_ap();
            }
            SwitchPage(ToPageId(action.page));
            break;

        case nav::ActionType::kHome:
            GoHome();
            break;

        case nav::ActionType::kRepaintPage:
            // Redraw of what is already stored. Not a fetch, and never
            // described as one.
            ESP_LOGI(kTag, "Repainting the current page from stored content");
            RefreshActivePage(true);
            break;

        case nav::ActionType::kWifiConfigAp:
            if (nav_hooks_.enter_wifi_config_ap) {
                nav_hooks_.enter_wifi_config_ap();
            }
            break;

        case nav::ActionType::kPttArm:
            if (nav_hooks_.ptt_arm) {
                nav_hooks_.ptt_arm();
            }
            break;

        case nav::ActionType::kFeedbackNoop:
            // The LED has already flashed. Saying nothing more is correct:
            // there is nothing to change.
            ESP_LOGI(kTag, "Gesture understood, nothing to change here");
            break;

        case nav::ActionType::kPendingLatched:
            ESP_LOGI(kTag, "Panel is refreshing; navigation intent held until it finishes");
            break;
    }
}

bool RawDrawUiManager::HandleNavEvent(nav::Event event) {
    SyncNavContext();
    const nav::Action action = nav_model_.Decide(event);
    char buf[48];
    ESP_LOGI(kTag, "Nav %s on %s -> %s",
             nav::EventName(event),
             nav::PageName(nav_model_.context().page),
             nav::ActionString(action, buf, sizeof(buf)));
    if (action.led_feedback && nav_hooks_.note_activity) {
        // Immediate, before any panel work: on a panel whose worst case is
        // minutes, this is the only feedback that arrives while the finger is
        // still on the button.
        nav_hooks_.note_activity();
    }
    ApplyNavAction(action);
    return true;
}

// ============================================================
// Rendering
// ============================================================

void RawDrawUiManager::RenderAll(uint8_t* fb, int width, int height) {
    if (!fb) {
        ESP_LOGW(kTag, "RenderAll called with null framebuffer");
        return;
    }
    std::lock_guard<std::mutex> lock(ui_state_mutex_);
    const bool gallery_fullscreen =
        current_page_ == RawDrawPageId::Gallery &&
        photo_gallery_renderer_ &&
        photo_gallery_renderer_->IsFullscreenMode();
#if DASHBOARD_MINIMAL_UI
    // The e-book page is not built in this configuration.
    const bool ebook_portrait_reader = false;
#else
    const bool ebook_portrait_reader =
        current_page_ == RawDrawPageId::Ebook &&
        ebook_renderer_ &&
        ebook_renderer_->IsPortraitReader();
#endif
    // The dashboard frame is composed off-device and occupies the whole panel;
    // drawing the status bar over it would cover the operator's own layout.
    const bool chrome_free_page = (current_page_ == RawDrawPageId::Dashboard);

    // Update central_text based on current page state
    status_bar_data_.central_text.clear();
#if !DASHBOARD_MINIMAL_UI
    if (current_page_ == RawDrawPageId::Ebook && ebook_renderer_ && !ebook_portrait_reader) {
        if (ebook_renderer_->IsReaderMode()) {
            status_bar_data_.central_text = ebook_renderer_->GetReaderFilename()
                + "  " + std::to_string(ebook_renderer_->GetCurrentPage() + 1)
                + "/" + std::to_string(ebook_renderer_->GetTotalPages());
        }
    }
    if (current_page_ == RawDrawPageId::Calendar && calendar_renderer_) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d-%02d  <- ->",
                 calendar_renderer_->GetYear(), calendar_renderer_->GetMonth());
        status_bar_data_.central_text = buf;
    }
#endif

    // Draw the active page content in the content area
    auto* renderer = GetActiveRenderer();
    if (renderer) {
        renderer->Render(fb, width, height);
    }

    // Fullscreen gallery is intentionally chrome-free: BOOT opens the selected
    // photo as a pure image view with no header/frame. The normal memory-card
    // gallery page still keeps the global shell with title/status bar.
    if (!gallery_fullscreen && !ebook_portrait_reader && !chrome_free_page) {
        // Status bar is drawn after page content so page renderers cannot
        // accidentally paint into the top menu area.
        DrawStatusBar(fb, width, height);

        // Draw the global shell frame last so it wraps the whole 400x300 panel,
        // including the top menu bar. Page renderers should not draw their own
        // outer frames; this single frame keeps the Macintosh-style window edge
        // consistent across all pages.
        DrawGlobalPageFrame(fb, width, height);
    }

    // Draw voice wakeup overlay if active
    if (rawdraw::VoiceWakeupIsVisible(&voice_wakeup_state_)) {
        rawdraw::VoiceWakeupDraw(fb, width, height, &voice_wakeup_state_);
    }

    if (quick_switch_open_) {
        DrawQuickSwitchOverlay(fb, width, height);
    }
}

void RawDrawUiManager::DrawGlobalPageFrame(uint8_t* fb, int width, int height) {
    if (!fb || width <= 4 || height <= 4) return;
    const auto border = rawdraw::ThemeManager::Get().Style(rawdraw::ThemeToken::Border);
    rawdraw::DrawRoundRectBorder(fb, width, height, {1, 1, width - 2, height - 2},
                                 Style::kBorderRadiusMD, border.border_width, border.border);
}

void RawDrawUiManager::DrawStatusBar(uint8_t* fb, int width, int height) {
    using namespace rawdraw;

    int bar_height = Style::kStatusBarHeight;
    int padding = Style::kStatusBarPadding;
    const lv_font_t* title_font = &SourceHanSansSC_Regular_slim;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle accent_style = theme.Style(ThemeToken::Accent);
    const PaintStyle border_style = theme.Style(ThemeToken::Border);
    const PaintStyle danger_style = theme.Style(ThemeToken::Danger);

    DrawStyledRect(fb, width, {0, 0, width, bar_height}, bg_style);
    DrawRect(fb, width, {1, bar_height - Style::kShellDividerThickness,
                         width - 2, Style::kShellDividerThickness}, border_style.border);

    const int center_y = bar_height / 2;
    const int sig_bar_w = 3;
    const int sig_bar_gap = 2;
    const int sig_bar_heights[] = {6, 9, 12, 15};
    const int wifi_group_w = 4 * sig_bar_w + 3 * sig_bar_gap;
    int x = padding + 3;

    if (status_bar_data_.wifi_connected) {
        for (int i = 0; i < 4; ++i) {
            int bx = x + i * (sig_bar_w + sig_bar_gap);
            int by = center_y + 7 - sig_bar_heights[i];
            DrawRect(fb, width, {bx, by, sig_bar_w, sig_bar_heights[i]}, theme.Style(ThemeToken::SuccessLike).fg);
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            int bx = x + i * (sig_bar_w + sig_bar_gap);
            int by = center_y + 7 - sig_bar_heights[i];
            DrawRectBorder(fb, width, {bx, by, sig_bar_w, sig_bar_heights[i]}, 1, text_style.fg);
        }
        int slash_x1 = x;
        int slash_y1 = center_y - 6;
        int slash_x2 = x + wifi_group_w;
        int slash_y2 = center_y + 7;
        DrawLine(fb, width, {slash_x1, slash_y1}, {slash_x2, slash_y2}, danger_style.border);
    }

    const int marker_x = x + wifi_group_w + 10;
    DrawServerStatusMarker(fb, width, marker_x, center_y, status_bar_data_.server_connected,
                           status_bar_data_.wifi_connected);
    int left_content_x = marker_x + 14;
    if (status_bar_data_.bluetooth_enabled) {
        const char* bt_icon = FA_SETTINGS_BLUETOOTH;
        const int bt_y = InkCenteredTextTopY(&fa_settings_16, bt_icon, center_y, 0);
        DrawStyledText(fb, width, left_content_x, bt_y, bt_icon, &fa_settings_16, accent_style, height);
        left_content_x += MeasureTextWidth(bt_icon, &fa_settings_16) + 8;
    }

    // Date + weekday on left side, after server marker (skip if "hidden")
    std::string date_str;
    const bool hide_date = status_bar_data_.date_format == "hidden";
    if (!hide_date) {
        if (!status_bar_data_.server_date.empty()) {
            if (status_bar_data_.date_format == "iso") {
                date_str = status_bar_data_.server_date;
            } else {
                int y = 0, m = 0, d = 0;
                sscanf(status_bar_data_.server_date.c_str(), "%d-%d-%d", &y, &m, &d);
                // Day-month, matching the composed dashboard rather than the
                // upstream Chinese "M月D日" form.
                char buf[16];
                snprintf(buf, sizeof(buf), "%02d/%02d", d, m);
                date_str = buf;
            }
            if (!status_bar_data_.server_weekday.empty()) {
                date_str += " " + status_bar_data_.server_weekday;
            }
        } else {
            date_str = rawdraw::Clock::GetDateString(status_bar_data_.date_format.empty() ? nullptr : status_bar_data_.date_format.c_str());
        }
    }
    const int date_x = left_content_x;
    const int date_w = hide_date ? 0 : MeasureTextWidth(date_str.c_str(), title_font);
    if (!hide_date) {
        DrawStyledText(fb, width, date_x,
                       InkCenteredTextTopY(title_font, date_str.c_str(), center_y, 0),
                       date_str.c_str(), title_font, text_style, height);
    }
    const int left_safe = date_x + date_w + 8;

    const char* time_str = rawdraw::Clock::GetTimeString();
    if (time_str == nullptr || time_str[0] == '\0') {
        time_str = "--:--";
    }

    const int battery_slot_w = 30;
    int right_x = width - padding;
    if (status_bar_data_.battery_level >= 0) {
        const int battery_h = status_bar_data_.battery_vertical ? 16 : 12;
        const int battery_w = status_bar_data_.battery_vertical ? 9 : 26;
        const int battery_x = width - padding - battery_slot_w + (battery_slot_w - battery_w);
        const int battery_y = center_y - battery_h / 2;
        DrawBatteryIcon(fb, width, battery_x, battery_y, status_bar_data_.battery_level,
                        status_bar_data_.battery_vertical);
        right_x = battery_x - 8;
    }

    // Time on right side
    const int clock_w = MiniTimeWidth(time_str);
    DrawMiniTimeText(fb, width, right_x - clock_w, center_y - 5, time_str, accent_style.fg);
    right_x = right_x - clock_w - 6;

    const char* title = !status_bar_data_.central_text.empty()
        ? status_bar_data_.central_text.c_str()
        : status_bar_data_.page_title.c_str();
    const int right_safe = std::max(left_safe + 40, right_x - 2);
    const char* title_icon = nullptr;
    for (const auto& item : GetQuickSwitchItems()) {
        if (item.repaint) continue;  // action row, not a page identity
        if (item.page == current_page_) {
            title_icon = item.icon;
            break;
        }
    }
    const int title_icon_gap = (title_icon && title_icon[0] != '\0') ? 5 : 0;
    const int title_icon_w = (title_icon && title_icon[0] != '\0')
        ? MeasureTextWidth(title_icon, &fa_settings_16) : 0;
    const int title_max_w = std::max(44, right_safe - left_safe);
    const int text_max_w = std::max(20, title_max_w - title_icon_w - title_icon_gap);
    std::string display_title = FitTextToWidth(title, title_font, text_max_w);
    int title_text_w = MeasureTextWidth(display_title.c_str(), title_font);
    int title_w = title_icon_w + title_icon_gap + title_text_w;
    int title_x = (width - title_w) / 2;
    if (title_x < left_safe) title_x = left_safe;
    if (title_x + title_w > right_safe) title_x = std::max(left_safe, right_safe - title_w);
    const int title_y = rawdraw::InkCenteredTextTopYInBox(title_font, display_title.c_str(),
                                                         0, bar_height, 0);
    static std::string s_last_logged_title_layout;
    const std::string title_layout_key = display_title + "@" + std::to_string(title_x) +
        "," + std::to_string(title_y) + "/" + std::to_string(title_max_w);
    if (s_last_logged_title_layout != title_layout_key) {
        s_last_logged_title_layout = title_layout_key;
        ESP_LOGI(kTag, "statusbar title raw='%s' central='%s' display='%s' x=%d y=%d max_w=%d left_safe=%d right_safe=%d",
                 status_bar_data_.page_title.c_str(),
                 status_bar_data_.central_text.c_str(),
                 display_title.c_str(),
                 title_x,
                 title_y,
                 title_max_w,
                 left_safe,
                 right_safe);
    }
    int title_text_x = title_x;
    if (title_icon_w > 0) {
        const int icon_y = rawdraw::InkCenteredTextTopYInBox(&fa_settings_16, title_icon, 0, bar_height, 0);
        DrawStyledText(fb, width, title_x, icon_y, title_icon, &fa_settings_16, text_style, height);
        title_text_x += title_icon_w + title_icon_gap;
    }
    DrawStyledText(fb, width, title_text_x, title_y, display_title.c_str(), title_font, text_style);
}

void RawDrawUiManager::DrawQuickSwitchOverlay(uint8_t* fb, int width, int height) {
    const auto& items = GetQuickSwitchItems();
    const rawdraw::Rect overlay = GetQuickSwitchBounds();
    const int overlay_x = overlay.x;
    const int overlay_y = overlay.y;
    const int overlay_w = overlay.w;
    const int overlay_h = overlay.h;
    const int titlebar_h = 28;
    const int shadow_offset = 2;
    const int item_h = 24;
    const int item_gap = 3;
    const lv_font_t* font = &SourceHanSansSC_Regular_slim;
    const lv_font_t* title_font = &SourceHanSansSC_Regular_slim;
    const auto& theme = rawdraw::ThemeManager::Get();
    const rawdraw::PaintStyle modal_style = theme.Component(rawdraw::ComponentRole::Modal);
    const rawdraw::PaintStyle selected_style = theme.Component(rawdraw::ComponentRole::QuickSwitchRow);
    const rawdraw::PaintStyle text_style = theme.Style(rawdraw::ThemeToken::TextPrimary);
    const rawdraw::PaintStyle border_style = theme.Style(rawdraw::ThemeToken::Border);
    const rawdraw::PaintStyle shadow_style = theme.Style(rawdraw::ThemeToken::Shadow);

    // Macintosh-style modal: solid offset shadow first, then a clean white
    // surface. This mirrors the Settings/About dialog and leaves only a 2px
    // right/bottom shadow visible, with no white gap between layers.
    rawdraw::DrawStyledRoundRect(fb, width, height,
                                 {overlay_x + shadow_offset, overlay_y + shadow_offset, overlay_w, overlay_h},
                                 Style::kBorderRadiusMD, shadow_style);
    rawdraw::DrawStyledRoundRect(fb, width, height, {overlay_x, overlay_y, overlay_w, overlay_h},
                                 Style::kBorderRadiusMD, modal_style);
    const char* title = "Quick switch";
    const int title_w = rawdraw::MeasureTextWidth(title, title_font);
    rawdraw::DrawStyledText(fb, width, overlay_x + (overlay_w - title_w) / 2,
                            rawdraw::InkCenteredTextTopYInBox(title_font, title, overlay_y, titlebar_h, 0),
                            title, title_font, text_style, height);
    rawdraw::DrawHLine(fb, width, overlay_y + titlebar_h, overlay_x + 1, overlay_x + overlay_w - 2, border_style.border);
    for (int yy = overlay_y + 6; yy < overlay_y + titlebar_h - 5; yy += 4) {
        rawdraw::DrawHLine(fb, width, yy, overlay_x + 14, overlay_x + (overlay_w - title_w) / 2 - 8, border_style.border);
        rawdraw::DrawHLine(fb, width, yy, overlay_x + (overlay_w + title_w) / 2 + 8, overlay_x + overlay_w - 14, border_style.border);
    }

    int y = overlay_y + titlebar_h + 10;
    const int kVisibleCount = 5;
    const int total = static_cast<int>(items.size());
    // Render only visible items
    for (int vi = 0; vi < kVisibleCount; ++vi) {
        int i = quick_switch_first_visible_ + vi;
        if (i >= total) break;
        const bool selected = i == quick_switch_index_;
        const int row_x = overlay_x + 12;
        const int row_w = overlay_w - 24 - 10;  // Leave space for scrollbar
        const rawdraw::PaintStyle row_style = selected ? selected_style : modal_style;
        if (selected) {
            rawdraw::DrawStyledRoundRect(fb, width, height, {row_x, y, row_w, item_h},
                                         Style::kBorderRadiusMD, row_style);
            rawdraw::DrawRect(fb, width, {row_x + 5, y + 6, 3, item_h - 12}, row_style.border);
        } else {
            rawdraw::DrawStyledRoundRect(fb, width, height, {row_x, y, row_w, item_h},
                                         Style::kBorderRadiusMD, row_style);
        }
        // Draw icon (if present) + label
        const lv_font_t* icon_font = &fa_settings_16;
        const int icon_gap = 6;
        const int text_start_x = row_x + 18;
        const rawdraw::Color text_color = row_style.fg;
        
        if (items[i].icon && items[i].icon[0] != '\0') {
            // Draw icon first
            const int icon_x = text_start_x;
            const int icon_y = rawdraw::InkCenteredTextTopYInBox(icon_font, items[i].icon, y, item_h, 0);
            rawdraw::DrawText(fb, width, icon_x, icon_y, items[i].icon, icon_font, text_color, height);
            
            // Draw label after icon with gap
            const int icon_w = rawdraw::MeasureTextWidth(items[i].icon, icon_font);
            const int label_x = icon_x + icon_w + icon_gap;
            const int label_y = rawdraw::InkCenteredTextTopYInBox(font, items[i].label, y, item_h, 0);
            rawdraw::DrawText(fb, width, label_x, label_y, items[i].label, font, text_color, height);
        } else {
            // No icon, draw label directly
            rawdraw::DrawText(fb, width, text_start_x,
                              rawdraw::InkCenteredTextTopYInBox(font, items[i].label, y, item_h, 0),
                              items[i].label, font, text_color, height);
        }
        y += item_h + item_gap;
    }

    // Scrollbar (only when items exceed visible count)
    if (total > kVisibleCount) {
        const int sb_w = 4;
        const int sb_x = overlay_x + overlay_w - 14;
        const int sb_top = overlay_y + titlebar_h + 10;
        const int sb_bottom = overlay_y + overlay_h - 28;
        const int sb_track_h = sb_bottom - sb_top;

        // Track
        rawdraw::DrawStyledRect(fb, width, {sb_x, sb_top, sb_w, sb_track_h}, modal_style);
        rawdraw::DrawRectBorder(fb, width, {sb_x, sb_top, sb_w, sb_track_h}, 1, border_style.border);

        // Thumb
        const int thumb_h = std::max(8, sb_track_h * kVisibleCount / total);
        const int max_scroll = std::max(1, total - kVisibleCount);
        const int thumb_y = sb_top + (sb_track_h - thumb_h) * quick_switch_first_visible_ / max_scroll;
        rawdraw::DrawRect(fb, width, {sb_x, thumb_y, sb_w, thumb_h}, selected_style.border);
    }

    rawdraw::DrawHLine(fb, width, overlay_y + overlay_h - 24, overlay_x + 14, overlay_x + overlay_w - 14, border_style.border);
    rawdraw::DrawStyledText(fb, width, overlay_x + 18,
                            rawdraw::InkCenteredTextTopYInBox(font, "UP/DN select  BOOT open", overlay_y + overlay_h - 24, 24, 0),
                            "UP/DN select  BOOT open", font, text_style, height);
}

rawdraw::Rect RawDrawUiManager::GetQuickSwitchBounds() const {
    const int overlay_w = 224;
    const int overlay_h = 204;
    return {(width_ - overlay_w) / 2, Style::kStatusBarHeight + 26, overlay_w, overlay_h};
}

void RawDrawUiManager::SnapshotQuickSwitchBacking(uint8_t* fb) {
    if (!fb || !lcd_) return;
    // The whole framebuffer, at its real byte length. On the 4-color panel that
    // is 2bpp (30000 bytes); a (width+7)/8 * height recompute would give the
    // 1bpp size (15000) and snapshot only the top half — which is exactly what
    // left half the quick-switch overlay on screen after it closed.
    const size_t frame_bytes = lcd_->GetFramebufferLen();
    if (frame_bytes == 0) return;
    quick_switch_backing_.assign(fb, fb + frame_bytes);
}

void RawDrawUiManager::RestoreQuickSwitchBacking(uint8_t* fb) {
    if (!fb || !lcd_ || quick_switch_backing_.empty()) return;
    const size_t frame_bytes = lcd_->GetFramebufferLen();
    if (frame_bytes == 0 || quick_switch_backing_.size() != frame_bytes) return;
    memcpy(fb, quick_switch_backing_.data(), frame_bytes);
    quick_switch_backing_.clear();
}

void RawDrawUiManager::RedrawQuickSwitchOnly(uint8_t* fb) {
    if (!fb) return;
    RestoreQuickSwitchBacking(fb);
    SnapshotQuickSwitchBacking(fb);
    DrawQuickSwitchOverlay(fb, width_, height_);
}

void RawDrawUiManager::RefreshRect(const rawdraw::Rect& rect, bool urgent) {
    if (!lcd_ || !refresh_cb_) return;
    rawdraw::Rect refresh_rect = rawdraw::align_x8(rawdraw::clamp_rect(
        {rect.x - 4, rect.y - 4, rect.w + 12, rect.h + 12}, width_, height_));
    auto* mutex = lcd_->GetMutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    if (mutex) {
        xSemaphoreGive(mutex);
        mutex = nullptr;
    }
    refresh_cb_(refresh_rect, urgent);
}

// ============================================================
// Refresh
// ============================================================

void RawDrawUiManager::TriggerRefresh(bool urgent) {
    if (!lcd_) return;

    // Use the framebuffer's dirty tracking
    // Mark the entire screen as dirty since we did a full clear + render
    auto* mutex = lcd_->GetMutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

    rawdraw::Rect full_rect = {0, 0, width_, height_};

    // Align for EPD hardware
    full_rect = rawdraw::align_x8(full_rect);

    if (refresh_cb_) {
        // Release mutex before calling callback (callback may re-acquire)
        if (mutex) xSemaphoreGive(mutex);
        mutex = nullptr;

        // The panel is about to be busy for anything up to minutes, so input
        // locks here rather than at one of the call sites. Locking only in
        // HandleInput() covered a renderer-handled click and nothing else:
        // SwitchPage(), RefreshActivePage(), ConfirmQuickSwitch(), GoHome(),
        // kExitTransfer and kRepaintPage all reach the panel through this
        // function without going through HandleInput(), so SyncNavContext()
        // kept reporting refresh_busy=false during their refreshes and every
        // further click applied immediately. That is the burst the depth-1
        // latch exists to prevent, and it was prevented in the model only.
        //
        // The release is unchanged: SetOnRefreshIdle() clears the flag when
        // the panel goes busy -> idle, and every path here marks the display
        // dirty, so an idle transition is always owed.
        input_refresh_locked_.store(true, std::memory_order_release);
        refresh_cb_(full_rect, urgent);
    }

    if (mutex) xSemaphoreGive(mutex);

    if (full_refresh_pending_) {
        full_refresh_pending_ = false;
        // The refresh callback should handle full vs partial refresh
        // based on the urgent flag or other state
    }
}

void RawDrawUiManager::RequestFullRefresh() {
    full_refresh_pending_ = true;
}

dashboard::RenderOutcome RawDrawUiManager::PaintDashboardFrame(
    const uint8_t* frame, int64_t timeout_us, uint32_t* panel_ms_out) {
    if (panel_ms_out) *panel_ms_out = 0;
    if (frame == nullptr || !dashboard_renderer_ || lcd_ == nullptr) {
        return dashboard::RenderOutcome::kFailed;
    }
    if (!dashboard_renderer_->SetFrame(frame)) {
        ESP_LOGE(kTag, "Dashboard renderer has no frame buffer");
        return dashboard::RenderOutcome::kFailed;
    }
    if (current_page_ != RawDrawPageId::Dashboard) {
        // An arriving frame does not steal the screen from someone reading
        // another page. It is already persisted; it appears when the dashboard
        // is opened again. This is a benign, expected refusal — reported as
        // kDeferred so the status route does not surface it as a fault.
        ESP_LOGI(kTag, "Dashboard frame stored while another page is open");
        return dashboard::RenderOutcome::kDeferred;
    }

    const int64_t t0 = esp_timer_get_time();

    // The ordering rules live in dashboard::RenderHandshake, which the host
    // tests exercise directly. An earlier version polled IsDisplayRefreshPending()
    // instead; that was wrong in three ways. It could observe a busy flag set by
    // some other requester and attribute that time to this frame, it had no way
    // to tell a completed refresh from one where read_busy() gave up on the BUSY
    // pin, and its timeout was shorter than a single read_busy() timeout, so a
    // legitimately slow refresh was reported as a failure.
    dashboard::RenderHandshakeHooks hooks;
    hooks.drain = [this]() { return lcd_->DrainRefreshDoneSignals(); };
    hooks.trigger = [this]() { RefreshActivePage(true); };
    hooks.wait = [this](uint32_t ms) { return lcd_->WaitRefreshDone(ms); };
    hooks.panel_ok = [this]() { return lcd_->LastRefreshOk(); };
    // Rule 1a. The clock tick, the status bar and the navigation pump all
    // trigger refreshes from the main task while this runs on the dashboard
    // render task. Draining cannot remove the completion signal a refresh that
    // is already in flight has not posted yet, so the handshake has to wait for
    // that one before it triggers — otherwise it adopts a stranger's signal and
    // a stranger's panel_ok, and the manager records a frame as displayed that
    // the panel never drew.
    hooks.panel_busy = [this]() { return lcd_->IsRefreshPending(); };

    // Never shorter than the driver's own worst case, so a timeout here always
    // means "the driver never came back", never "we gave up on a refresh that
    // was proceeding normally".
    uint32_t timeout_ms = static_cast<uint32_t>(timeout_us / 1000);
    if (timeout_ms < CustomLcdDisplay::kWorstCaseRefreshMs) {
        timeout_ms = CustomLcdDisplay::kWorstCaseRefreshMs;
    }

    const dashboard::AckResult ack = render_handshake_.Run(hooks, timeout_ms);

    if (panel_ms_out) {
        *panel_ms_out = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    }

    switch (ack) {
        case dashboard::AckResult::kCompleted:
            return dashboard::RenderOutcome::kDrawn;
        case dashboard::AckResult::kPanelFailed:
            ESP_LOGE(kTag, "Panel reported a BUSY timeout; the frame is not "
                           "known to be on the glass (busy timeouts total=%u)",
                     static_cast<unsigned>(lcd_->BusyTimeoutCount()));
            return dashboard::RenderOutcome::kFailed;
        case dashboard::AckResult::kTimedOut:
            ESP_LOGE(kTag, "No refresh completion after %u ms", static_cast<unsigned>(timeout_ms));
            return dashboard::RenderOutcome::kFailed;
        case dashboard::AckResult::kNotAttempted:
            ESP_LOGW(kTag, "Refresh handshake already in progress; frame not painted");
            return dashboard::RenderOutcome::kFailed;
    }
    return dashboard::RenderOutcome::kFailed;
}

void RawDrawUiManager::RequestActivePageRefresh() {
    active_page_refresh_pending_.store(true, std::memory_order_release);
}

bool RawDrawUiManager::ShowPhotoById(const std::string& photo_id) {
    if (!photo_gallery_renderer_ || photo_id.empty()) {
        return false;
    }
    photo_gallery_renderer_->RefreshPhotoList();
    if (!photo_gallery_renderer_->SetSelectedById(photo_id.c_str())) {
        ESP_LOGW(kTag, "ShowPhotoById failed: id=%s not found", photo_id.c_str());
        return false;
    }
    photo_gallery_renderer_->EnterFullscreenMode();
    SwitchPage(RawDrawPageId::Gallery);
    ESP_LOGI(kTag, "Show photo fullscreen from HTTP: id=%s", photo_id.c_str());
    return true;
}

void RawDrawUiManager::SetGallerySlideshowIntervalMinutes(int minutes) {
    gallery_slideshow_interval_minutes_ = std::max(0, minutes);
    gallery_slideshow_pending_.store(false, std::memory_order_release);
    ArmGallerySlideshowTimer();
    if (gallery_slideshow_interval_minutes_ <= 0) {
        ESP_LOGI(kTag, "Gallery slideshow disabled");
    } else {
        ESP_LOGI(kTag, "Gallery slideshow interval=%d minutes", gallery_slideshow_interval_minutes_);
    }
}

bool RawDrawUiManager::IsGallerySlideshowRunning() const {
    nav::SlideshowInputs in;
    in.interval_minutes = gallery_slideshow_interval_minutes_;
    in.on_gallery_page = (current_page_ == RawDrawPageId::Gallery);
    if (photo_gallery_renderer_) {
        in.fullscreen = photo_gallery_renderer_->IsFullscreenMode();
        in.dialog_open = photo_gallery_renderer_->IsDeleteDialogOpen();
    }
    return nav::SlideshowIsRunning(in);
}

void RawDrawUiManager::ArmGallerySlideshowTimer() {
    if (gallery_slideshow_timer_ == nullptr) return;
    esp_timer_stop(gallery_slideshow_timer_);
    if (gallery_slideshow_interval_minutes_ <= 0) return;

    const int64_t delay_us = static_cast<int64_t>(gallery_slideshow_interval_minutes_) * 60 * 1000 * 1000;
    esp_err_t ret = esp_timer_start_once(gallery_slideshow_timer_, delay_us);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Failed to arm slideshow timer: %s", esp_err_to_name(ret));
    }
}

void RawDrawUiManager::OnGallerySlideshowTimer(void* arg) {
    auto* self = static_cast<RawDrawUiManager*>(arg);
    if (self != nullptr) {
        self->gallery_slideshow_pending_.store(true, std::memory_order_release);
    }
}

bool RawDrawUiManager::AdvanceGallerySlideshow() {
    ArmGallerySlideshowTimer();
    if (current_page_ != RawDrawPageId::Gallery || !photo_gallery_renderer_) {
        return false;
    }
    if (!photo_gallery_renderer_->IsFullscreenMode() ||
        photo_gallery_renderer_->IsDeleteDialogOpen()) {
        return false;
    }
    if (!photo_gallery_renderer_->SelectNext(true)) {
        return false;
    }
    ESP_LOGI(kTag, "Gallery fullscreen slideshow advanced");
    RefreshActivePage(false);
    return true;
}

// ============================================================
// Status bar updates
// ============================================================

void RawDrawUiManager::UpdateStatusBar(const RawDrawStatusBarData& data) {
    std::lock_guard<std::mutex> lock(ui_state_mutex_);
    const bool changed = status_bar_data_.wifi_connected != data.wifi_connected ||
                         status_bar_data_.server_connected != data.server_connected ||
                         status_bar_data_.bluetooth_enabled != data.bluetooth_enabled ||
                         status_bar_data_.battery_level != data.battery_level ||
                         status_bar_data_.battery_vertical != data.battery_vertical ||
                         status_bar_data_.page_title != data.page_title ||
                         status_bar_data_.central_text != data.central_text;
    if (!data.page_title.empty()) {
        status_bar_data_.page_title = data.page_title;
    }
    status_bar_data_.central_text = data.central_text;
    status_bar_data_.wifi_connected = data.wifi_connected;
    status_bar_data_.server_connected = data.server_connected;
    status_bar_data_.bluetooth_enabled = data.bluetooth_enabled;
    status_bar_data_.battery_level = data.battery_level;
    status_bar_data_.battery_charging = data.battery_charging;
    status_bar_data_.battery_vertical = data.battery_vertical;
    status_bar_data_.date_format = data.date_format;
    status_bar_data_.server_date = data.server_date;
    status_bar_data_.server_weekday = data.server_weekday;
    if (changed) {
        ESP_LOGI(kTag, "statusbar update wifi=%d server=%d bt=%d battery=%d vertical=%d title='%s' central='%s'",
                 status_bar_data_.wifi_connected ? 1 : 0,
                 status_bar_data_.server_connected ? 1 : 0,
                 status_bar_data_.bluetooth_enabled ? 1 : 0,
                 status_bar_data_.battery_level,
                 status_bar_data_.battery_vertical ? 1 : 0,
                 status_bar_data_.page_title.c_str(),
                 status_bar_data_.central_text.c_str());
    }
}

void RawDrawUiManager::ArmClockRefreshTimer() {
    if (!kEnableMinuteClockRefresh) {
        if (clock_refresh_timer_ != nullptr) {
            esp_timer_stop(clock_refresh_timer_);
        }
        return;
    }
    if (clock_refresh_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(clock_refresh_timer_);
    const int64_t delay_ms = MsUntilNextMinuteBoundary();
    esp_err_t ret = esp_timer_start_once(clock_refresh_timer_, delay_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Failed to arm clock refresh timer: %s", esp_err_to_name(ret));
    }
}

void RawDrawUiManager::ArmTransientRefreshTimer(int delay_ms) {
    if (transient_refresh_timer_ == nullptr) return;
    esp_timer_stop(transient_refresh_timer_);
    const int ms = std::max(1, delay_ms);
    esp_err_t ret = esp_timer_start_once(transient_refresh_timer_, static_cast<int64_t>(ms) * 1000);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Failed to arm transient refresh timer: %s", esp_err_to_name(ret));
    }
}

void RawDrawUiManager::OnClockRefreshTimer(void* arg) {
    auto* self = static_cast<RawDrawUiManager*>(arg);
    if (self != nullptr) {
        self->clock_refresh_pending_.store(true, std::memory_order_release);
    }
}

void RawDrawUiManager::OnTransientRefreshTimer(void* arg) {
    auto* self = static_cast<RawDrawUiManager*>(arg);
    if (self != nullptr) {
        self->transient_refresh_pending_.store(true, std::memory_order_release);
    }
}

void RawDrawUiManager::PumpPendingNavIntent() {
    const bool idle_signalled = pending_nav_intent_.exchange(false, std::memory_order_acq_rel);
    if (input_refresh_locked_.load(std::memory_order_acquire)) {
        // A new refresh started before the pump ran. The intent stays latched
        // and the next idle will signal again; applying it now would land in
        // the middle of a panel refresh, which is the thing the latch exists
        // to prevent.
        return;
    }
    // The latch is authoritative, not the flag. If the refresh-idle callback
    // fired in the window between SyncNavContext() and Decide() latching the
    // click, no flag was ever raised for it, and the click would otherwise
    // sleep until some unrelated refresh happened to finish.
    if (!idle_signalled && !nav_model_.HasPendingIntent()) {
        return;
    }
    SyncNavContext();
    const nav::Action pending = nav_model_.OnRefreshIdle();
    if (pending.type == nav::ActionType::kNone) {
        return;
    }
    char buf[48];
    ESP_LOGI(kTag, "Applying the navigation intent held during the refresh: %s",
             nav::ActionString(pending, buf, sizeof(buf)));
    ApplyNavAction(pending);
}

void RawDrawUiManager::PumpClockRefresh() {
    PumpPendingNavIntent();
    const bool page_pending = active_page_refresh_pending_.exchange(false, std::memory_order_acq_rel);
    const bool transient_pending = transient_refresh_pending_.exchange(false, std::memory_order_acq_rel);
    const bool slideshow_pending = gallery_slideshow_pending_.exchange(false, std::memory_order_acq_rel);
    if (slideshow_pending) {
        AdvanceGallerySlideshow();
    }
    if (page_pending || transient_pending) {
        RefreshActivePage(false);
    }

    if (!kEnableMinuteClockRefresh) {
        clock_refresh_pending_.store(false, std::memory_order_release);
        return;
    }

    if (!clock_refresh_pending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const int minute_key = CurrentLocalMinuteKey();
    if (minute_key == last_clock_minute_key_) {
        ArmClockRefreshTimer();
        return;
    }

    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (fb != nullptr) {
        auto* mutex = lcd_->GetMutex();
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
        // Do NOT Clear() the whole buffer — redraw directly over existing pixels.
        // Only the status bar (clock) region changes each minute, so analyze_frame_diff
        // will see a small diff_ratio and trigger partial refresh (EPD_DisplayPart)
        // instead of a full refresh (EPD_Display) that causes visible flashing.
        RenderAll(fb, width_, height_);
        if (mutex) xSemaphoreGive(mutex);
        TriggerRefresh(false);
    }

    last_clock_minute_key_ = minute_key;
    ArmClockRefreshTimer();
}

// ============================================================
// Chat page data updates
// ============================================================

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::AddChatMessage(const std::string& text, rawdraw::ChatRole role) {
    if (chat_renderer_) {
        chat_renderer_->AddMessage(text, role);
        chat_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::ClearChat() {
    if (chat_renderer_) {
        chat_renderer_->Clear();
        chat_renderer_->MarkFullRefresh();
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::BeginChatStream() {
    if (chat_renderer_) {
        chat_renderer_->BeginStream();
        chat_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
bool RawDrawUiManager::AppendChatText(const char* chunk) {
    if (!chat_renderer_ || !chunk) return false;

    bool appended = chat_renderer_->AppendText(chunk);
    if (appended && current_page_ == RawDrawPageId::Chat) {
        chat_renderer_->MarkFullRefresh();
        RequestActivePageRefresh();
    }
    return appended;
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::EndChatStream() {
    if (chat_renderer_) {
        chat_renderer_->EndStream();
        chat_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::ShowChatStatus(const std::string& status, rawdraw::ChatRole role) {
    if (chat_renderer_) {
        chat_renderer_->ShowStatus(status, role);
        chat_renderer_->MarkFullRefresh();
        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::HideChatStatus() {
    if (chat_renderer_) {
        chat_renderer_->HideStatus();
        chat_renderer_->MarkFullRefresh();
        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::SetChatListening(bool listening) {
    if (chat_renderer_) {
        chat_renderer_->SetListening(listening);
        chat_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

#if !DASHBOARD_MINIMAL_UI
void RawDrawUiManager::SetChatBottomStatus(const std::string& status) {
    if (chat_renderer_) {
        chat_renderer_->SetBottomStatus(status);
        chat_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Chat) {
            RequestActivePageRefresh();
        }
    }
}
#endif

// ============================================================
// Settings page data updates
// ============================================================

void RawDrawUiManager::SetSettingsItems(const std::vector<rawdraw::SettingsItemDef>& items) {
    if (settings_renderer_) {
        settings_renderer_->SetItems(items);
        settings_renderer_->MarkFullRefresh();

        if (current_page_ == RawDrawPageId::Settings) {
            auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
            if (fb) {
                auto* mutex = lcd_->GetMutex();
                if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

                rawdraw::Clear(fb, width_, height_);
                RenderAll(fb, width_, height_);

                if (mutex) xSemaphoreGive(mutex);

                TriggerRefresh(false);
            }
        }
    }
}

void RawDrawUiManager::UpdateSettingsItem(int index, const std::string& value) {
    if (settings_renderer_) {
        settings_renderer_->UpdateItem(index, value);
        settings_renderer_->MarkFullRefresh();
    }
}

void RawDrawUiManager::UpdateSettingsChecked(int index, bool checked) {
    if (settings_renderer_) {
        settings_renderer_->UpdateChecked(index, checked);
        settings_renderer_->MarkFullRefresh();
    }
}

// ============================================================
// WiFi page data updates
// ============================================================

void RawDrawUiManager::UpdateWifiStatus(const rawdraw::WifiStatus& status) {
    if (wifi_renderer_) {
        wifi_renderer_->Update(status);
        wifi_renderer_->MarkFullRefresh();

        // Also update status bar WiFi indicator
        {
            std::lock_guard<std::mutex> lock(ui_state_mutex_);
            status_bar_data_.wifi_connected = (status.state == rawdraw::WifiState::Connected);
            status_bar_data_.server_connected = status.server_connected;
        }

        if (current_page_ == RawDrawPageId::Wifi) {
            auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
            if (fb) {
                auto* mutex = lcd_->GetMutex();
                if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

                rawdraw::Clear(fb, width_, height_);
                RenderAll(fb, width_, height_);

                if (mutex) xSemaphoreGive(mutex);

                TriggerRefresh(false);
            }
        }
    }
}

rawdraw::WifiStatus RawDrawUiManager::GetWifiStatus() const {
    if (wifi_renderer_) {
        return wifi_renderer_->GetStatus();
    }
    return {};
}

void RawDrawUiManager::SetWifiBlinking(bool blinking) {
    if (wifi_renderer_) {
        wifi_renderer_->SetBlinking(blinking);
        wifi_renderer_->MarkFullRefresh();
    }
}

// ============================================================
// LifeBar visibility toggle
// ============================================================

void RawDrawUiManager::SetLifeBarVisible(bool visible) {
#if DASHBOARD_MINIMAL_UI
    // The life-bar page is not built in this configuration.
    (void)visible;
#else
    if (lifebar_renderer_) {
        lifebar_renderer_->SetVisible(visible);
        lifebar_renderer_->MarkFullRefresh();

        // If currently on the LifeBar page, re-render
        if (current_page_ == RawDrawPageId::LifeBar) {
            auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
            if (fb) {
                auto* mutex = lcd_->GetMutex();
                if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
                rawdraw::Clear(fb, width_, height_);
                RenderAll(fb, width_, height_);
                if (mutex) xSemaphoreGive(mutex);
                TriggerRefresh(false);
            }
        }
    }
#endif
}

bool RawDrawUiManager::IsLifeBarVisible() const {
#if !DASHBOARD_MINIMAL_UI
    if (lifebar_renderer_) {
        return lifebar_renderer_->IsVisible();
    }
#endif
    return true;
}

// ============================================================
// Clock and voice wakeup integration
// ============================================================

void RawDrawUiManager::VoiceWakeupTick() {
    int64_t now = esp_timer_get_time();
    rawdraw::VoiceWakeupTick(&voice_wakeup_state_, now);
}

void RawDrawUiManager::VoiceWakeupTrigger(bool network_available) {
    if (network_available) {
        rawdraw::VoiceWakeupStartRecording(&voice_wakeup_state_);
    } else {
        rawdraw::VoiceWakeupShowOffline(&voice_wakeup_state_);
    }

    // Render the overlay
    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (fb) {
        auto* mutex = lcd_->GetMutex();
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

        RenderAll(fb, width_, height_);

        if (mutex) xSemaphoreGive(mutex);

        TriggerRefresh(false);
    }
}

void RawDrawUiManager::VoiceWakeupDone() {
    rawdraw::VoiceWakeupDone(&voice_wakeup_state_);

    auto* fb = lcd_ ? lcd_->GetFramebuffer() : nullptr;
    if (fb) {
        auto* mutex = lcd_->GetMutex();
        if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);

        RenderAll(fb, width_, height_);

        if (mutex) xSemaphoreGive(mutex);

        TriggerRefresh(false);
    }
}

bool RawDrawUiManager::VoiceWakeupIsActive() const {
    return rawdraw::VoiceWakeupIsVisible(&voice_wakeup_state_);
}

}  // namespace ui
