/**
 * @file build_gate_on.cc
 * @brief One translation unit that compiles dashboard_build_config.h with the
 *        Kconfig symbol set, so the ON half of the gate is a real compile.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Deliberately not named test_*.cc: run.sh's registration scanner globs that
 * pattern and would report a suite with no tests in it. This is a fixture, and
 * the assertions about what it produces live in test_build_gates.cc.
 *
 * What this fixture asserts, and the correction to what it used to claim: a
 * `#define` before the include shows what the derivation *does* with the symbol
 * in scope. It does not show how the symbol gets into scope. ESP-IDF does not
 * force-include the generated sdkconfig.h ahead of every source — it puts the
 * generated config directory on the include path and leaves the including to
 * the code. The header therefore includes <sdkconfig.h> itself, and that part
 * is covered by test_status_capabilities.cc, which compiles it with no such
 * `#define` in front of it.
 */

#define CONFIG_AUTONOMY_ENABLED 1
#include "dashboard_build_config.h"

/// What AUTONOMY_COMPILED derives to when the Kconfig symbol is set.
int AutonomyCompiledWithKconfigSet() { return AUTONOMY_COMPILED; }

/// And VOICE_PTT_ENABLED in that same build, so the test can show that turning
/// autonomy on does not drag the microphone path in with it.
int VoicePttWithAutonomyKconfigSet() { return VOICE_PTT_ENABLED; }
