/**
 * @file bluetooth_manager.cc
 * @brief BLE initialization and toggle control for ESP32-S3
 *
 * Minimal BLE support: init controller, start/stop advertising.
 * GATT services added for phone image push (ble_gatt_service.cc).
 */

#include "bluetooth_manager.h"
#include "ble_gatt_service.h"
#include "ble_image_receiver.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>

namespace {
constexpr char kTag[] = "BTManager";
static bool ble_enabled_ = false;
static bool ble_initialized_ = false;
static bool advertising_ = false;
static esp_ble_adv_params_t adv_params_ = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

void GapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
            if (param->adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(kTag, "Raw adv data config failed: %d", param->adv_data_raw_cmpl.status);
                break;
            }
            esp_err_t ret = esp_ble_gap_start_advertising(&adv_params_);
            if (ret != ESP_OK) {
                ESP_LOGE(kTag, "Start advertising failed: %s", esp_err_to_name(ret));
            }
            break;
        }
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(kTag, "Adv start failed: %d", param->adv_start_cmpl.status);
            } else {
                ble_enabled_ = true;
                advertising_ = true;
                ESP_LOGI(kTag, "BLE advertising started");
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(kTag, "Adv stop failed: %d", param->adv_stop_cmpl.status);
            } else {
                advertising_ = false;
                ESP_LOGI(kTag, "BLE advertising stopped");
            }
            break;
        default:
            break;
    }
}

}  // namespace

namespace bluetooth_manager {

bool Init() {
    ESP_LOGI(kTag, "Initializing BLE");
    esp_err_t ret;

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "BT controller init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Enable BLE mode (disable Classic BT)
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "BT controller enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize Bluedroid stack
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Register GAP event handler
    ret = esp_ble_gap_register_callback(GapEventHandler);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "GAP register failed: %s", esp_err_to_name(ret));
        return false;
    }

    ble_initialized_ = true;
    ESP_LOGI(kTag, "BLE initialized successfully");

    // Initialize GATT service for image push
    if (!ble_image_receiver::Init()) {
        ESP_LOGE(kTag, "BLE image receiver init failed");
    }
    if (!ble_gatt_service::Init()) {
        ESP_LOGE(kTag, "BLE GATT service init failed");
    }

    return true;
}

bool Enable() {
    if (!ble_initialized_) {
        ESP_LOGW(kTag, "BLE not initialized, attempting init");
        if (!Init()) return false;
    }

    if (ble_enabled_) {
        ESP_LOGW(kTag, "BLE already enabled");
        return true;
    }

    // Set minimal advertising data (device name)
    const char* dev_name = "InkScreen";
    esp_ble_gap_set_device_name(dev_name);

    // Raw advertising data: Flags + 0xF000 service UUID + Complete Local Name.
    // The AD length includes the type byte; "InkScreen" is 9 bytes, so the
    // Complete Local Name field length must be 10.
    uint8_t adv_data[] = {
        0x02, 0x01, 0x06,  // Flags: LE General Discoverable, BR/EDR Not Supported
        0x03, 0x03, 0x00, 0xF0,  // Complete List of 16-bit Service UUIDs: 0xF000
        0x0A, 0x09, 'I', 'n', 'k', 'S', 'c', 'r', 'e', 'e', 'n',  // Complete Name
    };
    esp_err_t ret = esp_ble_gap_config_adv_data_raw(adv_data, sizeof(adv_data));
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Config adv data failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(kTag, "BLE adv data configured, waiting to advertise as '%s'", dev_name);
    return true;
}

bool RestartAdvertising() {
    if (!ble_initialized_ || !ble_enabled_) {
        ESP_LOGI(kTag, "Skip BLE advertising restart: initialized=%d enabled=%d",
                 ble_initialized_ ? 1 : 0,
                 ble_enabled_ ? 1 : 0);
        return true;
    }

    ESP_LOGI(kTag, "Restarting BLE advertising after disconnect: advertising=%d",
             advertising_ ? 1 : 0);
    advertising_ = false;
    esp_err_t ret = esp_ble_gap_start_advertising(&adv_params_);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Restart advertising failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(kTag, "BLE advertising restart requested");
    return true;
}

bool Disable() {
    if (!ble_enabled_) {
        ESP_LOGW(kTag, "BLE already disabled");
        return true;
    }

    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Stop advertising failed: %s", esp_err_to_name(ret));
    }

    ble_enabled_ = false;
    advertising_ = false;
    ESP_LOGI(kTag, "BLE disabled");
    return true;
}

bool IsEnabled() {
    return ble_enabled_;
}

}  // namespace bluetooth_manager
