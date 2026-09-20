/**
 * @file ble_gatt_service.h
 * @brief BLE GATT service for image push to ePaper display
 *
 * Service UUID: 0xF000 (custom)
 * Characteristics:
 *   - 0xF001: Image Data (Write) - receive image chunks
 *   - 0xF002: Image Control (Write/Read) - start/stop/status
 *   - 0xF003: Device Info (Read) - battery, storage, firmware version
 */

#ifndef BLE_GATT_SERVICE_H
#define BLE_GATT_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <functional>

namespace ble_gatt_service {

// Service and Characteristic UUIDs (16-bit for efficiency)
constexpr uint16_t kServiceUuid = 0xF000;
constexpr uint16_t kCharUuidImageData = 0xF001;    // Write: receive image data chunks
constexpr uint16_t kCharUuidImageControl = 0xF002;  // Write/Read: control commands
constexpr uint16_t kCharUuidDeviceInfo = 0xF003;    // Read: device status

// Image control commands (written to 0xF002)
enum ImageControlCmd : uint8_t {
    kCmdStart = 0x01,      // Start new image transfer, followed by [size_high, size_low]
    kCmdCancel = 0x02,     // Cancel current transfer
    kCmdComplete = 0x03,   // Mark transfer complete, trigger display
    kCmdQueryStatus = 0x04, // Query current transfer status
};

// Image transfer status (read from 0xF002)
enum ImageTransferStatus : uint8_t {
    kStatusIdle = 0x00,
    kStatusReceiving = 0x01,
    kStatusComplete = 0x02,
    kStatusError = 0xFF,
};

// Device info structure (read from 0xF003)
struct DeviceInfo {
    uint8_t battery_percent;    // 0-100
    uint8_t storage_percent;    // SPIFFS usage 0-100
    uint8_t firmware_major;
    uint8_t firmware_minor;
    uint8_t firmware_patch;
    uint8_t display_width_high;   // 400 >> 8 = 1
    uint8_t display_height_high;  // 300 >> 8 = 1
    uint8_t display_width_low;    // 400 & 0xFF = 144
    uint8_t display_height_low;   // 300 & 0xFF = 44
} __attribute__((packed));

// Callback when image is fully received and ready to display
using ImageReadyCallback = std::function<void(const uint8_t* data, uint16_t size)>;

/**
 * @brief Initialize GATT service (call after bluetooth_manager::Init())
 * @return true if successful
 */
bool Init();

/**
 * @brief Set callback for when image is ready to display
 */
void SetImageReadyCallback(ImageReadyCallback callback);

/**
 * @brief Update device info (battery, storage) for GATT read
 */
void UpdateDeviceInfo(uint8_t battery, uint8_t storage);

/**
 * @brief Get current transfer status
 */
ImageTransferStatus GetStatus();

/**
 * @brief Get number of bytes received so far
 */
uint16_t GetReceivedBytes();

/**
 * @brief Get expected total image size
 */
uint16_t GetExpectedSize();

/**
 * @brief Check if GATT service is registered
 */
bool IsServiceReady();

}  // namespace ble_gatt_service

#endif  // BLE_GATT_SERVICE_H