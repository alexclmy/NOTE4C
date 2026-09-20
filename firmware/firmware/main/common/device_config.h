/**
 * @file device_config.h
 * @brief The typed device settings contract for API v2. Portable, host tested.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Like dashboard_service.h, this file deliberately contains no ESP-IDF header,
 * so the host tests exercise these exact translation units rather than a
 * parallel reimplementation of the rules. The device glue that owns NVS, the
 * HTTP body and the reboot lives in device_config_service.h and
 * ui/renderers/rawdraw/config_api.cc.
 *
 * What this owns
 * --------------
 * The *whole* answer to "may this change happen, and what does it mean". That
 * is: the allowlist of writable fields, the bounds of each one, the
 * compare-and-swap on the revision, the one-way rule on lockdown, the
 * confirmation literal for the setting that can sever the API, and the exact
 * JSON that goes on the wire.
 *
 * Why the JSON is here rather than in the route
 * ---------------------------------------------
 * Because "the response never contains the hub token" is a claim, and a claim
 * that lives in a route handler can only be checked by a device with a network
 * analyser attached. Rendering the bytes in a portable function means a host
 * test can search the actual response for the actual secret and fail if it
 * finds it. The parsing stays in the route, where cJSON is available.
 *
 * What this deliberately refuses to do
 * ------------------------------------
 * There is no generic setter. Field is a closed enum, the dotted names are a
 * fixed table, and a name that is not in the table is an error rather than a
 * key written somewhere. A config API whose field set is open is a raw NVS API
 * wearing a hat.
 */

#ifndef COMMON_DEVICE_CONFIG_H
#define COMMON_DEVICE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "dashboard_service.h"

namespace devcfg {

/// The api level this contract describes. Bumped on the status route too.
constexpr int kApiLevel = 2;

// ----------------------------------------------------------------- fields --

/**
 * @brief Every setting the config API may touch. There is no "other".
 *
 * Adding a member here is the only way to widen the remote surface, and it
 * forces a spec row, a bound, an apply mode and a persistence decision, all in
 * this file, all host tested.
 */
enum class Field {
    kGallerySlideMin = 0,  ///< gallery.slide_min
    kSyncInterval,         ///< sync.sync_interval
    kVoiceMuted,           ///< voice.muted
    kVoiceHubUrl,          ///< voice.hub_url
    kDashboardLockdown,    ///< dashboard.lockdown
    kNetworkLanService,    ///< network.lan_service
    kPowerMode,            ///< power.mode
    kPowerInteractiveMin,  ///< power.interactive_min
    kPowerWakeIntervalMin, ///< power.wake_interval_min
    kAutonomyEnabled,      ///< autonomy.enabled
    kCount,
};

constexpr size_t kFieldCount = static_cast<size_t>(Field::kCount);

enum class ValueKind {
    kInteger = 0,
    kBoolean,
    kString,
};

/**
 * @brief When a write takes effect, and whether it survives a power cut.
 *
 * Three values rather than the two the plan sketched, because the device has
 * three behaviours and flattening them would mean the tower tells the user
 * something false about at least one of them. `network.lan_service` is
 * runtime-only state in application.cc: it is applied the moment it is written
 * and it is *not* in NVS, so the next Wi-Fi connection starts the server again.
 * Reporting that as plain "immediate" would let the tower claim a setting had
 * stuck when it had not.
 */
enum class ApplyMode {
    kImmediate = 0,          ///< in force now, and persisted
    kImmediateNotPersisted,  ///< in force now, gone at the next boot
    kRestartRequired,        ///< stored now, in force after a restart
};

const char* ApplyModeName(ApplyMode mode);

/// One row of the allowlist.
struct FieldSpec {
    Field field;
    const char* dotted;  ///< the exact wire name, for example "gallery.slide_min"
    ValueKind kind;
    ApplyMode apply;
    /**
     * Typed confirmation literal the patch must carry for this field, or
     * nullptr when none is needed. Present only for the one setting whose
     * write can cut the caller off from the device.
     */
    const char* confirm;
};

/// The literal that must accompany turning the LAN service off.
extern const char kLanServiceOffConfirmation[];

/// Look a dotted name up in the allowlist. nullptr when it is not in it.
const FieldSpec* FindField(const char* dotted);
const FieldSpec* FindField(const std::string& dotted);

/// The spec for a known field. Total over Field, excluding kCount.
const FieldSpec& SpecFor(Field field);

/// The dotted wire name, for error messages. "" for kCount.
const char* FieldName(Field field);

// ------------------------------------------------------------------ bounds --

/// gallery.slide_min accepts exactly the four values the device menu cycles.
bool IsValidSlideMin(int32_t minutes);

/// sync.sync_interval is whole minutes, 0 (off) through 1440 (a day).
bool IsValidSyncInterval(int32_t minutes);

constexpr int32_t kSyncIntervalMax = 1440;

/**
 * power.mode accepts exactly the three names power::ModeName produces.
 *
 * Validated through power::ParseMode rather than a second list here, so there
 * is one place a mode can be added and the two files cannot drift into
 * disagreeing about what the wire accepts.
 */
bool IsValidPowerMode(const std::string& name);

/// power.interactive_min accepts exactly the four windows the tower offers.
bool IsValidInteractiveMinutes(int32_t minutes);

/// power.wake_interval_min is whole minutes, floored so the saver still saves.
bool IsValidWakeIntervalMinutes(int32_t minutes);

// ------------------------------------------------------------------ config --

/**
 * @brief The readable state of every allowlisted field.
 *
 * `voice_hub_token_set` is a boolean on purpose and there is nowhere in this
 * struct to put the token itself. A response cannot leak a value the type
 * system never carried.
 */
struct Config {
    int32_t gallery_slide_min = 5;
    int32_t sync_interval = 30;
    bool voice_muted = true;
    std::string voice_hub_url;
    bool voice_hub_token_set = false;
    bool dashboard_lockdown = true;
    bool network_lan_service = false;
    /**
     * The hybrid low-power settings. `power_mode` is the *base* mode, which is
     * what survives a reboot; an open interactive window is reported on the
     * status route and never here, because a window that came back from NVS
     * would be a window nobody opened.
     */
    std::string power_mode = "auto_saver";
    int32_t power_interactive_min = 15;
    int32_t power_wake_interval_min = 60;

    /**
     * @brief The autonomy kill-switch.
     *
     * False by default, and that default is the feature's whole safety
     * argument: a device that is flashed with this build and never configured
     * behaves exactly as the previous build did — a push target that draws
     * what the tower sends and nothing else. Autonomy is something an owner
     * turns on, never something a firmware update turns on for them.
     *
     * When false the stored profile is kept rather than erased. Turning the
     * switch back on therefore does not require a re-push, and the tower can
     * still show what the device is holding.
     *
     * Not persisted in this increment: see the table entry in device_config.cc.
     * The switch holds for the life of the boot and a power cycle returns the
     * device to off, which is what keeps a bench enable from outliving the
     * bench.
     */
    bool autonomy_enabled = false;
};

// ------------------------------------------------------------------- patch --

enum class PatchError {
    kNone = 0,
    kNotObject,           ///< the body was not a JSON object
    kNoSetObject,         ///< "set" was missing or not an object
    kEmptyPatch,          ///< "set" carried no fields
    kUnknownField,        ///< a dotted name that is not in the allowlist
    kWrongType,           ///< right name, wrong JSON type
    kOutOfRange,          ///< right type, outside the field's bounds
    kDuplicateField,      ///< the same field twice in one patch
    kTooManyFields,       ///< more entries than there are fields
    kMissingRevision,     ///< expected_revision absent or not a number
    kRevisionMismatch,    ///< somebody else changed the config first
    kMissingConfirmation, ///< a field that needs a literal did not get one
    kBadConfirmation,     ///< the literal did not match
    kLockdownIsOneWay,    ///< remote writes may only turn lockdown on
    kHubUrlNeedsToken,    ///< a URL with no token configures nothing
    kBadHubUrl,           ///< failed voice::ValidateHubUrl
    kValueTooLong,        ///< a string longer than the field accepts
};

const char* PatchErrorName(PatchError error);

/// HTTP status text for an error, so the route and the tests agree on it.
const char* PatchErrorHttpStatus(PatchError error);

/// One typed value, already lifted off the wire by the route's JSON parser.
struct FieldValue {
    Field field = Field::kCount;
    int32_t integer = 0;
    bool boolean = false;
    std::string text;
};

/**
 * @brief One PATCH body, accumulated and then validated as a whole.
 *
 * All-or-nothing is the point. A patch that sets three fields and fails on the
 * third must not leave the first two applied, because the caller's next read
 * would show a configuration nobody asked for and the revision would have
 * moved under a caller that believes its write failed. Validate() therefore
 * never mutates anything, and ApplyTo() never fails.
 */
class ConfigPatch {
public:
    /// Longest hub URL a patch may carry. Matches voice::kMaxHubUrlChars.
    static constexpr size_t kMaxStringChars = 200;

    void SetExpectedRevision(uint32_t revision);
    bool has_expected_revision() const { return has_expected_revision_; }
    uint32_t expected_revision() const { return expected_revision_; }

    /// The `confirm` literal from the body, if the caller supplied one.
    void SetConfirmation(const std::string& word);

    /**
     * @brief Record one field.
     *
     * @return kNone, or the first structural problem. The name is remembered
     *         in failed_field() so the route can name it back to the caller,
     *         which is the difference between one edit and an afternoon.
     */
    PatchError AddInteger(const char* dotted, int32_t value);
    PatchError AddBoolean(const char* dotted, bool value);
    PatchError AddString(const char* dotted, const std::string& value);

    /// Record that a known field arrived carrying the wrong JSON type.
    PatchError RejectType(const char* dotted);

    /// The dotted name the last error was about, or "" when there was none.
    const char* failed_field() const { return failed_field_.c_str(); }

    size_t size() const { return count_; }
    const FieldValue& at(size_t index) const { return values_[index]; }
    bool has(Field field) const;
    /// The recorded value for @p field. Undefined unless has(field).
    const FieldValue& value_for(Field field) const;

    /**
     * @brief Check the whole patch against the live configuration.
     *
     * Order matters and is deliberate: structure, then revision, then
     * per-field semantics. A caller with a stale revision learns that first,
     * because re-reading is what it must do regardless of what else is wrong.
     */
    PatchError Validate(const Config& current, uint32_t revision,
                        std::string* failed_field_out) const;

    /// Fold the patch into @p out. Only call after Validate returned kNone.
    void ApplyTo(Config* out) const;

    void Clear();

private:
    PatchError Record(const FieldSpec* spec, const FieldValue& value);

    FieldValue values_[kFieldCount];
    size_t count_ = 0;
    bool has_expected_revision_ = false;
    uint32_t expected_revision_ = 0;
    bool has_confirmation_ = false;
    std::string confirmation_;
    std::string failed_field_;
};

// ---------------------------------------------------------------- revision --

/**
 * @brief The monotonic config revision.
 *
 * It exists so a caller can say "change this, but only if it still looks like
 * what I read". That is why *every* change bumps it, including the ones made
 * by somebody standing at the device pressing buttons: a revision that only
 * tracked remote writes would let the tower overwrite a physical change it
 * never saw, which is exactly the lost update the field is here to prevent.
 *
 * Zero means "nothing has ever changed". The counter skips back to 1 rather
 * than 0 on wrap, so the value stays a meaningful "has changed" flag after
 * four billion edits that this device will never make.
 */
class RevisionCounter {
public:
    void Seed(uint32_t persisted) { value_ = persisted; }
    uint32_t value() const { return value_; }
    uint32_t Bump();

private:
    uint32_t value_ = 0;
};

// -------------------------------------------------------------- JSON output --

/**
 * @brief Escape @p in as the contents of a JSON string (without the quotes).
 *
 * @return characters written, not counting the terminator. 0 means nothing was
 *         written: either @p in was empty, or @p out was too small and has
 *         been left empty rather than holding a truncated escape. A caller
 *         already knows whether it passed an empty string.
 *
 * Control characters become \\u00XX rather than being dropped, so a value that
 * should never have reached here stays visible in the response instead of
 * silently changing shape on its way out.
 */
size_t JsonEscape(const std::string& in, char* out, size_t out_len);

/**
 * @brief Render the body of GET /api/v1/config.
 *
 * @return bytes written, or 0 when @p out was too small (in which case @p out
 *         is left as an empty string rather than a truncated JSON fragment).
 *
 * There is no overload that takes a token, and that is the whole design.
 */
size_t RenderConfigJson(const Config& config, uint32_t revision,
                        char* out, size_t out_len);

/// Render the body of a successful PATCH: the new config, the new revision,
/// and one honest apply mode per field that was actually written.
size_t RenderPatchResponseJson(const Config& config, uint32_t revision,
                               const ConfigPatch& patch,
                               char* out, size_t out_len);

/// Bytes a caller should reserve for either of the two renderers above.
/// Raised from 1024 when the power block was added, so the headroom over a
/// realistic hub URL stayed what it was rather than quietly shrinking by the
/// size of the new fields.
constexpr size_t kConfigJsonMax = 1152;

// ----------------------------------------------------------------- actions --

/// The two things the device can be told to do to itself.
enum class Action {
    kRestart = 0,
    kSleep,
};

const char* ActionName(Action action);
/// The exact literal the caller must send in `confirm` for @p action.
const char* ActionConfirmation(Action action);
/// Resolve a wire name. false when it is neither action.
bool ParseAction(const char* name, Action* out);

enum class ActionError {
    kNone = 0,
    kUnknownAction,
    kMissingConfirmation,
    kBadConfirmation,
    kMissingIdempotencyKey,
    kNoRunner,
};

const char* ActionErrorName(ActionError error);

/**
 * @brief Gate in front of restart and sleep.
 *
 * Three properties, and each one is here because its absence is a real bug:
 *
 *  1. **The literal.** `{"confirm":"restart"}` is the action's own name, so a
 *     body that reaches this route by accident, or a client that got its
 *     routes crossed, cannot reboot a device mid-refresh.
 *
 *  2. **Idempotency.** A caller that retries after a timeout must not reboot
 *     the device twice. The second request with the same key is answered with
 *     the first one's answer and schedules nothing.
 *
 *  3. **A runner, injected.** The device passes a closure that arms a timer;
 *     the host test passes one that appends to a vector. Nothing in this class
 *     knows how to reboot anything, so a test of the gating rules cannot kill
 *     the test process, and the gating rules get tested.
 *
 * The delay is not cosmetic: the HTTP response has to reach the socket before
 * the device stops being a device. One second is several orders of magnitude
 * more than the flush needs and is still short enough that a caller waiting on
 * it does not conclude the request was lost.
 */
class ActionGate {
public:
    /// Milliseconds between answering and acting.
    static constexpr uint32_t kDelayMs = 1000;

    using Runner = std::function<void(Action action, uint32_t delay_ms)>;

    void SetRunner(Runner runner) { runner_ = std::move(runner); }

    /**
     * @brief Validate and schedule one action.
     *
     * @param confirm   the caller's `confirm` string, may be nullptr.
     * @param idem_key  the Idempotency-Key header, may be nullptr.
     * @param replay_out set true when this key was already served.
     */
    ActionError Request(Action action, const char* confirm, const char* idem_key,
                        bool* replay_out);

    uint32_t scheduled_count() const { return scheduled_; }
    void Reset();

private:
    dashboard::IdempotencyCache idem_;
    Runner runner_;
    uint32_t scheduled_ = 0;
};

// ------------------------------------------------------------ capabilities --

/**
 * @brief The capability strings advertised on the status route.
 *
 * A list rather than an api integer alone because the two answer different
 * questions: the integer says which contract this is, the list says which
 * parts of it this build actually compiled. `voice.ptt.v1` is absent from a
 * VOICE_PTT_ENABLED=0 build, which is how the tower learns that push to talk
 * is not merely muted but not present.
 *
 * `autonomy.profile.v1` works the same way and for the same reason: a build
 * with CONFIG_AUTONOMY_ENABLED off leaves the string out of the rendered list,
 * and the tower's controls are absent rather than inert.
 *
 * Out of the *list*, precisely: the literal is still in the binary's .rodata,
 * because the function that would emit it is still linked. The gate is over
 * reachability rather than presence — `strings` on an autonomy-off image finds
 * `autonomy.profile.v1` and a tower reading the capability list does not. See
 * AUTONOMY_COMPILED in dashboard_build_config.h, which makes the same point
 * about the feature as a whole.
 */
size_t RenderCapabilitiesJson(bool ptt_compiled, bool autonomy_compiled, char* out,
                              size_t out_len);

}  // namespace devcfg

#endif  // COMMON_DEVICE_CONFIG_H
