/**
 * @file test_build_gates.cc
 * @brief Host tests for the build-time feature gates in dashboard_build_config.h.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHY A TEST FOR ONE #ifdef
 * -------------------------
 * Because the default is the safety argument, and a default is exactly the kind
 * of thing that changes by accident. `AUTONOMY_COMPILED` decides whether this
 * firmware can compose a panel for itself at all: off, the profile store is
 * never built, the two profile routes and the local render route answer 404
 * `autonomy_unsupported`, and the capability list omits `autonomy.profile.v1`
 * so a tower's controls are absent rather than inert.
 *
 * All of that follows from one derivation — `CONFIG_AUTONOMY_ENABLED` defined,
 * or not — and nothing else in the host suite compiles that derivation. An
 * `#ifdef` inverted in a merge would turn a feature on across the fleet with no
 * test noticing, because every other suite either has the macro or does not
 * care.
 *
 * So the derivation is compiled twice, for real, in two translation units: this
 * one, with nothing defined, which is a device nobody configured; and
 * build_gate_on.cc, which defines the symbol the way ESP-IDF's generated
 * sdkconfig.h does.
 *
 * What this file deliberately does NOT assert: what the routes then do.
 * `httpd_req_t` does not exist off-device. The link from
 * `AUTONOMY_COMPILED == 0` to a 404 is one early return in
 * InitialiseAutonomy() leaving the profile store null and each route's first
 * statement refusing on exactly that — both in
 * main/ui/renderers/rawdraw/dashboard_api.cc. The capability half of the same
 * claim is host-tested in test_device_config.cc
 * (test_a_build_with_autonomy_gated_off_does_not_claim_it).
 */

#include <cstdio>

// The header the firmware compiles, with no Kconfig macro in sight.
#include "dashboard_build_config.h"

// The same header, in a translation unit that does have the symbol.
int AutonomyCompiledWithKconfigSet();
int VoicePttWithAutonomyKconfigSet();

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

// ---------------------------------------------------------------- the gate --

/**
 * The default, and the only claim that actually ships today: a build nobody
 * configured has autonomy switched off.
 */
static void test_autonomy_is_off_in_a_build_nobody_configured() {
    CHECK(AUTONOMY_COMPILED == 0);
}

/// And the Kconfig symbol is what turns it on — checked against a translation
/// unit that really was compiled with the symbol, not against a copy of the
/// derivation written out again here.
static void test_the_kconfig_symbol_is_what_turns_it_on() {
    CHECK(AutonomyCompiledWithKconfigSet() == 1);
    CHECK(AUTONOMY_COMPILED != AutonomyCompiledWithKconfigSet());
}

/**
 * The gates are independent. Turning autonomy on must not drag the microphone
 * path in with it: the two risky features answer to separate decisions, and an
 * operator enabling one would otherwise silently get the other.
 */
static void test_the_feature_gates_do_not_share_a_switch() {
    CHECK(VOICE_PTT_ENABLED == 0);
    CHECK(VoicePttWithAutonomyKconfigSet() == 0);
    CHECK(DASHBOARD_MINIMAL_UI == 1);
}

/// Every gate is a 0/1 integer rather than a defined-ness test, so
/// `#if AUTONOMY_COMPILED` and `AUTONOMY_COMPILED != 0` agree. Call sites use
/// both spellings.
static void test_the_gates_are_integers_not_definedness() {
    CHECK(AUTONOMY_COMPILED == 0 || AUTONOMY_COMPILED == 1);
    CHECK(VOICE_PTT_ENABLED == 0 || VOICE_PTT_ENABLED == 1);
    CHECK(DASHBOARD_MINIMAL_UI == 0 || DASHBOARD_MINIMAL_UI == 1);
#if AUTONOMY_COMPILED
    CHECK(false);   // unreachable in the default build; see the test above
#else
    CHECK(AUTONOMY_COMPILED == 0);
#endif
}

int main() {
    std::printf("build gate host tests (real dashboard_build_config.h)\n\n");

    RUN(test_autonomy_is_off_in_a_build_nobody_configured);
    RUN(test_the_kconfig_symbol_is_what_turns_it_on);
    RUN(test_the_feature_gates_do_not_share_a_switch);
    RUN(test_the_gates_are_integers_not_definedness);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
