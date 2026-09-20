/**
 * @file status_caps_gate.cc
 * @brief The capability list of GET /api/v1/dashboard/status, derived exactly
 *        the way the firmware derives it, in a translation unit that reproduces
 *        the device's macro environment and nothing else.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Deliberately not named test_*.cc: run.sh's registration scanner globs that
 * pattern and would report a suite with no tests in it. This is the fixture;
 * the assertions live in test_status_capabilities.cc.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * test_device_config.cc already calls RenderCapabilitiesJson(false, true, ...)
 * and checks the string it returns. That pins the renderer, and pins nothing
 * about the device: it passes whatever the build gates evaluate to, because it
 * passes literals. The bug shape it cannot see is the gate itself resolving to
 * 0 on a device whose Kconfig says 1, which would take the capability out of
 * the status response with every test still green.
 *
 * So this file does what StatusHandler does, in the order StatusHandler does
 * it, with the macros rather than with literals:
 *
 *   - ESP_PLATFORM defined, as ESP-IDF defines it on every target compile;
 *   - <sdkconfig.h> reachable on the include path with CONFIG_AUTONOMY_ENABLED
 *     set, as the generated header is on a device built with the gate on;
 *   - dashboard_build_config.h included FIRST, with nothing in front of it that
 *     might have pulled sdkconfig.h in as a side effect. On the device that
 *     side effect is currently supplied by a chain of four unrelated headers
 *     starting at the force-included compat/cxx_math_compat.h. Here there is no
 *     such chain, so the header has to obtain its own config or get it wrong;
 *   - the same 256-byte buffer and the same `[]` fallback as the route.
 *
 * Compiled as part of the test_status_capabilities suite; see tests/host/run.sh.
 */

// What ESP-IDF's -DESP_PLATFORM does on every target compile.
#define ESP_PLATFORM 1

// First include of the translation unit, on purpose. Nothing above this line
// has defined CONFIG_AUTONOMY_ENABLED; if the header does not reach
// <sdkconfig.h> itself, AUTONOMY_COMPILED derives to 0 here.
#include "dashboard_build_config.h"

#include <stdio.h>
#include <string.h>

#include "common/device_config.h"

/// StatusHandler's capability block, copied structure for structure from
/// main/ui/renderers/rawdraw/dashboard_api.cc. The buffer size is part of the
/// copy: a list that outgrew 256 bytes would come back as `[]` on the device,
/// and that is a failure this fixture can therefore reproduce.
size_t RenderStatusCapabilities(char* out, size_t out_len) {
    char caps[256];
    if (devcfg::RenderCapabilitiesJson(VOICE_PTT_ENABLED != 0,
                                       AUTONOMY_COMPILED != 0, caps,
                                       sizeof(caps)) == 0) {
        snprintf(caps, sizeof(caps), "[]");
    }
    snprintf(out, out_len, "%s", caps);
    return strlen(caps);
}

/// The gates as this translation unit resolved them, so the test can say which
/// half failed when the string is wrong.
int TargetAutonomyCompiled() { return AUTONOMY_COMPILED; }
int TargetVoicePttEnabled() { return VOICE_PTT_ENABLED; }

/// The buffer the route actually gives the renderer, reported rather than
/// restated in the test.
///
/// The route names it `kStatusCapsMax` now that the buffer lives in a
/// heap-allocated block rather than on the handler's stack. This number is not
/// trusted to stay in step by itself: test_status_capabilities.cc reads the
/// constant back out of dashboard_api.cc and compares it to this one.
size_t StatusCapabilityBufferBytes() { return 256; }
