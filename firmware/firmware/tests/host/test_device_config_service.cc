/**
 * @file test_device_config_service.cc
 * @brief Host tests for DeviceConfigService::ApplyPatch and the power hooks.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * device_config.cc was already host tested; the *service* around it was not,
 * and that is where the two power bugs this file pins actually lived. Its only
 * ESP-IDF dependencies are the log macros and the NVS `Settings` wrapper, both
 * of which tests/host/shims/ replaces, so the translation unit the firmware
 * links is the one compiled here.
 *
 * What this exists to prove
 * -------------------------
 *  1. **Changing the interactive window length is not a mode change.** A patch
 *     carrying only `power.interactive_min` must reach the length hook and
 *     must not reach the mode hook, because the mode hook closes an open
 *     window — so adjusting the length used to throw the user out of the
 *     window they were adjusting.
 *
 *  2. **`interactive` never survives into the stored configuration.** It is a
 *     live window with a deadline. The device's own NVS write already maps it
 *     to `auto_saver`, so a config route that kept "interactive" was promising
 *     a reader a mode the device would not be in after a reboot.
 */

#include "common/device_config_service.h"

// The host shim (tests/host/shims/settings.h), for Settings::Clear().
#include "settings.h"

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

#define RUN(fn)                                                    \
    do {                                                           \
        g_current_test = #fn;                                      \
        const int before = g_failures;                             \
        Reset();                                                   \
        fn();                                                      \
        std::printf("%-62s %s\n", #fn,                             \
                    (g_failures == before) ? "ok" : "FAILED");     \
    } while (0)

// ------------------------------------------------------- recording the hooks --

/**
 * @brief What the service asked the device to do.
 *
 * Recorded rather than acted on, because the question every test below asks is
 * "which hook fired, with what", and a fake that also simulated a window would
 * be re-testing power_policy.cc rather than the dispatch under test.
 */
namespace {

struct HookLog {
    int mode_calls = 0;
    std::string mode_arg;
    int32_t mode_interactive_arg = 0;

    int interactive_calls = 0;
    int32_t interactive_arg = 0;

    int wake_interval_calls = 0;
    int32_t wake_interval_arg = 0;
};

HookLog g_hooks;

void Reset() {
    g_hooks = HookLog{};
    Settings::Clear();

    Config initial;
    initial.gallery_slide_min = 5;
    initial.sync_interval = 30;
    initial.voice_muted = true;
    initial.dashboard_lockdown = true;
    initial.network_lan_service = true;
    initial.power_mode = "auto_saver";
    initial.power_interactive_min = 15;
    initial.power_wake_interval_min = 60;

    ConfigHooks hooks;
    hooks.apply_power_mode = [](const std::string& mode, int32_t interactive_min) {
        ++g_hooks.mode_calls;
        g_hooks.mode_arg = mode;
        g_hooks.mode_interactive_arg = interactive_min;
    };
    hooks.apply_interactive_minutes = [](int32_t minutes) {
        ++g_hooks.interactive_calls;
        g_hooks.interactive_arg = minutes;
    };
    hooks.apply_wake_interval = [](int32_t minutes) {
        ++g_hooks.wake_interval_calls;
        g_hooks.wake_interval_arg = minutes;
    };
    hooks.apply_lan_service = [](bool enabled) { return enabled; };

    DeviceConfigService::GetInstance().Init(initial, hooks);
}

DeviceConfigService& Service() { return DeviceConfigService::GetInstance(); }

uint32_t Revision() { return Service().Revision(); }

/// Apply a patch at the service's current revision. Returns the error, if any.
///
/// The revision is filled in here rather than by each test, because the
/// compare-and-swap is not what these tests are about — only
/// test_a_stale_revision_is_a_conflict_and_changes_nothing sets its own.
PatchError Apply(ConfigPatch& patch, Config* out = nullptr) {
    if (!patch.has_expected_revision()) patch.SetExpectedRevision(Revision());
    Config next;
    uint32_t revision = 0;
    std::string failed;
    const PatchError error = Service().ApplyPatch(patch, &failed, &next, &revision);
    if (out != nullptr) *out = next;
    return error;
}



}  // namespace

// ------------------------------------------- the window length, on its own --

/**
 * The defect, stated as a test: a patch that carries only the window length
 * must not be routed through the mode hook.
 *
 * The mode hook closes any open window — that is its job, "be auto_saver now"
 * has to mean now. So sending the length through it meant a user with a
 * thirty-minute window open who changed the length to sixty had their window
 * closed on the spot and the device dropped back into power saving underneath
 * them. Before the split this asserted mode_calls == 1.
 */
static void test_the_window_length_alone_does_not_reach_the_mode_hook() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("power.interactive_min", 60) == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kNone);

    CHECK(g_hooks.mode_calls == 0);
    CHECK(g_hooks.interactive_calls == 1);
    CHECK(g_hooks.interactive_arg == 60);
}

/**
 * A patch that carries the mode *and* the length is still one call, and still
 * carries the new length: applying the mode against the length it replaced is
 * how "interactive for thirty minutes" would open a fifteen-minute window.
 */
static void test_mode_and_length_together_are_one_call_with_the_new_length() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "interactive") == PatchError::kNone);
    CHECK(patch.AddInteger("power.interactive_min", 30) == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kNone);

    CHECK(g_hooks.mode_calls == 1);
    CHECK(g_hooks.mode_arg == "interactive");
    CHECK(g_hooks.mode_interactive_arg == 30);
    // Not also routed through the length-only hook: one patch, one decision.
    CHECK(g_hooks.interactive_calls == 0);
}

static void test_a_mode_alone_carries_the_stored_length() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "interactive") == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kNone);

    CHECK(g_hooks.mode_calls == 1);
    CHECK(g_hooks.mode_interactive_arg == 15);  // the value already stored
    CHECK(g_hooks.interactive_calls == 0);
}

static void test_the_wake_interval_has_its_own_hook_and_its_own_trigger() {
    ConfigPatch patch;
    CHECK(patch.AddInteger("power.wake_interval_min", 120) == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kNone);

    CHECK(g_hooks.wake_interval_calls == 1);
    CHECK(g_hooks.wake_interval_arg == 120);
    CHECK(g_hooks.mode_calls == 0);
    CHECK(g_hooks.interactive_calls == 0);
}

// ------------------------------------------------ interactive is not stored --

/**
 * The stored configuration holds the *base* mode: what the device comes back
 * as after a reboot.
 *
 * The device's NVS write already maps `interactive` to `auto_saver`, because a
 * window that came back from storage would be a window nobody opened. Leaving
 * "interactive" in the config the tower reads therefore made the config route
 * disagree with NVS and promise a mode the device would not be in.
 */
static void test_an_interactive_write_is_stored_as_the_base_mode() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "interactive") == PatchError::kNone);
    Config after;
    CHECK(Apply(patch, &after) == PatchError::kNone);

    // The hook still hears the real request, so a window does open...
    CHECK(g_hooks.mode_arg == "interactive");
    // ...but nothing that survives a reboot claims the device is interactive.
    CHECK(after.power_mode == "auto_saver");
    CHECK(Service().Snapshot().power_mode == "auto_saver");
}

static void test_the_normalised_mode_is_what_the_config_route_renders() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "interactive") == PatchError::kNone);
    CHECK(patch.AddInteger("power.interactive_min", 30) == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kNone);

    char buf[kConfigJsonMax];
    const size_t n =
        RenderConfigJson(Service().Snapshot(), Revision(), buf, sizeof(buf));
    CHECK(n > 0);
    const std::string json(buf);
    CHECK(json.find("\"mode\":\"auto_saver\"") != std::string::npos);
    CHECK(json.find("\"mode\":\"interactive\"") == std::string::npos);
    // The length the user chose is a setting and does survive.
    CHECK(json.find("\"interactive_min\":30") != std::string::npos);
}

static void test_the_other_two_modes_are_stored_as_written() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "always_on") == PatchError::kNone);
    Config after;
    CHECK(Apply(patch, &after) == PatchError::kNone);
    CHECK(after.power_mode == "always_on");
    CHECK(g_hooks.mode_arg == "always_on");

    ConfigPatch back;
    CHECK(back.AddString("power.mode", "auto_saver") == PatchError::kNone);
    Config later;
    CHECK(Apply(back, &later) == PatchError::kNone);
    CHECK(later.power_mode == "auto_saver");
}

// ------------------------------------------------------- the device's own --

/**
 * NotePowerMode is the device telling the service what it did on its own — the
 * button opened a window, or one expired. It records the base mode for the
 * same reason ApplyPatch normalises it.
 */
static void test_note_power_mode_records_the_base_and_moves_the_revision() {
    const uint32_t before = Revision();
    Service().NotePowerMode("interactive", 30);
    CHECK(Service().Snapshot().power_mode == "auto_saver");
    CHECK(Service().Snapshot().power_interactive_min == 30);
    CHECK(Revision() > before);
    // And it does not call the hook back: the device has already done it.
    CHECK(g_hooks.mode_calls == 0);
}

static void test_note_power_mode_is_quiet_when_nothing_changed() {
    Service().NotePowerMode("auto_saver", 15);
    const uint32_t after_first = Revision();
    Service().NotePowerMode("auto_saver", 15);
    CHECK(Revision() == after_first);
}

/**
 * NoteWakeInterval exists so a change the device made itself reaches the
 * config the tower reads. It was defined and never called by anything, which
 * meant exactly that could not happen.
 */
static void test_note_wake_interval_updates_the_config_and_bumps() {
    const uint32_t before = Revision();
    Service().NoteWakeInterval(240);
    CHECK(Service().Snapshot().power_wake_interval_min == 240);
    CHECK(Revision() > before);

    const uint32_t after = Revision();
    Service().NoteWakeInterval(240);
    CHECK(Revision() == after);
}

// ------------------------------------------------------------------- misc --

static void test_a_refused_patch_calls_no_hooks_at_all() {
    ConfigPatch patch;
    CHECK(patch.AddString("power.mode", "turbo") == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kOutOfRange);
    CHECK(g_hooks.mode_calls == 0);
    CHECK(g_hooks.interactive_calls == 0);
    CHECK(g_hooks.wake_interval_calls == 0);
}

static void test_a_stale_revision_is_a_conflict_and_changes_nothing() {
    ConfigPatch patch;
    patch.SetExpectedRevision(Revision() + 7);
    CHECK(patch.AddString("power.mode", "always_on") == PatchError::kNone);
    CHECK(Apply(patch) == PatchError::kRevisionMismatch);
    CHECK(Service().Snapshot().power_mode == "auto_saver");
    CHECK(g_hooks.mode_calls == 0);
}

int main() {
    std::printf("test_device_config_service\n");

    RUN(test_the_window_length_alone_does_not_reach_the_mode_hook);
    RUN(test_mode_and_length_together_are_one_call_with_the_new_length);
    RUN(test_a_mode_alone_carries_the_stored_length);
    RUN(test_the_wake_interval_has_its_own_hook_and_its_own_trigger);

    RUN(test_an_interactive_write_is_stored_as_the_base_mode);
    RUN(test_the_normalised_mode_is_what_the_config_route_renders);
    RUN(test_the_other_two_modes_are_stored_as_written);

    RUN(test_note_power_mode_records_the_base_and_moves_the_revision);
    RUN(test_note_power_mode_is_quiet_when_nothing_changed);
    RUN(test_note_wake_interval_updates_the_config_and_bumps);

    RUN(test_a_refused_patch_calls_no_hooks_at_all);
    RUN(test_a_stale_revision_is_a_conflict_and_changes_nothing);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
