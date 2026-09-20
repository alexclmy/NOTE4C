/**
 * @file bluetooth_manager.h
 * @brief BLE initialization and toggle control for ESP32-S3
 */

#ifndef BLUETOOTH_MANAGER_H
#define BLUETOOTH_MANAGER_H

#include <stdbool.h>

namespace bluetooth_manager {

/**
 * @brief Initialize BLE subsystem (called once at boot)
 * @return true if BLE initialized successfully
 */
bool Init();

/**
 * @brief Enable BLE (start advertising)
 * @return true if BLE enabled successfully
 */
bool Enable();

/**
 * @brief Restart advertising after a GATT client disconnects
 * @return true if advertising restart was requested or BLE is disabled
 */
bool RestartAdvertising();

/**
 * @brief Disable BLE (stop advertising, deinit controller)
 * @return true if BLE disabled successfully
 */
bool Disable();

/**
 * @brief Check if BLE is currently enabled
 */
bool IsEnabled();

}  // namespace bluetooth_manager

#endif  // BLUETOOTH_MANAGER_H
