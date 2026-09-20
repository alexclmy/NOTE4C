/**
 * @file ble_gatt_service.cc
 * @brief BLE GATT service implementation for image push
 */

#include "ble_gatt_service.h"
#include "ble_image_receiver.h"
#include "bluetooth_manager.h"
#include <esp_log.h>
#include <esp_gatts_api.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_main.h>
#include <esp_gatt_common_api.h>
#include <cJSON.h>
#include <string.h>
#include <string>

namespace {
constexpr char kTag[] = "BLE_GATT";

// GATT service handle storage
static uint16_t service_handle = 0;
static uint16_t char_image_data_handle = 0;
static uint16_t char_image_control_handle = 0;
static uint16_t char_device_info_handle = 0;
static esp_gatt_if_t gatts_if = 0;
static bool service_ready = false;
static uint16_t conn_id = 0;
static uint16_t local_mtu = 23;  // Default BLE MTU

// Callback for image ready
ble_gatt_service::ImageReadyCallback image_ready_cb = nullptr;

// Device info to report
ble_gatt_service::DeviceInfo device_info = {};

// Forward declaration
void HandleControlCommand(const uint8_t* data, uint16_t len);
void HandleJsonControlCommand(const uint8_t* data, uint16_t len);

void GattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if_param, 
                       esp_ble_gatts_cb_param_t* param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            // Service registered, now add attributes
            ESP_LOGI(kTag, "GATTS REG_EVT, status=%d, app_id=%d", 
                     param->reg.status, param->reg.app_id);
            if (param->reg.status == ESP_GATT_OK) {
                gatts_if = gatts_if_param;
                // Create service
                esp_gatt_srvc_id_t service_id = {
                    .id = {
                        .uuid = {
                            .len = ESP_UUID_LEN_16,
                            .uuid = { .uuid16 = ble_gatt_service::kServiceUuid }
                        },
                        .inst_id = 0,
                    },
                    .is_primary = true,
                };
                esp_ble_gatts_create_service(gatts_if_param, &service_id, 7);  // 7 handles
            }
            break;

        case ESP_GATTS_CREATE_EVT: {
            // Service created
            ESP_LOGI(kTag, "CREATE_EVT, service_handle=%d", param->create.service_handle);
            service_handle = param->create.service_handle;
            
            // Add Image Data characteristic (Write only)
            esp_bt_uuid_t char_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = { .uuid16 = ble_gatt_service::kCharUuidImageData }
            };
            esp_gatt_perm_t perm = ESP_GATT_PERM_WRITE;
            esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_WRITE;
            esp_attr_value_t val = {
                .attr_max_len = 512,  // Max chunk size (MTU negotiated)
                .attr_len = 0,
                .attr_value = nullptr,
            };
            esp_ble_gatts_add_char(service_handle, &char_uuid, perm, prop, &val, nullptr);
            break;
        }

        case ESP_GATTS_ADD_CHAR_EVT: {
            // Characteristic added
            ESP_LOGI(kTag, "ADD_CHAR_EVT, attr_handle=%d, uuid=0x%04x", 
                     param->add_char.attr_handle, param->add_char.char_uuid.uuid.uuid16);
            
            if (param->add_char.char_uuid.uuid.uuid16 == ble_gatt_service::kCharUuidImageData) {
                char_image_data_handle = param->add_char.attr_handle;
                
                // Add Image Control characteristic (Write/Read)
                esp_bt_uuid_t ctrl_uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = { .uuid16 = ble_gatt_service::kCharUuidImageControl }
                };
                esp_ble_gatts_add_char(service_handle, &ctrl_uuid, 
                                       ESP_GATT_PERM_WRITE | ESP_GATT_PERM_READ,
                                       ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_READ,
                                       nullptr, nullptr);
            } else if (param->add_char.char_uuid.uuid.uuid16 == ble_gatt_service::kCharUuidImageControl) {
                char_image_control_handle = param->add_char.attr_handle;
                
                // Add Device Info characteristic (Read only)
                esp_bt_uuid_t info_uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = { .uuid16 = ble_gatt_service::kCharUuidDeviceInfo }
                };
                esp_ble_gatts_add_char(service_handle, &info_uuid,
                                       ESP_GATT_PERM_READ,
                                       ESP_GATT_CHAR_PROP_BIT_READ,
                                       nullptr, nullptr);
            } else if (param->add_char.char_uuid.uuid.uuid16 == ble_gatt_service::kCharUuidDeviceInfo) {
                char_device_info_handle = param->add_char.attr_handle;
                
                // Start service
                esp_ble_gatts_start_service(service_handle);
            }
            break;
        }

        case ESP_GATTS_START_EVT:
            // Service started, ready for connections
            ESP_LOGI(kTag, "START_EVT, service_handle=%d, status=%d", 
                     param->start.service_handle, param->start.status);
            service_ready = true;
            break;

        case ESP_GATTS_CONNECT_EVT:
            // Device connected
            ESP_LOGI(kTag, "CONNECT_EVT, conn_id=%d", param->connect.conn_id);
            conn_id = param->connect.conn_id;
            // Request larger MTU for faster transfer
            esp_ble_gatt_set_local_mtu(247);  // Request 247 bytes MTU
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            // Device disconnected
            ESP_LOGI(kTag, "DISCONNECT_EVT, conn_id=%d", param->disconnect.conn_id);
            conn_id = 0;
            local_mtu = 23;
            ble_image_receiver::Reset();  // Cancel any pending transfer
            bluetooth_manager::RestartAdvertising();
            break;

        case ESP_GATTS_MTU_EVT:
            // MTU negotiated
            ESP_LOGI(kTag, "MTU_EVT, mtu=%d", param->mtu.mtu);
            local_mtu = param->mtu.mtu;
            break;

        case ESP_GATTS_WRITE_EVT:
            // Data written to characteristic
            if (param->write.handle == char_image_data_handle) {
                // Image data chunk received
                ESP_LOGD(kTag, "ImageData write, len=%d", param->write.len);
                ble_image_receiver::ReceiveChunk(param->write.value, param->write.len);
            } else if (param->write.handle == char_image_control_handle) {
                // Control command received
                ESP_LOGI(kTag, "ImageControl write, len=%d, cmd=%d", 
                         param->write.len, param->write.value[0]);
                HandleControlCommand(param->write.value, param->write.len);
            }
            // Send response if needed
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, conn_id, param->write.trans_id, 
                                           ESP_GATT_OK, nullptr);
            }
            break;

        case ESP_GATTS_READ_EVT:
            // Read request
            if (param->read.handle == char_image_control_handle) {
                // Return transfer status
                uint8_t status[4] = {
                    ble_image_receiver::GetStatus(),
                    (uint8_t)(ble_image_receiver::GetReceivedBytes() >> 8),
                    (uint8_t)(ble_image_receiver::GetReceivedBytes() & 0xFF),
                    (uint8_t)(ble_image_receiver::GetExpectedSize() >> 8),
                };
                esp_gatt_rsp_t rsp = {};
                rsp.attr_value.len = 4;
                memcpy(rsp.attr_value.value, status, 4);
                esp_ble_gatts_send_response(gatts_if, conn_id, param->read.trans_id,
                                           ESP_GATT_OK, &rsp);
            } else if (param->read.handle == char_device_info_handle) {
                // Return device info
                esp_gatt_rsp_t rsp = {};
                rsp.attr_value.len = sizeof(device_info);
                memcpy(rsp.attr_value.value, &device_info, sizeof(device_info));
                esp_ble_gatts_send_response(gatts_if, conn_id, param->read.trans_id,
                                           ESP_GATT_OK, &rsp);
            }
            break;

        default:
            break;
    }
}

void HandleControlCommand(const uint8_t* data, uint16_t len) {
    if (len < 1) return;

    if (data[0] == '{') {
        HandleJsonControlCommand(data, len);
        return;
    }
    
    switch (data[0]) {
        case ble_gatt_service::kCmdStart:
            // Start new transfer: [cmd, size_high, size_low]
            if (len >= 3) {
                uint16_t total_size = (data[1] << 8) | data[2];
                ble_image_receiver::StartTransfer(total_size);
                ESP_LOGI(kTag, "Start transfer, total_size=%d", total_size);
            }
            break;
        
        case ble_gatt_service::kCmdCancel:
            ble_image_receiver::Reset();
            ESP_LOGI(kTag, "Transfer cancelled");
            break;
        
        case ble_gatt_service::kCmdComplete:
            // Transfer complete, trigger display
            if (ble_image_receiver::IsComplete()) {
                ESP_LOGI(kTag, "Transfer complete, triggering display");
                if (image_ready_cb) {
                    image_ready_cb(ble_image_receiver::GetData(), 
                                  ble_image_receiver::GetReceivedBytes());
                } else {
                    ESP_LOGW(kTag, "Image ready callback is not registered");
                }
            } else {
                ESP_LOGW(kTag, "Complete cmd but not all data received");
            }
            ble_image_receiver::Reset();
            break;
        
        default:
            ESP_LOGW(kTag, "Unknown control cmd: %d", data[0]);
            break;
    }
}

void HandleJsonControlCommand(const uint8_t* data, uint16_t len) {
    std::string json(reinterpret_cast<const char*>(data), len);
    cJSON* root = cJSON_ParseWithLength(json.c_str(), json.size());
    if (!root) {
        ESP_LOGW(kTag, "Invalid JSON control command");
        return;
    }

    cJSON* cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";
    if (strcmp(cmd, "begin") == 0 || strcmp(cmd, "start") == 0) {
        cJSON* size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
        const int size = cJSON_IsNumber(size_item) ? size_item->valueint : 0;
        if (size > 0 && size <= ble_image_receiver::kMaxImageSize) {
            ble_image_receiver::StartTransfer(static_cast<uint16_t>(size));
            ESP_LOGI(kTag, "JSON start transfer, total_size=%d", size);
        } else {
            ESP_LOGW(kTag, "Invalid JSON transfer size: %d", size);
        }
    } else if (strcmp(cmd, "cancel") == 0) {
        ble_image_receiver::Reset();
        ESP_LOGI(kTag, "JSON transfer cancelled");
    } else if (strcmp(cmd, "complete") == 0 ||
               strcmp(cmd, "finish") == 0 ||
               strcmp(cmd, "end") == 0) {
        if (ble_image_receiver::IsComplete()) {
            ESP_LOGI(kTag, "JSON transfer complete, triggering display");
            if (image_ready_cb) {
                image_ready_cb(ble_image_receiver::GetData(),
                               ble_image_receiver::GetReceivedBytes());
            } else {
                ESP_LOGW(kTag, "Image ready callback is not registered");
            }
        } else {
            ESP_LOGW(kTag, "JSON complete cmd but not all data received");
        }
        ble_image_receiver::Reset();
    } else {
        ESP_LOGW(kTag, "Unknown JSON control cmd: %s", cmd);
    }
    cJSON_Delete(root);
}

}  // namespace

namespace ble_gatt_service {

bool Init() {
    ESP_LOGI(kTag, "Registering GATT service");
    
    // Register GATT server callback
    esp_err_t ret = esp_ble_gatts_register_callback(GattsEventHandler);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "gatts register callback failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register GATT server app
    ret = esp_ble_gatts_app_register(0);  // App ID 0
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "gatts app register failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Initialize device info defaults
    device_info.battery_percent = 0;
    device_info.storage_percent = 0;
    device_info.firmware_major = 6;
    device_info.firmware_minor = 7;
    device_info.firmware_patch = 0;
device_info.display_width_high = 400 >> 8;
    device_info.display_width_low = 400 & 0xFF;
    device_info.display_height_high = 300 >> 8;
    device_info.display_height_low = 300 & 0xFF;
    
    ESP_LOGI(kTag, "GATT service init complete, waiting for registration event");
    return true;
}

void SetImageReadyCallback(ImageReadyCallback callback) {
    image_ready_cb = callback;
}

void UpdateDeviceInfo(uint8_t battery, uint8_t storage) {
    device_info.battery_percent = battery;
    device_info.storage_percent = storage;
}

ImageTransferStatus GetStatus() {
    ble_image_receiver::Status recv_status = ble_image_receiver::GetStatus();
    switch (recv_status) {
        case ble_image_receiver::kStatusIdle: return kStatusIdle;
        case ble_image_receiver::kStatusReceiving: return kStatusReceiving;
        case ble_image_receiver::kStatusComplete: return kStatusComplete;
        case ble_image_receiver::kStatusError: return kStatusError;
        default: return kStatusError;
    }
}

uint16_t GetReceivedBytes() {
    return ble_image_receiver::GetReceivedBytes();
}

uint16_t GetExpectedSize() {
    return ble_image_receiver::GetExpectedSize();
}

bool IsServiceReady() {
    return service_ready;
}

}  // namespace ble_gatt_service
