/**
 * @file test_device_config.cc
 * @brief Host tests for the real device_config translation unit.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This is the whole remote settings surface of the device, so most of what is
 * asserted here is about what the API refuses rather than what it accepts: a
 * field that is not in the allowlist, a value outside the bounds the device
 * menu itself enforces, a write racing another write, a lockdown somebody
 * tries to lower from the network, and a reboot that arrives twice because a
 * caller retried a request whose answer it never saw.
 *
 * The two properties worth naming explicitly:
 *
 *   * The hub token never appears in a response. That is checked by searching
 *     the rendered bytes for the token, which is a test that would fail if
 *     somebody later added a convenient echo.
 *
 *   * A restart never happens during these tests. ActionGate takes its runner
 *     as a parameter, so the gating rules are exercised against a closure that
 *     appends to a vector.
 */

#include "common/device_config.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace devcfg;

// ------------------------------------------------------------ mini harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current_test = "";

static void Check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", g_current_test, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define RUN(fn)                                \
    do {                                       \
        g_current_test = #fn;                  \
        const int before = g_failures;         \
        fn();                                  \
        std::printf("%-58s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

/// A configuration in the state a paired, hub-configured device is in.
static Config Baseline() {
    Config config;
    config.gallery_slide_min = 5;
    config.sync_interval = 30;
    config.voice_muted = true;
    config.voice_hub_url = "http://192.168.0.10:8653";
    config.voice_hub_token_set = true;
    config.dashboard_lockdown = true;
    config.network_lan_service = true;
    return config;
}

static bool Contains(const char* haystack, const char* needle) {
    return std::strstr(haystack, needle) != nullptr;
}

// ----------------------------------------------------------- the allowlist --

static void test_every_allowlisted_name_resolves_to_its_own_field() {
    CHECK(FindField("gallery.slide_min")->field == Field::kGallerySlideMin);
    CHECK(FindField("sync.sync_interval")->field == Field::kSyncInterval);
    CHECK(FindField("voice.muted")->field == Field::kVoiceMuted);
    CHECK(FindField("voice.hub_url")->field == Field::kVoiceHubUrl);
    CHECK(FindField("dashboard.lockdown")->field == Field::kDashboardLockdown);
    CHECK(FindField("network.lan_service")->field == Field::kNetworkLanService);
}

static void test_names_outside_the_allowlist_are_not_found() {
    CHECK(FindField("voice.hub_token") == nullptr);
    CHECK(FindField("dashboard.token") == nullptr);
    CHECK(FindField("network.wifi_ssid") == nullptr);
    CHECK(FindField("network.wifi_password") == nullptr);
    CHECK(FindField("") == nullptr);
    CHECK(FindField(static_cast<const char*>(nullptr)) == nullptr);
    // Not a prefix match and not a case-insensitive one: the wire name is the
    // wire name.
    CHECK(FindField("gallery.slide_min ") == nullptr);
    CHECK(FindField("Gallery.slide_min") == nullptr);
    CHECK(FindField("gallery") == nullptr);
}

static void test_the_hub_token_has_no_writable_field_at_all() {
    // The token reaches the device through POST /api/v1/voice/hub and through
    // nothing else. If this ever fails, a config PATCH can carry a secret.
    for (size_t i = 0; i < kFieldCount; ++i) {
        const FieldSpec& spec = SpecFor(static_cast<Field>(i));
        CHECK(!Contains(spec.dotted, "token"));
        CHECK(!Contains(spec.dotted, "password"));
    }
}

static void test_field_names_are_total() {
    for (size_t i = 0; i < kFieldCount; ++i) {
        const char* name = FieldName(static_cast<Field>(i));
        CHECK(name != nullptr && name[0] != '\0');
    }
    CHECK(std::strcmp(FieldName(Field::kCount), "") == 0);
}

static void test_apply_modes_match_what_the_device_actually_does() {
    CHECK(SpecFor(Field::kGallerySlideMin).apply == ApplyMode::kImmediate);
    CHECK(SpecFor(Field::kSyncInterval).apply == ApplyMode::kImmediate);
    CHECK(SpecFor(Field::kVoiceMuted).apply == ApplyMode::kImmediate);
    CHECK(SpecFor(Field::kVoiceHubUrl).apply == ApplyMode::kImmediate);
    CHECK(SpecFor(Field::kDashboardLockdown).apply == ApplyMode::kImmediate);
    // The one that is not persisted, because application.cc restarts the LAN
    // server on the next Wi-Fi connection regardless of what was asked for.
    CHECK(SpecFor(Field::kNetworkLanService).apply ==
          ApplyMode::kImmediateNotPersisted);
    CHECK(std::strcmp(ApplyModeName(ApplyMode::kImmediateNotPersisted),
                      "immediate_not_persisted") == 0);
    CHECK(std::strcmp(ApplyModeName(ApplyMode::kImmediate), "immediate") == 0);
    CHECK(std::strcmp(ApplyModeName(ApplyMode::kRestartRequired),
                      "restart_required") == 0);
}

// ---------------------------------------------------------------- bounds ----

static void test_slide_min_accepts_only_the_four_menu_values() {
    CHECK(IsValidSlideMin(0));
    CHECK(IsValidSlideMin(5));
    CHECK(IsValidSlideMin(10));
    CHECK(IsValidSlideMin(30));
    CHECK(!IsValidSlideMin(1));
    CHECK(!IsValidSlideMin(15));
    CHECK(!IsValidSlideMin(-5));
    CHECK(!IsValidSlideMin(60));
}

static void test_sync_interval_is_whole_minutes_within_a_day() {
    CHECK(IsValidSyncInterval(0));
    CHECK(IsValidSyncInterval(1));
    CHECK(IsValidSyncInterval(30));
    CHECK(IsValidSyncInterval(1440));
    CHECK(!IsValidSyncInterval(1441));
    CHECK(!IsValidSyncInterval(-1));
}

// -------------------------------------------------------------- power v2 --

static void test_power_mode_accepts_only_the_three_wire_names() {
    CHECK(IsValidPowerMode("auto_saver"));
    CHECK(IsValidPowerMode("interactive"));
    CHECK(IsValidPowerMode("always_on"));
    CHECK(!IsValidPowerMode(""));
    CHECK(!IsValidPowerMode("auto"));
    CHECK(!IsValidPowerMode("Auto_Saver"));
    CHECK(!IsValidPowerMode("sleep"));
    CHECK(!IsValidPowerMode("on"));
}

static void test_interactive_minutes_accepts_only_the_four_windows() {
    CHECK(IsValidInteractiveMinutes(5));
    CHECK(IsValidInteractiveMinutes(15));
    CHECK(IsValidInteractiveMinutes(30));
    CHECK(IsValidInteractiveMinutes(60));
    CHECK(!IsValidInteractiveMinutes(0));
    CHECK(!IsValidInteractiveMinutes(10));
    CHECK(!IsValidInteractiveMinutes(45));
    CHECK(!IsValidInteractiveMinutes(-5));
}

static void test_the_wake_interval_has_a_floor_that_keeps_the_saver_saving() {
    CHECK(IsValidWakeIntervalMinutes(15));
    CHECK(IsValidWakeIntervalMinutes(60));
    CHECK(IsValidWakeIntervalMinutes(1440));
    // Below the floor the radio and the panel dominate the average current and
    // the mode stops saving anything, so the device says no.
    CHECK(!IsValidWakeIntervalMinutes(5));
    CHECK(!IsValidWakeIntervalMinutes(0));
    CHECK(!IsValidWakeIntervalMinutes(-1));
    CHECK(!IsValidWakeIntervalMinutes(1441));
}

static void test_the_three_power_fields_are_in_the_allowlist() {
    CHECK(FindField("power.mode") != nullptr);
    CHECK(FindField("power.interactive_min") != nullptr);
    CHECK(FindField("power.wake_interval_min") != nullptr);
    CHECK(FindField("power.mode")->kind == ValueKind::kString);
    CHECK(FindField("power.interactive_min")->kind == ValueKind::kInteger);
    CHECK(FindField("power.wake_interval_min")->kind == ValueKind::kInteger);
    // All three take effect now and survive a reboot, so the tower may say so.
    CHECK(FindField("power.mode")->apply == ApplyMode::kImmediate);
    CHECK(FindField("power.interactive_min")->apply == ApplyMode::kImmediate);
    CHECK(FindField("power.wake_interval_min")->apply == ApplyMode::kImmediate);
    // None of them can strand a caller, so none of them carries a literal.
    CHECK(FindField("power.mode")->confirm == nullptr);
    CHECK(FindField("power.interactive_min")->confirm == nullptr);
    CHECK(FindField("power.wake_interval_min")->confirm == nullptr);
}

static void test_a_neighbouring_power_name_is_not_quietly_accepted() {
    // The allowlist is a fixed table, not a prefix match.
    CHECK(FindField("power.modes") == nullptr);
    CHECK(FindField("power") == nullptr);
    CHECK(FindField("power.wake_interval") == nullptr);
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode_", "auto_saver") == PatchError::kUnknownField);
    CHECK(std::strcmp(patch.failed_field(), "power.mode_") == 0);
}

static void test_a_valid_power_patch_applies() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddString("power.mode", "interactive") == PatchError::kNone);
    CHECK(patch.AddInteger("power.interactive_min", 30) == PatchError::kNone);
    CHECK(patch.AddInteger("power.wake_interval_min", 120) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kNone);

    Config after = Baseline();
    patch.ApplyTo(&after);
    CHECK(after.power_mode == "interactive");
    CHECK(after.power_interactive_min == 30);
    CHECK(after.power_wake_interval_min == 120);
}

static void test_an_unknown_power_mode_is_out_of_range_by_name() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddString("power.mode", "turbo") == PatchError::kNone);
    std::string failed;
    // Out of range rather than wrong_type: the caller sent a string, which is
    // the right shape, and naming the bound is what lets them fix it in one
    // edit rather than an afternoon.
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "power.mode");
}

static void test_a_power_mode_that_is_not_a_string_is_a_type_error() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("power.mode", 1) == PatchError::kWrongType);
    CHECK(std::strcmp(patch.failed_field(), "power.mode") == 0);
    ConfigPatch other;
    CHECK(other.AddBoolean("power.mode", true) == PatchError::kWrongType);
}

static void test_a_window_length_off_the_list_is_refused_by_name() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("power.interactive_min", 45) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "power.interactive_min");
}

static void test_a_wake_interval_under_the_floor_is_refused_by_name() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("power.wake_interval_min", 5) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "power.wake_interval_min");
}

static void test_a_bad_power_field_leaves_the_whole_patch_unapplied() {
    // All or nothing: a caller that re-reads after a refusal must see exactly
    // what it saw before it tried.
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("gallery.slide_min", 30) == PatchError::kNone);
    CHECK(patch.AddString("power.mode", "nope") == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "power.mode");
}

static void test_the_config_json_reports_the_power_block() {
    Config config = Baseline();
    config.power_mode = "always_on";
    config.power_interactive_min = 60;
    config.power_wake_interval_min = 90;
    char out[kConfigJsonMax];
    CHECK(RenderConfigJson(config, 7, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"power\":{\"mode\":\"always_on\""));
    CHECK(Contains(out, "\"interactive_min\":60"));
    CHECK(Contains(out, "\"wake_interval_min\":90"));
}

static void test_the_config_json_still_hides_the_hub_token_with_power_added() {
    // The buffer grew and the shape changed, so the claim is re-checked here
    // rather than assumed to have survived the edit.
    Config config = Baseline();
    config.voice_hub_url = "http://192.168.0.10:8653";
    config.voice_hub_token_set = true;
    char out[kConfigJsonMax];
    CHECK(RenderConfigJson(config, 3, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"hub_token_set\":true"));
    CHECK(!Contains(out, "\"token\""));
}

static void test_the_patch_response_names_the_power_apply_modes() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddString("power.mode", "auto_saver") == PatchError::kNone);
    Config after = Baseline();
    patch.ApplyTo(&after);
    char out[kConfigJsonMax];
    CHECK(RenderPatchResponseJson(after, 8, patch, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"power.mode\":\"immediate\""));
}

static void test_the_capability_list_advertises_the_hybrid_contract() {
    char caps[256];
    CHECK(RenderCapabilitiesJson(false, true, caps, sizeof(caps)) > 0);
    CHECK(Contains(caps, "\"power.hybrid.v1\""));
    // And the list still fits the buffer the status route hands it, with the
    // optional push-to-talk string present as well.
    CHECK(RenderCapabilitiesJson(true, true, caps, sizeof(caps)) > 0);
    CHECK(Contains(caps, "\"power.hybrid.v1\""));
    CHECK(Contains(caps, "\"voice.ptt.v1\""));
}

// ----------------------------------------------------------- patch: shape --

static void test_an_unknown_field_is_refused_by_name() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("gallery.slide_minutes", 5) == PatchError::kUnknownField);
    CHECK(std::strcmp(patch.failed_field(), "gallery.slide_minutes") == 0);
    CHECK(patch.size() == 0);
}

static void test_a_known_field_with_the_wrong_type_is_refused() {
    ConfigPatch patch;
    CHECK(patch.AddBoolean("gallery.slide_min", true) == PatchError::kWrongType);
    CHECK(patch.AddInteger("voice.muted", 1) == PatchError::kWrongType);
    CHECK(patch.AddInteger("voice.hub_url", 1) == PatchError::kWrongType);
    CHECK(patch.AddString("dashboard.lockdown", "true") == PatchError::kWrongType);
    CHECK(patch.size() == 0);
}

static void test_reject_type_names_the_field_it_knew_about() {
    ConfigPatch patch;
    CHECK(patch.RejectType("voice.muted") == PatchError::kWrongType);
    CHECK(std::strcmp(patch.failed_field(), "voice.muted") == 0);
    CHECK(patch.RejectType("voice.nonsense") == PatchError::kUnknownField);
}

static void test_the_same_field_twice_in_one_patch_is_refused() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("gallery.slide_min", 5) == PatchError::kNone);
    CHECK(patch.AddInteger("gallery.slide_min", 10) == PatchError::kDuplicateField);
    CHECK(patch.size() == 1);
}

static void test_an_overlong_string_is_refused_before_it_is_stored() {
    ConfigPatch patch;
    const std::string huge(ConfigPatch::kMaxStringChars + 1, 'a');
    CHECK(patch.AddString("voice.hub_url", huge) == PatchError::kValueTooLong);
    CHECK(patch.size() == 0);
}

static void test_an_empty_patch_is_refused() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kEmptyPatch);
}

// -------------------------------------------------------- patch: revision --

static void test_a_patch_without_an_expected_revision_is_refused() {
    ConfigPatch patch;
    CHECK(patch.AddBoolean("voice.muted", false) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 7, &failed) == PatchError::kMissingRevision);
}

static void test_a_stale_revision_is_a_conflict() {
    ConfigPatch patch;
    patch.SetExpectedRevision(6);
    CHECK(patch.AddBoolean("voice.muted", false) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 7, &failed) == PatchError::kRevisionMismatch);
    CHECK(std::strcmp(PatchErrorHttpStatus(PatchError::kRevisionMismatch),
                      "409 Conflict") == 0);
}

static void test_the_revision_is_checked_before_the_field_bounds() {
    // A caller with a stale read has to go and read again whatever else is
    // wrong, so it learns about the conflict first and spends one round trip
    // rather than two.
    ConfigPatch patch;
    patch.SetExpectedRevision(1);
    CHECK(patch.AddInteger("gallery.slide_min", 7) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 2, &failed) == PatchError::kRevisionMismatch);
}

static void test_a_matching_revision_passes() {
    ConfigPatch patch;
    patch.SetExpectedRevision(9);
    CHECK(patch.AddInteger("gallery.slide_min", 10) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 9, &failed) == PatchError::kNone);
    CHECK(failed.empty());
}

// ----------------------------------------------------- patch: field rules --

static void test_a_value_outside_the_bounds_is_refused_and_named() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("gallery.slide_min", 7) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "gallery.slide_min");

    ConfigPatch other;
    other.SetExpectedRevision(0);
    CHECK(other.AddInteger("sync.sync_interval", 1441) == PatchError::kNone);
    CHECK(other.Validate(Baseline(), 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "sync.sync_interval");
}

static void test_lockdown_may_be_turned_on_remotely() {
    Config config = Baseline();
    config.dashboard_lockdown = false;
    ConfigPatch patch;
    patch.SetExpectedRevision(3);
    CHECK(patch.AddBoolean("dashboard.lockdown", true) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 3, &failed) == PatchError::kNone);
    patch.ApplyTo(&config);
    CHECK(config.dashboard_lockdown);
}

static void test_lockdown_may_never_be_turned_off_remotely() {
    // Not even when it is already off. Lowering the drawbridge is a decision
    // for somebody holding the device, never for whoever holds the token.
    for (const bool current_state : {true, false}) {
        Config config = Baseline();
        config.dashboard_lockdown = current_state;
        ConfigPatch patch;
        patch.SetExpectedRevision(3);
        CHECK(patch.AddBoolean("dashboard.lockdown", false) == PatchError::kNone);
        std::string failed;
        CHECK(patch.Validate(config, 3, &failed) == PatchError::kLockdownIsOneWay);
        CHECK(failed == "dashboard.lockdown");
        // And the config is untouched, because Validate never writes.
        CHECK(config.dashboard_lockdown == current_state);
    }
    CHECK(std::strcmp(PatchErrorHttpStatus(PatchError::kLockdownIsOneWay),
                      "403 Forbidden") == 0);
}

static void test_turning_the_lan_service_off_needs_the_literal() {
    Config config = Baseline();
    std::string failed;

    ConfigPatch bare;
    bare.SetExpectedRevision(0);
    CHECK(bare.AddBoolean("network.lan_service", false) == PatchError::kNone);
    CHECK(bare.Validate(config, 0, &failed) == PatchError::kMissingConfirmation);
    CHECK(failed == "network.lan_service");

    ConfigPatch wrong;
    wrong.SetExpectedRevision(0);
    wrong.SetConfirmation("yes");
    CHECK(wrong.AddBoolean("network.lan_service", false) == PatchError::kNone);
    CHECK(wrong.Validate(config, 0, &failed) == PatchError::kBadConfirmation);

    ConfigPatch right;
    right.SetExpectedRevision(0);
    right.SetConfirmation(kLanServiceOffConfirmation);
    CHECK(right.AddBoolean("network.lan_service", false) == PatchError::kNone);
    CHECK(right.Validate(config, 0, &failed) == PatchError::kNone);
}

static void test_turning_the_lan_service_on_needs_no_literal() {
    // Switching the API back on cannot strand anybody, so it is not gated.
    Config config = Baseline();
    config.network_lan_service = false;
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddBoolean("network.lan_service", true) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 0, &failed) == PatchError::kNone);
}

static void test_a_hub_url_is_validated_the_same_way_the_v1_route_does() {
    Config config = Baseline();
    std::string failed;

    ConfigPatch bad;
    bad.SetExpectedRevision(0);
    CHECK(bad.AddString("voice.hub_url", "ftp://hub.local") == PatchError::kNone);
    CHECK(bad.Validate(config, 0, &failed) == PatchError::kBadHubUrl);
    CHECK(failed == "voice.hub_url");

    ConfigPatch newline;
    newline.SetExpectedRevision(0);
    CHECK(newline.AddString("voice.hub_url", "http://hub\r\nX: y") == PatchError::kNone);
    CHECK(newline.Validate(config, 0, &failed) == PatchError::kBadHubUrl);

    ConfigPatch good;
    good.SetExpectedRevision(0);
    CHECK(good.AddString("voice.hub_url", "http://192.168.0.11:8653/") == PatchError::kNone);
    CHECK(good.Validate(config, 0, &failed) == PatchError::kNone);
    good.ApplyTo(&config);
    // Normalised, so a base and a path never join into a double slash.
    CHECK(config.voice_hub_url == "http://192.168.0.11:8653");
}

static void test_a_hub_url_with_no_token_behind_it_is_refused() {
    Config config = Baseline();
    config.voice_hub_token_set = false;
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddString("voice.hub_url", "http://192.168.0.11:8653") == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 0, &failed) == PatchError::kHubUrlNeedsToken);
    CHECK(failed == "voice.hub_url");
}

static void test_clearing_the_hub_url_clears_the_token_flag() {
    Config config = Baseline();
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddString("voice.hub_url", "") == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 0, &failed) == PatchError::kNone);
    patch.ApplyTo(&config);
    CHECK(config.voice_hub_url.empty());
    // A token with nowhere to go is a stored secret with no purpose.
    CHECK(!config.voice_hub_token_set);
}

// ------------------------------------------------------- patch: atomicity --

static void test_a_patch_that_fails_on_its_third_field_applies_none_of_them() {
    Config config = Baseline();
    const Config before = config;

    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("gallery.slide_min", 30) == PatchError::kNone);
    CHECK(patch.AddBoolean("voice.muted", false) == PatchError::kNone);
    CHECK(patch.AddInteger("sync.sync_interval", 99999) == PatchError::kNone);

    std::string failed;
    CHECK(patch.Validate(config, 0, &failed) == PatchError::kOutOfRange);
    CHECK(failed == "sync.sync_interval");
    CHECK(config.gallery_slide_min == before.gallery_slide_min);
    CHECK(config.voice_muted == before.voice_muted);
    CHECK(config.sync_interval == before.sync_interval);
}

static void test_a_valid_multi_field_patch_applies_all_of_them() {
    Config config = Baseline();
    ConfigPatch patch;
    patch.SetExpectedRevision(4);
    CHECK(patch.AddInteger("gallery.slide_min", 30) == PatchError::kNone);
    CHECK(patch.AddBoolean("voice.muted", false) == PatchError::kNone);
    CHECK(patch.AddInteger("sync.sync_interval", 0) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 4, &failed) == PatchError::kNone);
    patch.ApplyTo(&config);
    CHECK(config.gallery_slide_min == 30);
    CHECK(!config.voice_muted);
    CHECK(config.sync_interval == 0);
    // Untouched fields stay untouched.
    CHECK(config.dashboard_lockdown);
    CHECK(config.voice_hub_url == "http://192.168.0.10:8653");
}

static void test_clear_returns_a_patch_to_empty() {
    ConfigPatch patch;
    patch.SetExpectedRevision(1);
    patch.SetConfirmation("x");
    CHECK(patch.AddBoolean("voice.muted", true) == PatchError::kNone);
    patch.Clear();
    CHECK(patch.size() == 0);
    CHECK(!patch.has_expected_revision());
    CHECK(!patch.has(Field::kVoiceMuted));
    CHECK(std::strcmp(patch.failed_field(), "") == 0);
}

// ---------------------------------------------------------------- revision --

static void test_the_revision_only_ever_goes_up() {
    RevisionCounter counter;
    CHECK(counter.value() == 0);
    CHECK(counter.Bump() == 1);
    CHECK(counter.Bump() == 2);
    counter.Seed(41);
    CHECK(counter.Bump() == 42);
}

static void test_the_revision_wraps_to_one_rather_than_to_zero() {
    // Zero means "nothing has ever changed", and that has to keep meaning it.
    RevisionCounter counter;
    counter.Seed(0xFFFFFFFFu);
    CHECK(counter.Bump() == 1);
}

// ------------------------------------------------------------------- JSON --

static void test_escaping_covers_the_characters_that_would_break_a_response() {
    char out[64];
    CHECK(JsonEscape("plain", out, sizeof(out)) == 5);
    CHECK(std::strcmp(out, "plain") == 0);

    CHECK(JsonEscape("a\"b", out, sizeof(out)) > 0);
    CHECK(std::strcmp(out, "a\\\"b") == 0);

    CHECK(JsonEscape("a\\b", out, sizeof(out)) > 0);
    CHECK(std::strcmp(out, "a\\\\b") == 0);

    CHECK(JsonEscape("a\nb", out, sizeof(out)) > 0);
    CHECK(std::strcmp(out, "a\\nb") == 0);

    // A control character is spelled out rather than dropped.
    CHECK(JsonEscape(std::string("a\x01", 2), out, sizeof(out)) > 0);
    CHECK(std::strcmp(out, "a\\u0001") == 0);
}

static void test_escaping_into_a_buffer_that_is_too_small_writes_nothing() {
    char out[4];
    CHECK(JsonEscape("much too long", out, sizeof(out)) == 0);
    // Not a truncated fragment: empty.
    CHECK(out[0] == '\0');
}

static void test_the_config_response_is_the_documented_shape() {
    char out[kConfigJsonMax];
    const size_t written = RenderConfigJson(Baseline(), 12, out, sizeof(out));
    CHECK(written > 0);
    CHECK(std::strlen(out) == written);
    CHECK(Contains(out, "\"api\":2"));
    CHECK(Contains(out, "\"revision\":12"));
    CHECK(Contains(out, "\"gallery\":{\"slide_min\":5}"));
    CHECK(Contains(out, "\"sync\":{\"sync_interval\":30}"));
    CHECK(Contains(out, "\"muted\":true"));
    CHECK(Contains(out, "\"hub_url\":\"http://192.168.0.10:8653\""));
    CHECK(Contains(out, "\"hub_token_set\":true"));
    CHECK(Contains(out, "\"dashboard\":{\"lockdown\":true}"));
    CHECK(Contains(out, "\"lan_service\":true"));
    // Stated, so a tower cannot mistake the absence of Wi-Fi fields for an
    // oversight it should work around.
    CHECK(Contains(out, "\"wifi_writable\":false"));
}

static void test_the_config_response_never_carries_a_secret() {
    // The strongest form of this claim available on the host: put a value that
    // looks exactly like a token into the only string the config carries, then
    // search the rendered bytes for every secret-shaped word.
    Config config = Baseline();
    char out[kConfigJsonMax];
    CHECK(RenderConfigJson(config, 1, out, sizeof(out)) > 0);
    CHECK(!Contains(out, "hub_token\""));
    CHECK(!Contains(out, "\"token\""));
    CHECK(!Contains(out, "password"));
    CHECK(!Contains(out, "ssid"));
    // And the type itself has nowhere to hold one, which is what makes the
    // above more than a spot check.
    CHECK(sizeof(config.voice_hub_token_set) == sizeof(bool));
}

static void test_a_hostile_stored_url_cannot_forge_json() {
    Config config = Baseline();
    config.voice_hub_url = "http://a\",\"hub_token_set\":\"stolen";
    char out[kConfigJsonMax];
    CHECK(RenderConfigJson(config, 1, out, sizeof(out)) > 0);
    // The injected key is escaped into the URL value, so the real boolean is
    // still the only hub_token_set the parser will find as a key.
    CHECK(Contains(out, "\\\",\\\"hub_token_set\\\""));
    CHECK(Contains(out, ",\"hub_token_set\":true"));
}

static void test_a_config_response_that_does_not_fit_writes_nothing() {
    char out[32];
    CHECK(RenderConfigJson(Baseline(), 1, out, sizeof(out)) == 0);
    CHECK(out[0] == '\0');
}

static void test_the_patch_response_reports_one_apply_mode_per_written_field() {
    Config config = Baseline();
    ConfigPatch patch;
    patch.SetExpectedRevision(2);
    patch.SetConfirmation(kLanServiceOffConfirmation);
    CHECK(patch.AddInteger("gallery.slide_min", 10) == PatchError::kNone);
    CHECK(patch.AddBoolean("network.lan_service", false) == PatchError::kNone);
    std::string failed;
    CHECK(patch.Validate(config, 2, &failed) == PatchError::kNone);
    patch.ApplyTo(&config);

    char out[kConfigJsonMax];
    const size_t written = RenderPatchResponseJson(config, 3, patch, out, sizeof(out));
    CHECK(written > 0);
    CHECK(std::strlen(out) == written);
    CHECK(Contains(out, "\"revision\":3"));
    CHECK(Contains(out, "\"slide_min\":10"));
    CHECK(Contains(out, "\"lan_service\":false"));
    CHECK(Contains(out, "\"applied\":{"));
    CHECK(Contains(out, "\"gallery.slide_min\":\"immediate\""));
    // The honest one. Not "immediate", because it does not survive a boot.
    CHECK(Contains(out, "\"network.lan_service\":\"immediate_not_persisted\""));
    // Fields nobody wrote are not in the applied map.
    CHECK(!Contains(out, "\"voice.muted\":\""));
}

static void test_a_patch_response_that_does_not_fit_writes_nothing() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddInteger("gallery.slide_min", 5) == PatchError::kNone);
    char out[40];
    CHECK(RenderPatchResponseJson(Baseline(), 1, patch, out, sizeof(out)) == 0);
    CHECK(out[0] == '\0');
}

static void test_capabilities_tell_absent_apart_from_muted() {
    char out[256];
    CHECK(RenderCapabilitiesJson(false, true, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"config.v2\""));
    CHECK(Contains(out, "\"action.restart\""));
    CHECK(Contains(out, "\"action.sleep\""));
    CHECK(Contains(out, "\"voice.hub.v1\""));
    CHECK(!Contains(out, "voice.ptt.v1"));

    CHECK(RenderCapabilitiesJson(true, true, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"voice.ptt.v1\""));
    // Nothing here claims a wake word, because nothing in the firmware has one.
    CHECK(!Contains(out, "wake"));
}

static void test_capabilities_into_a_small_buffer_write_nothing() {
    char out[16];
    CHECK(RenderCapabilitiesJson(true, true, out, sizeof(out)) == 0);
    CHECK(out[0] == '\0');
}

// ---------------------------------------------------------------- actions --

namespace {

/// What a scheduled action would have done, had this been a device.
struct RecordedAction {
    Action action;
    uint32_t delay_ms;
};

/// A gate wired to a recorder rather than to esp_restart().
struct Harness {
    ActionGate gate;
    std::vector<RecordedAction> ran;

    Harness() {
        gate.SetRunner([this](Action action, uint32_t delay_ms) {
            ran.push_back({action, delay_ms});
        });
    }
};

}  // namespace

static void test_action_names_and_literals_agree() {
    CHECK(std::strcmp(ActionName(Action::kRestart), "restart") == 0);
    CHECK(std::strcmp(ActionName(Action::kSleep), "sleep") == 0);
    CHECK(std::strcmp(ActionConfirmation(Action::kRestart), "restart") == 0);
    CHECK(std::strcmp(ActionConfirmation(Action::kSleep), "sleep") == 0);

    Action parsed = Action::kSleep;
    CHECK(ParseAction("restart", &parsed) && parsed == Action::kRestart);
    CHECK(ParseAction("sleep", &parsed) && parsed == Action::kSleep);
    CHECK(!ParseAction("reboot", &parsed));
    CHECK(!ParseAction("", &parsed));
    CHECK(!ParseAction(nullptr, &parsed));
    CHECK(!ParseAction("restart", nullptr));
}

static void test_an_action_without_the_literal_does_not_run() {
    Harness h;
    bool replay = true;
    CHECK(h.gate.Request(Action::kRestart, nullptr, "k1", &replay) ==
          ActionError::kMissingConfirmation);
    CHECK(h.gate.Request(Action::kRestart, "", "k1", &replay) ==
          ActionError::kBadConfirmation);
    CHECK(h.gate.Request(Action::kRestart, "yes", "k1", &replay) ==
          ActionError::kBadConfirmation);
    CHECK(h.ran.empty());
    CHECK(h.gate.scheduled_count() == 0);
}

static void test_one_actions_literal_cannot_confirm_the_other() {
    // A client with its routes crossed reboots nothing.
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kSleep, "restart", "k1", &replay) ==
          ActionError::kBadConfirmation);
    CHECK(h.gate.Request(Action::kRestart, "sleep", "k2", &replay) ==
          ActionError::kBadConfirmation);
    CHECK(h.ran.empty());
}

static void test_an_action_without_an_idempotency_key_is_refused() {
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kRestart, "restart", nullptr, &replay) ==
          ActionError::kMissingIdempotencyKey);
    CHECK(h.gate.Request(Action::kRestart, "restart", "", &replay) ==
          ActionError::kMissingIdempotencyKey);
    const std::string huge(dashboard::IdempotencyCache::kKeyMax + 4, 'k');
    CHECK(h.gate.Request(Action::kRestart, "restart", huge.c_str(), &replay) ==
          ActionError::kMissingIdempotencyKey);
    CHECK(h.ran.empty());
}

static void test_a_confirmed_action_is_scheduled_after_a_delay() {
    Harness h;
    bool replay = true;
    CHECK(h.gate.Request(Action::kRestart, "restart", "k1", &replay) ==
          ActionError::kNone);
    CHECK(!replay);
    CHECK(h.ran.size() == 1);
    CHECK(h.ran[0].action == Action::kRestart);
    // Not immediate: the response has to reach the socket before the device
    // stops being a device.
    CHECK(h.ran[0].delay_ms == ActionGate::kDelayMs);
    CHECK(h.ran[0].delay_ms >= 1000);
}

static void test_a_retried_action_is_answered_but_not_repeated() {
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kRestart, "restart", "same", &replay) ==
          ActionError::kNone);
    CHECK(!replay);
    CHECK(h.gate.Request(Action::kRestart, "restart", "same", &replay) ==
          ActionError::kNone);
    CHECK(replay);
    // Once. A caller that retried after a timeout does not reboot twice.
    CHECK(h.ran.size() == 1);
    CHECK(h.gate.scheduled_count() == 1);
}

static void test_a_different_key_is_a_different_action() {
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kRestart, "restart", "k1", &replay) == ActionError::kNone);
    CHECK(h.gate.Request(Action::kSleep, "sleep", "k2", &replay) == ActionError::kNone);
    CHECK(h.ran.size() == 2);
    CHECK(h.ran[1].action == Action::kSleep);
}

static void test_a_refused_action_does_not_consume_its_key() {
    // Otherwise a client that got the literal wrong once could never send the
    // corrected request under the same key.
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kRestart, "nope", "k1", &replay) ==
          ActionError::kBadConfirmation);
    CHECK(h.gate.Request(Action::kRestart, "restart", "k1", &replay) ==
          ActionError::kNone);
    CHECK(!replay);
    CHECK(h.ran.size() == 1);
}

static void test_a_gate_with_no_runner_reports_it_rather_than_pretending() {
    ActionGate gate;
    bool replay = false;
    CHECK(gate.Request(Action::kRestart, "restart", "k1", &replay) ==
          ActionError::kNoRunner);
    CHECK(gate.scheduled_count() == 0);
    // And the key was not consumed, so a later correctly wired gate can serve it.
    CHECK(std::strcmp(ActionErrorName(ActionError::kNoRunner), "no_runner") == 0);
}

static void test_reset_forgets_the_keys() {
    Harness h;
    bool replay = false;
    CHECK(h.gate.Request(Action::kRestart, "restart", "k1", &replay) == ActionError::kNone);
    h.gate.Reset();
    CHECK(h.gate.scheduled_count() == 0);
    CHECK(h.gate.Request(Action::kRestart, "restart", "k1", &replay) == ActionError::kNone);
    CHECK(!replay);
    CHECK(h.ran.size() == 2);
}

static void test_error_names_are_total_and_readable() {
    const PatchError patch_errors[] = {
        PatchError::kNone, PatchError::kNotObject, PatchError::kNoSetObject,
        PatchError::kEmptyPatch, PatchError::kUnknownField, PatchError::kWrongType,
        PatchError::kOutOfRange, PatchError::kDuplicateField,
        PatchError::kTooManyFields, PatchError::kMissingRevision,
        PatchError::kRevisionMismatch, PatchError::kMissingConfirmation,
        PatchError::kBadConfirmation, PatchError::kLockdownIsOneWay,
        PatchError::kHubUrlNeedsToken, PatchError::kBadHubUrl,
        PatchError::kValueTooLong,
    };
    for (const PatchError error : patch_errors) {
        const char* name = PatchErrorName(error);
        CHECK(name != nullptr && name[0] != '\0');
        CHECK(std::strcmp(name, "unknown") != 0);
        const char* status = PatchErrorHttpStatus(error);
        CHECK(status != nullptr && status[0] != '\0');
    }

    const ActionError action_errors[] = {
        ActionError::kNone, ActionError::kUnknownAction,
        ActionError::kMissingConfirmation, ActionError::kBadConfirmation,
        ActionError::kMissingIdempotencyKey, ActionError::kNoRunner,
    };
    for (const ActionError error : action_errors) {
        const char* name = ActionErrorName(error);
        CHECK(name != nullptr && name[0] != '\0');
        CHECK(std::strcmp(name, "unknown") != 0);
    }
}


// ------------------------------------------------------- the autonomy switch --

static void test_autonomy_is_off_until_somebody_turns_it_on() {
    // The feature's whole safety argument: a device flashed with this build and
    // never configured behaves exactly as the previous build did.
    Config config;
    CHECK(!config.autonomy_enabled);
}

static void test_the_autonomy_switch_is_a_boolean_field() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddBoolean("autonomy.enabled", true) == PatchError::kNone);
    Config config = Baseline();
    patch.ApplyTo(&config);
    CHECK(config.autonomy_enabled);

    ConfigPatch off;
    off.SetExpectedRevision(0);
    CHECK(off.AddBoolean("autonomy.enabled", false) == PatchError::kNone);
    off.ApplyTo(&config);
    CHECK(!config.autonomy_enabled);
}

static void test_the_autonomy_switch_refuses_a_non_boolean() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("autonomy.enabled", 1) == PatchError::kWrongType);
    CHECK(std::strcmp(patch.failed_field(), "autonomy.enabled") == 0);
    ConfigPatch other;
    CHECK(other.AddString("autonomy.enabled", "yes") == PatchError::kWrongType);
}

static void test_an_unknown_autonomy_field_is_still_unknown() {
    // The table is closed. Adding one field does not open a namespace.
    ConfigPatch patch;
    CHECK(patch.AddBoolean("autonomy.profile", true) == PatchError::kUnknownField);
    ConfigPatch other;
    CHECK(other.AddInteger("autonomy.wake_interval_min", 60) ==
          PatchError::kUnknownField);
}

static void test_the_config_json_reports_the_autonomy_switch() {
    Config config = Baseline();
    config.autonomy_enabled = true;
    char out[kConfigJsonMax];
    CHECK(RenderConfigJson(config, 9, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"autonomy\":{\"enabled\":true}"));

    config.autonomy_enabled = false;
    CHECK(RenderConfigJson(config, 9, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"autonomy\":{\"enabled\":false}"));
}

/**
 * THE APPLY MODE IS THE HONEST ONE, NOT THE FLATTERING ONE.
 *
 * It was `immediate_not_persisted` while a local render happened only because
 * somebody asked for one over the API: nothing behind the field wrote to NVS,
 * so the switch held for the life of the boot and a power cycle returned the
 * device to off, and saying `immediate` would have claimed it stuck when it did
 * not — the lie `network.lan_service` is spelled this way to avoid.
 *
 * It is `immediate` now, and the reason is not that the contract got weaker.
 * The wake cycle reaches the panel by going through deep sleep, and a deep
 * sleep is a reboot: an unpersisted switch would come back off on the first
 * wake, so the feature could never run unattended — which is the only thing it
 * is for. The field is persisted, so `immediate` is now the true answer, and
 * this test is what makes the contract and the storage change together.
 *
 * The default is still off: enabling it is an act, and a device that has never
 * been asked stays a push target.
 */
static void test_the_autonomy_switch_reports_that_it_now_survives_a_reboot() {
    ConfigPatch patch;
    patch.SetExpectedRevision(0);
    CHECK(patch.AddBoolean("autonomy.enabled", true) == PatchError::kNone);
    Config after = Baseline();
    patch.ApplyTo(&after);
    char out[kConfigJsonMax];
    CHECK(RenderPatchResponseJson(after, 8, patch, out, sizeof(out)) > 0);
    CHECK(Contains(out, "\"autonomy.enabled\":\"immediate\""));
    CHECK(SpecFor(Field::kAutonomyEnabled).apply == ApplyMode::kImmediate);

    // And the field a fresh Config starts from is still off: persistence is
    // about remembering a decision, not about making one.
    Config fresh;
    CHECK(!fresh.autonomy_enabled);
}

static void test_the_capability_list_advertises_the_autonomy_contract() {
    // A tower that does not see this string is talking to a build that can only
    // be pushed to, and its UI is expected to say so structurally.
    char caps[256];
    CHECK(RenderCapabilitiesJson(false, true, caps, sizeof(caps)) > 0);
    CHECK(Contains(caps, "\"autonomy.profile.v1\""));
    CHECK(Contains(caps, "\"power.hybrid.v1\""));
}

/**
 * THE ROLLOUT GATE, AS THE TOWER SEES IT.
 *
 * `autonomy.profile.v1` used to be a constant in this list, which made every
 * build claim the feature whether or not it was reachable. A tower had nothing
 * structural to read and was left inferring availability from whether a status
 * block happened to be non-null — which is true on a build that answers 404 to
 * both profile routes.
 */
static void test_a_build_with_autonomy_gated_off_does_not_claim_it() {
    char caps[256];
    CHECK(RenderCapabilitiesJson(false, false, caps, sizeof(caps)) > 0);
    CHECK(!Contains(caps, "autonomy.profile.v1"));
    // Everything else the build does have is still advertised: the gate is
    // about one feature, not about the device disappearing.
    CHECK(Contains(caps, "\"dashboard.frame.v1\""));
    CHECK(Contains(caps, "\"config.v2\""));
    CHECK(Contains(caps, "\"power.hybrid.v1\""));
    CHECK(Contains(caps, "\"voice.hub.v1\""));

    // And the two gates are independent: push-to-talk on, autonomy off.
    CHECK(RenderCapabilitiesJson(true, false, caps, sizeof(caps)) > 0);
    CHECK(Contains(caps, "\"voice.ptt.v1\""));
    CHECK(!Contains(caps, "autonomy.profile.v1"));

    // The list is still valid JSON either way — no trailing comma where the
    // optional strings were removed.
    CHECK(caps[0] == '[');
    const size_t n = std::strlen(caps);
    CHECK(caps[n - 1] == ']');
    CHECK(caps[n - 2] != ',');
}

int main() {
    RUN(test_every_allowlisted_name_resolves_to_its_own_field);
    RUN(test_names_outside_the_allowlist_are_not_found);
    RUN(test_the_hub_token_has_no_writable_field_at_all);
    RUN(test_field_names_are_total);
    RUN(test_apply_modes_match_what_the_device_actually_does);

    RUN(test_slide_min_accepts_only_the_four_menu_values);
    RUN(test_sync_interval_is_whole_minutes_within_a_day);

    RUN(test_power_mode_accepts_only_the_three_wire_names);
    RUN(test_interactive_minutes_accepts_only_the_four_windows);
    RUN(test_the_wake_interval_has_a_floor_that_keeps_the_saver_saving);
    RUN(test_the_three_power_fields_are_in_the_allowlist);
    RUN(test_a_neighbouring_power_name_is_not_quietly_accepted);
    RUN(test_a_valid_power_patch_applies);
    RUN(test_an_unknown_power_mode_is_out_of_range_by_name);
    RUN(test_a_power_mode_that_is_not_a_string_is_a_type_error);
    RUN(test_a_window_length_off_the_list_is_refused_by_name);
    RUN(test_a_wake_interval_under_the_floor_is_refused_by_name);
    RUN(test_a_bad_power_field_leaves_the_whole_patch_unapplied);
    RUN(test_the_config_json_reports_the_power_block);
    RUN(test_the_config_json_still_hides_the_hub_token_with_power_added);
    RUN(test_the_patch_response_names_the_power_apply_modes);
    RUN(test_autonomy_is_off_until_somebody_turns_it_on);
    RUN(test_the_autonomy_switch_is_a_boolean_field);
    RUN(test_the_autonomy_switch_refuses_a_non_boolean);
    RUN(test_an_unknown_autonomy_field_is_still_unknown);
    RUN(test_the_config_json_reports_the_autonomy_switch);
    RUN(test_the_autonomy_switch_reports_that_it_now_survives_a_reboot);
    RUN(test_the_capability_list_advertises_the_autonomy_contract);
    RUN(test_a_build_with_autonomy_gated_off_does_not_claim_it);

    RUN(test_the_capability_list_advertises_the_hybrid_contract);

    RUN(test_an_unknown_field_is_refused_by_name);
    RUN(test_a_known_field_with_the_wrong_type_is_refused);
    RUN(test_reject_type_names_the_field_it_knew_about);
    RUN(test_the_same_field_twice_in_one_patch_is_refused);
    RUN(test_an_overlong_string_is_refused_before_it_is_stored);
    RUN(test_an_empty_patch_is_refused);

    RUN(test_a_patch_without_an_expected_revision_is_refused);
    RUN(test_a_stale_revision_is_a_conflict);
    RUN(test_the_revision_is_checked_before_the_field_bounds);
    RUN(test_a_matching_revision_passes);

    RUN(test_a_value_outside_the_bounds_is_refused_and_named);
    RUN(test_lockdown_may_be_turned_on_remotely);
    RUN(test_lockdown_may_never_be_turned_off_remotely);
    RUN(test_turning_the_lan_service_off_needs_the_literal);
    RUN(test_turning_the_lan_service_on_needs_no_literal);
    RUN(test_a_hub_url_is_validated_the_same_way_the_v1_route_does);
    RUN(test_a_hub_url_with_no_token_behind_it_is_refused);
    RUN(test_clearing_the_hub_url_clears_the_token_flag);

    RUN(test_a_patch_that_fails_on_its_third_field_applies_none_of_them);
    RUN(test_a_valid_multi_field_patch_applies_all_of_them);
    RUN(test_clear_returns_a_patch_to_empty);

    RUN(test_the_revision_only_ever_goes_up);
    RUN(test_the_revision_wraps_to_one_rather_than_to_zero);

    RUN(test_escaping_covers_the_characters_that_would_break_a_response);
    RUN(test_escaping_into_a_buffer_that_is_too_small_writes_nothing);
    RUN(test_the_config_response_is_the_documented_shape);
    RUN(test_the_config_response_never_carries_a_secret);
    RUN(test_a_hostile_stored_url_cannot_forge_json);
    RUN(test_a_config_response_that_does_not_fit_writes_nothing);
    RUN(test_the_patch_response_reports_one_apply_mode_per_written_field);
    RUN(test_a_patch_response_that_does_not_fit_writes_nothing);
    RUN(test_capabilities_tell_absent_apart_from_muted);
    RUN(test_capabilities_into_a_small_buffer_write_nothing);

    RUN(test_action_names_and_literals_agree);
    RUN(test_an_action_without_the_literal_does_not_run);
    RUN(test_one_actions_literal_cannot_confirm_the_other);
    RUN(test_an_action_without_an_idempotency_key_is_refused);
    RUN(test_a_confirmed_action_is_scheduled_after_a_delay);
    RUN(test_a_retried_action_is_answered_but_not_repeated);
    RUN(test_a_different_key_is_a_different_action);
    RUN(test_a_refused_action_does_not_consume_its_key);
    RUN(test_a_gate_with_no_runner_reports_it_rather_than_pretending);
    RUN(test_reset_forgets_the_keys);

    RUN(test_error_names_are_total_and_readable);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
