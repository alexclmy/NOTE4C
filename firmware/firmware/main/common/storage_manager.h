/**
 * @file storage_manager.h
 * @brief SPIFFS storage info query (total/used/free, file listing)
 */

#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stdint.h>
#include <string>
#include <vector>

struct StorageFileInfo {
    std::string name;    // filename (e.g. "img123.bin")
    uint32_t size;       // file size in bytes
    std::string type;    // "image" or "document"
};

struct StorageInfo {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    int photo_count;
    int txt_count;
    std::vector<StorageFileInfo> files;
};

namespace storage_manager {

/// Query SPIFFS partition info and enumerate files.
StorageInfo GetStorageInfo();

/// Delete a file from SPIFFS by name. Returns true on success.
bool DeleteFile(const std::string& filename);

/// Get list of TXT files in SPIFFS (for ebook page).
std::vector<std::string> ListTxtFiles();

/// Read a TXT file content into string. Returns empty on failure.
std::string ReadTxtFile(const std::string& filename);

/// Format bytes as human-readable string (e.g. "2.1MB").
std::string FormatBytes(uint32_t bytes);

}  // namespace storage_manager

#endif  // STORAGE_MANAGER_H