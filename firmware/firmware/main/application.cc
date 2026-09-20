#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/device_config_service.h"
#include "common/photo_storage.h"
#include "display.h"
#include "product_identity.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "ui/renderers/rawdraw/settings_menu_order.h"
#include "wifi_manager.h"

#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_sntp.h>

// Named rather than inherited. The autonomy block below allocates its PSRAM
// buffers, placement-news a Profile into one of them, and opens the SPIFFS slot
// files; all four headers were reaching this translation unit transitively, and
// a transitive include is a build that breaks for a reason nobody can see.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include "common/autonomy_compose.h"
#include "common/autonomy_cycle.h"
#include "common/autonomy_service.h"
#include "common/autonomy_status.h"
#include "common/openmeteo_client.h"
#include "common/weather_cache.h"
#include "common/dashboard_manager.h"
#include "dashboard_build_config.h"
#include <esp_system.h>
#include <esp_wifi.h>

#include <ctime>

// Narrow accessors into the board file, declared here the same way
// factory_test_service.cc declares them: this board's charge pins and battery
// ADC are not part of the cross-board Board interface, and widening that
// interface for one board would be the larger change.
#include "boards/zectrix-s3-epaper-4.2/charge_status.h"
extern "C" ChargeStatus::Snapshot ZectrixGetChargeSnapshot();
extern "C" bool ZectrixReadBatteryMillivolts(uint16_t* mv_out, bool* calibrated_out);

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
// Software mute for the voice path, persisted so a device that was muted
// before a power cut is still muted after it (PRODUCT-PLAN.md section 3.2).
// It is read once at boot, before any gesture can reach the state machine.
constexpr char kVoiceNamespace[] = "voice";
constexpr char kVoiceMutedKey[] = "muted";
// The hybrid low-power settings. The *base* mode only: an interactive window
// is live state with a deadline and is never written here, because a window
// that came back from NVS would be a window nobody opened.
constexpr char kPowerNamespace[] = "power";
constexpr char kPowerModeKey[] = "mode";
constexpr char kPowerInteractiveKey[] = "inter_min";
constexpr char kPowerWakeIntervalKey[] = "wake_min";
/// Consecutive failed update cycles, persisted so the retry ladder survives
/// the deep sleep that sits between two attempts. Without this every wake
/// would look like the first failure and retry at the same short delay all day.
constexpr char kPowerFailuresKey[] = "fails";
/**
 * @brief The autonomy kill-switch, persisted.
 *
 * In its own namespace rather than under "power": it is a content decision, not
 * an energy one, and the two are read at different points in the boot.
 *
 * Persisted at all because the wake cycle goes through deep sleep, which is a
 * reboot. A switch that reset on every wake would mean the feature could only
 * ever run while somebody was standing there to re-enable it. The default when
 * the key has never been written is false, so a device that has never been
 * asked is a push target — persistence remembers a decision, it does not make
 * one.
 */
constexpr char kAutonomyNamespace[] = "autonomy";
constexpr char kAutonomyEnabledKey[] = "enabled";
/**
 * @brief How little budget must be left before a deferred cycle restarts it.
 *
 * A cycle that is deferred — a refresh in flight, the provisioning portal, a
 * slideshow — must not be allowed to run out, because that would record a
 * budget-exhausted failure nothing did wrong. But restarting it on every tick
 * would rewrite the budget once a second for as long as the deferral lasted.
 * So it is topped up only when it is genuinely running low.
 */
constexpr uint32_t kCycleDeferRestartMs = 30000;
/**
 * @brief How often the push-to-talk timeouts are advanced.
 *
 * 25 ms. The arm threshold is 300 ms, so this is the worst-case error between
 * the moment a hold becomes an utterance and the moment the machine notices.
 * The previous once-a-second tick in Run() could be four times the threshold
 * late, which is the difference between hold-to-talk and a button that seems
 * not to work.
 */
constexpr int64_t kPttTickPeriodUs = 25 * 1000;
// Every settings-row index is derived from the one place the order lives now:
// rawdraw::settings_menu::Item. The builder below pushes rows in that same
// order, so an index and the row it names cannot drift apart, and the menu can
// be reordered by editing the enum rather than by hand-counting literals.
namespace smenu = rawdraw::settings_menu;
constexpr int kSettingsSlideshowIndex = smenu::Index(smenu::Item::SlideshowInterval);
constexpr int kSettingsWifiIndex = smenu::Index(smenu::Item::Wifi);
constexpr int kSettingsHttpServerIndex = smenu::Index(smenu::Item::LanService);
constexpr int kSettingsLanIpIndex = smenu::Index(smenu::Item::LanAddress);
// Upper bound on one four-color panel refresh.
//
// This must exceed the driver's own worst case, not a guess at how long a
// refresh "should" take. custom_lcd_display.cc read_busy() waits up to 120 s
// per call and EPD_TurnOnDisplay() calls it three times, so the driver can
// legitimately be inside one refresh for about six minutes. An earlier version
// used 90 s here, which is shorter than a *single* read_busy() timeout and
// would have reported healthy slow refreshes as failures.
//
// PaintDashboardFrame clamps this up to CustomLcdDisplay::kWorstCaseRefreshMs
// anyway; the value is kept explicit here so the reasoning is visible at the
// call site.
constexpr int64_t kPanelRefreshTimeoutUs =
    static_cast<int64_t>(CustomLcdDisplay::kWorstCaseRefreshMs) * 1000;
// The dashboard rows now sit near the top (frequent actions), and their
// indices come from the same enum as everything else rather than from a count
// of where they happened to land.
constexpr int kSettingsPairIndex = smenu::Index(smenu::Item::PairDashboard);
constexpr int kSettingsLockdownIndex = smenu::Index(smenu::Item::BlockLegacyWrites);

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "Off";
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "Off" : "On";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

void UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return;
    renderer->UpdateChecked(kSettingsWifiIndex, connected);
    renderer->UpdateItem(kSettingsWifiIndex, value ? value : (connected ? "Connected" : "Not connected"));
}

void UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "On" : "Off";
    }
    renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    renderer->UpdateItem(kSettingsHttpServerIndex, value);
}

void UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return;
    renderer->UpdateItem(kSettingsLanIpIndex, ip_address.empty() ? "Not assigned" : ip_address);
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    // Montreal, with DST rules, replacing the upstream Asia/Shanghai default.
    // The dashboard image itself carries its own timestamp, but the device
    // clock still drives sleep scheduling and log timestamps.
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // Region-neutral pool only. The upstream aliyun/cn servers are the wrong
    // side of the planet here and are an avoidable third-party dependency.
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().UpdateStatusBarForUi();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=America/Montreal servers=pool.ntp.org,time.nist.gov");
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
        esp_timer_delete(sleep_timer_);
        sleep_timer_ = nullptr;
    }
    if (action_timer_ != nullptr) {
        esp_timer_stop(action_timer_);
        esp_timer_delete(action_timer_);
        action_timer_ = nullptr;
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // First, because the wake cause is what decides whether a person is
    // standing in front of the device, and several things below behave
    // differently when one is. esp_sleep_get_wakeup_cause() stays valid for
    // the whole boot, but reading it up front keeps the ordering obvious.
    InitializePowerState();

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });

    // Services the navigation model may need but must not reach for itself.
    ui::RawDrawUiManager::NavHooks nav_hooks;
    nav_hooks.enter_wifi_config_ap = [this]() { EnterWifiConfigMode(); };
    nav_hooks.exit_wifi_config_ap = [this]() { LeaveWifiConfigApIfActive(); };
    nav_hooks.repaint_dashboard_frame = [this]() { RepaintStoredDashboardFrame(); };
    nav_hooks.ptt_arm = [this]() { OnPushToTalkGesture(); };
    // Feedback the user can perceive before the panel does anything. On a
    // four-color panel a refresh can take tens of seconds, so this is the only
    // acknowledgement that arrives while the finger is still on the button.
    nav_hooks.note_activity = []() { Board::GetInstance().FlashActivityLed(); };
    rawdraw_ui_manager_->SetNavHooks(std::move(nav_hooks));
    InitializeAudioFsm();
    StartPttTickTimer();

    // Dashboard storage. Must come after photo_storage_init(), which is what
    // mounts /spiffs; the frame slots live in that same assets partition.
    const bool dashboard_ready = dashboard::DashboardManager::GetInstance().Init();
    if (!dashboard_ready) {
        ESP_LOGE(kTag, "Dashboard manager init failed; dashboard page unavailable");
    } else {
        // The painter runs on the dashboard render task, never on the HTTP
        // task, and is allowed to block for the whole panel refresh.
        dashboard::DashboardManager::GetInstance().SetPanelPainter(
            [this](const uint8_t* frame,
                   uint32_t* panel_ms_out) -> dashboard::RenderOutcome {
                if (!rawdraw_ui_manager_) return dashboard::RenderOutcome::kFailed;
                return rawdraw_ui_manager_->PaintDashboardFrame(
                    frame, kPanelRefreshTimeoutUs, panel_ms_out);
            });
    }

    // Restore the last dashboard into the renderer on boot. `has_frame_` is RAM
    // state the reboot clears, but the frame itself is persisted in SPIFFS, and
    // the panel physically still shows it. Without this, a wake redraws the
    // welcome/QR over the retained dashboard (the renderer thinks it holds
    // nothing), which is why the QR flashed back on every wake. SetFrame only
    // copies the bytes in and sets the flag — it does not paint on its own — so
    // this is quiet: the panel is left as it is until the next real refresh.
    if (dashboard_ready && rawdraw_ui_manager_ != nullptr) {
        auto& dash = dashboard::DashboardManager::GetInstance();
        auto* renderer = rawdraw_ui_manager_->GetDashboardRenderer();
        if (renderer != nullptr && dash.HasFrame()) {
            auto* buf = static_cast<uint8_t*>(
                heap_caps_malloc(dashboard::kFrameBytes, MALLOC_CAP_SPIRAM));
            if (buf != nullptr) {
                if (dash.CopyFrame(buf) && renderer->SetFrame(buf)) {
                    ESP_LOGI(kTag, "Restored the stored dashboard into the renderer on boot");
                } else {
                    ESP_LOGW(kTag, "Could not restore the stored dashboard frame on boot");
                }
                heap_caps_free(buf);
            }
        }
    }

    // The autonomy profile store and the forecast cache. Here, and not earlier,
    // for the same reason the dashboard storage above is here: both read files
    // out of /spiffs, and photo_storage_init() is what mounts it. Brought up
    // before InitializeConfigService(), which seeds the kill-switch from NVS,
    // and long before the LAN server exists — the profile routes only ask for
    // the store when a request arrives.
    //
    // This ran immediately after InitializePowerState() while it was being
    // written, which is before the mount. Every Load() then found no file,
    // reported "no valid autonomy profile stored", and quietly discarded a
    // pushed profile and a cached forecast on every single boot — with nothing
    // in the log to distinguish it from a device that had never been pushed to.
    InitializeAutonomy();

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        Settings gallery_nvs(kGalleryNamespace, false);
        int slideshow_interval = gallery_nvs.GetInt(kSlideshowIntervalKey, 5);
        if (slideshow_interval != 0 && slideshow_interval != 5 &&
            slideshow_interval != 10 && slideshow_interval != 30) {
            slideshow_interval = 5;
        }
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        // The row order lives in rawdraw::settings_menu::Item, and these
        // push_backs follow it exactly. The frequent actions the owner named —
        // Restart, Power saving, Pair dashboard, Block legacy writes — are
        // pulled into the first two sections (System, Dashboard) so they are
        // reachable without scrolling past every sub-setting; each e-paper
        // repaint while scrolling costs ~20 s, so a row near the top is worth
        // real seconds to the person standing at the fridge. The hard-coded row
        // indices above derive from the same enum, so order and index cannot
        // drift apart.
        auto& dash = dashboard::DashboardManager::GetInstance();
        std::vector<rawdraw::SettingsItemDef> items;

        // --- System: the two most common standalone actions ---
        items.push_back({"System", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Restart", "Run", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"Power saving", "Enter now", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});

        // --- Dashboard: hoisted near the top, its two rows are frequent ---
        items.push_back({"Dashboard", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Pair dashboard",
                         dash.Provisioned() ? "Paired" : "Not paired", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             // Opening the window is the physical half of
                             // pairing. The token itself is never shown here:
                             // e-paper keeps its last image with the power off,
                             // so anything drawn is effectively left lying about.
                             auto& d = dashboard::DashboardManager::GetInstance();
                             if (!d.OpenPairing()) {
                                 sr->UpdateItem(kSettingsPairIndex, "Unavailable");
                                 return;
                             }
                             char label[32];
                             snprintf(label, sizeof(label), "Open %us",
                                      static_cast<unsigned>(d.PairingRemainingSeconds()));
                             sr->UpdateItem(kSettingsPairIndex, label);
                             ESP_LOGI(kTag, "Dashboard pairing window opened from settings");
                         }});
        items.push_back({"Block legacy writes",
                         dash.LockdownEnabled() ? "On" : "Off", nullptr,
                         rawdraw::SettingsItemType::Checkbox, dash.LockdownEnabled(),
                         [sr]() {
                             // The device menu may turn lockdown off. The
                             // network may not, and that asymmetry is enforced
                             // in the patch validator rather than here: the
                             // rule is about who is asking, not about which
                             // value is allowed.
                             auto& d = dashboard::DashboardManager::GetInstance();
                             const bool next = !d.LockdownEnabled();
                             devcfg::DeviceConfigService::GetInstance()
                                 .ApplyLocalLockdown(next);
                             sr->UpdateChecked(kSettingsLockdownIndex, next);
                             sr->UpdateItem(kSettingsLockdownIndex, next ? "On" : "Off");
                         }});

        // --- Network ---
        items.push_back({"Network", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Wi-Fi", "Not connected", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                             if (wifi_connected_.load(std::memory_order_acquire) || wifi.IsConnected()) {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                 if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                     rawdraw_ui_manager_->StopLanHttpServer();
                                     devcfg::DeviceConfigService::GetInstance()
                                         .NoteLanService(false);
                                     UpdateHttpServerSettingsItem(sr, false);
                                 }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 UpdateWifiSettingsItem(sr, false, "Connecting");
                                 wifi.StartStation();
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"LAN service", "Off", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             const bool want = !rawdraw_ui_manager_->IsLanHttpServerRunning();
                             // The service runs the same hook the config API
                             // runs, so the physical toggle and a remote patch
                             // cannot end up doing two different things.
                             const bool running =
                                 devcfg::DeviceConfigService::GetInstance()
                                     .ApplyLocalLanService(want);
                             const std::string ip = WifiManager::GetInstance().GetIpAddress();
                             if (want && !running) {
                                 // It did not start, and the row says why
                                 // rather than showing an OFF the user did not
                                 // ask for with no explanation.
                                 const bool have_wifi =
                                     wifi_connected_.load(std::memory_order_acquire) ||
                                     WifiManager::GetInstance().IsConnected();
                                 UpdateHttpServerSettingsItem(
                                     sr, false,
                                     have_wifi ? "Waiting for IP" : "Connect Wi-Fi first");
                             } else {
                                 UpdateHttpServerSettingsItem(sr, running, running ? ip : "");
                                 UpdateLanIpSettingsItem(sr, ip);
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"LAN address", "Not assigned", nullptr, rawdraw::SettingsItemType::Normal, false});

        // --- Gallery: photo transfer belongs with the gallery it feeds ---
        items.push_back({"Gallery", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Slideshow interval", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [sr]() {
                             // Through the config service, not around it. The
                             // service persists, applies and bumps the config
                             // revision, so a tower holding a stale read loses
                             // its compare-and-swap instead of overwriting a
                             // change somebody just made by hand.
                             auto& service = devcfg::DeviceConfigService::GetInstance();
                             const int current =
                                 static_cast<int>(service.Snapshot().gallery_slide_min);
                             const int next = NextSlideshowInterval(current);
                             service.ApplyLocalSlideMin(next);
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({"Photo transfer", "Start", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Photo transfer requested from settings");
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->StartApTransferMode();
                             }
                         }});

        // --- About: conventionally last ---
        items.push_back({"About", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Firmware", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});

        // Mute voice keeps the trailing slot whose index is read from the
        // vector rather than written down, matching Item::MuteVoice at the end
        // of the enum.
        const int voice_mute_index = static_cast<int>(items.size());
        bool muted_now = false;
        {
            std::lock_guard<std::mutex> lock(ptt_mutex_);
            muted_now = ptt_fsm_.muted();
        }
        items.push_back({"Mute voice (software)", muted_now ? "On" : "Off", nullptr,
                         rawdraw::SettingsItemType::Checkbox, muted_now,
                         [this, sr, voice_mute_index]() {
                             // "software" is in the label on purpose. This
                             // suppresses recording and playback; it does not
                             // cut power to the microphone, and the device must
                             // not imply that it does.
                             bool next = false;
                             {
                                 std::lock_guard<std::mutex> lock(ptt_mutex_);
                                 next = !ptt_fsm_.muted();
                             }
                             devcfg::DeviceConfigService::GetInstance()
                                 .ApplyLocalVoiceMuted(next);
                             sr->UpdateChecked(voice_mute_index, next);
                             sr->UpdateItem(voice_mute_index, next ? "On" : "Off");
                         }});

        // The builder above must match rawdraw::settings_menu::Item row for row,
        // because the hard-coded indices (Wi-Fi, pairing, lockdown, slideshow)
        // are derived from that enum. If a row is added or moved without the
        // enum, these no longer line up and the wrong row gets updated — so it
        // is asserted here rather than discovered on the glass.
        if (static_cast<int>(items.size()) != smenu::kItemCount ||
            voice_mute_index != smenu::Index(smenu::Item::MuteVoice)) {
            ESP_LOGE(kTag,
                     "Settings menu order drifted from settings_menu_order.h: "
                     "built %d rows (enum expects %d), voice row %d (enum %d)",
                     static_cast<int>(items.size()), smenu::kItemCount,
                     voice_mute_index, smenu::Index(smenu::Item::MuteVoice));
        }

        sr->SetItems(items);
        sr->SetFirmwareVersion(product::kFirmwareVersion);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");

        InitializeConfigService(slideshow_interval, muted_now);
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(kTag, "Wake from deep sleep: flash activity LED and refresh UI");
        board.FlashActivityLed();
        if (rawdraw_ui_manager_) {
            // Only repaint when there is genuinely no dashboard to show. When a
            // frame is stored it was restored into the renderer just above and
            // the panel still physically shows it, so a RequestActivePageRefresh
            // here would repaint the welcome/QR over the retained dashboard —
            // the flash-back the owner sees on every wake. Leave the retained
            // dashboard; the tower's next push does the one visible refresh.
            if (!dashboard::DashboardManager::GetInstance().HasFrame()) {
                rawdraw_ui_manager_->RequestActivePageRefresh();
            }
        }
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    const std::string ip = data.empty() ? WifiManager::GetInstance().GetIpAddress() : data;
                    if (!ip.empty()) {
                        const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                        ESP_LOGI(kTag, "LAN HTTP server auto-start after WiFi: started=%d url=http://%s/",
                                 started ? 1 : 0, ip.c_str());
                        // Record only. The server is already running, and
                        // calling the apply hook again would try to start a
                        // second one. The revision still moves, because a
                        // reader of the config would otherwise see lan_service
                        // change with no change in the revision.
                        devcfg::DeviceConfigService::GetInstance().NoteLanService(started);
                        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                            UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                            UpdateLanIpSettingsItem(sr, ip);
                        }
                    }
                }
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to dashboard");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Dashboard);
                }
                UpdateStatusBarForUi();
                ArmSyncSleepTimer();
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                    devcfg::DeviceConfigService::GetInstance().NoteLanService(false);
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to dashboard");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Dashboard);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
        // Every branch above has just settled wifi_connected_. Push-to-talk
        // reads it as half of its transport gate: with no network there is
        // nowhere to send an utterance, and the device says so at the arm
        // threshold instead of recording fifteen seconds it will discard.
        RefreshVoiceTransportReadiness();
    });

    // Start network (non-blocking, WiFi connects asynchronously)
    board.RequestNetwork();

    SetDeviceState(kDeviceStateIdle);
}

void Application::DispatchNavEvent(nav::Event event) {
    // Somebody is touching the device, so this is the physical half of
    // "interactive mode, activatable by button": a press opens an interactive
    // window in the saver, and refreshes one that is already open. Without it
    // the sleep timer would fire mid-navigation and put the device away while
    // it was being used.
    //
    // It is here rather than in the button driver so that every gesture counts
    // once, whatever page it lands on.
    NotePhysicalActivity();

    // One path for every physical gesture. The mapping itself lives in
    // main/common/nav_model.cc and is host-tested there; this function only
    // makes sure the user gets feedback even when the UI manager is not up
    // yet (early boot, factory test).
    if (!rawdraw_ui_manager_) {
        Board::GetInstance().FlashActivityLed();
        return;
    }
    rawdraw_ui_manager_->HandleNavEvent(event);
}

void Application::OnUpClick() {
    ESP_LOGI(kTag, "UP click");
    DispatchNavEvent(nav::Event::kUpClick);
}

void Application::OnDownClick() {
    ESP_LOGI(kTag, "DOWN click");
    DispatchNavEvent(nav::Event::kDownClick);
}

void Application::OnUpLongPress() {
    // Universal Back. It used to leave Settings and do nothing anywhere else.
    ESP_LOGI(kTag, "UP long press");
    DispatchNavEvent(nav::Event::kUpLong);
}

void Application::OnDownLongPress() {
    // Universal Home. It used to be "enter Settings", which left no way back
    // from anywhere else.
    ESP_LOGI(kTag, "DOWN long press");
    DispatchNavEvent(nav::Event::kDownLong);
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    DispatchNavEvent(nav::Event::kComboLong);
}

void Application::OnBootClick() {
    ESP_LOGI(kTag, "BOOT click");
    DispatchNavEvent(nav::Event::kBootClick);
}

void Application::OnBootLongPress() {
    // Reserved for push-to-talk. Nothing else may bind it: the AP-transfer
    // enter/exit and Wi-Fi-AP exit that used to live here are now Back and
    // Home, which work the same way from every screen.
    ESP_LOGI(kTag, "BOOT long press");
    DispatchNavEvent(nav::Event::kBootLong);
}

void Application::RepaintStoredDashboardFrame() {
    // The honest redraw, reachable from the quick switch entry "Repaint
    // screen". It does NOT fetch anything. The Mac is the one that pushes
    // frames, and the device has no route back to it: the composer listens on
    // loopback and the tailnet, neither of which the device can reach from the
    // LAN. Wiring a "fetch" here would mean either opening a listening socket
    // on the Mac, which has not been authorised, or printing a reassuring
    // message and doing nothing. The honest action is a forced redraw, which
    // is genuinely useful after ghosting or a failed refresh.
    auto& dash = dashboard::DashboardManager::GetInstance();
    if (!dash.HasFrame()) {
        ESP_LOGI(kTag, "Repaint requested but no dashboard frame is stored yet");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Dashboard);
        }
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() != ui::RawDrawPageId::Dashboard) {
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Dashboard);
    }
    const auto d = dash.RequestRedraw(true);
    ESP_LOGI(kTag, "Repaint of the stored frame requested (%d)", static_cast<int>(d));
}

void Application::InitializeAudioFsm() {
    audio_ui::Hooks hooks;

    // Earcons are synthesised on demand (main/common/earcon_gen.cc) and played
    // on a task of their own (main/audio/earcon_player.h), because this hook
    // runs on the button task and the amplifier is not something to make that
    // task wait for. Whether any of it is audible is HG5.1 and has never been
    // checked, which is why the failure to queue one is logged rather than
    // assumed impossible.
    earcon_player_.Start(&audio_service_);
    hooks.earcon = [this](audio_ui::Earcon earcon) {
        ESP_LOGI(kTag, "Earcon: %s", audio_ui::EarconName(earcon));
        earcon_player_.Request(earcon);
    };
    // The LED is real and immediate, and is the only feedback that does not
    // depend on the panel or on the speaker.
    hooks.led = [](audio_ui::Led led) {
        if (led != audio_ui::Led::kOff) {
            Board::GetInstance().FlashActivityLed();
        }
    };

#if VOICE_PTT_ENABLED
    voice_capture_.Start(&audio_service_, [this](voice::CaptureResult&& capture) {
        OnUtteranceCaptured(std::move(capture));
    });
    voice_uploader_.Start([this](const voice::UploadResult& result) {
        OnUploadFinished(result);
    });

    hooks.mic_start = [this]() { voice_capture_.BeginUtterance(); };
    hooks.mic_stop = [this]() { voice_capture_.EndUtterance(); };
    // The upload hook does not upload. The audio is still inside the encoder
    // when the button comes up, so the capture task finishes draining and
    // calls OnUtteranceCaptured, which is where the body is handed over. The
    // state machine is already in kWaitingResponse and does not wait for any
    // of this.
    hooks.upload = []() {};
    hooks.discard = [this]() { voice_capture_.Discard(); };
    hooks.playback_start = []() {
        // Unreachable in this build: the hub's v1 wire carries no audio and
        // the machine is fed kResponseAnswered instead of kResponseReady.
        // Kept so that the day a response does carry audio, the hook is the
        // place it goes rather than a new path invented under pressure.
        ESP_LOGW(kTag, "Response playback requested, and this firmware cannot speak");
    };
    hooks.playback_stop = []() {};
    hooks.cancel_request = [this]() { voice_uploader_.Cancel(); };
#endif

    ptt_fsm_.SetHooks(std::move(hooks));
    ptt_fsm_.SetVoiceEnabled(VOICE_PTT_ENABLED != 0);
    RefreshVoiceTransportReadiness();

    // Restore the persisted software mute before anything can arm the machine.
    // Doing this after the first gesture could reach the user would mean a
    // device that was muted when it lost power comes back listening, which is
    // the one direction this setting must never fail in.
    //
    // The default is **muted**. A device flashed with this firmware for the
    // first time has no value under this key, and the safe reading of "no
    // value" is that nobody has asked for a microphone yet. Turning the mute
    // off in Settings is the first deliberate act, and on this board it is
    // also the HG5 audio bring-up test, because none of this has been heard.
    {
        Settings nvs(kVoiceNamespace, false);
        const bool muted = nvs.GetBool(kVoiceMutedKey, true);
        ptt_fsm_.SetMuted(muted, esp_timer_get_time() / 1000);
        ESP_LOGI(kTag, "Voice software mute restored from NVS: %s",
                 muted ? "muted" : "not muted");
    }
    ESP_LOGI(kTag, "Push-to-talk ready: voice path %s, hub %s, mute %s",
             VOICE_PTT_ENABLED ? "compiled in" : "compiled out",
             voice_uploader_.configured() ? "configured" : "not configured",
             ptt_fsm_.muted() ? "on" : "off");
    ESP_LOGW(kTag, "No audio path on this board has ever been tested on hardware "
                   "(docs/HARDWARE-ACCEPTANCE.md HG5)");
}

void Application::RefreshVoiceTransportReadiness() {
    // Two things must both hold before the microphone may open: a network, and
    // a hub to send to. Recording fifteen seconds with nowhere to put them is
    // not a feature, and the two reasons are logged separately because
    // "no Wi-Fi" and "nobody configured a hub" are different problems.
    const bool network = wifi_connected_.load(std::memory_order_acquire);
    const bool hub = voice_uploader_.configured();
    const bool ready = network && hub;
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    if (ready != ptt_fsm_.transport_ready()) {
        ESP_LOGI(kTag, "Voice transport %s (network %s, hub %s)",
                 ready ? "available" : "unavailable",
                 network ? "up" : "down", hub ? "configured" : "not configured");
    }
    ptt_fsm_.SetTransportReady(ready);
}

void Application::OnUtteranceCaptured(voice::CaptureResult&& capture) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    if (capture.body.empty()) {
        // The microphone produced nothing. On a board where HG5.4 has never
        // passed this is the expected outcome, and calling it a failed upload
        // would point at the wrong thing entirely.
        ESP_LOGW(kTag, "Nothing was captured: no encoded audio came back from the codec");
        ptt_fsm_.Handle(audio_ui::Event::kResponseFailed, now);
        return;
    }
    if (capture.truncated) {
        ESP_LOGW(kTag, "Utterance truncated at a buffer limit; sending what there is");
    }
    const size_t bytes = capture.body.size();
    if (!voice_uploader_.Submit(std::move(capture.body), voice::kUtteranceMime)) {
        ESP_LOGW(kTag, "Upload refused: not configured, or one is already in flight");
        ptt_fsm_.Handle(audio_ui::Event::kResponseFailed, now);
        return;
    }
    ESP_LOGI(kTag, "Utterance handed to the uploader: %u bytes, %u ms",
             static_cast<unsigned>(bytes),
             static_cast<unsigned>(capture.duration_ms));
}

void Application::OnUploadFinished(const voice::UploadResult& result) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    if (!result.ok) {
        ptt_fsm_.Handle(audio_ui::Event::kResponseFailed, now);
        return;
    }
    // Received is not transcribed is not executed. The hub says which of those
    // it did in `state` and `source`, and this firmware repeats it rather than
    // flattening it into "done".
    ESP_LOGI(kTag, "Hub answered: state=%s source=%s reply_chars=%u duplicate=%s",
             result.state.c_str(), result.source.c_str(),
             static_cast<unsigned>(result.reply_chars),
             result.duplicate ? "yes" : "no");
    if (result.has_playable_audio) {
        ptt_fsm_.Handle(audio_ui::Event::kResponseReady, now);
        return;
    }
    // The shipped case. The answer is text, this firmware has no speech
    // synthesis, and the acknowledgement is a tone rather than a claim that
    // anything was said out loud.
    ptt_fsm_.Handle(audio_ui::Event::kResponseAnswered, now);
}

void Application::SetVoiceMuted(bool muted) {
    // Write first, then apply. If the write succeeds and the process dies
    // before the machine is told, the next boot restores the intended state;
    // the other order can leave flash saying "not muted" on a device the user
    // just muted.
    {
        Settings nvs(kVoiceNamespace, true);
        nvs.SetBool(kVoiceMutedKey, muted);
    }
    {
        std::lock_guard<std::mutex> lock(ptt_mutex_);
        ptt_fsm_.SetMuted(muted, esp_timer_get_time() / 1000);
    }
    // Software mute, and named as one: nothing here cuts power to the codec.
    ESP_LOGI(kTag, "Voice software mute set to %s (persisted)",
             muted ? "on" : "off");
}

void Application::ConfigureVoiceHub(const std::string& url, const std::string& token) {
    voice::HubCredentials creds;
    creds.base_url = url;
    creds.token = token;
    voice::SaveHubCredentials(creds);
    voice_uploader_.ReloadCredentials();
    RefreshVoiceTransportReadiness();
    // The URL is logged, the token never is, and the reminder is logged too:
    // somebody reading this line later should not have to guess whether
    // configuring a hub also switched the microphone on. It did not.
    bool muted = true;
    {
        std::lock_guard<std::mutex> lock(ptt_mutex_);
        muted = ptt_fsm_.muted();
    }
    ESP_LOGI(kTag, "Voice hub %s. The software mute is still %s",
             url.empty() ? "cleared" : "configured",
             muted ? "on, so nothing will be recorded" : "off");
}

void Application::OnBootPressed() {
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    ptt_fsm_.Handle(audio_ui::Event::kPttPressed, esp_timer_get_time() / 1000);
}

void Application::OnPushToTalkGesture() {
    // A fallback, not the main path. The press-down above has normally armed
    // the machine seven hundred milliseconds before the driver reports a long
    // press, and the machine's own ptt_held_ guard makes this second
    // kPttPressed a no-op for a press that is already in flight. It stays
    // wired for the case the driver never delivered a press-down at all,
    // which is what happens to a press that straddles a wake from sleep.
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    const auto before = ptt_fsm_.state();
    ptt_fsm_.Handle(audio_ui::Event::kPttPressed, esp_timer_get_time() / 1000);
    ESP_LOGI(kTag, "BOOT long press: push-to-talk was already %s, now %s (voice %s)",
             audio_ui::StateName(before), audio_ui::StateName(ptt_fsm_.state()),
             ptt_fsm_.voice_enabled() ? "enabled" : "disabled");
}

bool Application::OnBootReleased() {
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    ptt_fsm_.Handle(audio_ui::Event::kPttReleased, esp_timer_get_time() / 1000);
    const bool consumed = ptt_fsm_.press_consumed();
    if (consumed) {
        ESP_LOGI(kTag, "BOOT release consumed by push-to-talk: state=%s reason=%s",
                 audio_ui::StateName(ptt_fsm_.state()),
                 audio_ui::ReasonName(ptt_fsm_.reason()));
    }
    return consumed;
}

void Application::StartPttTickTimer() {
    if (ptt_tick_timer_ != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* self = static_cast<Application*>(arg);
        std::lock_guard<std::mutex> lock(self->ptt_mutex_);
        self->ptt_fsm_.Tick(esp_timer_get_time() / 1000);
    };
    args.arg = this;
    // ESP_TIMER_TASK rather than ISR: the tick can emit an earcon request and
    // a queue send, neither of which belongs in an interrupt.
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "ptt_tick";
    ESP_ERROR_CHECK(esp_timer_create(&args, &ptt_tick_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ptt_tick_timer_, kPttTickPeriodUs));
    ESP_LOGI(kTag, "Push-to-talk timeouts advance every %d ms",
             static_cast<int>(kPttTickPeriodUs / 1000));
}

void Application::LeaveWifiConfigApIfActive() {
    if (!WifiManager::GetInstance().IsConfigMode()) {
        return;
    }
    ESP_LOGI(kTag, "Leaving the Wi-Fi setup access point");
    WifiManager::GetInstance().StartStation();
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
        devcfg::DeviceConfigService::GetInstance().NoteLanService(false);
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::InitializeConfigService(int slideshow_interval, bool muted) {
    devcfg::Config initial;
    initial.gallery_slide_min = slideshow_interval;
    {
        Settings nvs(kSyncNamespace, false);
        initial.sync_interval = nvs.GetInt(kSyncIntervalKey, 30);
    }
    initial.voice_muted = muted;
    {
        // Read the pair once to learn the URL and whether a token exists. The
        // token itself goes no further than this scope: the config type has
        // nowhere to put it, which is what makes "the API never returns it" a
        // property of the design rather than of the route handler.
        voice::HubCredentials creds = voice::LoadHubCredentials();
        initial.voice_hub_url = creds.base_url;
        initial.voice_hub_token_set = !creds.token.empty();
        if (!creds.token.empty()) {
            memset(&creds.token[0], 0, creds.token.size());
        }
    }
    initial.dashboard_lockdown = dashboard::DashboardManager::GetInstance().LockdownEnabled();
    initial.network_lan_service =
        rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning();
    {
        // From the live power state rather than from NVS again: it was seeded
        // in InitializePowerState() and re-reading the keys here would be a
        // second place for a default to live.
        std::lock_guard<std::mutex> guard(power_mutex_);
        initial.power_mode = power::ModeName(power_state_.PersistableMode());
        initial.power_interactive_min = power_state_.interactive_minutes();
        initial.power_wake_interval_min = power_state_.wake_interval_min();
    }
    {
        // Off unless this device was explicitly told otherwise, and the default
        // is what a device with no key at all reads. See kAutonomyEnabledKey.
        Settings nvs(kAutonomyNamespace, false);
        initial.autonomy_enabled = nvs.GetInt(kAutonomyEnabledKey, 0) != 0;
    }

    devcfg::ConfigHooks hooks;
    hooks.apply_slide_min = [this](int32_t minutes) {
        {
            Settings nvs(kGalleryNamespace, true);
            nvs.SetInt(kSlideshowIntervalKey, static_cast<int32_t>(minutes));
        }
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(minutes);
        }
        // A running slideshow and an idle sleep timer are mutually exclusive:
        // a device that went to sleep between slides would show one photo.
        if (minutes > 0 && sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
            ESP_LOGI(kTag, "Sync sleep timer paused while gallery slideshow is enabled");
        } else if (minutes <= 0 &&
                   (wifi_connected_.load(std::memory_order_acquire) ||
                    WifiManager::GetInstance().IsConnected())) {
            ArmSyncSleepTimer();
        }
    };
    hooks.apply_sync_interval = [this](int32_t minutes) {
        {
            Settings nvs(kSyncNamespace, true);
            nvs.SetInt(kSyncIntervalKey, minutes);
        }
        // Re-armed from the new value immediately, so the response can honestly
        // say "immediate" rather than "on the next reboot".
        if (sleep_timer_ != nullptr) esp_timer_stop(sleep_timer_);
        if (minutes > 0) ArmSyncSleepTimer();
        else ESP_LOGI(kTag, "Idle deep sleep turned off");
    };
    hooks.apply_voice_muted = [this](bool value) { SetVoiceMuted(value); };
    hooks.apply_hub_url = [this](const std::string& url) { SetVoiceHubUrl(url); };
    hooks.apply_lockdown = [](bool enabled) {
        dashboard::DashboardManager::GetInstance().SetLockdownEnabled(enabled);
    };
    hooks.apply_lan_service = [this](bool enabled) { return SetLanService(enabled); };
    hooks.apply_power_mode = [this](const std::string& mode, int32_t interactive_min) {
        ApplyPowerMode(mode, interactive_min);
    };
    hooks.apply_interactive_minutes = [this](int32_t minutes) {
        SetInteractiveMinutes(minutes);
    };
    hooks.apply_wake_interval = [this](int32_t minutes) {
        SetWakeIntervalMinutes(minutes);
    };
    hooks.apply_autonomy_enabled = [](bool enabled) {
        // Written straight through, so the route may honestly answer
        // "immediate" — which now means "in force, and still in force after the
        // deep sleep this feature wakes from". There is nothing else to do
        // here: the wake cycle reads the live snapshot on every tick, and the
        // stored profile is deliberately kept either way, so turning autonomy
        // back on does not require a re-push.
        Settings nvs(kAutonomyNamespace, true);
        nvs.SetInt(kAutonomyEnabledKey, enabled ? 1 : 0);
        ESP_LOGI(kTag, "Autonomy %s", enabled ? "enabled" : "disabled");
    };
    hooks.run_action = [this](devcfg::Action action, uint32_t delay_ms) {
        ScheduleDeviceAction(action, delay_ms);
    };

    devcfg::DeviceConfigService::GetInstance().Init(initial, std::move(hooks));
}

bool Application::SetLanService(bool enabled) {
    if (!rawdraw_ui_manager_) return false;

    if (!enabled) {
        if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
            ESP_LOGW(kTag, "LAN HTTP server stopping; the device API goes with it");
            rawdraw_ui_manager_->StopLanHttpServer();
            if (wifi_connected_.load(std::memory_order_acquire) ||
                WifiManager::GetInstance().IsConnected()) {
                ArmSyncSleepTimer();
            }
        }
        return false;
    }

    if (rawdraw_ui_manager_->IsLanHttpServerRunning()) return true;

    auto& wifi = WifiManager::GetInstance();
    if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
        ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
        return false;
    }
    const std::string ip = wifi.GetIpAddress();
    if (ip.empty()) {
        ESP_LOGW(kTag, "LAN HTTP server requires station IP");
        return false;
    }
    const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
    ESP_LOGI(kTag, "LAN HTTP server start requested: started=%d url=http://%s/",
             started ? 1 : 0, ip.c_str());
    // Deliberately does *not* stop the sleep timer any more. Cancelling it here
    // is what made the API and the battery mutually exclusive: the tower turns
    // the LAN server on, the timer is cancelled, and nothing re-arms it. The
    // device is now expected to sleep with the server configured, and to serve
    // the API during the window it is awake.
    return started;
}

void Application::SetVoiceHubUrl(const std::string& url) {
    voice::HubCredentials creds = voice::LoadHubCredentials();
    if (url.empty()) {
        // Clearing the address clears the pair. A token with nowhere to go is
        // a stored secret with no purpose, and leaving it would let a later
        // URL write re-arm an upload path the operator thought they had
        // switched off.
        ConfigureVoiceHub("", "");
    } else {
        ConfigureVoiceHub(url, creds.token);
    }
    if (!creds.token.empty()) {
        memset(&creds.token[0], 0, creds.token.size());
    }
}

void Application::ScheduleDeviceAction(devcfg::Action action, uint32_t delay_ms) {
    // Deferred on purpose. The HTTP response has to reach the socket before
    // the device stops being a device, and the handler is still holding that
    // socket when this is called.
    if (action_timer_ != nullptr) {
        esp_timer_stop(action_timer_);
        esp_timer_delete(action_timer_);
        action_timer_ = nullptr;
    }
    pending_action_ = action;
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* self = static_cast<Application*>(arg);
        if (self->pending_action_ == devcfg::Action::kSleep) {
            ESP_LOGW(kTag, "Remote sleep action firing");
            self->EnterManualSleep();
            return;
        }
        ESP_LOGW(kTag, "Remote restart action firing");
        esp_restart();
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "app_remote_action";
    if (esp_timer_create(&args, &action_timer_) != ESP_OK) {
        ESP_LOGE(kTag, "could not create the action timer; nothing was scheduled");
        action_timer_ = nullptr;
        return;
    }
    esp_timer_start_once(action_timer_, static_cast<int64_t>(delay_ms) * 1000);
}

void Application::ArmSyncSleepTimer() {
    // The provisioning portal, and only the provisioning portal. This used to
    // be IsLocalHttpServiceRunning(), which is also true for the LAN API
    // server, and that single predicate is why a device with the tower
    // connected never slept at all: starting the API cancelled the sleep timer
    // and nothing ever re-armed it. In the hybrid design the API is expected
    // to be reachable only during a window, and the tower is built for that.
    //
    // The AP portal still blocks, because somebody is standing in front of the
    // device typing a Wi-Fi password and sleeping would strand them.
    if (rawdraw_ui_manager_ != nullptr &&
        rawdraw_ui_manager_->IsApTransferModeRunning()) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while the AP provisioning portal is open");
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while gallery slideshow is enabled");
        return;
    }

    // always_on and an open interactive window both mean "do not schedule a
    // sleep". The window re-arms this from PowerTick() the moment it expires,
    // so nothing is lost by not arming now.
    uint32_t backstop_ms = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const power::Mode mode = power_state_.effective_mode(now_ms);
        if (mode != power::Mode::kAutoSaver) {
            if (sleep_timer_ != nullptr) esp_timer_stop(sleep_timer_);
            sleep_intent_ = false;
            next_wake_at_ms_ = 0;
            cycle_active_ = false;
            cycle_deferred_ = false;
            wake_budget_.Reset();
            ESP_LOGI(kTag, "Sync sleep timer not armed: power mode is %s",
                     power::ModeName(mode));
            return;
        }
        sleep_intent_ = true;
        // Auto saver, so a bounded cycle owns this awake period. If one is not
        // running — the device just fell out of an interactive window, say —
        // start one here rather than leaving the device up with nothing
        // counting down.
        if (!cycle_active_) BeginWakeCycleLocked(now_ms);
        backstop_ms = wake_budget_.TotalRemainingMs(now_ms);
    }

    // The legacy `sync.interval` is deliberately *not* consulted here any
    // more, and setting it to zero deliberately no longer turns power saving
    // off. It used to be the only thing that put this device to sleep, so
    // `sync.interval = 0` meant "never sleep" — a device left in automatic
    // power saving with that one legacy key at zero would stay awake for ever
    // while the tower, the config route and the UI all reported it was saving
    // power. The saver's schedule now comes from `power.wake_interval_min`,
    // which has a floor and cannot express "never".
    //
    // What is armed here is a *backstop*, not the schedule: the wake budget is
    // driven by ServiceWakeCycle() on the one-second tick, and this timer is
    // what still ends the cycle if that tick is starved by a long panel
    // refresh. Either way the device is awake for the budget total, not for
    // half an hour.
    if (sleep_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->OnWakeBudgetBackstop();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_wake_budget";
        ESP_ERROR_CHECK(esp_timer_create(&args, &sleep_timer_));
    }
    esp_timer_stop(sleep_timer_);
    // A cycle whose budget is already spent still needs a tick to land on, so
    // the backstop is floored rather than armed at zero (which esp_timer would
    // reject) or skipped (which would leave the device awake).
    if (backstop_ms < 1000u) backstop_ms = 1000u;
    ESP_LOGI(kTag, "Wake budget backstop armed: %u s (auto_saver)",
             static_cast<unsigned>(backstop_ms / 1000));
    ESP_ERROR_CHECK(esp_timer_start_once(
        sleep_timer_, static_cast<int64_t>(backstop_ms) * 1000));
}

void Application::OnWakeBudgetBackstop() {
    // The one-second tick in Run() should have ended this cycle by now. It did
    // not — most likely because that loop is starved by a long panel refresh
    // on the same task — so the cycle is advanced from the esp_timer task
    // instead.
    //
    // Deliberately the same function rather than an immediate sleep. Ending
    // the cycle unconditionally here would record budget_exhausted for a
    // device that was legitimately deferred (a refresh in flight, the
    // provisioning portal, a slideshow), which is a failure nothing did wrong
    // and which would back the wake interval off for it. ServiceWakeCycle
    // already knows the difference; the backstop's job is only to make sure
    // *something* asks.
    const int64_t now_ms = esp_timer_get_time() / 1000;
    ESP_LOGW(kTag, "Wake budget backstop fired; advancing the cycle");
    ServiceWakeCycle(now_ms);

    // Still going, so the cycle was deferred or is mid-phase. Re-arm, or the
    // backstop would be a one-shot and a starved tick loop would leave the
    // device awake with nothing counting down.
    bool still_active = false;
    uint32_t remaining_ms = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        still_active = cycle_active_;
        remaining_ms = wake_budget_.TotalRemainingMs(now_ms);
    }
    if (!still_active || sleep_timer_ == nullptr) return;
    if (remaining_ms < 1000u) remaining_ms = 1000u;
    esp_timer_stop(sleep_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(
        sleep_timer_, static_cast<int64_t>(remaining_ms) * 1000));
}

void Application::EnterScheduledSleep() {
    // One decision, taken by the host-tested policy rather than by a ladder of
    // ifs that only the device can exercise.
    const power::WakeInputs inputs = CollectWakeInputs();
    const power::WakePlan plan = power::PlanNextWake(inputs);

    if (plan.action != power::WakeAction::kSleep) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: %s", plan.reason);
        ArmSyncSleepTimer();
        return;
    }
    EnterHybridSleep(plan.wake_in_ms, plan.reason);
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering manual deep sleep; stopping local services and WiFi");
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsHttpServerRunning()) {
        rawdraw_ui_manager_->StopApTransferMode();
    }

    // Asking for sleep now ends the *interactive window*, not the hourly
    // refresh. Before the hybrid contract this path armed only the button, so
    // "sleep now" quietly turned the device off until somebody found it again;
    // with a product whose promise is an automatic hourly update, that is a
    // setting nobody would knowingly choose from a button labelled "sleep".
    //
    // So the base mode is restored and the normal wake is armed. The one thing
    // this must not do is leave the device in an interactive window it was
    // just asked to leave, which is what closing the window here is for.
    uint32_t wake_in_ms = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        power_state_.Request(power_state_.PersistableMode(), 0,
                             esp_timer_get_time() / 1000);
        int32_t interval = power_state_.wake_interval_min();
        if (!power::IsValidWakeInterval(interval)) {
            interval = power::kDefaultWakeIntervalMin;
        }
        wake_in_ms = static_cast<uint32_t>(interval) * 60u * 1000u;
    }

    UpdateStatusBarForUi();
    // The UI has to reach the panel, and an HTTP response its socket, before
    // the device stops being a device.
    vTaskDelay(pdMS_TO_TICKS(300));
    EnterHybridSleep(wake_in_ms, "manual_sleep");
}

// ------------------------------------------------------- hybrid low power --

namespace {

/**
 * @brief Read the chip and hand the answer to power::ClassifyWake.
 *
 * Nothing but translation lives here any more. The *rule* — the button beating
 * a coincident timer, and only the two human resets buying an interactive
 * window — moved to common/power_policy.cc, where a host suite compiles it;
 * inside this anonymous namespace, wrapped around three ESP-IDF calls, it was
 * reachable only by a device with a serial cable attached.
 *
 * esp_sleep_get_wakeup_causes() rather than the deprecated singular form,
 * because on this hybrid both sources are armed for every sleep and they can
 * genuinely coincide. The singular API loses one of them arbitrarily.
 */
power::WakeReason WakeReasonFromChip() {
    const uint32_t causes = esp_sleep_get_wakeup_causes();

    power::DeepSleepCauses c;
    // ext0 is armed on BOOT_BUTTON_GPIO and on nothing else, so it can only
    // mean the button.
    c.ext0 = (causes & BIT(ESP_SLEEP_WAKEUP_EXT0)) != 0;
    c.timer = (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
    // Not armed by this firmware today; see ClassifyWake for why it is still
    // mapped rather than folded into "button".
    c.ext1 = (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0;

    power::ResetClass reset = power::ResetClass::kOther;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  // the cell was connected, or the key held
        case ESP_RST_EXT:      // the external reset line
            reset = power::ResetClass::kHuman;
            break;
        default:
            // Watchdog, brownout, panic, software restart, a USB or JTAG
            // reset, and anything a later IDF adds. Nobody is standing there.
            break;
    }

    return power::ClassifyWake(causes != 0, c, reset);
}

}  // namespace


// ---------------------------------------------------------------- autonomy --

namespace {

/**
 * @brief SPIFFS-backed slots for the autonomy profile.
 *
 * A second pair of files on the same filesystem the frame slots use. The
 * partition table is deliberately unchanged — there is nowhere else to put
 * them — and a different magic in the record header keeps the two kinds apart:
 * a frame in a profile slot fails validation and reads as "no profile" rather
 * than as a profile of garbage.
 *
 * Deliberately not shared with dashboard_manager.cc's SpiffsSlotIo: that one
 * carries frame-specific space accounting and write counters, and
 * parameterising it would have meant touching the store that holds the panel.
 */
class AutonomySpiffsSlotIo : public record::SlotIo {
public:
    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        if (slot < 0 || slot >= record::kSlotCount) return -1;
        FILE* f = fopen(kPath[slot], "rb");
        if (f == nullptr) return 0;  // absent is an empty slot, not an error
        const size_t n = fread(buf, 1, max, f);
        const bool bad = ferror(f) != 0;
        fclose(f);
        if (bad) {
            ESP_LOGE(kTag, "read %s failed: %s", kPath[slot], strerror(errno));
            return -1;
        }
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (slot < 0 || slot >= record::kSlotCount) return false;
        FILE* f = fopen(kPath[slot], "wb");
        if (f == nullptr) {
            ESP_LOGE(kTag, "open %s failed: %s", kPath[slot], strerror(errno));
            return false;
        }
        const size_t written = fwrite(data, 1, len, f);
        // fflush before fclose so a filesystem error surfaces here rather than
        // being swallowed by close. The caller verifies by reading back anyway;
        // this is about reporting the failure, not about detecting it.
        const bool ok = written == len && fflush(f) == 0;
        fclose(f);
        if (!ok) ESP_LOGE(kTag, "write %s short or failed", kPath[slot]);
        return ok;
    }

private:
    static constexpr const char* kPath[record::kSlotCount] = {
        "/spiffs/prof0.rec",
        "/spiffs/prof1.rec",
    };
};

/**
 * @brief The forecast cache's two slots, on the same filesystem.
 *
 * A third pair of files and a third magic. The duplication with the class above
 * is deliberate and small: the alternative is a path-parameterised base that
 * would have to be threaded through the frame store too, and the frame store is
 * the one holding what is on the glass.
 */
class WeatherSpiffsSlotIo : public record::SlotIo {
public:
    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        if (slot < 0 || slot >= record::kSlotCount) return -1;
        FILE* f = fopen(kPath[slot], "rb");
        if (f == nullptr) return 0;
        const size_t n = fread(buf, 1, max, f);
        const bool bad = ferror(f) != 0;
        fclose(f);
        if (bad) {
            ESP_LOGE(kTag, "read %s failed: %s", kPath[slot], strerror(errno));
            return -1;
        }
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (slot < 0 || slot >= record::kSlotCount) return false;
        FILE* f = fopen(kPath[slot], "wb");
        if (f == nullptr) {
            ESP_LOGE(kTag, "open %s failed: %s", kPath[slot], strerror(errno));
            return false;
        }
        const size_t written = fwrite(data, 1, len, f);
        const bool ok = written == len && fflush(f) == 0;
        fclose(f);
        if (!ok) ESP_LOGE(kTag, "write %s short or failed", kPath[slot]);
        return ok;
    }

private:
    static constexpr const char* kPath[record::kSlotCount] = {
        "/spiffs/wthr0.rec",
        "/spiffs/wthr1.rec",
    };
};

}  // namespace

/**
 * @brief Bring the profile store up, or leave autonomy inert.
 *
 * Inert is a supported outcome, not a failure to paper over. Without PSRAM for
 * the buffers the routes answer 404 and the device is an ordinary push target
 * — which is exactly what it was before this feature existed, and is a better
 * answer than a half-initialised store that accepts a profile it cannot keep.
 */
void Application::InitializeAutonomy() {
    // The rollout gate, and it is checked first so nothing below runs on a
    // build where the feature is not meant to be reachable. `autonomy_service_`
    // stays null, which is a state the rest of this file and both profile
    // routes already handle: the routes answer 404 autonomy_unsupported, the
    // capability list omits autonomy.profile.v1, the cycle's planner sees no
    // profile and stands down, and the device is the push target it was before
    // this feature existed. See AUTONOMY_COMPILED in dashboard_build_config.h
    // for why this is a separate switch from `autonomy.enabled`.
    if (AUTONOMY_COMPILED == 0) {
        ESP_LOGI(kTag, "Autonomy is not compiled in; this build is push-only");
        return;
    }

    // 17 KB for the parsed profile and its string arena, 16 KB of slot
    // scratch. Both in PSRAM: neither belongs in internal SRAM, and the device
    // has 8 MB of PSRAM doing nothing.
    autonomy_profile_ = static_cast<autonomy::Profile*>(
        heap_caps_calloc(1, sizeof(autonomy::Profile), MALLOC_CAP_SPIRAM));
    autonomy_scratch_ = static_cast<uint8_t*>(
        heap_caps_malloc(autonomy::kProfileRecordBytes, MALLOC_CAP_SPIRAM));
    if (autonomy_profile_ == nullptr || autonomy_scratch_ == nullptr) {
        ESP_LOGW(kTag, "no PSRAM for the autonomy store; autonomy stays inert");
        if (autonomy_profile_ != nullptr) heap_caps_free(autonomy_profile_);
        if (autonomy_scratch_ != nullptr) heap_caps_free(autonomy_scratch_);
        autonomy_profile_ = nullptr;
        autonomy_scratch_ = nullptr;
        return;
    }
    new (autonomy_profile_) autonomy::Profile();

    static AutonomySpiffsSlotIo slot_io;
    autonomy_slot_io_ = &slot_io;
    static_assert(sizeof(autonomy::AutonomyService) < 4096,
                  "the service itself is small; the buffers are what is large");
    autonomy_service_ = new autonomy::AutonomyService(
        autonomy_slot_io_, autonomy_scratch_, autonomy::kProfileRecordBytes,
        autonomy_profile_);

    // The same credential the frame route uses, asked for rather than copied.
    //
    // There is one secret on this device, it is bootstrapped by a physical
    // pairing action, and DashboardManager owns it. Copying it in here would
    // make a second one that goes stale exactly when it must not: re-pairing
    // from the Settings menu replaces the token *because* the old one is
    // suspect, and a profile route still honouring the old value would be a
    // hole opened by the act meant to close one.
    autonomy_service_->SetTokenVerifier(
        [](const char* token, void*) {
            switch (dashboard::DashboardManager::GetInstance().CheckToken(token)) {
                case dashboard::AuthResult::kOk:
                    return autonomy::AuthVerdict::kOk;
                case dashboard::AuthResult::kNotProvisioned:
                    return autonomy::AuthVerdict::kNotProvisioned;
                // A caller already locked out by the frame route is refused
                // here too, reported as a bad token. The distinction is not
                // lost: this service keeps its own lockout, and the one that
                // fires first is the one the caller is told about.
                case dashboard::AuthResult::kBadToken:
                case dashboard::AuthResult::kLockedOut:
                    break;
            }
            return autonomy::AuthVerdict::kBadToken;
        },
        nullptr);

    if (autonomy_service_->Load()) {
        ESP_LOGI(kTag, "autonomy profile revision %d loaded",
                 static_cast<int>(autonomy_service_->revision()));
    } else {
        // Either nothing has ever been pushed, or both slots failed their
        // checksum. Both read as "no profile" and both leave the device a push
        // target; the tower notices and offers to re-push.
        ESP_LOGI(kTag, "no valid autonomy profile stored");
    }

    // The forecast cache and the compositor's buffers. Separately fallible from
    // the profile store on purpose: a device that can hold a profile but cannot
    // find 120 KB for a canvas should still accept and report the profile, and
    // should still be honest that it cannot draw from it.
    weather_scratch_ = static_cast<uint8_t*>(
        heap_caps_malloc(weather::kWeatherRecordBytes, MALLOC_CAP_SPIRAM));
    compose_canvas_ = static_cast<uint8_t*>(
        heap_caps_malloc(autonomy::kCanvasBytes, MALLOC_CAP_SPIRAM));
    compose_frame_ = static_cast<uint8_t*>(
        heap_caps_malloc(autonomy::kPackedBytes, MALLOC_CAP_SPIRAM));
    if (weather_scratch_ == nullptr || compose_canvas_ == nullptr ||
        compose_frame_ == nullptr) {
        ESP_LOGW(kTag, "no PSRAM for the compositor; the device will not compose");
        heap_caps_free(weather_scratch_);
        heap_caps_free(compose_canvas_);
        heap_caps_free(compose_frame_);
        weather_scratch_ = nullptr;
        compose_canvas_ = nullptr;
        compose_frame_ = nullptr;
        return;
    }

    static WeatherSpiffsSlotIo weather_io;
    weather_slot_io_ = &weather_io;
    static weather::WeatherCache cache(weather_slot_io_, weather_scratch_,
                                       weather::kWeatherRecordBytes);
    weather_cache_ = &cache;
    if (weather_cache_->Load()) {
        ESP_LOGI(kTag, "cached forecast from epoch %lld",
                 static_cast<long long>(weather_cache_->fetched_epoch()));
    } else {
        // No forecast yet, or both slots unreadable. Either way the first wake
        // that wants one will fetch it; nothing is invented in the meantime.
        ESP_LOGI(kTag, "no cached forecast");
    }
}

int64_t Application::NowEpochOrZero() {
    const time_t now = time(nullptr);
    // Later than 2020, which is how the rest of this file decides the clock has
    // been set. A device that has never reached SNTP starts at 1970 and must
    // report "unknown" rather than count down to a date in the past.
    return now > 1600000000 ? static_cast<int64_t>(now) : 0;
}

autonomy::CycleInputs Application::CollectCycleInputs(
    bool network_up, uint32_t tower_wait_remaining_ms,
    uint32_t work_remaining_ms) const {
    autonomy::CycleInputs in;

    in.autonomy_enabled =
        devcfg::DeviceConfigService::GetInstance().Snapshot().autonomy_enabled;
    // "Can compose" is part of "has a profile" as far as the cycle is
    // concerned: a device with a profile and no canvas cannot draw from it, and
    // pretending otherwise would have the planner ask for a compose that the
    // executor would silently decline.
    const bool can_compose = autonomy_service_ != nullptr &&
                             autonomy_service_->has_profile() &&
                             compose_canvas_ != nullptr && compose_frame_ != nullptr;
    in.profile_present = can_compose;

    if (can_compose) {
        // Read under the service's own lock: the HTTP task can be applying a
        // PUT to this exact object while the main task walks it. Copied out
        // field by field rather than held by reference, so nothing below is
        // still pointing at a profile that has since been replaced.
        autonomy::AutonomyService::ProfileLock guard(*autonomy_service_);
        const autonomy::Profile& p = guard.profile();
        for (uint8_t i = 0; i < p.module_count; ++i) {
            if (p.modules[i].mode == autonomy::Mode::kDevice) in.has_device_module = true;
            if (p.modules[i].mode == autonomy::Mode::kAuto) in.has_auto_module = true;
        }
        in.wants_weather = p.WantsWeather();
        in.wake_interval_min = p.wake_interval_min;
        if (p.has_weather) {
            in.min_fetch_interval_min = p.weather.min_fetch_interval_min;
        }
    }

    in.network_up = network_up;
    in.tower_wait_remaining_ms = tower_wait_remaining_ms;
    in.work_remaining_ms = work_remaining_ms;

    in.fetch_attempted = cycle_fetch_attempted_;
    in.fetch_ok = cycle_fetch_ok_;
    in.compose_result = cycle_compose_result_;

    if (weather_cache_ != nullptr && weather_cache_->has_forecast()) {
        in.has_cache = true;
        in.cache_fetched_epoch = weather_cache_->fetched_epoch();
    }

    in.now_epoch = NowEpochOrZero();
    in.clock_set = in.now_epoch > 0;

    const dashboard::DashboardStatus st =
        dashboard::DashboardManager::GetInstance().Status();
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        in.tower_frame_arrived = cycle_saw_new_frame_;
    }
    // The arbitration rule is about the frame the panel was last *told* to
    // show, which is the stored one: e-paper keeps its image, so a stored frame
    // is on the glass whether or not this boot watched it get there. That is a
    // different question from the status route's `displayed_origin`, which is
    // the weaker claim and is reported separately.
    if (st.has_stored_frame) {
        in.displayed_origin = st.stored_origin_is_local ? autonomy::Origin::kLocal
                                                        : autonomy::Origin::kTower;
        // The age is only a measurement when both ends of the subtraction are
        // real. A frame stored while the clock was unset carries epoch 0, and
        // calling that "fifty-six years old" would make the arbitration rule
        // fire on a number nobody measured.
        if (in.clock_set && st.stored_source_epoch > 0) {
            in.displayed_age_s =
                in.now_epoch - static_cast<int64_t>(st.stored_source_epoch);
            in.displayed_age_known = true;
        }
    }

    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        // Somebody is standing in front of the device pressing its button.
        // Repainting the dashboard under them is not an improvement.
        in.interactive_window_open =
            power_state_.interactive_remaining_ms(esp_timer_get_time() / 1000) > 0;
    }
    return in;
}

/**
 * @brief One bounded forecast fetch, straight into the cache.
 *
 * Always leaves `cycle_fetch_attempted_` true, including on every failure path.
 * That is what makes "one attempt per wake" hold: a fetch that threw, timed out
 * or came back as a captive portal login page must not be retried inside the
 * same budget, because the thing that went wrong will still be wrong four
 * seconds later. The retry is the next wake, with the backoff that already
 * exists.
 */
void Application::RunWeatherFetch(uint32_t budget_ms) {
    cycle_fetch_attempted_ = true;
    cycle_fetch_status_.attempted = true;

    if (weather_cache_ == nullptr || autonomy_service_ == nullptr) return;

    // The coordinates, copied out under the profile lock and the lock then
    // dropped. Deliberately not held across the fetch: that is seconds of
    // network, and a PUT arriving in the middle of it must not be made to wait
    // on this device's own errand.
    double latitude = 0;
    double longitude = 0;
    {
        autonomy::AutonomyService::ProfileLock guard(*autonomy_service_);
        if (!guard.has_profile() || !guard.profile().has_weather) return;
        latitude = guard.profile().weather.latitude;
        longitude = guard.profile().weather.longitude;
    }

    // The response buffer, 32 KB, from PSRAM and freed on every path out. Not a
    // member: it is live for the two seconds of a fetch once an hour, and
    // holding 32 KB for the life of the device to save that allocation would be
    // the wrong trade on a board where the compositor already owns 150 KB.
    char* body = static_cast<char*>(
        heap_caps_malloc(weather::kMaxResponseBytes, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        ESP_LOGW(kTag, "no PSRAM for a forecast response; skipping the fetch");
        return;
    }

    // The deadline the planner allocated, which is never more than the client's
    // own twenty-second ceiling and is often less. This is the one number that
    // stops a slow server from holding the wake open, and it has to be, because
    // nothing else can: the wake-budget backstop dispatches on the esp_timer
    // task and ServiceWakeCycle refuses reentry while this fetch holds the
    // cycle-advance gate, so the backstop cannot cut a long fetch short.
    //
    // The device transport enforces it at the socket, on every read, rather
    // than around the esp_http_client calls — those loop internally and cannot
    // be bounded from outside. Connect is inside the same deadline: the name is
    // resolved first through lwIP's asynchronous resolver, bounded, and the
    // handshake gets only what resolution left. See openmeteo_transport_esp.cc
    // and common/bounded_connect.h.
    static_assert(autonomy::kFetchWireTimeoutMs ==
                      static_cast<uint32_t>(weather::kFetchTimeoutMs),
                  "the planner and the client must agree on the fetch ceiling");
    weather::Forecast forecast;
    const weather::FetchResult result = weather::FetchForecast(
        weather::DeviceHttpTransport(), latitude, longitude, body,
        weather::kMaxResponseBytes, NowEpochOrZero(), &forecast,
        static_cast<int32_t>(budget_ms));
    heap_caps_free(body);

    cycle_fetch_status_.duration_ms = result.http.duration_ms;
    cycle_fetch_status_.http_status = result.http.status;
    cycle_fetch_status_.bytes = static_cast<uint32_t>(result.http.bytes);

    if (!result.ok) {
        ESP_LOGW(kTag, "forecast fetch failed (%s%s%s, http %d)",
                 result.error != nullptr ? result.error : "?",
                 result.detail[0] != '\0' ? ": " : "", result.detail,
                 static_cast<int>(result.http.status));
        return;
    }

    forecast.fetched_epoch = NowEpochOrZero();
    const record::StoreResult stored = weather_cache_->Store(forecast);
    if (stored != record::StoreResult::kOk &&
        stored != record::StoreResult::kDuplicate) {
        // Fetched fine, could not keep it. The panel can still be drawn from
        // the forecast in hand this wake, but the cycle is degraded because the
        // next wake will have to fetch again.
        ESP_LOGW(kTag, "forecast fetched but not cached (%d)",
                 static_cast<int>(stored));
        return;
    }

    cycle_fetch_ok_ = true;
    cycle_fetch_status_.ok = true;
    cycle_fetch_status_.fetched_epoch = forecast.fetched_epoch;
    ESP_LOGI(kTag, "forecast cached: %u hours, %u bytes in %u ms",
             static_cast<unsigned>(forecast.hour_count),
             static_cast<unsigned>(result.http.bytes),
             static_cast<unsigned>(cycle_fetch_status_.duration_ms));
}

/**
 * @brief Compose from the profile, the cache and the clock, and store it.
 *
 * The dedup is the store's, not this function's, and that is load-bearing: the
 * A/B record compares against the bytes actually on flash, so a composition
 * identical to what is displayed costs no write and no twenty-five-second panel
 * refresh. On an hourly wake with a countdown that has not crossed a boundary,
 * that is the overwhelmingly common outcome.
 *
 * The caller holds @c compose_mutex_. There are two of them — the wake cycle
 * and the authenticated render route — and they share this body rather than
 * each having one, because "what does a locally composed panel contain" must
 * have exactly one answer whichever way it was asked for.
 */
Application::LocalComposeReport Application::ComposeLocked(bool degraded) {
    LocalComposeReport report;
    report.degraded = degraded;

    if (autonomy_service_ == nullptr) {
        report.refusal = "autonomy_unsupported";
        return report;
    }
    if (compose_canvas_ == nullptr || compose_frame_ == nullptr) {
        report.refusal = "no_compositor";
        return report;
    }

    weather::Forecast forecast;
    const bool have_forecast =
        weather_cache_ != nullptr && weather_cache_->Read(&forecast);
    report.had_forecast = have_forecast;

    // The sequence the store holds *now*, read before the composition starts.
    // This is the expected half of SubmitLocal's compare-and-swap: a PUT that
    // lands during the CPU-bound seconds below moves it, and the submit then
    // refuses rather than writing over the operator's frame.
    const uint32_t expected_seq =
        dashboard::DashboardManager::GetInstance().Status().stored_seq;
    report.expected_seq = expected_seq;

    autonomy::ComposeInput in;
    in.forecast = have_forecast ? &forecast : nullptr;
    in.now_epoch = NowEpochOrZero();
    in.clock_set = in.now_epoch > 0;
    in.degraded = degraded;

    // The panel's own offset, resolved from the TZ this device already has.
    // See autonomy_compose.h: the compositor does arithmetic, not timezone
    // policy, and the mirror is handed the same number. UtcOffsetSeconds is
    // shared, tested against both sides of a daylight-saving boundary, and
    // replaces three lines here that were an hour wrong for eight months a year.
    in.utc_offset_s = autonomy::UtcOffsetSeconds(in.now_epoch);

    autonomy::ComposeResult result;
    {
        // Held across the composition itself, which is CPU and PSRAM only: no
        // network, no flash. The alternative — copying an 18 KB profile so the
        // lock could be dropped — costs more RAM than this device has spare.
        autonomy::AutonomyService::ProfileLock guard(*autonomy_service_);
        if (!guard.has_profile()) {
            report.refusal = "no_profile";
            return report;
        }
        in.profile = &guard.profile();
        report.composed =
            autonomy::Compose(in, compose_canvas_, compose_frame_, &result);
    }
    if (!report.composed) {
        ESP_LOGE(kTag, "compose refused its own buffers");
        report.refusal = "compose_failed";
        return report;
    }
    report.modules_drawn = result.modules_drawn;
    report.empty_panel = result.empty;

    // A composed frame and a pushed one are the same kind of object, and this
    // is where that stops being a claim: the store refuses any other length,
    // so a divergence between the compositor's packing and the panel's frame
    // size must fail here at compile time rather than as a 400 at 3am.
    static_assert(autonomy::kPackedBytes == dashboard::kFrameBytes,
                  "the compositor's frame and the store's frame are one format");

    const uint32_t source_epoch =
        in.clock_set ? static_cast<uint32_t>(in.now_epoch) : 0u;
    report.push = dashboard::DashboardManager::GetInstance().SubmitLocal(
        compose_frame_, autonomy::kPackedBytes, source_epoch, expected_seq);

    switch (report.push.outcome) {
        case dashboard::PushOutcome::kDeduped:
            // The best answer this path produces: the glass is already right,
            // and it cost no flash write and no twenty-five-second refresh.
            report.outcome = autonomy::ComposeOutcome::kDeduped;
            ESP_LOGI(kTag, "composed locally; identical to what is displayed");
            break;
        case dashboard::PushOutcome::kAccepted:
            report.outcome = autonomy::ComposeOutcome::kAccepted;
            ESP_LOGI(kTag, "composed locally: %u modules drawn, seq %u",
                     static_cast<unsigned>(result.modules_drawn),
                     static_cast<unsigned>(report.push.seq));
            break;
        case dashboard::PushOutcome::kSuperseded:
        case dashboard::PushOutcome::kBusy:
            // A push landed while this frame was being built. Not a failure:
            // it is the arbitration rule working exactly as intended, and the
            // operator's frame is the one going up. Recorded as its own outcome
            // so the status route says so rather than reporting damage.
            report.outcome = autonomy::ComposeOutcome::kSuperseded;
            ESP_LOGI(kTag, "local frame stood aside for a pushed one");
            break;
        default:
            report.outcome = autonomy::ComposeOutcome::kFailed;
            ESP_LOGW(kTag, "local frame not stored (outcome %d)",
                     static_cast<int>(report.push.outcome));
            break;
    }
    return report;
}

/// The wake cycle's entry. Records what happened into this wake's fields, which
/// is what the planner reads back on the next tick.
void Application::RunLocalCompose() {
    // Deliberately *not* set to a success value up front. An earlier revision
    // set `composed = true` before it knew anything, so a compose that refused
    // its own buffers, or whose store failed, was reported to the tower as an
    // update. The result starts as a failure and is improved only by evidence.
    cycle_compose_result_ = autonomy::ComposeOutcome::kFailed;

    // Try, never wait. The other caller is an operator holding the device's
    // token asking for a composition through the render route, and blocking a
    // wake-cycle tick behind two seconds of somebody else's CPU-bound work
    // would spend budget this cycle guaranteed to need. A refused tick is
    // honest: this wake did not draw what it meant to, which is a degraded
    // cycle and earns the retry rather than reporting success.
    std::unique_lock<std::mutex> guard(compose_mutex_, std::try_to_lock);
    if (!guard.owns_lock()) {
        ESP_LOGW(kTag, "a composition was already in flight; not composing this wake");
        cycle_degraded_ = true;
        return;
    }

    const LocalComposeReport report = ComposeLocked(cycle_degraded_);
    cycle_compose_expected_seq_ = report.expected_seq;
    cycle_compose_result_ = report.outcome;
    if (report.outcome == autonomy::ComposeOutcome::kAccepted) {
        // Remembered so the frame this device just stored is not mistaken
        // for one the tower pushed when the sequence is seen to move.
        std::lock_guard<std::mutex> power(power_mutex_);
        cycle_local_seq_ = report.push.seq;
    } else if (report.outcome == autonomy::ComposeOutcome::kFailed) {
        cycle_degraded_ = true;
    }
}

/**
 * @brief Compose one panel now, on behalf of the authenticated render route.
 *
 * The bench path, and deliberately a pull: a local frame reaches the glass
 * outside a wake cycle only when somebody holding the device's token asks for
 * one. The gates it adds over the wake cycle's entry are the two the cycle's
 * planner has already applied by the time it asks for a compose — the runtime
 * kill-switch, and one composition at a time.
 */
Application::LocalComposeReport Application::ComposeLocalFrameNow() {
    LocalComposeReport report;

    if (autonomy_service_ == nullptr) {
        report.refusal = "autonomy_unsupported";
        return report;
    }
    if (!devcfg::DeviceConfigService::GetInstance().Snapshot().autonomy_enabled) {
        // The runtime half of the double gate. Compiled in, switched off.
        report.refusal = "autonomy_disabled";
        return report;
    }

    // One composition at a time: the canvas has a single owner. Refused rather
    // than queued, for the same reason the frame store refuses a second writer.
    std::unique_lock<std::mutex> guard(compose_mutex_, std::try_to_lock);
    if (!guard.owns_lock()) {
        report.refusal = "compose_busy";
        return report;
    }

    // Degraded when the cache has nothing to draw a forecast from. Said rather
    // than inferred: a panel whose weather module is blank because no fetch has
    // ever succeeded is a different thing from one that is simply up to date.
    const bool degraded =
        weather_cache_ == nullptr || !weather_cache_->has_forecast();
    return ComposeLocked(degraded);
}

/**
 * @brief Run one tick of the autonomy half of the fetch phase.
 *
 * **This function never ends the cycle.** It executes at most one step and
 * hands the decision back; the caller owns the phase transitions and the exit.
 *
 * That division is the fix for a real defect. This used to call
 * FinishWakeCycle() itself whenever the planner said "finish" — and the planner
 * said "finish" immediately for a device with autonomy switched off or with no
 * profile, which is the majority of devices. The result was that the first tick
 * of the fetch phase put the device straight back to sleep, closing the
 * ninety-second window a queued push needs to land in, and reporting "unchanged"
 * for wakes that had in fact failed to reach the network at all.
 */
autonomy::CycleDecision Application::ServiceAutonomyStep(
    int64_t now_ms, bool network_up, uint32_t tower_wait_remaining_ms) {
    uint32_t work_remaining = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        // What is left of the *total*, less the render and settle caps. Not the
        // fetch phase's cap: that sizes the tower's rendezvous, and it is the
        // total that has to cover the panel actually drawing what we compose.
        work_remaining = wake_budget_.WorkRemainingMs(now_ms);
    }

    const autonomy::CycleInputs in =
        CollectCycleInputs(network_up, tower_wait_remaining_ms, work_remaining);
    const autonomy::CycleDecision decision = autonomy::PlanCycleStep(in);
    cycle_degraded_ = cycle_degraded_ || decision.degraded;

    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        cycle_decision_ = decision;
        cycle_autonomy_participated_ =
            cycle_autonomy_participated_ || decision.participated;
    }

    switch (decision.step) {
        case autonomy::CycleStep::kWaitForTower:
        case autonomy::CycleStep::kStandDown:
            break;

        case autonomy::CycleStep::kFetchWeather:
            // Deliberately does not fall through to the compose. The fetch may
            // have taken its whole allocation; the budget must be re-read
            // before anything else is started, and the next tick does that.
            RunWeatherFetch(decision.budget_ms);
            break;

        case autonomy::CycleStep::kCompose:
            RunLocalCompose();
            break;
    }

    if (decision.step == autonomy::CycleStep::kStandDown && decision.participated) {
        ESP_LOGI(kTag, "Autonomy: %s (%s)",
                 autonomy::CycleOutcomeName(decision.outcome), decision.reason);
    }
    return decision;
}

/**
 * @brief Write one truthful account of this wake, from one place.
 *
 * Called by FinishWakeCycle and nowhere else. Two things are reconciled here
 * that used to be recorded in different places and could disagree:
 *
 *  - **What the power cycle concluded constrains what the content cycle may
 *    claim.** A frame that was composed and stored but never drawn — the render
 *    phase ran out, the panel was busy — is not an update. The compositor's
 *    opinion is downgraded by what the panel actually did.
 *  - **The origins are read now, not remembered.** `stored_origin` comes from
 *    the header of the record the store holds; `displayed_origin` is only a
 *    claim when the manager can confirm the frame on the glass is that same
 *    record. Everything else is `unknown`, which is a real answer.
 */
void Application::CommitAutonomyRecord(power::CycleOutcome outcome) {
    const dashboard::DashboardStatus st =
        dashboard::DashboardManager::GetInstance().Status();

    std::lock_guard<std::mutex> guard(power_mutex_);
    if (!cycle_autonomy_participated_) {
        // Autonomy took no part in this wake: switched off, or no profile. It
        // has nothing to report, and reporting a default would tell the tower
        // that a content cycle ran on a device where none did.
        return;
    }

    autonomy_cycle_.known = true;
    autonomy_cycle_.outcome = cycle_decision_.outcome;

    // The panel has the last word. `drew` is the render phase's own count
    // moving, which is the only evidence this device has that anything reached
    // the glass.
    const bool drew = cycle_frame_drawn_;
    if (!drew && (autonomy_cycle_.outcome == autonomy::CycleOutcome::kUpdated ||
                  autonomy_cycle_.outcome == autonomy::CycleOutcome::kDegraded)) {
        // Asked of the policy rather than named outcome by outcome, so a new
        // failure — kRenderFailed is one — is a failure here too from the day it
        // exists rather than from the day somebody remembers to add it. For the
        // outcomes that were already listed this is the same answer.
        autonomy_cycle_.outcome = power::CycleSucceeded(outcome)
                                      ? autonomy::CycleOutcome::kUnchanged
                                      : autonomy::CycleOutcome::kFailed;
    }
    if ((outcome == power::CycleOutcome::kBudgetExhausted ||
         outcome == power::CycleOutcome::kRenderFailed) &&
        autonomy_cycle_.outcome != autonomy::CycleOutcome::kUpdated) {
        autonomy_cycle_.outcome = autonomy::CycleOutcome::kFailed;
    }

    // Where the frame this cycle ended on came from. The local sequence is the
    // one this device stored; anything else that moved is the tower's.
    if (st.has_stored_frame) {
        if (cycle_local_seq_ != 0 && st.stored_seq == cycle_local_seq_) {
            autonomy_cycle_.origin = autonomy::Origin::kLocal;
        } else {
            autonomy_cycle_.origin = st.stored_origin_is_local
                                         ? autonomy::Origin::kLocal
                                         : autonomy::Origin::kTower;
        }
    } else {
        autonomy_cycle_.origin = autonomy::Origin::kNone;
    }

    const int64_t now_epoch = NowEpochOrZero();
    autonomy_cycle_.rendered_epoch = now_epoch;
    autonomy_cycle_.rendered_epoch_known = now_epoch > 0;
    autonomy_cycle_.wifi_attempted = true;
    autonomy_cycle_.wifi_connected = wifi_connected_.load(std::memory_order_acquire);
    autonomy_cycle_.fetch = cycle_fetch_status_;
    if (weather_cache_ != nullptr && weather_cache_->has_forecast()) {
        autonomy_cycle_.fetch.fetched_epoch = weather_cache_->fetched_epoch();
        if (now_epoch > 0) {
            autonomy_cycle_.fetch.cache_age_s =
                now_epoch - weather_cache_->fetched_epoch();
            autonomy_cycle_.fetch.age_known = true;
        }
    }
}

autonomy::AutonomyStatus Application::AutonomySnapshot() const {
    autonomy::AutonomyStatus s;
    s.enabled = devcfg::DeviceConfigService::GetInstance().Snapshot().autonomy_enabled;

    if (autonomy_service_ != nullptr) {
        // Under the profile lock: this runs on the HTTP task while the main
        // task may be composing from the same object.
        autonomy::AutonomyService::ProfileLock guard(*autonomy_service_);
        if (guard.has_profile()) {
            s.profile.present = true;
            snprintf(s.profile.sha256, sizeof(s.profile.sha256), "%s",
                     guard.sha256());
            s.profile.revision = guard.revision();
            s.profile.profile_version = guard.profile().profile_version;
            s.profile.applied_epoch = guard.applied_epoch();
            s.profile.applied_epoch_known = guard.applied_epoch_known();
        }
    }

    {
        std::lock_guard<std::mutex> lock(power_mutex_);
        s.last_cycle = autonomy_cycle_;
    }

    // Both origins derived from the manager rather than remembered here. See
    // DashboardStatus::displayed_origin_known: the stored origin is always
    // answerable, the displayed one only when the frame on the glass is still
    // the record the store holds — which it is not after a deep sleep, because
    // e-paper keeps its image and the coordinator does not.
    const dashboard::DashboardStatus frames =
        dashboard::DashboardManager::GetInstance().Status();
    if (!frames.has_stored_frame) {
        s.stored_origin = autonomy::Origin::kNone;
    } else {
        s.stored_origin = frames.stored_origin_is_local ? autonomy::Origin::kLocal
                                                        : autonomy::Origin::kTower;
    }
    if (!frames.has_displayed_frame) {
        s.displayed_origin = frames.has_stored_frame ? autonomy::Origin::kUnknown
                                                     : autonomy::Origin::kNone;
    } else if (!frames.displayed_origin_known) {
        s.displayed_origin = autonomy::Origin::kUnknown;
    } else {
        s.displayed_origin = frames.displayed_origin_is_local
                                 ? autonomy::Origin::kLocal
                                 : autonomy::Origin::kTower;
    }

    // Repeated from the power block for the tower's convenience, and read from
    // the same place, so the two cannot disagree.
    const power::PowerStatus power = PowerSnapshot();
    s.next_wake_epoch = power.next_wake_epoch;
    s.next_wake_epoch_known = power.next_wake_epoch > 0;
    return s;
}

void Application::InitializePowerState() {
    std::string mode;
    int32_t interactive_min = power::kDefaultInteractiveMinutes;
    int32_t wake_interval = power::kDefaultWakeIntervalMin;
    {
        Settings nvs(kPowerNamespace, false);
        mode = nvs.GetString(kPowerModeKey, power::ModeName(power::Mode::kAutoSaver));
        interactive_min = nvs.GetInt(kPowerInteractiveKey,
                                     power::kDefaultInteractiveMinutes);
        wake_interval = nvs.GetInt(kPowerWakeIntervalKey,
                                   power::kDefaultWakeIntervalMin);
        consecutive_failures_ =
            static_cast<uint32_t>(nvs.GetInt(kPowerFailuresKey, 0));
    }

    power::Mode parsed = power::Mode::kAutoSaver;
    if (!power::ParseMode(mode.c_str(), &parsed)) {
        // A value NVS held that this build does not recognise. The safe
        // reading is the mode that preserves the battery, not the one that
        // keeps the radio on for a string nobody can parse.
        ESP_LOGW(kTag, "Unrecognised persisted power mode \"%s\"; using auto_saver",
                 mode.c_str());
        parsed = power::Mode::kAutoSaver;
    }

    const power::WakeReason reason = WakeReasonFromChip();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        power_state_.Init(parsed, wake_interval, interactive_min, now_ms, reason);
        // Nothing has finished a cycle yet, and saying "updated" here is how
        // the status route ended up telling the tower the last update
        // succeeded on a device that has never completed one. The field stays
        // unknown until NoteCycleOutcome puts a real answer in it.
        last_cycle_outcome_ = power::CycleOutcome::kUpdated;
        has_cycle_outcome_ = false;
        next_wake_at_ms_ = 0;
        sleep_intent_ = false;
        cycle_active_ = false;
        cycle_deferred_ = false;
        wake_budget_.Reset();
        net_recovery_.Reset();

        // The bounded cycle starts here, at the top of the wake, because the
        // total it guarantees is the total time this device is awake — not the
        // total of the part that happens after everything else has finished
        // initialising. A timer wake that spent ninety seconds mounting SPIFFS
        // has ninety seconds less to spend on the network, and that is the
        // honest accounting.
        //
        // Only in auto_saver: always_on has no cycle by definition, and a
        // user-initiated wake has just opened an interactive window, which
        // means somebody is standing there and the device is theirs until it
        // expires.
        if (power_state_.effective_mode(now_ms) == power::Mode::kAutoSaver) {
            BeginWakeCycleLocked(now_ms);
        }
    }

    ESP_LOGI(kTag,
             "Power state: wake=%s base=%s effective=%s interval=%d min "
             "window=%d min failures=%u",
             power::WakeReasonName(reason), power::ModeName(parsed),
             power::ModeName(power_state_.effective_mode(now_ms)),
             static_cast<int>(wake_interval), static_cast<int>(interactive_min),
             static_cast<unsigned>(consecutive_failures_));
}

power::WakeInputs Application::CollectWakeInputs() const {
    power::WakeInputs in;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        in.mode = power_state_.effective_mode(now_ms);
        in.wake_interval_min = power_state_.wake_interval_min();
        in.interactive_remaining_ms = power_state_.interactive_remaining_ms(now_ms);
        in.last_outcome = last_cycle_outcome_;
        in.consecutive_failures = consecutive_failures_;
    }

    // Status() is not const: it samples counters under the manager's own lock.
    auto& dash = dashboard::DashboardManager::GetInstance();
    const dashboard::DashboardStatus st = dash.Status();
    // A panel refresh in flight outranks every mode: e-paper holds whatever
    // was on the glass when the power went, so sleeping mid-refresh leaves a
    // half-drawn image until the next one.
    in.refresh_in_flight = st.rendering || st.pending;

    if (rawdraw_ui_manager_) {
        in.provisioning_portal_open = rawdraw_ui_manager_->IsApTransferModeRunning();
        // "A slideshow is running", not "an interval is configured". The two
        // were the same expression here, and the shipped default interval is
        // five minutes, so every auto-saver device answered yes on every tick
        // and PlanNextWake — which refuses sleep for a slideshow ahead of the
        // mode — never let one sleep at all. See nav::SlideshowIsRunning.
        in.slideshow_active = rawdraw_ui_manager_->IsGallerySlideshowRunning();
    }

    const ChargeStatus::Snapshot charge = ZectrixGetChargeSnapshot();
    in.charging = charge.power_present;
    return in;
}

void Application::EnterHybridSleep(uint32_t wake_in_ms, const char* reason) {
    if (wake_in_ms == 0) {
        // PlanNextWake never returns a zero delay with kSleep, but a zero here
        // would arm a timer that fires immediately and spin the device through
        // boot after boot. Refusing is cheaper than that loop.
        ESP_LOGE(kTag, "Refusing a zero-length sleep (%s)", reason);
        return;
    }

    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        sleep_intent_ = true;
        next_wake_at_ms_ = (esp_timer_get_time() / 1000) +
                           static_cast<int64_t>(wake_in_ms);
    }

    ESP_LOGI(kTag,
             "Entering deep sleep (%s); timer wake in %u s, BOOT also wakes. "
             "Nothing on the network can.",
             reason, static_cast<unsigned>(wake_in_ms / 1000));

    // Persist the failure count before the radio goes down. Deep sleep clears
    // RAM, so a counter that lived only in memory would make every wake look
    // like the first failure and retry at the same short delay all day.
    PersistPowerSettings();

    if (sleep_timer_ != nullptr) esp_timer_stop(sleep_timer_);
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();

    // Both sources, every time. The timer is what makes the hourly refresh
    // happen; the button is what makes the device answerable to a person. A
    // remote wake is not on this list because there is no such thing: the
    // radio is off and nothing is listening on any socket.
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wake_in_ms) * 1000ULL);
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::PowerTick() {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    bool expired = false;
    std::string base_mode;
    int32_t interactive_min = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        expired = power_state_.Tick(now_ms);
        base_mode = power::ModeName(power_state_.PersistableMode());
        interactive_min = power_state_.interactive_minutes();
    }
    if (expired) {
        // The window ran out on its own, which is the entire point of it being
        // a window. Re-arm the sleep timer and let the revision move, so a
        // tower polling the config sees that the mode changed without asking.
        // ArmSyncSleepTimer() starts a fresh bounded cycle on the way past.
        ESP_LOGI(kTag, "Interactive window expired; returning to power saving");
        devcfg::DeviceConfigService::GetInstance().NotePowerMode(base_mode,
                                                                interactive_min);
        ArmSyncSleepTimer();
        UpdateStatusBarForUi();
        return;
    }

    ServiceWakeCycle(now_ms);
}

// ----------------------------------------------------------- the wake cycle --

void Application::BeginWakeCycleLocked(int64_t now_ms) {
    // This wake's content state. "Did we already fetch" is a question about
    // this wake and nothing else: carrying it across wakes would mean a device
    // that failed one fetch never trying again.
    cycle_fetch_attempted_ = false;
    cycle_fetch_ok_ = false;
    cycle_compose_result_ = autonomy::ComposeOutcome::kNotTried;
    cycle_compose_expected_seq_ = 0;
    cycle_local_seq_ = 0;
    cycle_degraded_ = false;
    cycle_fetch_status_ = autonomy::FetchStatus{};
    cycle_decision_ = autonomy::CycleDecision{};
    cycle_autonomy_participated_ = false;
    cycle_forced_outcome_known_ = false;
    cycle_forced_outcome_ = power::CycleOutcome::kUnchanged;

    RestartWakeBudgetLocked(now_ms);
}

/**
 * @brief Give the cycle a fresh budget without forgetting what it has done.
 *
 * Split out from BeginWakeCycleLocked because a deferral and a new wake are
 * different events and used to be treated as the same one. A deferral — a
 * refresh in flight, the provisioning portal, a slideshow — is time the cycle
 * was not *allowed* to use, so the budget restarts. But the fetch it already
 * made, and the frame it already composed, still happened: clearing them meant
 * a slideshow left running had the device fetch the forecast again every two
 * minutes for as long as it was on, and compose again each time.
 */
void Application::RestartWakeBudgetLocked(int64_t now_ms) {
    // Caller holds power_mutex_.
    wake_budget_.Start(now_ms, power::WakeBudgetLimits{});
    net_recovery_.Reset();
    cycle_active_ = true;
    cycle_deferred_ = false;
    cycle_phase_ = power::WakePhase::kNetwork;
    cycle_phase_started_ms_ = now_ms;
    cycle_saw_new_frame_ = false;
    cycle_frame_drawn_ = false;
    cycle_start_render_count_ = 0;
    cycle_start_stored_seq_ = 0;
    cycle_start_failed_renders_ = 0;
    cycle_render_failed_ = false;
    cycle_baseline_taken_ = false;
    net_retry_at_ms_ = 0;
    sleep_intent_ = true;
}

void Application::EnterCyclePhaseLocked(power::WakePhase phase, int64_t now_ms) {
    // Caller holds power_mutex_.
    const power::WakePhase from = cycle_phase_;
    const int64_t spent = now_ms - cycle_phase_started_ms_;
    cycle_phase_ = phase;
    cycle_phase_started_ms_ = now_ms;

    // docs/POWER.md HB7 asks where a wake cycle's budget went. The status route
    // reports only the phase that ran *out*; this is the rest of the account,
    // one line per transition, in the key=value style the power lines already
    // use. A backwards clock reads as zero rather than as an enormous duration,
    // for the same reason WakeBudget treats it that way.
    ESP_LOGI(kTag, "wake phase: %s after %u ms in %s",
             power::WakePhaseName(phase),
             static_cast<unsigned>(spent > 0 ? spent : 0),
             power::WakePhaseName(from));
}

/**
 * @brief One step of the bounded wake cycle. Runs on the one-second tick.
 *
 * This is the function that makes `WakeBudget` and `NetworkRecovery` mean
 * something. Before it existed both classes were compiled, host tested and
 * never called: the only thing that put an auto-saver device to sleep was the
 * legacy `sync.interval` timer, so the device advertised a two-minute bounded
 * cycle and then stayed awake for half an hour on every single timer wake.
 *
 * Every exit from here is either "the cycle continues" or a call to
 * FinishWakeCycle, and FinishWakeCycle always ends in sleep. That is the
 * guarantee, and it is why the budget is checked before the phases rather than
 * after: a phase cannot talk its way past a total that is already spent.
 */
void Application::ServiceWakeCycle(int64_t now_ms) {
    // Only one task inside the cycle at a time. The Run() loop ticks this once
    // a second and the wake-budget backstop calls it from the esp_timer task,
    // and the work below is not all short: a bounded forecast fetch blocks the
    // calling task for as long as its deadline allows. Without this, the
    // backstop could enter behind a fetch and run the compose and the status
    // record concurrently with the task that started them.
    //
    // Refused rather than waited on, deliberately — see CycleAdvanceGate. A
    // caller that does not get in has lost one tick of a cycle driven by
    // repetition, and the backstop re-arms.
    power::CycleAdvanceClaim advancing(cycle_advance_gate_);
    if (!advancing.entered()) return;

    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (!cycle_active_) return;
    }

    // Physical activity, and the mode, outrank the cycle. PlanNextWake holds
    // the whole of that precedence and is host tested; asking it here rather
    // than re-deriving the conditions keeps one answer to "may this device
    // sleep right now".
    const power::WakeInputs inputs = CollectWakeInputs();
    const power::WakePlan plan = power::PlanNextWake(inputs);
    if (plan.action != power::WakeAction::kSleep) {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (inputs.mode != power::Mode::kAutoSaver) {
            // Somebody opened a window or asked for always_on. There is no
            // cycle to run and no outcome to record: the device is theirs now.
            cycle_active_ = false;
            cycle_deferred_ = false;
            wake_budget_.Reset();
            sleep_intent_ = false;
            ESP_LOGI(kTag, "Wake cycle stood down: power mode is %s",
                     power::ModeName(inputs.mode));
            return;
        }
        // A refresh in flight, the provisioning portal, or a slideshow. The
        // budget is restarted rather than allowed to run out, because a cycle
        // that expired while the device was *required* to be awake would
        // record a budget-exhausted failure that nothing did wrong, and back
        // the wake interval off for it.
        //
        // Restarted only when it is actually running low, and logged only on
        // the way into the deferral: this runs once a second, and a slideshow
        // left on would otherwise restart the budget and print a line sixty
        // times a minute for as long as it ran.
        if (wake_budget_.TotalRemainingMs(now_ms) < kCycleDeferRestartMs) {
            RestartWakeBudgetLocked(now_ms);
        }
        if (!cycle_deferred_) {
            cycle_deferred_ = true;
            ESP_LOGI(kTag, "Wake cycle deferred: %s", plan.reason);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (cycle_deferred_) {
            cycle_deferred_ = false;
            // Whatever was holding the device up has finished. The cycle gets
            // a full budget from this moment, because the time it spent
            // waiting was not time it was allowed to use — but it keeps what it
            // has already done, so the fetch is not repeated.
            RestartWakeBudgetLocked(now_ms);
            ESP_LOGI(kTag, "Wake cycle resumed; budget restarted");
            return;
        }
    }

    power::WakePhase phase = power::WakePhase::kNetwork;
    bool exhausted = false;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        phase = cycle_phase_;
        exhausted = wake_budget_.Exhausted(now_ms);
        if (exhausted) {
            // Recorded before the cycle ends so the status route can say which
            // phase ran out, which is a much more useful field report than
            // "the device gave up".
            wake_budget_.NoteExhaustedIn(phase);
        }
    }
    if (exhausted) {
        ESP_LOGW(kTag, "Wake budget exhausted in %s; sleeping",
                 power::WakePhaseName(phase));
        FinishWakeCycle(power::CycleOutcome::kBudgetExhausted);
        return;
    }

    const dashboard::DashboardStatus st =
        dashboard::DashboardManager::GetInstance().Status();
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (!cycle_baseline_taken_) {
            cycle_start_render_count_ = st.render_count;
            cycle_start_stored_seq_ = st.stored_seq;
            cycle_start_failed_renders_ = st.failed_renders;
            cycle_baseline_taken_ = true;
        }
        // Sampled on every tick, not only in the render phase. A refusal is
        // fast — the painter can decline before the panel is touched at all —
        // so the failure can come and go inside one second, and a phase that
        // only looked once would miss it and report the wake as unchanged.
        if (st.failed_renders > cycle_start_failed_renders_) {
            cycle_render_failed_ = true;
        }
    }

    switch (phase) {
        case power::WakePhase::kNetwork: {
            if (wifi_connected_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> guard(power_mutex_);
                EnterCyclePhaseLocked(power::WakePhase::kFetch, now_ms);
                ESP_LOGI(kTag, "Wake cycle: network up, waiting for an update");
                return;
            }
            ServiceNetworkRecovery(now_ms);
            return;
        }

        case power::WakePhase::kFetch: {
            autonomy::FetchPhaseInputs phase_in;
            int64_t phase_started = 0;
            {
                std::lock_guard<std::mutex> guard(power_mutex_);
                // A frame this device composed moves `stored_seq` exactly as a
                // pushed one does, so the sequence alone cannot tell them
                // apart. Excluding our own is what stops the cycle reporting
                // its own composition as an operator push.
                if (st.stored_seq != cycle_start_stored_seq_ &&
                    st.stored_seq != cycle_local_seq_) {
                    cycle_saw_new_frame_ = true;
                }
                phase_started = cycle_phase_started_ms_;
                phase_in.tower_frame_seen = cycle_saw_new_frame_;
                phase_in.local_frame_stored =
                    cycle_compose_result_ == autonomy::ComposeOutcome::kAccepted;
                phase_in.phase_left_ms = wake_budget_.RemainingInPhase(
                    power::WakePhase::kFetch, cycle_phase_started_ms_, now_ms);
                phase_in.autonomy_participated = cycle_autonomy_participated_;
                phase_in.autonomy_outcome = cycle_decision_.outcome;
                // Autonomy is settled before it has been asked anything, and
                // again once it stands down. In between — waiting for the
                // tower, or having fetched and not yet drawn — the rendezvous
                // cap must not cut its sequence in half.
                phase_in.autonomy_settled =
                    !cycle_autonomy_participated_ ||
                    cycle_decision_.step == autonomy::CycleStep::kStandDown;
            }
            phase_in.network_up = wifi_connected_.load(std::memory_order_acquire);
            phase_in.panel_busy = st.rendering || st.pending;

            const autonomy::FetchPhaseDecision phase =
                autonomy::PlanFetchPhase(phase_in);

            switch (phase.act) {
                case autonomy::FetchPhaseAct::kBackToNetwork: {
                    std::lock_guard<std::mutex> guard(power_mutex_);
                    EnterCyclePhaseLocked(power::WakePhase::kNetwork, now_ms);
                    return;
                }
                case autonomy::FetchPhaseAct::kGoToRender: {
                    std::lock_guard<std::mutex> guard(power_mutex_);
                    EnterCyclePhaseLocked(power::WakePhase::kRender, now_ms);
                    return;
                }
                case autonomy::FetchPhaseAct::kFinish:
                    ESP_LOGI(kTag, "Wake cycle: %s", phase.reason);
                    FinishWakeCycle(phase.failed ? power::CycleOutcome::kNetworkFailed
                                                 : power::CycleOutcome::kUnchanged);
                    return;
                case autonomy::FetchPhaseAct::kAutonomyStep:
                    break;
            }

            // How long the tower gets before the device considers drawing for
            // itself, as milliseconds still to run. The planner clamps this
            // against the budget itself — it is the one that knows whether a
            // fetch still has to fit afterwards — so nothing is clamped twice.
            uint32_t tower_wait_remaining_ms = 0;
            if (autonomy_service_ != nullptr) {
                autonomy::AutonomyService::ProfileLock profile(*autonomy_service_);
                if (profile.has_profile()) {
                    const int64_t wait_ms =
                        static_cast<int64_t>(profile.profile().tower_wait_s) * 1000;
                    const int64_t elapsed = now_ms - phase_started;
                    tower_wait_remaining_ms =
                        elapsed >= wait_ms ? 0u
                                           : static_cast<uint32_t>(wait_ms - elapsed);
                }
            }

            // The autonomy path. It executes at most one step and never ends
            // the cycle: a device with autonomy off, or with nothing to draw,
            // stands down and the rendezvous above keeps its full ninety
            // seconds, exactly as before this feature existed.
            ServiceAutonomyStep(now_ms, true, tower_wait_remaining_ms);
            if (cycle_compose_result_ == autonomy::ComposeOutcome::kAccepted) {
                // A frame was stored and a refresh requested. The render phase
                // confirms it reached the glass, and the cycle's outcome comes
                // from that rather than from the compositor's opinion of its
                // own work. Taken here rather than on the next tick because the
                // compose may have been the last thing the budget could cover.
                std::lock_guard<std::mutex> guard(power_mutex_);
                EnterCyclePhaseLocked(power::WakePhase::kRender, now_ms);
            }
            return;
        }

        case power::WakePhase::kRender: {
            uint32_t phase_left = 0;
            uint32_t start_render_count = 0;
            {
                std::lock_guard<std::mutex> guard(power_mutex_);
                // What is left of the render cap, not its whole size. The
                // difference matters here for the same reason it does in the
                // fetch phase: this is measured against when the phase started.
                phase_left = wake_budget_.RemainingInPhase(
                    power::WakePhase::kRender, cycle_phase_started_ms_, now_ms);
                start_render_count = cycle_start_render_count_;
            }
            if (st.rendering || st.pending) {
                if (phase_left == 0) {
                    // The panel is still busy and the render cap is spent.
                    // PlanNextWake refuses to sleep through a refresh, so this
                    // does not put a half-drawn frame on the glass: it records
                    // the truth and the next tick's plan keeps the device up
                    // until the refresh finishes.
                    std::lock_guard<std::mutex> guard(power_mutex_);
                    wake_budget_.NoteExhaustedIn(power::WakePhase::kRender);
                }
                return;
            }
            std::lock_guard<std::mutex> guard(power_mutex_);
            EnterCyclePhaseLocked(power::WakePhase::kSettle, now_ms);
            // Kept apart from cycle_saw_new_frame_, which answers a different
            // question — "did the *tower* send something" — and was being
            // overwritten here with the answer to this one.
            cycle_frame_drawn_ = st.render_count > start_render_count;
            return;
        }

        case power::WakePhase::kSettle: {
            bool drew = false;
            bool forced = false;
            power::CycleOutcome forced_outcome = power::CycleOutcome::kUnchanged;
            {
                std::lock_guard<std::mutex> guard(power_mutex_);
                drew = cycle_frame_drawn_;
                forced = cycle_forced_outcome_known_;
                forced_outcome = cycle_forced_outcome_;
            }
            // A wake that never reached the network stays a failed wake even
            // when it managed to draw something from its cache on the way out.
            // The panel is better for it; the retry schedule must not be.
            if (forced) {
                FinishWakeCycle(forced_outcome);
                return;
            }
            // "unchanged" is not a consolation prize. A frame that arrived and
            // matched what is already on the glass is a cycle that did exactly
            // what it should, and the panel was spared a twenty-second refresh.
            FinishWakeCycle(drew ? power::CycleOutcome::kUpdated
                                 : power::CycleOutcome::kUnchanged);
            return;
        }

        case power::WakePhase::kCount:
            return;
    }
}

/**
 * @brief The bounded half of "retry the association", from NetworkRecovery.
 *
 * The attempt counter is not the thing that stops this. The budget is: an
 * attempt is only started when the time left covers the backoff *and* an
 * attempt long enough to be worth making, so a device whose router is dead
 * stops and sleeps rather than retrying its way through the battery.
 */
void Application::ServiceNetworkRecovery(int64_t now_ms) {
    uint32_t attempt = 0;
    bool give_up = false;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);

        // Still inside the backoff for this attempt. The delay is observed by
        // *not acting* on these ticks rather than by blocking, so the cycle
        // stays responsive to a window opening underneath it.
        if (now_ms < net_retry_at_ms_) return;

        uint32_t delay_ms = 0;
        if (!net_recovery_.ShouldRetry(wake_budget_.TotalRemainingMs(now_ms),
                                       &delay_ms)) {
            give_up = true;
            attempt = net_recovery_.attempts();
        } else {
            net_recovery_.NoteAttempt();
            attempt = net_recovery_.attempts();
            // BackoffMs() reads the counter we just incremented, so this is the
            // wait before the *next* attempt.
            net_retry_at_ms_ = now_ms + static_cast<int64_t>(net_recovery_.BackoffMs());
        }
    }

    if (give_up) {
        // Out of attempts, or the time left will not cover a backoff and an
        // attempt worth making. Either way the honest thing is to sleep with a
        // stale panel and come back sooner, which RetryDelayMs arranges.
        {
            std::lock_guard<std::mutex> guard(power_mutex_);
            wake_budget_.NoteExhaustedIn(power::WakePhase::kNetwork);
        }
        ESP_LOGW(kTag, "Network recovery gave up after %u attempt(s)",
                 static_cast<unsigned>(attempt));

        // No radio is not the same as nothing to draw. A countdown, a message
        // and a cached forecast are all still true, and this is the wake where
        // drawing them matters most — the tower is unreachable, so nobody else
        // is going to.
        //
        // The tower's turn is over by definition here: there was no network to
        // give it one over, so the wait remaining is zero. The planner still
        // applies every other rule, so a device with autonomy off, or with a
        // fresh tower frame on the glass, sleeps exactly as it did before this
        // feature existed.
        //
        // The outcome is kNetworkFailed either way, and that is the point. A
        // wake that never reached the network is a failed wake, and it has to
        // earn the retry backoff whether or not the device managed to draw a
        // countdown from its own cache in the meantime. An earlier revision let
        // the autonomy step overwrite this with "unchanged", which quietly
        // turned every offline wake into a successful one.
        cycle_degraded_ = true;
        ServiceAutonomyStep(now_ms, false, 0);

        if (cycle_compose_result_ == autonomy::ComposeOutcome::kAccepted) {
            // Something was drawn from the cache and a refresh is now running.
            // Sleeping on top of it would leave a half-drawn panel and a status
            // block claiming an update nobody watched happen, so the cycle goes
            // through the render and settle phases exactly as an online one
            // does — and still ends as a network failure, which is what it was.
            std::lock_guard<std::mutex> guard(power_mutex_);
            cycle_forced_outcome_ = power::CycleOutcome::kNetworkFailed;
            cycle_forced_outcome_known_ = true;
            EnterCyclePhaseLocked(power::WakePhase::kRender, now_ms);
            ESP_LOGI(kTag, "No network; drew from the cache, waiting for the panel");
            return;
        }

        ESP_LOGW(kTag, "No network this wake; sleeping");
        FinishWakeCycle(power::CycleOutcome::kNetworkFailed);
        return;
    }

    ESP_LOGI(kTag, "Network attempt %u of %u", static_cast<unsigned>(attempt),
             static_cast<unsigned>(power::NetworkRecovery::kMaxAttempts));
    Board::GetInstance().RequestNetwork();
}

/**
 * @brief End the cycle: record what it achieved, then sleep.
 *
 * Idempotent, because two things can reach it in the same instant — the
 * one-second tick and the backstop timer — and recording one cycle twice would
 * double-count a failure and halve the retry delay.
 */
void Application::FinishWakeCycle(power::CycleOutcome outcome) {
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (!cycle_active_) return;
        cycle_active_ = false;
        cycle_deferred_ = false;

        // A refresh that started during this wake and did not reach the glass.
        // Applied here rather than in the settle phase because the cycle has
        // several exits and the refusal can happen under any of them: the
        // painter can decline in milliseconds, long before the render phase is
        // reached. Only "unchanged" is promoted — kUpdated means something did
        // draw, and the failure outcomes already earn the backoff.
        if (cycle_render_failed_ && outcome == power::CycleOutcome::kUnchanged) {
            outcome = power::CycleOutcome::kRenderFailed;
            ESP_LOGW(kTag, "A refresh was refused or failed this wake; the stored "
                           "frame is not on the glass. Retrying sooner rather "
                           "than reporting success.");
        }
    }
    // One place, every exit. The autonomy block used to be written only on the
    // path where autonomy itself ended the cycle, which left the ordinary case
    // — compose, render, settle — reporting nothing at all.
    CommitAutonomyRecord(outcome);
    NoteCycleOutcome(outcome);
    EnterScheduledSleep();
}

void Application::NotePhysicalActivity() {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    int32_t window_min = 0;
    bool opened = false;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        // always_on has nothing to extend, and a device already in a window is
        // simply given a fresh one.
        if (power_state_.effective_mode(now_ms) == power::Mode::kAlwaysOn) return;
        window_min = power_state_.interactive_minutes();
        opened = power_state_.Request(power::Mode::kInteractive, window_min, now_ms);
    }
    if (!opened) return;

    // An interactive window means someone is at the device. The saver keeps
    // Wi-Fi down between autonomy wakes, so without this a press leaves the
    // panel awake but unreachable: the tower can neither apply a queued config
    // change nor push a fresh frame on demand, and there is no way to read the
    // panel's status to see what it is doing. Bring the station up — it is
    // idempotent (a no-op when already connected or connecting), and the
    // NetworkEvent::Connected handler starts the LAN server with power-save
    // off, exactly as it does on a cold boot.
    {
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsConfigMode() &&
            !wifi_connected_.load(std::memory_order_acquire) &&
            !wifi.IsConnected()) {
            ESP_LOGI(kTag,
                     "Interactive window: bringing Wi-Fi up for tower reachability");
            wifi.StartStation();
        }
    }

    // Deliberately not persisted and deliberately not bumping the config
    // revision. A window is live state with a deadline, and making every
    // button press move the compare-and-swap counter would give the tower a
    // revision conflict every time somebody walked past the device.
    if (sleep_timer_ != nullptr) esp_timer_stop(sleep_timer_);
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        sleep_intent_ = false;
        next_wake_at_ms_ = 0;
    }
}

void Application::ApplyPowerMode(const std::string& mode, int32_t interactive_min) {
    power::Mode parsed = power::Mode::kAutoSaver;
    if (!power::ParseMode(mode.c_str(), &parsed)) {
        // The config contract validated this before it got here, so reaching
        // this is a bug rather than bad input. Refusing beats guessing.
        ESP_LOGE(kTag, "ApplyPowerMode called with an unknown mode \"%s\"",
                 mode.c_str());
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    bool ok = false;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        ok = power_state_.Request(parsed, interactive_min, now_ms);
    }
    if (!ok) {
        ESP_LOGE(kTag, "ApplyPowerMode refused window length %d",
                 static_cast<int>(interactive_min));
        return;
    }

    ESP_LOGI(kTag, "Power mode is now %s (window %d min)", mode.c_str(),
             static_cast<int>(interactive_min));
    PersistPowerSettings();
    // Re-arm or cancel, whichever the new mode calls for. ArmSyncSleepTimer
    // reads the mode itself, so it does the right thing for all three.
    ArmSyncSleepTimer();
    UpdateStatusBarForUi();
}

void Application::SetWakeIntervalMinutes(int32_t minutes) {
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (!power_state_.SetWakeInterval(minutes)) {
            ESP_LOGE(kTag, "Refused wake interval %d minutes",
                     static_cast<int>(minutes));
            return;
        }
    }
    ESP_LOGI(kTag, "Auto-saver wake interval is now %d minutes",
             static_cast<int>(minutes));
    PersistPowerSettings();
    // Record it in the config the tower reads. A no-op when this call came
    // from a config patch (the service already holds the value and NoteWakeInterval
    // returns early when nothing changed); the reason it is here is the other
    // callers — a clamp, or a future Settings menu item — whose change would
    // otherwise never reach the config route and would leave the tower reading
    // an interval the device is not using.
    devcfg::DeviceConfigService::GetInstance().NoteWakeInterval(minutes);
}

void Application::SetInteractiveMinutes(int32_t minutes) {
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        if (!power_state_.SetInteractiveMinutes(minutes)) {
            ESP_LOGE(kTag, "Refused interactive window length %d minutes",
                     static_cast<int>(minutes));
            return;
        }
    }
    // Deliberately does not touch an open window, re-arm the sleep timer or
    // call ArmSyncSleepTimer. Changing how long the *next* window will be is
    // not a request to end the one the user is currently standing in.
    ESP_LOGI(kTag, "Interactive window length is now %d minutes",
             static_cast<int>(minutes));
    PersistPowerSettings();
}

void Application::NoteCycleOutcome(power::CycleOutcome outcome) {
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        last_cycle_outcome_ = outcome;
        has_cycle_outcome_ = true;
        if (power::CycleSucceeded(outcome)) {
            consecutive_failures_ = 0;
        } else if (consecutive_failures_ < 1000u) {
            // Bounded so a device that has been offline for months does not
            // overflow the counter the retry ladder reads.
            ++consecutive_failures_;
        }
    }
    ESP_LOGI(kTag, "Update cycle outcome: %s (consecutive failures %u)",
             power::CycleOutcomeName(outcome),
             static_cast<unsigned>(consecutive_failures_));
    PersistPowerSettings();
}

void Application::PersistPowerSettings() {
    std::string mode;
    int32_t interactive_min = 0;
    int32_t wake_interval = 0;
    uint32_t failures = 0;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        // PersistableMode(), not effective_mode(): an interactive window is
        // live state with a deadline and must never come back from NVS.
        mode = power::ModeName(power_state_.PersistableMode());
        interactive_min = power_state_.interactive_minutes();
        wake_interval = power_state_.wake_interval_min();
        failures = consecutive_failures_;
    }
    Settings nvs(kPowerNamespace, true);
    nvs.SetString(kPowerModeKey, mode);
    nvs.SetInt(kPowerInteractiveKey, interactive_min);
    nvs.SetInt(kPowerWakeIntervalKey, wake_interval);
    nvs.SetInt(kPowerFailuresKey, static_cast<int32_t>(failures));
}

power::PowerStatus Application::PowerSnapshot() const {
    power::PowerStatus s;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    {
        std::lock_guard<std::mutex> guard(power_mutex_);
        s.effective = power_state_.effective_mode(now_ms);
        s.desired = power_state_.desired_mode();
        s.ack = power_state_.ack_state(now_ms);
        s.interactive_remaining_s =
            power_state_.interactive_remaining_ms(now_ms) / 1000u;
        s.wake_interval_min = power_state_.wake_interval_min();
        s.last_wake_reason = power_state_.wake_reason();
        s.last_outcome = last_cycle_outcome_;
        s.last_outcome_known = has_cycle_outcome_;
        s.consecutive_failures = consecutive_failures_;
        s.sleep_intent = sleep_intent_;
        if (wake_budget_.has_exhausted_phase()) {
            s.budget_exhausted_phase = wake_budget_.exhausted_phase();
        }
        if (next_wake_at_ms_ > now_ms) {
            s.timer_armed = true;
            s.next_wake_in_s =
                static_cast<uint32_t>((next_wake_at_ms_ - now_ms) / 1000);
        }
    }

    // Anything answering this route is by definition awake. The field is here
    // for the tower, which shows it next to a last-seen time: "awake" in a
    // cached response means "awake when this was read", and the tower says so.
    s.awake = true;

    // Only a device with a set clock can name a wall-clock time. A device that
    // has not reached SNTP reports null rather than an epoch computed from a
    // clock that starts at 1970.
    if (s.timer_armed) {
        const time_t now = time(nullptr);
        if (now > 1600000000) {  // later than 2020, so the clock has been set
            s.next_wake_epoch = static_cast<uint32_t>(now) + s.next_wake_in_s;
        }
    }

    int battery_percent = -1;
    bool charging = false;
    bool discharging = false;
    const bool read_ok =
        Board::GetInstance().GetBatteryLevel(battery_percent, charging, discharging);
    uint16_t millivolts = 0;
    bool calibrated = false;
    ZectrixReadBatteryMillivolts(&millivolts, &calibrated);
    s.battery = power::EvaluateBattery(read_ok && battery_percent >= 0, calibrated,
                                       millivolts,
                                       battery_percent < 0
                                           ? 0
                                           : static_cast<uint8_t>(battery_percent));

    const ChargeStatus::Snapshot charge = ZectrixGetChargeSnapshot();
    switch (charge.state) {
        case ChargeStatus::State::kNoPower:   s.charge_state = "no_power"; break;
        case ChargeStatus::State::kCharging:  s.charge_state = "charging"; break;
        case ChargeStatus::State::kFull:      s.charge_state = "full"; break;
        case ChargeStatus::State::kNoBattery: s.charge_state = "no_battery"; break;
    }
    s.charging = charge.charging;
    return s;
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        // Folds an expired interactive window back into power saving. Once a
        // second is ample: the shortest window is five minutes, so the worst
        // case overshoot is a second on three hundred.
        PowerTick();
        // The push-to-talk timeouts are no longer advanced here. A once-a-second
        // tick could be more than three times the 300 ms arm threshold late,
        // which is the difference between hold-to-talk and a button that seems
        // dead. StartPttTickTimer() runs it at 25 ms on its own esp_timer.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return false;
}

void Application::UpdateStatusBarForUi() {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        UpdateWifiSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), wifi_connected);
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        UpdateLanIpSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), lan_ip);
        UpdateHttpServerSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning()
                                         ? lan_ip
                                         : "");
        // Keep the dashboard placeholder honest. Without this call it can only
        // ever claim "Waiting for Wi-Fi", which is what it did before.
        if (auto* dashboard_renderer = rawdraw_ui_manager_->GetDashboardRenderer()) {
            dashboard_renderer->SetPlaceholderInfo(
                lan_ip,
                dashboard::DashboardManager::GetInstance().Provisioned(),
                rawdraw_ui_manager_->IsLanHttpServerRunning());
        }
        UpdateAboutInfo(lan_ip);
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return;
}

void Application::UpdateAboutInfo(const std::string& lan_ip) {
    if (!rawdraw_ui_manager_) return;
    auto* sr = rawdraw_ui_manager_->GetSettingsRenderer();
    if (sr == nullptr) return;

    auto& dash = dashboard::DashboardManager::GetInstance();
    const dashboard::DashboardStatus status = dash.Status();

    rawdraw::SettingsRenderer::AboutInfo info;
    info.product = product::kName;
    info.firmware = product::kFirmwareVersion;
    info.hardware = product::kHardware;
    info.upstream = product::kUpstreamBase;

    if (!lan_ip.empty()) {
        info.lan_address = rawdraw_ui_manager_->IsLanHttpServerRunning()
                               ? lan_ip
                               : lan_ip + " (service off)";
    } else {
        info.lan_address = wifi_connected_.load(std::memory_order_acquire)
                               ? "Waiting for IP"
                               : "No Wi-Fi";
    }

    info.pairing = status.provisioned ? "Paired" : "Not paired";

    // Three facts in the order that decides whether a microphone can open:
    // is the code even here, is the mute off, and is there anywhere to send
    // audio. Any one of them being wrong means nothing is recorded, and the
    // row says which one it is rather than a single word.
    {
        bool muted = true;
        {
            std::lock_guard<std::mutex> lock(ptt_mutex_);
            muted = ptt_fsm_.muted();
        }
        if (VOICE_PTT_ENABLED == 0) {
            info.voice = "Not compiled in";
        } else if (muted) {
            info.voice = "Muted (software)";
        } else if (!voice_uploader_.configured()) {
            info.voice = "Unmuted, no hub";
        } else {
            info.voice = "Unmuted, hub set";
        }
    }

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "PTT %s, UI %s",
                 VOICE_PTT_ENABLED ? "on" : "off",
                 DASHBOARD_MINIMAL_UI ? "minimal" : "full");
        info.build = buf;
    }

    {
        // The counters that mean "the panel stopped updating and nobody would
        // otherwise know": frames that reached the glass, and writes the
        // filesystem refused.
        char buf[80];
        snprintf(buf, sizeof(buf), "%u renders, %u store errors",
                 static_cast<unsigned>(status.render_count),
                 static_cast<unsigned>(status.storage_write_failures +
                                       status.storage_read_failures));
        info.diagnostic = buf;
    }

    sr->SetAboutInfo(info);
}
