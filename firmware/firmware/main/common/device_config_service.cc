/**
 * @file device_config_service.cc
 * @brief Implementation of the single owner of the device configuration.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "device_config_service.h"

#include <esp_log.h>

#include "settings.h"

namespace devcfg {

namespace {

constexpr char kTag[] = "DeviceConfig";
constexpr char kNamespace[] = "config";
constexpr char kRevisionKey[] = "revision";

uint32_t LoadPersistedRevision() {
    Settings nvs(kNamespace, false);
    const int32_t stored = nvs.GetInt(kRevisionKey, 0);
    // A negative value can only mean a corrupted or hand-edited entry. Treat it
    // as "unknown" rather than sign-extending it into a wild revision that no
    // caller could ever match.
    return stored > 0 ? static_cast<uint32_t>(stored) : 0u;
}

void StoreRevision(uint32_t revision) {
    Settings nvs(kNamespace, true);
    // NVS carries this as a signed 32-bit value. Clamping rather than wrapping
    // keeps the stored number monotonic for the whole life of a device that
    // would need billions of settings changes to get here.
    const int32_t clamped = revision > 0x7FFFFFFFu
                                ? 0x7FFFFFFF
                                : static_cast<int32_t>(revision);
    nvs.SetInt(kRevisionKey, clamped);
}

}  // namespace

DeviceConfigService& DeviceConfigService::GetInstance() {
    static DeviceConfigService instance;
    return instance;
}

void DeviceConfigService::Init(const Config& initial, ConfigHooks hooks) {
    std::lock_guard<std::mutex> guard(lock_);
    config_ = initial;
    hooks_ = std::move(hooks);
    revision_.Seed(LoadPersistedRevision());
    if (hooks_.run_action) {
        actions_.SetRunner(hooks_.run_action);
    }
    initialised_ = true;
    // The URL is logged because it is an address on the home LAN and the
    // operator needs to see it; the token is not logged anywhere, and the
    // service has never held one.
    ESP_LOGI(kTag,
             "config v%d ready at revision %u: slide_min=%d sync=%d muted=%d "
             "lockdown=%d lan=%d hub=%s",
             kApiLevel, static_cast<unsigned>(revision_.value()),
             static_cast<int>(config_.gallery_slide_min),
             static_cast<int>(config_.sync_interval),
             config_.voice_muted ? 1 : 0,
             config_.dashboard_lockdown ? 1 : 0,
             config_.network_lan_service ? 1 : 0,
             config_.voice_hub_url.empty() ? "not configured" : "configured");
}

Config DeviceConfigService::Snapshot() {
    std::lock_guard<std::mutex> guard(lock_);
    return config_;
}

uint32_t DeviceConfigService::Revision() {
    std::lock_guard<std::mutex> guard(lock_);
    return revision_.value();
}

uint32_t DeviceConfigService::BumpLocked() {
    const uint32_t next = revision_.Bump();
    StoreRevision(next);
    return next;
}

PatchError DeviceConfigService::ApplyPatch(const ConfigPatch& patch,
                                           std::string* failed_field,
                                           Config* new_config,
                                           uint32_t* new_revision) {
    std::lock_guard<std::mutex> guard(lock_);

    if (new_revision != nullptr) *new_revision = revision_.value();

    const PatchError error = patch.Validate(config_, revision_.value(), failed_field);
    if (error != PatchError::kNone) {
        ESP_LOGW(kTag, "config patch refused: %s (%s)", PatchErrorName(error),
                 failed_field != nullptr && !failed_field->empty()
                     ? failed_field->c_str() : "no field");
        return error;
    }

    // Compute the whole new configuration before touching the device, so a
    // hook that is missing cannot leave half a patch applied.
    Config next = config_;
    patch.ApplyTo(&next);

    if (patch.has(Field::kGallerySlideMin) && hooks_.apply_slide_min) {
        hooks_.apply_slide_min(next.gallery_slide_min);
    }
    if (patch.has(Field::kSyncInterval) && hooks_.apply_sync_interval) {
        hooks_.apply_sync_interval(next.sync_interval);
    }
    if (patch.has(Field::kVoiceMuted) && hooks_.apply_voice_muted) {
        hooks_.apply_voice_muted(next.voice_muted);
    }
    if (patch.has(Field::kVoiceHubUrl) && hooks_.apply_hub_url) {
        hooks_.apply_hub_url(next.voice_hub_url);
    }
    if (patch.has(Field::kDashboardLockdown) && hooks_.apply_lockdown) {
        hooks_.apply_lockdown(next.dashboard_lockdown);
    }
    // The mode the caller actually asked for, before it is normalised below.
    // The hook needs "interactive" to open a window; the stored configuration
    // must not keep it. See the normalisation after this block.
    const std::string requested_power_mode = next.power_mode;

    // Two different sentences, two different hooks.
    //
    // "Be interactive" changes the mode and needs the window length that
    // travels with it, so a patch carrying both applies the mode against the
    // new length rather than the one it replaced — that is why it is one call
    // and not two.
    //
    // "The interactive window should be thirty minutes" is not a mode change
    // at all. Routing it through the mode hook is how a caller adjusting the
    // *length* of a window ended up closing the window they were standing in
    // the middle of, so a patch without power.mode takes the second hook and
    // leaves any open window alone.
    if (patch.has(Field::kPowerMode) && hooks_.apply_power_mode) {
        hooks_.apply_power_mode(requested_power_mode, next.power_interactive_min);
    } else if (patch.has(Field::kPowerInteractiveMin) &&
               hooks_.apply_interactive_minutes) {
        hooks_.apply_interactive_minutes(next.power_interactive_min);
    }

    // The configuration holds the *base* mode: what the device comes back as
    // after a reboot. An interactive window is live state with a deadline, it
    // is reported on the status route next to the countdown that makes it
    // meaningful, and the device's own NVS write already maps it away. Leaving
    // "interactive" here would make this route disagree with NVS and promise a
    // reader a mode the device will not be in after a restart.
    if (next.power_mode == "interactive") {
        next.power_mode = "auto_saver";
    }
    if (patch.has(Field::kPowerWakeIntervalMin) && hooks_.apply_wake_interval) {
        hooks_.apply_wake_interval(next.power_wake_interval_min);
    }
    if (patch.has(Field::kAutonomyEnabled) && hooks_.apply_autonomy_enabled) {
        hooks_.apply_autonomy_enabled(next.autonomy_enabled);
    }
    if (patch.has(Field::kNetworkLanService) && hooks_.apply_lan_service) {
        // Reported back as what actually happened. Asking for the LAN server
        // without Wi-Fi does not start one, and the response must not claim it
        // did.
        next.network_lan_service = hooks_.apply_lan_service(next.network_lan_service);
    }

    config_ = next;
    const uint32_t revision = BumpLocked();
    if (new_config != nullptr) *new_config = config_;
    if (new_revision != nullptr) *new_revision = revision;
    ESP_LOGI(kTag, "config patch applied: %u field(s), revision %u",
             static_cast<unsigned>(patch.size()), static_cast<unsigned>(revision));
    return PatchError::kNone;
}

// ------------------------------------------------- changes from the device --

void DeviceConfigService::ApplyLocalSlideMin(int32_t minutes) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (!IsValidSlideMin(minutes)) {
        // The device menu cycles a fixed set, so this can only be a caller
        // that invented a value. Refusing it here keeps the local path and the
        // remote path to the same bounds.
        ESP_LOGW(kTag, "refusing local slide_min=%d: not one of 0, 5, 10, 30",
                 static_cast<int>(minutes));
        return;
    }
    if (hooks_.apply_slide_min) hooks_.apply_slide_min(minutes);
    config_.gallery_slide_min = minutes;
    BumpLocked();
}

void DeviceConfigService::ApplyLocalVoiceMuted(bool muted) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (hooks_.apply_voice_muted) hooks_.apply_voice_muted(muted);
    config_.voice_muted = muted;
    BumpLocked();
}

void DeviceConfigService::ApplyLocalLockdown(bool enabled) {
    // The device menu may turn lockdown off. Only the network may not: the
    // one-way rule is about who is asking, not about which value is allowed.
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (hooks_.apply_lockdown) hooks_.apply_lockdown(enabled);
    config_.dashboard_lockdown = enabled;
    BumpLocked();
}

bool DeviceConfigService::ApplyLocalLanService(bool enabled) {
    std::lock_guard<std::mutex> guard(lock_);
    // Before Init() there are no hooks, so nothing was started. Reporting the
    // value that was asked for would be the one thing this method exists not
    // to do.
    if (!initialised_) return false;
    bool resulting = enabled;
    if (hooks_.apply_lan_service) resulting = hooks_.apply_lan_service(enabled);
    config_.network_lan_service = resulting;
    BumpLocked();
    return resulting;
}

void DeviceConfigService::NoteSlideMin(int32_t minutes) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.gallery_slide_min == minutes) return;
    config_.gallery_slide_min = minutes;
    BumpLocked();
}

void DeviceConfigService::NoteSyncInterval(int32_t minutes) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.sync_interval == minutes) return;
    config_.sync_interval = minutes;
    BumpLocked();
}

void DeviceConfigService::NoteVoiceMuted(bool muted) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.voice_muted == muted) return;
    config_.voice_muted = muted;
    BumpLocked();
}

void DeviceConfigService::NoteHub(const std::string& url, bool token_set) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.voice_hub_url == url && config_.voice_hub_token_set == token_set) {
        return;
    }
    config_.voice_hub_url = url;
    config_.voice_hub_token_set = token_set;
    BumpLocked();
}

void DeviceConfigService::NoteLockdown(bool enabled) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.dashboard_lockdown == enabled) return;
    config_.dashboard_lockdown = enabled;
    BumpLocked();
}

void DeviceConfigService::NoteLanService(bool enabled) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!initialised_) return;
    if (config_.network_lan_service == enabled) return;
    config_.network_lan_service = enabled;
    BumpLocked();
}

void DeviceConfigService::NotePowerMode(const std::string& mode,
                                        int32_t interactive_min) {
    std::lock_guard<std::mutex> guard(lock_);
    // An interactive window is live state with a deadline; the *base* mode is
    // what belongs in the configuration. Recording "interactive" here would
    // put a value in NVS that PowerState refuses to restore, and a reader
    // would see a mode the device will not be in after a reboot.
    const std::string base = mode == "interactive" ? std::string("auto_saver") : mode;
    if (config_.power_mode == base &&
        config_.power_interactive_min == interactive_min) {
        return;
    }
    config_.power_mode = base;
    config_.power_interactive_min = interactive_min;
    BumpLocked();
}

void DeviceConfigService::NoteWakeInterval(int32_t minutes) {
    std::lock_guard<std::mutex> guard(lock_);
    if (config_.power_wake_interval_min == minutes) return;
    config_.power_wake_interval_min = minutes;
    BumpLocked();
}

// ----------------------------------------------------------------- actions --

ActionError DeviceConfigService::RequestAction(Action action, const char* confirm,
                                               const char* idem_key, bool* replay) {
    std::lock_guard<std::mutex> guard(lock_);
    const ActionError error = actions_.Request(action, confirm, idem_key, replay);
    if (error != ActionError::kNone) {
        ESP_LOGW(kTag, "%s refused: %s", ActionName(action), ActionErrorName(error));
    } else if (replay != nullptr && *replay) {
        ESP_LOGI(kTag, "%s replayed from the idempotency ring; nothing scheduled",
                 ActionName(action));
    } else {
        ESP_LOGW(kTag, "%s scheduled in %u ms", ActionName(action),
                 static_cast<unsigned>(ActionGate::kDelayMs));
    }
    return error;
}

}  // namespace devcfg
