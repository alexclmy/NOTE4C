/**
 * @file ble_image_receiver.h
 * @brief Image chunk receiver and reassembly for BLE transfer
 *
 * 400x300 1bpp image = 15000 bytes
 * BLE MTU ~247 bytes -> ~61 chunks
 */

#ifndef BLE_IMAGE_RECEIVER_H
#define BLE_IMAGE_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>

namespace ble_image_receiver {

// Maximum image size (400x300 1bpp)
constexpr uint16_t kMaxImageSize = 15000;  // 400 * 300 / 8 = 15000 bytes

// Transfer status (mirrors ble_gatt_service::ImageTransferStatus)
enum Status : uint8_t {
    kStatusIdle = 0x00,
    kStatusReceiving = 0x01,
    kStatusComplete = 0x02,
    kStatusError = 0xFF,
};

/**
 * @brief Initialize receiver (allocate buffer)
 */
bool Init();

/**
 * @brief Start a new image transfer
 * @param total_size Expected total size in bytes
 */
void StartTransfer(uint16_t total_size);

/**
 * @brief Receive a chunk of image data
 * @param data Chunk data
 * @param len Chunk length
 */
void ReceiveChunk(const uint8_t* data, uint16_t len);

/**
 * @brief Reset receiver state (cancel transfer)
 */
void Reset();

/**
 * @brief Check if transfer is complete
 */
bool IsComplete();

/**
 * @brief Get current status
 */
Status GetStatus();

/**
 * @brief Get number of bytes received so far
 */
uint16_t GetReceivedBytes();

/**
 * @brief Get expected total size
 */
uint16_t GetExpectedSize();

/**
 * @brief Get pointer to received image data (1bpp format)
 */
const uint8_t* GetData();

/**
 * @brief Get the internal buffer (for direct rendering)
 */
uint8_t* GetBuffer();

}  // namespace ble_image_receiver

#endif  // BLE_IMAGE_RECEIVER_H