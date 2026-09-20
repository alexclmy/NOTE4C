/**
 * @file device_config_service.h
 * @brief Device glue around the portable config contract.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * device_config.h holds the rules; this holds the one copy of the answer.
 *
 * Why a single owner
 * ------------------
 * The revision is a compare-and-swap token, and a CAS token is worthless if
 * some writers do not touch it. The Settings menu on the device and the PATCH
 * route over the network change the same seven values, so they go through the
 * same object here, and every change bumps the same counter. A revision that
 * only tracked remote writes would let the tower overwrite a change somebody
 * made by pressing buttons thirty seconds earlier, which is precisely the lost
 * update the CAS exists to stop.
 *
 * Persistence
 * -----------
 * This class persists exactly one thing: the revision, in NVS namespace
 * `config`. Every *setting* is persisted by the code that already owned it
 * (Application for the gallery, sync and voice values, DashboardManager for
 * lockdown), reached through the hooks below. There is deliberately no raw NVS
 * write here: a config service that could write arbitrary keys would be the
 * raw NVS API this project refused to ship.
 *
 * The revision survives a reboot so a tower that read revision 12, watched the
 * device restart and then sent a patch expecting 12 is not silently served a
 * counter that went back to 0 and matched by accident.
 */

#ifndef COMMON_DEVICE_CONFIG_SERVICE_H
#define COMMON_DEVICE_CONFIG_SERVICE_H

#include <functional>
#include <mutex>
#include <string>

#include "device_config.h"

namespace devcfg {

/**
 * @brief How the service reaches the code that owns each setting.
 *
 * Every hook is "do the thing and persist it". None of them calls back into
 * the service: the service records the new value itself, which is what keeps
 * one change to one bump.
 */
struct ConfigHooks {
    std::function<void(int32_t minutes)> apply_slide_min;
    std::function<void(int32_t minutes)> apply_sync_interval;
    std::function<void(bool muted)> apply_voice_muted;
    /// Replace the hub base URL, keeping the stored token. An empty URL clears
    /// the pair, because a token with nowhere to go is a secret with no purpose.
    std::function<void(const std::string& url)> apply_hub_url;
    std::function<void(bool enabled)> apply_lockdown;
    /// @return the state the device is actually in afterwards, which is not
    ///         always the state that was asked for: starting the LAN server
    ///         needs Wi-Fi and an address.
    std::function<bool(bool enabled)> apply_lan_service;
    /**
     * Apply a power mode, and persist the base mode.
     *
     * Called only when the patch actually carried `power.mode`. Both values
     * travel together because a patch may set the mode and the window length
     * at once and the device has to end up in one consistent state:
     * @p interactive_min is the window length to use if @p mode is
     * "interactive", and it is stored regardless, so a later interactive
     * request without an explicit length gets the one the user last chose.
     */
    std::function<void(const std::string& mode, int32_t interactive_min)> apply_power_mode;
    /**
     * Change the interactive window length on its own.
     *
     * Called when a patch carried `power.interactive_min` and *not*
     * `power.mode`. That is a different request from a mode change: it sets
     * how long the next window will be and must not disturb one that is open,
     * because a user adjusting the length of a window is not asking to be
     * thrown out of it.
     */
    std::function<void(int32_t minutes)> apply_interactive_minutes;
    /// Set the auto-saver wake interval and persist it.
    std::function<void(int32_t minutes)> apply_wake_interval;
    /**
     * @brief Set the autonomy kill-switch and persist it.
     *
     * Persisted, which is what lets the field report `immediate` honestly. The
     * wake cycle wakes from deep sleep — a reboot — so a switch held only in
     * this service's snapshot would come back off every wake and the feature
     * could never run unattended.
     */
    std::function<void(bool enabled)> apply_autonomy_enabled;
    /// Arm a one-shot timer that performs @p action after @p delay_ms.
    std::function<void(Action action, uint32_t delay_ms)> run_action;
};

class DeviceConfigService {
public:
    static DeviceConfigService& GetInstance();

    DeviceConfigService(const DeviceConfigService&) = delete;
    DeviceConfigService& operator=(const DeviceConfigService&) = delete;

    /**
     * @brief Adopt the device's current settings and wire up the hooks.
     *
     * @param initial what the owning code already read out of NVS at boot. The
     *        service does not re-read it, so there is exactly one place where
     *        each default lives.
     */
    void Init(const Config& initial, ConfigHooks hooks);

    bool initialised() const { return initialised_; }

    Config Snapshot();
    uint32_t Revision();

    /**
     * @brief Apply one remote patch, all or nothing.
     *
     * @param failed_field  set to the dotted name the refusal was about.
     * @param new_config    the configuration after the write, on success.
     * @param new_revision  the revision after the write, on success. On a
     *                      revision mismatch this is set to the *current*
     *                      revision, so the caller can re-read in one hop.
     */
    PatchError ApplyPatch(const ConfigPatch& patch,
                          std::string* failed_field,
                          Config* new_config,
                          uint32_t* new_revision);

    // ---- changes that came from the device itself -------------------------
    //
    // Two shapes, and the difference matters. ApplyLocal* runs the hook: the
    // caller wants the service to make the change. Note* only records: the
    // caller has already made it, and calling the hook again would toggle it
    // back or start a server twice.

    void ApplyLocalSlideMin(int32_t minutes);
    void ApplyLocalVoiceMuted(bool muted);
    void ApplyLocalLockdown(bool enabled);
    /// @return the resulting state, which may differ from @p enabled.
    bool ApplyLocalLanService(bool enabled);

    void NoteSlideMin(int32_t minutes);
    void NoteSyncInterval(int32_t minutes);
    void NoteVoiceMuted(bool muted);
    void NoteHub(const std::string& url, bool token_set);
    void NoteLockdown(bool enabled);
    void NoteLanService(bool enabled);
    /// The device changed its own power mode: the button opened an interactive
    /// window, or one expired. Records and bumps; does not call the hook back.
    void NotePowerMode(const std::string& mode, int32_t interactive_min);
    void NoteWakeInterval(int32_t minutes);

    // ---- actions ----------------------------------------------------------

    ActionError RequestAction(Action action, const char* confirm,
                              const char* idem_key, bool* replay);

private:
    DeviceConfigService() = default;

    /// Bump and persist. The caller holds lock_.
    uint32_t BumpLocked();

    std::mutex lock_;
    Config config_;
    RevisionCounter revision_;
    ActionGate actions_;
    ConfigHooks hooks_;
    bool initialised_ = false;
};

}  // namespace devcfg

#endif  // COMMON_DEVICE_CONFIG_SERVICE_H
