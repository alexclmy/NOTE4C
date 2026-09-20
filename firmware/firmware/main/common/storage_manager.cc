/**
 * @file storage_manager.cc
 * @brief SPIFFS storage info query implementation
 */

#include "storage_manager.h"
#include <esp_spiffs.h>
#include <esp_log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace storage_manager {

static const char* kTag = "StorageMgr";
static const char* kSpiffsBasePath = "/spiffs";
static const char* kSpiffsPartitionLabel = "assets";

StorageInfo GetStorageInfo() {
    StorageInfo info = {};
    info.total_bytes = 0;
    info.used_bytes = 0;
    info.free_bytes = 0;
    info.photo_count = 0;
    info.txt_count = 0;

    // Query SPIFFS partition size (ESP-IDF v6.0 uses size_t*)
    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info(kSpiffsPartitionLabel, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "esp_spiffs_info failed: %s", esp_err_to_name(ret));
        // Fallback: try NULL partition label (default)
        ret = esp_spiffs_info(nullptr, &total, &used);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "SPIFFS info unavailable");
            return info;
        }
    }
    info.total_bytes = static_cast<uint32_t>(total);
    info.used_bytes = static_cast<uint32_t>(used);
    info.free_bytes = info.total_bytes - info.used_bytes;

    // Enumerate files
    DIR* dir = opendir(kSpiffsBasePath);
    if (!dir) {
        ESP_LOGW(kTag, "Cannot open %s directory", kSpiffsBasePath);
        return info;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (!name || name[0] == '\0') continue;

        // Skip index/meta files
        if (strstr(name, ".idx") || strstr(name, ".meta") || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char path[280];
        snprintf(path, sizeof(path), "%s/%s", kSpiffsBasePath, name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        // Classify by extension
        const char* ext = strrchr(name, '.');
        if (ext) {
            if (strcmp(ext, ".bin") == 0 || strcmp(ext, ".pbm") == 0 || strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0) {
                info.photo_count++;
            } else if (strcmp(ext, ".txt") == 0) {
                info.txt_count++;
            }
        }
    }
    closedir(dir);

    ESP_LOGI(kTag, "Storage: %u KB total, %u KB used, %u photos, %u txts",
             info.total_bytes / 1024, info.used_bytes / 1024,
             info.photo_count, info.txt_count);
    return info;
}

bool DeleteFile(const std::string& filename) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", kSpiffsBasePath, filename.c_str());

    // Also delete associated .meta file
    char meta_path[128];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", kSpiffsBasePath, filename.c_str());
    remove(meta_path);

    int ret = remove(path);
    if (ret != 0) {
        ESP_LOGW(kTag, "Failed to delete %s", path);
        return false;
    }
    ESP_LOGI(kTag, "Deleted %s", path);
    return true;
}

std::vector<std::string> ListTxtFiles() {
    std::vector<std::string> txts;
    DIR* dir = opendir(kSpiffsBasePath);
    if (!dir) return txts;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (!name) continue;
        const char* ext = strrchr(name, '.');
        if (ext && strcmp(ext, ".txt") == 0) {
            txts.push_back(name);
        }
    }
    closedir(dir);
    std::sort(txts.begin(), txts.end());
    return txts;
}

std::string ReadTxtFile(const std::string& filename) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", kSpiffsBasePath, filename.c_str());

    FILE* f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(kTag, "Cannot open %s", path);
        return "";
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 512000) {  // Max 500KB TXT
        fclose(f);
        return "";
    }

    std::string content;
    content.resize(static_cast<size_t>(size));
    size_t read = fread(content.data(), 1, content.size(), f);
    fclose(f);

    if (read != content.size()) {
        content.resize(read);
    }
    return content;
}

std::string FormatBytes(uint32_t bytes) {
    if (bytes < 1024) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%luB", (unsigned long)bytes);
        return buf;
    }
    if (bytes < 1024 * 1024) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1fKB", bytes / 1024.0);
        return buf;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0 * 1024.0));
    return buf;
}

}  // namespace storage_manager
