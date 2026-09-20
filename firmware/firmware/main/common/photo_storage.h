/**
 * @file photo_storage.h
 * @brief SPIFFS-based photo storage for 1bpp images
 *
 * Stores 1bpp image data as .bin files and metadata as .json files
 * in the SPIFFS "assets" partition. Maintains a photos.idx index file.
 *
 * Image format: Raw 1bpp data, no header. Width always 400 for ESP32 display.
 * Bytes per row = 400/8 = 50. Total size = 50 * height.
 */

#ifndef PHOTO_STORAGE_H
#define PHOTO_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <string>

#define PHOTO_MAX_PHOTOS 50
#define PHOTO_MAX_PATH 64
#define PHOTO_TITLE_LEN 64
#define PHOTO_DATE_LEN 24
#define PHOTO_LOCATION_LEN 64
#define PHOTO_BODY_LEN 256

/**
 * @brief Photo metadata
 */
struct PhotoInfo {
    char id[16];                         // Unique ID (from server)
    char title[PHOTO_TITLE_LEN];         // Short headline
    char date[PHOTO_DATE_LEN];           // ISO date or display date
    char location[PHOTO_LOCATION_LEN];   // Photo location
    char body[PHOTO_BODY_LEN];           // Narrative body text
    uint16_t width;                      // Image width in pixels
    uint16_t height;                     // Image height in pixels
    uint32_t file_size;                  // 1bpp data size in bytes
    uint32_t timestamp;                  // Upload timestamp (epoch seconds)
    char path[PHOTO_MAX_PATH];           // SPIFFS path to .bin file
};

/**
 * @brief Initialize photo storage
 *
 * Mounts SPIFFS, loads the index file.
 * Call once during app startup.
 *
 * @return 0 on success, -1 on failure
 */
int photo_storage_init(void);

/**
 * @brief Save a photo to SPIFFS
 *
 * Writes .bin file (raw 1bpp data) and .meta JSON file.
 * Updates the index.
 *
 * @param info Photo metadata
 * @param data_1bpp Pointer to 1bpp image data
 * @return 0 on success, -1 on failure
 */
int photo_save(const PhotoInfo *info, const uint8_t *data_1bpp);

/**
 * @brief Load a photo's 1bpp data into buffer
 *
 * @param id Photo ID
 * @param out_buffer Output buffer (caller-allocated, must be >= file_size)
 * @param max_size Size of out_buffer
 * @return Bytes read on success, -1 on failure
 */
int photo_load(const char *id, uint8_t *out_buffer, uint32_t max_size);

/**
 * @brief List all photos
 *
 * @param out_list Output array (caller-allocated)
 * @param max_count Maximum entries to return
 * @return Number of photos listed
 */
int photo_list(PhotoInfo *out_list, int max_count);

/**
 * @brief Delete a photo by ID
 *
 * Removes .bin, .meta, and updates index.
 *
 * @param id Photo ID
 * @return 0 on success, -1 if not found
 */
int photo_delete(const char *id);

/**
 * @brief Get total photo count
 */
int photo_get_count(void);

/**
 * @brief Get photo info by index (0-based, in timestamp order newest-first)
 *
 * @param index Index into photo list
 * @param out Output PhotoInfo
 * @return 0 on success, -1 if index out of range
 */
int photo_get_by_index(int index, PhotoInfo *out);

/**
 * @brief Check if a photo exists by ID
 */
bool photo_exists(const char *id);

/**
 * @brief Update editable photo metadata fields by ID
 */
int photo_update_info(const char *id, const PhotoInfo *updates);

/**
 * @brief Move a photo in the display order by delta (-1 up, +1 down)
 */
int photo_move(const char *id, int delta);

#endif  // PHOTO_STORAGE_H
