/**
 * @file ble_image_receiver.cc
 * @brief BLE image chunk receiver and reassembly
 */

#include "ble_image_receiver.h"
#include <esp_log.h>
#include <cstring>
#include <esp_heap_caps.h>

namespace {
constexpr char kTag[] = "BLE_Image";

// Buffer for received image data (allocated in PSRAM if available)
static uint8_t* image_buffer = nullptr;
static uint16_t expected_size = 0;
static uint16_t received_size = 0;
static ble_image_receiver::Status status = ble_image_receiver::kStatusIdle;
static bool initialized = false;

}  // namespace

namespace ble_image_receiver {

bool Init() {
    if (initialized) return true;
    
    // Allocate buffer (prefer PSRAM for large buffer)
    size_t alloc_size = kMaxImageSize;
    image_buffer = (uint8_t*)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (image_buffer == nullptr) {
        // Fallback to internal RAM
        image_buffer = (uint8_t*)malloc(alloc_size);
        ESP_LOGW(kTag, "Allocated image buffer in internal RAM (PSRAM unavailable)");
    }
    
    if (image_buffer == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate image buffer (%d bytes)", alloc_size);
        return false;
    }
    
    ESP_LOGI(kTag, "Image buffer allocated: %d bytes", alloc_size);
    initialized = true;
    return true;
}

void StartTransfer(uint16_t total_size) {
    if (!initialized) {
        ESP_LOGE(kTag, "Not initialized");
        status = kStatusError;
        return;
    }
    
    if (total_size > kMaxImageSize) {
        ESP_LOGE(kTag, "Image too large: %d > %d", total_size, kMaxImageSize);
        status = kStatusError;
        return;
    }
    
    expected_size = total_size;
    received_size = 0;
    memset(image_buffer, 0xFF, total_size);  // Clear to white (1bpp)
    status = kStatusReceiving;
    
    ESP_LOGI(kTag, "Transfer started: expecting %d bytes", total_size);
}

void ReceiveChunk(const uint8_t* data, uint16_t len) {
    if (status != kStatusReceiving) {
        ESP_LOGW(kTag, "Not in receiving state, ignoring chunk");
        return;
    }
    
    if (received_size + len > expected_size) {
        ESP_LOGE(kTag, "Chunk overflow: %d + %d > %d", 
                 received_size, len, expected_size);
        status = kStatusError;
        return;
    }
    
    memcpy(image_buffer + received_size, data, len);
    received_size += len;
    
    ESP_LOGD(kTag, "Chunk received: %d bytes, total %d/%d", 
             len, received_size, expected_size);
    
    // Check if complete
    if (received_size >= expected_size) {
        status = kStatusComplete;
        ESP_LOGI(kTag, "Transfer complete: %d bytes received", received_size);
    }
}

void Reset() {
    expected_size = 0;
    received_size = 0;
    status = kStatusIdle;
    ESP_LOGI(kTag, "Receiver reset");
}

bool IsComplete() {
    return status == kStatusComplete && received_size == expected_size;
}

Status GetStatus() {
    return status;
}

uint16_t GetReceivedBytes() {
    return received_size;
}

uint16_t GetExpectedSize() {
    return expected_size;
}

const uint8_t* GetData() {
    return image_buffer;
}

uint8_t* GetBuffer() {
    return image_buffer;
}

}  // namespace ble_image_receiver