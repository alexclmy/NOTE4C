/**
 * @file photo_downloader.h
 * @brief HTTP photo downloader from server to SPIFFS
 *
 * Syncs photo list from server and downloads new photos.
 * Uses esp_http_client with select()-based timeout (NO setsockopt).
 *
 * Server API:
 *   GET /api/photos              -> JSON list of photos
 *   GET /api/photos/{id}.bin     -> 1bpp raw data
 *   POST /api/photos/{id}/downloaded -> confirm download
 */

#ifndef PHOTO_DOWNLOADER_H
#define PHOTO_DOWNLOADER_H

#include <stdint.h>
#include <stdbool.h>

#define PHOTO_DOWNLOADER_URL_MAX 128

/**
 * @brief Photo downloader configuration
 */
typedef struct {
    char server_url[PHOTO_DOWNLOADER_URL_MAX];  // e.g., "http://<host-ip>:8080"
} PhotoDownloaderConfig;

/**
 * @brief Initialize the photo downloader
 *
 * @param cfg Configuration with server URL
 * @return 0 on success, -1 on failure
 */
int photo_downloader_init(const PhotoDownloaderConfig *cfg);

/**
 * @brief Sync photos from server
 *
 * Fetches photo list from server, compares with local index,
 * downloads only new photos.
 *
 * @return Number of new photos downloaded, -1 on error
 */
int photo_sync(void);

/**
 * @brief Download a single photo by ID
 *
 * @param photo_id Photo ID from server
 * @return 0 on success, -1 on failure
 */
int photo_download_single(const char *photo_id);

/**
 * @brief Check if downloader is initialized
 */
bool photo_downloader_is_ready(void);

/**
 * @brief Check if a sync is in progress
 */
bool photo_downloader_is_syncing(void);

#endif  // PHOTO_DOWNLOADER_H
