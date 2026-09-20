/**
 * @file settings.h
 * @brief Host shim for the NVS-backed Settings wrapper (main/settings.h).
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * An in-memory map with the same surface. It exists so device_config_service.cc
 * — whose only two ESP-IDF dependencies are the log macros and this class, and
 * which uses this one solely to keep the config revision monotonic across
 * reboots — can be compiled and tested on a host.
 *
 * The store is process-global and survives destruction of a Settings object,
 * because that is the property the real NVS has and the one the revision
 * counter depends on. `Clear()` lets a test start from a blank device.
 */

#ifndef HOST_SHIM_SETTINGS_H
#define HOST_SHIM_SETTINGS_H

#include <map>
#include <string>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false)
        : ns_(ns), read_write_(read_write) {}

    std::string GetString(const std::string& key, const std::string& def = "") {
        const auto it = Strings().find(Key(key));
        return it == Strings().end() ? def : it->second;
    }
    void SetString(const std::string& key, const std::string& value) {
        if (!read_write_) return;
        Strings()[Key(key)] = value;
    }
    int32_t GetInt(const std::string& key, int32_t def = 0) {
        const auto it = Ints().find(Key(key));
        return it == Ints().end() ? def : it->second;
    }
    void SetInt(const std::string& key, int32_t value) {
        if (!read_write_) return;
        Ints()[Key(key)] = value;
    }
    bool GetBool(const std::string& key, bool def = false) {
        return GetInt(key, def ? 1 : 0) != 0;
    }
    void SetBool(const std::string& key, bool value) { SetInt(key, value ? 1 : 0); }
    void EraseKey(const std::string& key) {
        Ints().erase(Key(key));
        Strings().erase(Key(key));
    }
    void EraseAll() { Clear(); }

    /// Wipe the simulated flash. For a test that wants a factory-fresh device.
    static void Clear() {
        Ints().clear();
        Strings().clear();
    }

private:
    std::string Key(const std::string& key) const { return ns_ + "/" + key; }
    static std::map<std::string, int32_t>& Ints() {
        static std::map<std::string, int32_t> ints;
        return ints;
    }
    static std::map<std::string, std::string>& Strings() {
        static std::map<std::string, std::string> strings;
        return strings;
    }

    std::string ns_;
    bool read_write_ = false;
};

#endif  // HOST_SHIM_SETTINGS_H
