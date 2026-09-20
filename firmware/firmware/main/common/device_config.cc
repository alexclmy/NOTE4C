/**
 * @file device_config.cc
 * @brief Implementation of the typed device settings contract.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "device_config.h"

#include <stdio.h>
#include <string.h>

#include "power_policy.h"
#include "voice_hub_config.h"

namespace devcfg {

namespace {

/**
 * The allowlist, and the only one.
 *
 * Kept in Field order so SpecFor() is an index rather than a search, and so a
 * new enumerator without a row here fails to compile the static assertion
 * below rather than silently becoming an unwritable field.
 */
constexpr FieldSpec kSpecs[] = {
    {Field::kGallerySlideMin, "gallery.slide_min", ValueKind::kInteger,
     ApplyMode::kImmediate, nullptr},
    {Field::kSyncInterval, "sync.sync_interval", ValueKind::kInteger,
     ApplyMode::kImmediate, nullptr},
    {Field::kVoiceMuted, "voice.muted", ValueKind::kBoolean,
     ApplyMode::kImmediate, nullptr},
    {Field::kVoiceHubUrl, "voice.hub_url", ValueKind::kString,
     ApplyMode::kImmediate, nullptr},
    {Field::kDashboardLockdown, "dashboard.lockdown", ValueKind::kBoolean,
     ApplyMode::kImmediate, nullptr},
    // Runtime-only in application.cc: the LAN server is started when Wi-Fi
    // comes up and nothing writes the choice to NVS. Reporting it as persisted
    // would be a lie the tower would repeat.
    {Field::kNetworkLanService, "network.lan_service", ValueKind::kBoolean,
     ApplyMode::kImmediateNotPersisted, kLanServiceOffConfirmation},
    // The hybrid low-power surface. All three are kImmediate: the mode changes
    // the moment it is written and the base mode is persisted, so a device
    // that reboots comes back in the mode it was left in.
    //
    // None of them carries a confirmation literal, and that is deliberate
    // rather than an omission. The setting that could strand a caller is
    // network.lan_service, which severs the API while the device stays up.
    // Writing power.mode cannot strand anybody who is not already able to
    // reach the device: the caller is talking to an awake device, and the two
    // modes that end that conversation (auto_saver, and an interactive window
    // running out) both leave the device coming back on its own timer. The
    // honest warning about what always_on costs belongs in the tower, next to
    // the button, and it is there.
    {Field::kPowerMode, "power.mode", ValueKind::kString,
     ApplyMode::kImmediate, nullptr},
    {Field::kPowerInteractiveMin, "power.interactive_min", ValueKind::kInteger,
     ApplyMode::kImmediate, nullptr},
    {Field::kPowerWakeIntervalMin, "power.wake_interval_min", ValueKind::kInteger,
     ApplyMode::kImmediate, nullptr},
    // The autonomy kill-switch, and the whole remote surface of the feature's
    // on/off state.
    //
    // kImmediate, and it was kImmediateNotPersisted until the wake cycle
    // arrived. The change is not a tidy-up; it is the point at which the other
    // mode became wrong.
    //
    // While a local render happened only because somebody asked for one over
    // the API, a switch that reset on every power cycle was the honest and the
    // safer reading: a bench enable did not outlive the bench. But a wake cycle
    // reaches the panel by going through deep sleep, and a deep sleep is a
    // reboot. An unpersisted switch would come back off on the very first wake,
    // so the feature could never run unattended at all — which is the only
    // thing it is for. Every wake would have needed an operator to re-enable
    // it, and the device would have looked broken rather than switched off.
    //
    // No confirmation literal. Turning it *off* is the safe direction — it
    // makes the device an ordinary push target again, which is exactly what it
    // was before this feature existed — and turning it on cannot strand
    // anybody either: the profile still has to be pushed, validated and stored
    // before anything changes on the glass.
    {Field::kAutonomyEnabled, "autonomy.enabled", ValueKind::kBoolean,
     ApplyMode::kImmediate, nullptr},
};

static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == kFieldCount,
              "every Field needs exactly one allowlist row");

/**
 * Write the JSON escape for one character into @p out.
 *
 * @return characters written, never 0 for a valid buffer of 7 or more bytes.
 * Control characters become \\u00XX rather than being dropped: a value that
 * should never have got this far stays visible in the response instead of
 * quietly changing shape on its way out.
 */
size_t JsonEscapeChar(char raw, char* out, size_t out_len) {
    if (out == nullptr || out_len < 7) return 0;
    const unsigned char c = static_cast<unsigned char>(raw);
    const char* literal = nullptr;
    switch (c) {
        case '"':  literal = "\\\""; break;
        case '\\': literal = "\\\\"; break;
        case '\b': literal = "\\b";  break;
        case '\f': literal = "\\f";  break;
        case '\n': literal = "\\n";  break;
        case '\r': literal = "\\r";  break;
        case '\t': literal = "\\t";  break;
        default: break;
    }
    if (literal != nullptr) {
        memcpy(out, literal, 3);
        return 2;
    }
    if (c < 0x20) {
        snprintf(out, out_len, "\\u%04x", c);
        return 6;
    }
    out[0] = raw;
    out[1] = '\0';
    return 1;
}

/// Append a NUL-terminated fragment, tracking overflow without ever writing
/// past the end. Returns false once the buffer is exhausted.
bool Append(char* out, size_t out_len, size_t* used, const char* text) {
    const size_t len = strlen(text);
    if (*used + len + 1 > out_len) return false;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

bool AppendInt(char* out, size_t out_len, size_t* used, int32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    return Append(out, out_len, used, buf);
}

bool AppendUint(char* out, size_t out_len, size_t* used, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(value));
    return Append(out, out_len, used, buf);
}

bool AppendBool(char* out, size_t out_len, size_t* used, bool value) {
    return Append(out, out_len, used, value ? "true" : "false");
}

/**
 * Append @p text as a quoted, escaped JSON string, one escape at a time.
 *
 * Streaming rather than escaping into a scratch buffer first: the worst case
 * for a 200 character URL is 1200 bytes, and this runs on the httpd task's
 * stack alongside a kilobyte of request body. Borrowing that much stack to
 * avoid a loop would be a poor trade on a device with this much internal RAM.
 */
bool AppendQuoted(char* out, size_t out_len, size_t* used, const std::string& text) {
    if (!Append(out, out_len, used, "\"")) return false;
    char piece[8];
    for (const char raw : text) {
        if (JsonEscapeChar(raw, piece, sizeof(piece)) == 0) return false;
        if (!Append(out, out_len, used, piece)) return false;
    }
    return Append(out, out_len, used, "\"");
}

}  // namespace

const char kLanServiceOffConfirmation[] = "lan_service_off";

// ----------------------------------------------------------------- fields --

const char* ApplyModeName(ApplyMode mode) {
    switch (mode) {
        case ApplyMode::kImmediate:             return "immediate";
        case ApplyMode::kImmediateNotPersisted: return "immediate_not_persisted";
        case ApplyMode::kRestartRequired:       return "restart_required";
    }
    return "unknown";
}

const FieldSpec* FindField(const char* dotted) {
    if (dotted == nullptr) return nullptr;
    for (const FieldSpec& spec : kSpecs) {
        if (strcmp(spec.dotted, dotted) == 0) return &spec;
    }
    return nullptr;
}

const FieldSpec* FindField(const std::string& dotted) {
    return FindField(dotted.c_str());
}

const FieldSpec& SpecFor(Field field) {
    const size_t index = static_cast<size_t>(field);
    // Out of range can only mean kCount, which no caller should hold. Return
    // the first row rather than reading past the array.
    return kSpecs[index < kFieldCount ? index : 0];
}

const char* FieldName(Field field) {
    const size_t index = static_cast<size_t>(field);
    if (index >= kFieldCount) return "";
    return kSpecs[index].dotted;
}

// ------------------------------------------------------------------ bounds --

bool IsValidSlideMin(int32_t minutes) {
    return minutes == 0 || minutes == 5 || minutes == 10 || minutes == 30;
}

bool IsValidSyncInterval(int32_t minutes) {
    return minutes >= 0 && minutes <= kSyncIntervalMax;
}

bool IsValidPowerMode(const std::string& name) {
    // Delegated rather than duplicated. A fourth mode added to power_policy.h
    // becomes writable here automatically, and the two files cannot drift into
    // disagreeing about what the wire accepts.
    power::Mode parsed;
    return power::ParseMode(name.c_str(), &parsed);
}

bool IsValidInteractiveMinutes(int32_t minutes) {
    return power::IsValidInteractiveMinutes(minutes);
}

bool IsValidWakeIntervalMinutes(int32_t minutes) {
    return power::IsValidWakeInterval(minutes);
}

// ------------------------------------------------------------------- patch --

const char* PatchErrorName(PatchError error) {
    switch (error) {
        case PatchError::kNone:                return "none";
        case PatchError::kNotObject:           return "not_object";
        case PatchError::kNoSetObject:         return "no_set_object";
        case PatchError::kEmptyPatch:          return "empty_patch";
        case PatchError::kUnknownField:        return "unknown_field";
        case PatchError::kWrongType:           return "wrong_type";
        case PatchError::kOutOfRange:          return "out_of_range";
        case PatchError::kDuplicateField:      return "duplicate_field";
        case PatchError::kTooManyFields:       return "too_many_fields";
        case PatchError::kMissingRevision:     return "missing_expected_revision";
        case PatchError::kRevisionMismatch:    return "revision_mismatch";
        case PatchError::kMissingConfirmation: return "missing_confirmation";
        case PatchError::kBadConfirmation:     return "bad_confirmation";
        case PatchError::kLockdownIsOneWay:    return "lockdown_is_one_way";
        case PatchError::kHubUrlNeedsToken:    return "hub_url_needs_token";
        case PatchError::kBadHubUrl:           return "bad_hub_url";
        case PatchError::kValueTooLong:        return "value_too_long";
    }
    return "unknown";
}

const char* PatchErrorHttpStatus(PatchError error) {
    switch (error) {
        case PatchError::kNone:
            return "200 OK";
        // A stale revision is the one error that is nobody's mistake: two
        // callers simply raced, and 409 is the answer that tells the loser to
        // re-read rather than to change its request.
        case PatchError::kRevisionMismatch:
            return "409 Conflict";
        // Turning lockdown off remotely is not a malformed request, it is a
        // refused one. 403 says "this route will never do that" where 400
        // would invite the caller to try a different spelling.
        case PatchError::kLockdownIsOneWay:
            return "403 Forbidden";
        default:
            return "400 Bad Request";
    }
}

void ConfigPatch::SetExpectedRevision(uint32_t revision) {
    has_expected_revision_ = true;
    expected_revision_ = revision;
}

void ConfigPatch::SetConfirmation(const std::string& word) {
    has_confirmation_ = true;
    confirmation_ = word;
}

bool ConfigPatch::has(Field field) const {
    for (size_t i = 0; i < count_; ++i) {
        if (values_[i].field == field) return true;
    }
    return false;
}

const FieldValue& ConfigPatch::value_for(Field field) const {
    for (size_t i = 0; i < count_; ++i) {
        if (values_[i].field == field) return values_[i];
    }
    return values_[0];
}

PatchError ConfigPatch::Record(const FieldSpec* spec, const FieldValue& value) {
    if (spec == nullptr) return PatchError::kUnknownField;
    if (has(spec->field)) {
        failed_field_ = spec->dotted;
        return PatchError::kDuplicateField;
    }
    if (count_ >= kFieldCount) {
        failed_field_ = spec->dotted;
        return PatchError::kTooManyFields;
    }
    values_[count_] = value;
    values_[count_].field = spec->field;
    count_++;
    return PatchError::kNone;
}

PatchError ConfigPatch::AddInteger(const char* dotted, int32_t value) {
    const FieldSpec* spec = FindField(dotted);
    if (spec == nullptr) {
        failed_field_ = dotted != nullptr ? dotted : "";
        return PatchError::kUnknownField;
    }
    if (spec->kind != ValueKind::kInteger) {
        failed_field_ = spec->dotted;
        return PatchError::kWrongType;
    }
    FieldValue entry;
    entry.integer = value;
    return Record(spec, entry);
}

PatchError ConfigPatch::AddBoolean(const char* dotted, bool value) {
    const FieldSpec* spec = FindField(dotted);
    if (spec == nullptr) {
        failed_field_ = dotted != nullptr ? dotted : "";
        return PatchError::kUnknownField;
    }
    if (spec->kind != ValueKind::kBoolean) {
        failed_field_ = spec->dotted;
        return PatchError::kWrongType;
    }
    FieldValue entry;
    entry.boolean = value;
    return Record(spec, entry);
}

PatchError ConfigPatch::AddString(const char* dotted, const std::string& value) {
    const FieldSpec* spec = FindField(dotted);
    if (spec == nullptr) {
        failed_field_ = dotted != nullptr ? dotted : "";
        return PatchError::kUnknownField;
    }
    if (spec->kind != ValueKind::kString) {
        failed_field_ = spec->dotted;
        return PatchError::kWrongType;
    }
    if (value.size() > kMaxStringChars) {
        failed_field_ = spec->dotted;
        return PatchError::kValueTooLong;
    }
    FieldValue entry;
    entry.text = value;
    return Record(spec, entry);
}

PatchError ConfigPatch::RejectType(const char* dotted) {
    const FieldSpec* spec = FindField(dotted);
    failed_field_ = spec != nullptr ? spec->dotted : (dotted != nullptr ? dotted : "");
    return spec != nullptr ? PatchError::kWrongType : PatchError::kUnknownField;
}

PatchError ConfigPatch::Validate(const Config& current, uint32_t revision,
                                 std::string* failed_field_out) const {
    const auto fail = [&](PatchError error, const char* field) {
        if (failed_field_out != nullptr) *failed_field_out = field != nullptr ? field : "";
        return error;
    };
    if (failed_field_out != nullptr) failed_field_out->clear();

    if (count_ == 0) return fail(PatchError::kEmptyPatch, "");

    // Revision first. A caller holding a stale read has to go and read again
    // whatever else is wrong with its body, so telling it about a bound it
    // also got wrong would just cost it a second round trip.
    if (!has_expected_revision_) return fail(PatchError::kMissingRevision, "");
    if (expected_revision_ != revision) {
        return fail(PatchError::kRevisionMismatch, "");
    }

    for (size_t i = 0; i < count_; ++i) {
        const FieldValue& entry = values_[i];
        const FieldSpec& spec = SpecFor(entry.field);

        if (spec.confirm != nullptr) {
            // Only the dangerous direction needs the word. Turning the LAN
            // service on cannot strand anybody.
            const bool dangerous =
                (entry.field == Field::kNetworkLanService) && !entry.boolean;
            if (dangerous) {
                if (!has_confirmation_) {
                    return fail(PatchError::kMissingConfirmation, spec.dotted);
                }
                if (confirmation_ != spec.confirm) {
                    return fail(PatchError::kBadConfirmation, spec.dotted);
                }
            }
        }

        switch (entry.field) {
            case Field::kGallerySlideMin:
                if (!IsValidSlideMin(entry.integer)) {
                    return fail(PatchError::kOutOfRange, spec.dotted);
                }
                break;

            case Field::kSyncInterval:
                if (!IsValidSyncInterval(entry.integer)) {
                    return fail(PatchError::kOutOfRange, spec.dotted);
                }
                break;

            case Field::kDashboardLockdown:
                // One way, on purpose. A remote caller may raise the drawbridge
                // and may never lower it: re-enabling the unauthenticated
                // legacy write routes is a decision for somebody holding the
                // device, not for whoever currently holds the token.
                if (!entry.boolean) {
                    return fail(PatchError::kLockdownIsOneWay, spec.dotted);
                }
                break;

            case Field::kVoiceHubUrl: {
                if (entry.text.empty()) break;  // clearing is always allowed
                if (voice::ValidateHubUrl(entry.text) != voice::HubConfigError::kNone) {
                    return fail(PatchError::kBadHubUrl, spec.dotted);
                }
                // A URL with no token behind it configures nothing and would
                // leave the device claiming to be set up. The token arrives
                // through POST /api/v1/voice/hub, which is the only route that
                // ever carries one.
                if (!current.voice_hub_token_set) {
                    return fail(PatchError::kHubUrlNeedsToken, spec.dotted);
                }
                break;
            }

            case Field::kPowerMode:
                if (!IsValidPowerMode(entry.text)) {
                    // Out of range rather than wrong_type: the caller sent a
                    // string, which is the right shape, and naming the bound
                    // is what lets them fix it in one edit.
                    return fail(PatchError::kOutOfRange, spec.dotted);
                }
                break;

            case Field::kPowerInteractiveMin:
                if (!IsValidInteractiveMinutes(entry.integer)) {
                    return fail(PatchError::kOutOfRange, spec.dotted);
                }
                break;

            case Field::kPowerWakeIntervalMin:
                if (!IsValidWakeIntervalMinutes(entry.integer)) {
                    return fail(PatchError::kOutOfRange, spec.dotted);
                }
                break;

            case Field::kVoiceMuted:
            case Field::kNetworkLanService:
            // A boolean with two legal values has nothing left to bound: the
            // wire type check upstream has already refused everything else.
            case Field::kAutonomyEnabled:
            case Field::kCount:
                break;
        }
    }
    return PatchError::kNone;
}

void ConfigPatch::ApplyTo(Config* out) const {
    if (out == nullptr) return;
    for (size_t i = 0; i < count_; ++i) {
        const FieldValue& entry = values_[i];
        switch (entry.field) {
            case Field::kGallerySlideMin:
                out->gallery_slide_min = entry.integer;
                break;
            case Field::kSyncInterval:
                out->sync_interval = entry.integer;
                break;
            case Field::kVoiceMuted:
                out->voice_muted = entry.boolean;
                break;
            case Field::kVoiceHubUrl:
                out->voice_hub_url = voice::NormaliseHubUrl(entry.text);
                // Clearing the URL clears the pair: a token with nowhere to go
                // is a stored secret with no purpose.
                if (out->voice_hub_url.empty()) out->voice_hub_token_set = false;
                break;
            case Field::kDashboardLockdown:
                out->dashboard_lockdown = entry.boolean;
                break;
            case Field::kNetworkLanService:
                out->network_lan_service = entry.boolean;
                break;
            case Field::kPowerMode:
                out->power_mode = entry.text;
                break;
            case Field::kPowerInteractiveMin:
                out->power_interactive_min = entry.integer;
                break;
            case Field::kPowerWakeIntervalMin:
                out->power_wake_interval_min = entry.integer;
                break;
            case Field::kAutonomyEnabled:
                out->autonomy_enabled = entry.boolean;
                break;
            case Field::kCount:
                break;
        }
    }
}

void ConfigPatch::Clear() {
    for (size_t i = 0; i < kFieldCount; ++i) values_[i] = FieldValue();
    count_ = 0;
    has_expected_revision_ = false;
    expected_revision_ = 0;
    has_confirmation_ = false;
    confirmation_.clear();
    failed_field_.clear();
}

// ---------------------------------------------------------------- revision --

uint32_t RevisionCounter::Bump() {
    value_ = (value_ == 0xFFFFFFFFu) ? 1u : value_ + 1u;
    return value_;
}

// -------------------------------------------------------------- JSON output --

size_t JsonEscape(const std::string& in, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    char piece[8];
    for (const char raw : in) {
        if (JsonEscapeChar(raw, piece, sizeof(piece)) == 0 ||
            !Append(out, out_len, &used, piece)) {
            out[0] = '\0';
            return 0;
        }
    }
    return used;
}

size_t RenderConfigJson(const Config& config, uint32_t revision,
                        char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    bool ok = true;

    ok = ok && Append(out, out_len, &used, "{\"api\":");
    ok = ok && AppendInt(out, out_len, &used, kApiLevel);
    ok = ok && Append(out, out_len, &used, ",\"revision\":");
    ok = ok && AppendUint(out, out_len, &used, revision);
    ok = ok && Append(out, out_len, &used, ",\"config\":{\"gallery\":{\"slide_min\":");
    ok = ok && AppendInt(out, out_len, &used, config.gallery_slide_min);
    ok = ok && Append(out, out_len, &used, "},\"sync\":{\"sync_interval\":");
    ok = ok && AppendInt(out, out_len, &used, config.sync_interval);
    ok = ok && Append(out, out_len, &used, "},\"voice\":{\"muted\":");
    ok = ok && AppendBool(out, out_len, &used, config.voice_muted);
    ok = ok && Append(out, out_len, &used, ",\"hub_url\":");
    ok = ok && AppendQuoted(out, out_len, &used, config.voice_hub_url);
    // The token is reported as a boolean and there is no branch here that
    // could ever print it: the Config type does not carry one.
    ok = ok && Append(out, out_len, &used, ",\"hub_token_set\":");
    ok = ok && AppendBool(out, out_len, &used, config.voice_hub_token_set);
    ok = ok && Append(out, out_len, &used, "},\"dashboard\":{\"lockdown\":");
    ok = ok && AppendBool(out, out_len, &used, config.dashboard_lockdown);
    ok = ok && Append(out, out_len, &used, "},\"network\":{\"lan_service\":");
    ok = ok && AppendBool(out, out_len, &used, config.network_lan_service);
    // Stated rather than implied: Wi-Fi credentials are not in this contract,
    // and a tower that reads this cannot mistake their absence for an
    // oversight it should work around.
    ok = ok && Append(out, out_len, &used, ",\"wifi_writable\":false}");
    // The base mode only. An interactive window is live state with a deadline,
    // and it is reported on the status route next to the countdown that makes
    // it meaningful, not here among the settings that survive a reboot.
    ok = ok && Append(out, out_len, &used, ",\"power\":{\"mode\":");
    ok = ok && AppendQuoted(out, out_len, &used, config.power_mode);
    ok = ok && Append(out, out_len, &used, ",\"interactive_min\":");
    ok = ok && AppendInt(out, out_len, &used, config.power_interactive_min);
    ok = ok && Append(out, out_len, &used, ",\"wake_interval_min\":");
    ok = ok && AppendInt(out, out_len, &used, config.power_wake_interval_min);
    // The kill-switch only. Everything else about autonomy — whether a profile
    // is stored and what it says — is live state and is reported on the profile
    // route, not among the settings that survive a reboot.
    ok = ok && Append(out, out_len, &used, "},\"autonomy\":{\"enabled\":");
    ok = ok && AppendBool(out, out_len, &used, config.autonomy_enabled);
    ok = ok && Append(out, out_len, &used, "}}}");

    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

size_t RenderPatchResponseJson(const Config& config, uint32_t revision,
                               const ConfigPatch& patch,
                               char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';

    // The configuration and the revision first, so a caller that only wants
    // the new state parses the same shape it gets from GET.
    const size_t base = RenderConfigJson(config, revision, out, out_len);
    if (base == 0) return 0;

    // Splice the applied map in before the closing brace.
    if (base < 1 || out[base - 1] != '}') {
        out[0] = '\0';
        return 0;
    }
    size_t used = base - 1;
    out[used] = '\0';

    bool ok = Append(out, out_len, &used, ",\"applied\":{");
    for (size_t i = 0; i < patch.size() && ok; ++i) {
        const FieldSpec& spec = SpecFor(patch.at(i).field);
        if (i > 0) ok = ok && Append(out, out_len, &used, ",");
        ok = ok && Append(out, out_len, &used, "\"");
        ok = ok && Append(out, out_len, &used, spec.dotted);
        ok = ok && Append(out, out_len, &used, "\":\"");
        ok = ok && Append(out, out_len, &used, ApplyModeName(spec.apply));
        ok = ok && Append(out, out_len, &used, "\"");
    }
    ok = ok && Append(out, out_len, &used, "}}");

    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

// ----------------------------------------------------------------- actions --

const char* ActionName(Action action) {
    switch (action) {
        case Action::kRestart: return "restart";
        case Action::kSleep:   return "sleep";
    }
    return "unknown";
}

const char* ActionConfirmation(Action action) {
    // The literal is the action's own name. A body that arrives at the wrong
    // route therefore cannot confirm the action it reached.
    return ActionName(action);
}

bool ParseAction(const char* name, Action* out) {
    if (name == nullptr || out == nullptr) return false;
    if (strcmp(name, "restart") == 0) { *out = Action::kRestart; return true; }
    if (strcmp(name, "sleep") == 0)   { *out = Action::kSleep;   return true; }
    return false;
}

const char* ActionErrorName(ActionError error) {
    switch (error) {
        case ActionError::kNone:                  return "none";
        case ActionError::kUnknownAction:         return "unknown_action";
        case ActionError::kMissingConfirmation:   return "missing_confirmation";
        case ActionError::kBadConfirmation:       return "bad_confirmation";
        case ActionError::kMissingIdempotencyKey: return "missing_idempotency_key";
        case ActionError::kNoRunner:              return "no_runner";
    }
    return "unknown";
}

ActionError ActionGate::Request(Action action, const char* confirm,
                                const char* idem_key, bool* replay_out) {
    if (replay_out != nullptr) *replay_out = false;

    if (confirm == nullptr) return ActionError::kMissingConfirmation;
    if (strcmp(confirm, ActionConfirmation(action)) != 0) {
        return ActionError::kBadConfirmation;
    }

    // Required, not optional. Without a key there is no way to tell a retry
    // from a second deliberate reboot, and the safe reading of that ambiguity
    // is to refuse rather than to guess.
    if (idem_key == nullptr) return ActionError::kMissingIdempotencyKey;
    const size_t key_len = strlen(idem_key);
    if (key_len == 0 || key_len >= dashboard::IdempotencyCache::kKeyMax) {
        return ActionError::kMissingIdempotencyKey;
    }

    if (idem_.Seen(idem_key, key_len)) {
        if (replay_out != nullptr) *replay_out = true;
        return ActionError::kNone;
    }

    if (!runner_) return ActionError::kNoRunner;

    idem_.Record(idem_key, key_len);
    scheduled_++;
    runner_(action, kDelayMs);
    return ActionError::kNone;
}

void ActionGate::Reset() {
    idem_.Clear();
    scheduled_ = 0;
}

// ------------------------------------------------------------ capabilities --

size_t RenderCapabilitiesJson(bool ptt_compiled, bool autonomy_compiled, char* out,
                              size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    bool ok = true;
    ok = ok && Append(out, out_len, &used,
                      "[\"dashboard.frame.v1\","
                      "\"dashboard.refresh.v1\","
                      "\"dashboard.pair.v1\","
                      "\"config.v2\","
                      "\"action.restart\","
                      "\"action.sleep\","
                      // The hybrid low-power contract: the power block on this
                      // route, the three power.* config fields, and a device
                      // that comes back on a timer rather than only on the
                      // button. A tower that does not see this string is
                      // talking to a build whose only sleep is one-way.
                      "\"power.hybrid.v1\","
                      "\"voice.hub.v1\"");
    if (autonomy_compiled) {
        // The autonomy contract: the two profile routes, the local render
        // action and the autonomy.enabled config field. A tower that does not
        // see this string is talking to a build that can only be pushed to, and
        // its UI says so structurally rather than offering controls that would
        // do nothing.
        ok = ok && Append(out, out_len, &used, ",\"autonomy.profile.v1\"");
    }
    if (ptt_compiled) {
        // Present only when the capture path is actually in the binary. A
        // build with VOICE_PTT_ENABLED=0 has no microphone code at all, and
        // saying so here is how the tower learns the difference between muted
        // and absent.
        ok = ok && Append(out, out_len, &used, ",\"voice.ptt.v1\"");
    }
    ok = ok && Append(out, out_len, &used, "]");
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

}  // namespace devcfg
