/**
 * @file photo_downloader.cc
 * @brief HTTP photo downloader implementation
 *
 * Uses esp_http_client for HTTP requests.
 * JSON parsing with cJSON for the photo list response.
 *
 * IMPORTANT: Uses config.timeout_ms (NOT setsockopt) for timeouts.
 */

#include "photo_downloader.h"
#include "photo_storage.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* kTag = "PhotoDL";

// ============================================================
// Static state
// ============================================================

static char s_server_url[PHOTO_DOWNLOADER_URL_MAX] = {0};
static bool s_initialized = false;
static bool s_syncing = false;

// HTTP response buffer (reused across requests)
static char s_http_buf[4096];
static int s_http_len = 0;

// Static buffer for photo download (400x300 1bpp = 15000 bytes)
#define PHOTO_BUF_SIZE (400 * 300 / 8)  // 15000 bytes
static uint8_t s_photo_buf[PHOTO_BUF_SIZE];

// ============================================================
// HTTP event handler (collects response data)
// ============================================================

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (s_http_len + evt->data_len < sizeof(s_http_buf)) {
                memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
                s_http_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ============================================================
// Helper: HTTP GET returning response length or -1
// ============================================================

static int http_get(const char* url, int timeout_ms) {
    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = http_event_handler;
    config.timeout_ms = timeout_ms;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "Failed to init HTTP client for %s", url);
        return -1;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "HTTP GET %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status %d for %s", status, url);
        esp_http_client_cleanup(client);
        return -1;
    }

    int len = s_http_len;
    s_http_buf[s_http_len] = '\0';
    esp_http_client_cleanup(client);
    return len;
}

// ============================================================
// Helper: HTTP POST (no body, just confirm download)
// ============================================================

static int http_post(const char* url) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "POST %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return (status == 200) ? 0 : -1;
}

// ============================================================
// Parse server photo list JSON
// Returns number of photos parsed, or -1 on error
// ============================================================

// Server photo entry (temporary, for sync comparison)
typedef struct {
    char id[16];
    char title[64];
    uint16_t width;
    uint16_t height;
    uint32_t file_size;
    uint32_t timestamp;
} ServerPhotoEntry;

static int parse_photo_list(const char* json, ServerPhotoEntry* entries, int max_entries) {
    if (!json || !entries) return -1;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(kTag, "Failed to parse photo list JSON");
        return -1;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGE(kTag, "Expected JSON array for photo list");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(root);
    if (count > max_entries) count = max_entries;

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!item) continue;

        cJSON* j_id = cJSON_GetObjectItem(item, "id");
        cJSON* j_title = cJSON_GetObjectItem(item, "title");
        cJSON* j_width = cJSON_GetObjectItem(item, "width");
        cJSON* j_height = cJSON_GetObjectItem(item, "height");
        cJSON* j_size = cJSON_GetObjectItem(item, "size");
        cJSON* j_ts = cJSON_GetObjectItem(item, "ts");

        if (j_id && j_id->valuestring) {
            strncpy(entries[i].id, j_id->valuestring, sizeof(entries[i].id) - 1);
        }
        if (j_title && j_title->valuestring) {
            strncpy(entries[i].title, j_title->valuestring, sizeof(entries[i].title) - 1);
        }
        if (j_width) entries[i].width = (uint16_t)j_width->valueint;
        if (j_height) entries[i].height = (uint16_t)j_height->valueint;
        if (j_size) entries[i].file_size = (uint32_t)j_size->valueint;
        if (j_ts) entries[i].timestamp = (uint32_t)j_ts->valueint;
    }

    cJSON_Delete(root);
    return count;
}

// ============================================================
// Download a single photo binary data
// ============================================================

static int download_photo_binary(const char* photo_id, uint8_t* out_buf, uint32_t max_size) {
    char url[512];
    snprintf(url, sizeof(url), "%s/api/photos/%s.bin", s_server_url, photo_id);

    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 30000;  // 30s for large downloads

    // Custom handler that writes directly to our buffer
    int written = 0;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }

    // We need a custom event handler for binary data
    // Reuse s_http_buf as intermediate, then copy
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Download %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(kTag, "Download HTTP status %d", status);
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_get_content_length(client);
    if (content_len < 0 || (uint32_t)content_len > max_size) {
        ESP_LOGE(kTag, "Photo size invalid: %d", content_len);
        esp_http_client_cleanup(client);
        return -1;
    }

    // Read response into buffer using the accumulated s_http_buf
    if (s_http_len > 0 && s_http_len <= (int)max_size) {
        memcpy(out_buf, s_http_buf, s_http_len);
        written = s_http_len;
    }

    esp_http_client_cleanup(client);
    return written;
}

// ============================================================
// Public API
// ============================================================

int photo_downloader_init(const PhotoDownloaderConfig* cfg) {
    if (!cfg || !cfg->server_url[0]) {
        ESP_LOGE(kTag, "Invalid config");
        return -1;
    }

    strncpy(s_server_url, cfg->server_url, sizeof(s_server_url) - 1);
    s_initialized = true;
    s_syncing = false;

    ESP_LOGI(kTag, "Photo downloader initialized: %s", s_server_url);
    return 0;
}

int photo_sync(void) {
    if (!s_initialized) {
        ESP_LOGE(kTag, "Not initialized");
        return -1;
    }

    if (s_syncing) {
        ESP_LOGW(kTag, "Sync already in progress");
        return -1;
    }

    s_syncing = true;
    int downloaded = 0;

    // Fetch photo list from server
    char url[512];
    snprintf(url, sizeof(url), "%s/api/photos", s_server_url);
    ESP_LOGI(kTag, "Fetching photo list from %s", url);

    int resp_len = http_get(url, 10000);
    if (resp_len < 0) {
        ESP_LOGE(kTag, "Failed to fetch photo list");
        s_syncing = false;
        return -1;
    }

    ESP_LOGI(kTag, "Server returned %d bytes", resp_len);

    // Parse server list
    #define MAX_SERVER_PHOTOS PHOTO_MAX_PHOTOS
    ServerPhotoEntry server_entries[MAX_SERVER_PHOTOS];
    int server_count = parse_photo_list(s_http_buf, server_entries, MAX_SERVER_PHOTOS);
    if (server_count < 0) {
        ESP_LOGE(kTag, "Failed to parse server photo list");
        s_syncing = false;
        return -1;
    }

    ESP_LOGI(kTag, "Server has %d photos", server_count);

    // Compare with local and download new ones
    for (int i = 0; i < server_count; i++) {
        if (photo_exists(server_entries[i].id)) {
            ESP_LOGD(kTag, "Already have photo %s, skipping", server_entries[i].id);
            continue;
        }

        ESP_LOGI(kTag, "Downloading new photo %s: %s (%dx%d, %d bytes)",
                 server_entries[i].id, server_entries[i].title,
                 server_entries[i].width, server_entries[i].height,
                 server_entries[i].file_size);

        // Download binary data
        int bytes = download_photo_binary(server_entries[i].id, s_photo_buf, sizeof(s_photo_buf));
        if (bytes <= 0) {
            ESP_LOGE(kTag, "Failed to download %s", server_entries[i].id);
            continue;
        }

        // Save to SPIFFS
        PhotoInfo info;
        memset(&info, 0, sizeof(info));
        strncpy(info.id, server_entries[i].id, sizeof(info.id) - 1);
        strncpy(info.title, server_entries[i].title, sizeof(info.title) - 1);
        info.width = server_entries[i].width;
        info.height = server_entries[i].height;
        info.file_size = (uint32_t)bytes;
        info.timestamp = server_entries[i].timestamp;

        if (photo_save(&info, s_photo_buf) == 0) {
            downloaded++;

            // Confirm download to server
            char confirm_url[8192];
            snprintf(confirm_url, sizeof(confirm_url),
                     "%s/api/photos/%s/downloaded", s_server_url, server_entries[i].id);
            http_post(confirm_url);
        } else {
            ESP_LOGE(kTag, "Failed to save photo %s to SPIFFS", server_entries[i].id);
        }
    }

    s_syncing = false;
    ESP_LOGI(kTag, "Sync complete: %d new photos downloaded (total: %d)",
             downloaded, photo_get_count());
    return downloaded;
}

int photo_download_single(const char* photo_id) {
    if (!s_initialized || !photo_id) {
        return -1;
    }

    if (photo_exists(photo_id)) {
        ESP_LOGI(kTag, "Photo %s already exists locally", photo_id);
        return 0;
    }

    ESP_LOGI(kTag, "Downloading single photo: %s", photo_id);

    int bytes = download_photo_binary(photo_id, s_photo_buf, sizeof(s_photo_buf));
    if (bytes <= 0) {
        return -1;
    }

    // We need metadata - fetch from server list to get it
    char url[256];
    snprintf(url, sizeof(url), "%s/api/photos", s_server_url);
    int resp_len = http_get(url, 10000);
    if (resp_len < 0) {
        return -1;
    }

    #define MAX_SERVER_PHOTOS_SINGLE 1
    ServerPhotoEntry entry;
    int count = parse_photo_list(s_http_buf, &entry, MAX_SERVER_PHOTOS_SINGLE);
    (void)count;

    PhotoInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.id, photo_id, sizeof(info.id) - 1);
    snprintf(info.title, sizeof(info.title), "%s", photo_id);  // fallback title
    info.width = 400;
    info.height = 300;
    info.file_size = (uint32_t)bytes;
    info.timestamp = 0;

    return photo_save(&info, s_photo_buf);
}

bool photo_downloader_is_ready(void) {
    return s_initialized;
}

bool photo_downloader_is_syncing(void) {
    return s_syncing;
}
