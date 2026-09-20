#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "FT/factory_test_service.h"
#include "application.h"
#include "board.h"
#include "board_power_bsp.h"
#include "boards/common/i2c_bus_lock.h"
#include "boards/zectrix/zectrix_nfc.h"
#include "button.h"
#include "common/battery_monitor.h"
#include "common/button_gestures.h"
#include "charge_status.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "display/pages/factory_test_page_adapter.h"
#include "esp_network.h"
#include "network_interface.h"
#include "rtc_pcf8563.h"
#include "ssid_manager.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "ZectrixFtBoard";
constexpr uint16_t kNavLongPressMs = 1000;
constexpr uint16_t kFactoryComboLongPressMs = 3000;
constexpr gpio_num_t kBoardUpButtonGpio = TODO_UP_BUTTON_GPIO;
constexpr gpio_num_t kBoardDownButtonGpio = TODO_DOWN_BUTTON_GPIO;
constexpr gpio_num_t kBoardConfirmButtonGpio = BOOT_BUTTON_GPIO;

// Press/hold/release/suppression/combo recognition used to be nine file-static
// atomics right here. It now lives in main/common/button_gestures.cc, where the
// host tests drive every interleaving; this file is glue. The recogniser is not
// internally locked because every callback below runs on the single iot_button
// task, which is also all the atomics ever really provided.
gesture::Recognizer& Gestures() {
    static gesture::Recognizer recognizer{gesture::Config{kNavLongPressMs}};
    return recognizer;
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/// Send one recognised gesture to the application. kNone means the raw event
/// was part of a larger gesture and has already been accounted for.
void DispatchGesture(gesture::Semantic semantic) {
    if (semantic == gesture::Semantic::kNone) {
        return;
    }
    auto& app = Application::GetInstance();
    ESP_LOGI(kTag, "Gesture: %s", gesture::SemanticName(semantic));
    switch (semantic) {
        case gesture::Semantic::kUpClick:      app.OnUpClick(); break;
        case gesture::Semantic::kDownClick:    app.OnDownClick(); break;
        case gesture::Semantic::kConfirmClick: app.OnBootClick(); break;
        case gesture::Semantic::kUpLong:       app.OnUpLongPress(); break;
        case gesture::Semantic::kDownLong:     app.OnDownLongPress(); break;
        case gesture::Semantic::kConfirmLong:  app.OnBootLongPress(); break;
        case gesture::Semantic::kComboLong:    app.OnWifiConfigComboLongPress(); break;
        case gesture::Semantic::kNone:         break;
    }
}

class CustomBoard : public Board {
public:
    CustomBoard()
        : up_button_(kBoardUpButtonGpio, false, kNavLongPressMs),
          down_button_(kBoardDownButtonGpio, false, kNavLongPressMs),
          confirm_button_(kBoardConfirmButtonGpio, false, kNavLongPressMs) {
        InitializePower();
        InitializeI2c();
        InitializeRtc();
        InitializeNfc();
        InitializeChargeStatus();
        InitializeBatteryMonitor();
        InitializeLcdDisplay();
        InitializeButtons();
        BindFactoryTestCallbacks();
    }

    /// The board is a singleton and is not expected to be torn down in normal
    /// operation, but "not expected to" is not "cannot": the ADC unit has an
    /// owner now, and an owner that never releases is the same unowned handle
    /// this class just stopped having. Closing here also runs before the
    /// handle members go away, so no hook can ever see a half-destroyed board.
    ~CustomBoard() override {
        battery_.Close();
    }

    std::string GetBoardType() override {
        return "zectrix-s3-epaper-4.2";
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec codec(i2c_bus_,
                                      I2C_NUM_0,
                                      AUDIO_INPUT_SAMPLE_RATE,
                                      AUDIO_OUTPUT_SAMPLE_RATE,
                                      AUDIO_I2S_GPIO_MCLK,
                                      AUDIO_I2S_GPIO_BCLK,
                                      AUDIO_I2S_GPIO_WS,
                                      AUDIO_I2S_GPIO_DOUT,
                                      AUDIO_I2S_GPIO_DIN,
                                      AUDIO_CODEC_PA_PIN,
                                      AUDIO_CODEC_ES8311_ADDR);
        return &codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    NetworkInterface* GetNetwork() override {
        return &network_;
    }

    void StartNetwork() override {
        if (network_started_) {
            return;
        }

        WifiManagerConfig config;
        config.ssid_prefix = "ZecTrix";
        config.ap_password = "";  // Open AP — no password needed for easy config
        config.language = "zh-CN";
        if (!WifiManager::GetInstance().Initialize(config)) {
            ESP_LOGE(kTag, "WiFi manager init failed");
            if (network_event_callback_) {
                network_event_callback_(NetworkEvent::Disconnected, "");
            }
            return;
        }

        WifiManager::GetInstance().SetEventCallback([this](WifiEvent event) {
            if (!network_event_callback_) {
                return;
            }

            switch (event) {
                case WifiEvent::Scanning:
                    network_event_callback_(NetworkEvent::Scanning, "");
                    break;
                case WifiEvent::Connecting:
                    network_event_callback_(NetworkEvent::Connecting, WifiManager::GetInstance().GetSsid());
                    break;
                case WifiEvent::Connected:
                    network_event_callback_(NetworkEvent::Connected, WifiManager::GetInstance().GetIpAddress());
                    break;
                case WifiEvent::Disconnected:
                    network_event_callback_(NetworkEvent::Disconnected, "");
                    break;
                case WifiEvent::ConfigModeEnter:
                    network_event_callback_(
                        NetworkEvent::WifiConfigModeEnter,
                        "AP " + WifiManager::GetInstance().GetApSsid() +
                            " PWD " + WifiManager::GetInstance().GetApPassword() +
                            " " + WifiManager::GetInstance().GetApWebUrl());
                    break;
                case WifiEvent::ConfigModeExit:
                    network_event_callback_(NetworkEvent::WifiConfigModeExit, "");
                    break;
            }
        });

        if (SsidManager::GetInstance().GetSsidList().empty()) {
            ESP_LOGW(kTag, "No saved WiFi credentials, starting config AP");
            WifiManager::GetInstance().StartConfigAp();
        } else {
            WifiManager::GetInstance().StartStation();
        }

        network_started_ = true;
    }

    bool IsFactoryTestMode() const override {
        return false;
    }

    void EnterFactoryTestFlow() override {
        if (display_ == nullptr) {
            return;
        }
        display_->ShowFactoryTestPage();
        display_->RequestUrgentFullRefresh();
        FactoryTestService::Instance().StartFlow();
    }

    const char* GetNetworkStateIcon() override {
        return nullptr;
    }

    /**
     * @brief Charge flags and a battery percentage, from any task.
     *
     * This is called from the UI task for the status bar and from the HTTP
     * task for the status route, so it does only things that are safe from
     * both: `charge_status_.Get()` is an atomic load, and `battery_.Get()` is
     * locked and rate-limited.
     *
     * It no longer calls `charge_status_.Tick()`. Tick mutates the debounce
     * timers, which are plain members, and the LED task in board_power_bsp.cc
     * already ticks it twice a second — so the tick here was a second writer
     * racing that one, corrupting the hold windows and re-firing the state
     * callback, for a snapshot that was at most 500 ms fresher.
     */
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        ChargeStatus::Snapshot snapshot = charge_status_.Get();
        charging = snapshot.charging;
        discharging = !snapshot.power_present;

        const battery::Sample sample = battery_.Get();
        level = static_cast<int>(sample.percent);
        return sample.valid;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        switch (level) {
            case PowerSaveLevel::LOW_POWER:
                WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
                break;
            case PowerSaveLevel::BALANCED:
                WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
                break;
            case PowerSaveLevel::PERFORMANCE:
                WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
                break;
        }
    }

    std::string GetBoardJson() override {
        return R"({"type":"zectrix-s3-epaper-4.2","mode":"gallery"})";
    }

    std::string GetDeviceStatusJson() override {
        return R"({"mode":"gallery"})";
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        network_event_callback_ = callback;
    }

    RtcPcf8563* GetRtc() {
        return rtc_.get();
    }

    ZectrixNfc* GetNfc() {
        return nfc_.get();
    }

    ChargeStatus::Snapshot GetChargeSnapshot() const {
        return charge_status_.Get();
    }

    /// Cell millivolts and whether the ADC calibration behind them exists.
    /// @return false when there is nothing fit to report.
    bool ReadBatteryMillivolts(uint16_t* mv_out, bool* calibrated_out) {
        const battery::Sample sample = battery_.Get();
        if (mv_out != nullptr) *mv_out = sample.millivolts;
        if (calibrated_out != nullptr) *calibrated_out = sample.calibrated;
        return sample.valid;
    }

    ChargeStatus::Snapshot RefreshChargeSnapshotForFactoryTest() {
        charge_status_.Tick(GetNowMs());
        return charge_status_.Get();
    }

    /// The factory test is measuring the hardware rather than reporting on it,
    /// so it bypasses the cache — but not the lock, and not the single open.
    bool ReadBatteryPercentForFactoryTest(int* level) {
        if (level == nullptr) {
            return false;
        }

        const battery::Sample sample = battery_.GetFresh();
        *level = static_cast<int>(sample.percent);
        return sample.valid;
    }

    void SetFactoryLedOverride(bool enabled, bool blink) {
        if (power_ != nullptr) {
            power_->SetFactoryLedOverride(enabled, blink);
        }
    }

    void FlashActivityLed() override {
        if (power_ != nullptr) {
            power_->FlashActivityLed();
        }
    }

private:
    static int64_t GetNowMs() {
        return esp_timer_get_time() / 1000;
    }

    void InitializePower() {
        // Audio_AMP_PIN (GPIO46) is not passed: it belongs to the ES8311
        // codec, which holds it around every output enable. See
        // board_power_bsp.h for why two owners stopped being survivable.
        power_ = std::make_unique<BoardPowerBsp>(EPD_PWR_PIN,
                                                 Audio_PWR_PIN,
                                                 VBAT_PWR_PIN,
                                                 &charge_status_);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        while (!gpio_get_level(VBAT_PWR_GPIO)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void InitializeI2c() {
        ScopedI2cBusLock bus_lock("CustomBoard::InitializeI2c");
        ESP_ERROR_CHECK(bus_lock.status());

        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = static_cast<i2c_port_t>(0);
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeRtc() {
        rtc_ = std::make_unique<RtcPcf8563>(i2c_bus_, RTC_I2C_ADDR);
        if (!rtc_->Init(RTC_INT_GPIO)) {
            ESP_LOGW(kTag, "RTC init failed");
        }
    }

    void InitializeNfc() {
        nfc_ = std::make_unique<ZectrixNfc>(i2c_bus_,
                                            NFC_I2C_ADDR,
                                            NFC_PWR_GPIO,
                                            NFC_FD_GPIO,
                                            NFC_FD_ACTIVE_LEVEL);
        if (!nfc_->Init()) {
            ESP_LOGW(kTag, "NFC init failed");
            nfc_.reset();
        }
    }

    void InitializeChargeStatus() {
        charge_status_.Init(CHARGE_DETECT_GPIO, CHARGE_FULL_GPIO, GetNowMs());
    }

    void InitializeLcdDisplay() {
        custom_lcd_spi_t lcd_spi_data = {};
        lcd_spi_data.cs = EPD_CS_PIN;
        lcd_spi_data.dc = EPD_DC_PIN;
        lcd_spi_data.rst = EPD_RST_PIN;
        lcd_spi_data.busy = EPD_BUSY_PIN;
        lcd_spi_data.mosi = EPD_MOSI_PIN;
        lcd_spi_data.scl = EPD_SCK_PIN;
        lcd_spi_data.power = EPD_PWR_PIN;
        lcd_spi_data.spi_host = EPD_SPI_NUM;
#if CONFIG_ZECTRIX_EPD_PANEL_4COLOR_SSD2683
        lcd_spi_data.panel_type = EPD_PANEL_4COLOR_SSD2683;
#else
        lcd_spi_data.panel_type = EPD_PANEL_1BPP;
#endif
        // RawDraw now keeps a 2bpp semantic framebuffer even for 1bpp panels.
        // The display driver down-converts RED/YELLOW/BLACK to black and WHITE
        // to white when sending data to a black/white EPD.
        lcd_spi_data.buffer_len = ((EXAMPLE_LCD_WIDTH * 2 + 7) / 8) * EXAMPLE_LCD_HEIGHT;
        display_ = new CustomLcdDisplay(nullptr,
                                        nullptr,
                                        EXAMPLE_LCD_WIDTH,
                                        EXAMPLE_LCD_HEIGHT,
                                        DISPLAY_OFFSET_X,
                                        DISPLAY_OFFSET_Y,
                                        DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y,
                                        DISPLAY_SWAP_XY,
                                        lcd_spi_data);
    }

    void InitializeButtons() {
        // Every callback does the same two things: hand the raw event to the
        // recogniser with a timestamp, and dispatch whatever it decides. The
        // factory-test fallback stays: with no UI manager there is no
        // navigation to speak of.
        up_button_.OnPressDown([]() { Gestures().OnPressDown(gesture::Button::kUp, NowMs()); });
        up_button_.OnPressUp([]() {
            DispatchGesture(Gestures().OnPressUp(gesture::Button::kUp, NowMs()));
        });
        up_button_.OnLongPress([]() {
            DispatchGesture(Gestures().OnLongPress(gesture::Button::kUp, NowMs()));
        });
        up_button_.OnClick([]() {
            DispatchGesture(Gestures().OnClick(gesture::Button::kUp, NowMs()));
        });

        down_button_.OnPressDown([]() { Gestures().OnPressDown(gesture::Button::kDown, NowMs()); });
        down_button_.OnPressUp([]() {
            DispatchGesture(Gestures().OnPressUp(gesture::Button::kDown, NowMs()));
        });
        down_button_.OnLongPress([]() {
            DispatchGesture(Gestures().OnLongPress(gesture::Button::kDown, NowMs()));
        });
        down_button_.OnClick([]() {
            DispatchGesture(Gestures().OnClick(gesture::Button::kDown, NowMs()));
        });

        confirm_button_.OnPressDown([]() {
            Gestures().OnPressDown(gesture::Button::kConfirm, NowMs());
            auto& app = Application::GetInstance();
            if (app.GetRawDrawUiManager() == nullptr) {
                return;
            }
            // Push-to-talk arms here, on the press-down, not at the driver's
            // 1000 ms long press. Arming at the long press could never be
            // hold-to-talk: the microphone would open 1300 ms after the finger
            // landed. The state machine's own 300 ms threshold now decides
            // whether this press is a tap or an utterance.
            app.OnBootPressed();
        });
        confirm_button_.OnPressUp([]() {
            auto& app = Application::GetInstance();
            if (app.GetRawDrawUiManager() == nullptr) {
                // Factory test: the recogniser still needs the release so its
                // per-button state does not go stale.
                Gestures().OnPressUp(gesture::Button::kConfirm, NowMs());
                return;
            }
            // The state machine decides first, because what it decides changes
            // whether this press is still available to navigation. A hold that
            // crossed the arm threshold consumed the press, and the click the
            // driver is about to deliver must be swallowed: otherwise a 500 ms
            // hold on the Dashboard records an utterance and quick-switches the
            // page, which is two outcomes for one press.
            const bool consumed = app.OnBootReleased();
            if (consumed) {
                Gestures().SuppressNextClick(gesture::Button::kConfirm);
            }
            const auto semantic = Gestures().OnPressUp(gesture::Button::kConfirm, NowMs());
            DispatchGesture(semantic);
        });
        confirm_button_.OnClick([]() {
            const auto semantic = Gestures().OnClick(gesture::Button::kConfirm, NowMs());
            if (semantic == gesture::Semantic::kNone) {
                return;
            }
            if (Application::GetInstance().GetRawDrawUiManager() == nullptr) {
                FactoryTestService::Instance().HandleButton(FactoryTestButton::kConfirmClick);
                return;
            }
            DispatchGesture(semantic);
        });
        confirm_button_.OnLongPress([]() {
            const auto semantic = Gestures().OnLongPress(gesture::Button::kConfirm, NowMs());
            if (semantic == gesture::Semantic::kNone) {
                return;
            }
            if (Application::GetInstance().GetRawDrawUiManager() == nullptr) {
                FactoryTestService::Instance().HandleButton(FactoryTestButton::kConfirmLongPress);
                return;
            }
            DispatchGesture(semantic);
        });
    }

    void BindFactoryTestCallbacks() {
        auto& factory_test = FactoryTestService::Instance();
        factory_test.SetSnapshotCallback([this](const FactoryTestSnapshot& snapshot) {
            if (display_ == nullptr) {
                return;
            }

            auto* page = display_->GetFactoryTestPageAdapter();
            if (page == nullptr) {
                return;
            }

            DisplayLockGuard lock(display_);
            page->UpdateSnapshot(snapshot);
            display_->RequestUrgentRefresh();
        });

        factory_test.SetShutdownCallback([this]() {
            if (power_ != nullptr) {
                power_->VbatPowerOff();
            }
        });
    }

    /**
     * @brief Install the ADC calls battery::Monitor drives, without making any.
     *
     * Nothing here touches the converter: the Monitor opens it once, on the
     * first reading, under its own lock. Every hook below runs under that same
     * lock, which is what makes `adc_handle_` and `cali_handle_` safe to be
     * plain members read from the UI task and the HTTP task alike.
     *
     * Note what is *not* here: `ESP_ERROR_CHECK`. This path is reachable from
     * an HTTP request handler, and a converter that is momentarily busy is a
     * battery this device cannot report right now — not a reason to panic the
     * firmware. Failures come back as `false` and the status route renders
     * null, which is what its contract already says an unknown battery is.
     */
    void InitializeBatteryMonitor() {
        battery::Hooks hooks;

        hooks.open = [this]() -> battery::OpenResult {
            battery::OpenResult result;

            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "battery ADC unit init failed: %s", esp_err_to_name(err));
                adc_handle_ = nullptr;
                return result;
            }

            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            err = adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_3, &ch_config);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "battery ADC channel config failed: %s", esp_err_to_name(err));
                // Hand the unit straight back. Leaving it registered is what
                // made the old code unable to ever succeed on a retry.
                adc_oneshot_del_unit(adc_handle_);
                adc_handle_ = nullptr;
                return result;
            }
            result.unit_ready = true;

            // The curve-fitting scheme only exists if this particular chip was
            // factory calibrated. Without it the driver has counts and no
            // volts, and the Monitor reports null rather than inventing a
            // percentage from them — but the unit stays open and claimed, so
            // nothing tries to register it a second time.
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = ADC_CHANNEL_3,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle_) == ESP_OK) {
                result.calibrated = true;
            } else {
                ESP_LOGW(kTag, "battery ADC has no factory calibration; reporting null");
                cali_handle_ = nullptr;
            }
            return result;
        };

        hooks.read_mv = [this](uint16_t* mv_out) -> bool {
            if (adc_handle_ == nullptr || cali_handle_ == nullptr || mv_out == nullptr) {
                return false;
            }
            int raw_value = 0;
            if (adc_oneshot_read(adc_handle_, ADC_CHANNEL_3, &raw_value) != ESP_OK) {
                return false;
            }
            int raw_voltage = 0;
            if (adc_cali_raw_to_voltage(cali_handle_, raw_value, &raw_voltage) != ESP_OK) {
                return false;
            }
            // The cell sits behind a two-to-one divider.
            *mv_out = static_cast<uint16_t>(raw_voltage * 2);
            return true;
        };

        hooks.close = [this]() {
            if (cali_handle_ != nullptr) {
                adc_cali_delete_scheme_curve_fitting(cali_handle_);
                cali_handle_ = nullptr;
            }
            if (adc_handle_ != nullptr) {
                adc_oneshot_del_unit(adc_handle_);
                adc_handle_ = nullptr;
            }
        };

        hooks.now_ms = []() { return GetNowMs(); };

        battery_.Begin(std::move(hooks));
    }

    EspNetwork network_;
    NetworkEventCallback network_event_callback_;
    bool network_started_ = false;
    CustomLcdDisplay* display_ = nullptr;
    std::unique_ptr<BoardPowerBsp> power_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    std::unique_ptr<RtcPcf8563> rtc_;
    std::unique_ptr<ZectrixNfc> nfc_;
    ChargeStatus charge_status_;
    /// Sole owner of the battery ADC. Opens it once, serialises every reader,
    /// rate-limits the converter, and releases it in ~CustomBoard. See
    /// common/battery_monitor.h for the three defects this replaced.
    battery::Monitor battery_;
    /// Touched only inside the hooks installed in InitializeBatteryMonitor(),
    /// which battery::Monitor calls under its own lock. Not atomic, and not
    /// static, on purpose: the lock is what makes them safe, and the Monitor's
    /// lifetime is what says when they may be released.
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t cali_handle_ = nullptr;
    Button up_button_;
    Button down_button_;
    Button confirm_button_;
};

}  // namespace

DECLARE_BOARD(CustomBoard);

extern "C" void BoardOnNetworkConnected() {
}

extern "C" void BoardOnNetworkDisconnected() {
}

extern "C" RtcPcf8563* ZectrixGetRtc() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetRtc();
}

extern "C" ChargeStatus::Snapshot ZectrixGetChargeSnapshot() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetChargeSnapshot();
}

/**
 * @brief The battery reading the status route needs, calibration flag included.
 *
 * Board::GetBatteryLevel already returns a percentage, but not whether the
 * conversion behind it was calibrated, and that is exactly the fact that
 * decides whether the number may be shown to a human. Rather than widen the
 * cross-board Board interface for one board's ADC, this follows the pattern
 * the rest of this file already uses for RTC and charge state: a narrow
 * extern "C" accessor the application declares where it needs it.
 */
extern "C" bool ZectrixReadBatteryMillivolts(uint16_t* mv_out, bool* calibrated_out) {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.ReadBatteryMillivolts(mv_out, calibrated_out);
}

extern "C" ChargeStatus::Snapshot ZectrixRefreshChargeSnapshotForFactoryTest() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.RefreshChargeSnapshotForFactoryTest();
}

extern "C" bool ZectrixReadBatteryPercentForFactoryTest(int* level) {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.ReadBatteryPercentForFactoryTest(level);
}

extern "C" void ZectrixSetFactoryLedOverride(bool enabled, bool blink) {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    board.SetFactoryLedOverride(enabled, blink);
}

extern "C" ZectrixNfc* ZectrixGetNfc() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetNfc();
}
