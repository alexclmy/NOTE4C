#!/usr/bin/env bash
# Build and run the host test suites against the real firmware sources.
#
# These compile main/common/*.cc directly with the host compiler. They need no
# ESP-IDF, no device and no network, so they run in a couple of seconds and are
# the fast feedback loop for the storage and refresh logic.
set -euo pipefail

FW_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${TEST_OUT_DIR:-${TMPDIR:-/tmp}/note4c-host-tests}"
mkdir -p "$OUT_DIR"

CXX="${CXX:-clang++}"
# For the one vendored C source. See the compile loop for why it is not handed
# to the C++ compiler.
CC="${CC:-clang}"
# Sanitizers on by default: these tests deliberately drive truncated reads,
# corrupted records and undersized buffers, which is exactly where an
# out-of-bounds access would hide.
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
          -fno-omit-frame-pointer -I "${FW_DIR}/main")
# The same sanitizers for the vendored C source, and deliberately without
# -Werror: it is third-party code kept byte-identical to upstream, and a warning
# in it is a thing to report upstream rather than a thing to patch here.
CFLAGS=(-std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined
        -fno-omit-frame-pointer -I "${FW_DIR}/main")
# Host stand-ins for the ESP-IDF headers an otherwise-portable source pulls in:
# esp_log.h (which has no host equivalent) and settings.h (the NVS wrapper,
# replaced by an in-memory map). Searched *before* main/, because shadowing
# main/settings.h is the whole point — it includes nvs_flash.h, which does not
# exist off-device. Keep this directory to exactly those two headers: anything
# else added here would silently shadow real firmware code under test.
SHIM_INCLUDE=(-I "${FW_DIR}/tests/host/shims")

# Entries are "name:sources" or "name:sources:extra compiler flags". The third
# field exists for exactly one suite — the one that has to reproduce the
# device's include path — and everything else leaves it off.
SUITES=(
    "test_dashboard_slot:main/common/dashboard_slot.cc"
    "test_dashboard_service:main/common/dashboard_service.cc main/common/dashboard_slot.cc"
    "test_nav_model:main/common/nav_model.cc"
    "test_button_gestures:main/common/button_gestures.cc"
    "test_audio_fsm:main/common/audio_fsm.cc main/common/earcon_gen.cc"
    "test_utterance_buffer:main/common/utterance_buffer.cc"
    "test_voice_transport:main/common/voice_transport.cc"
    "test_voice_hub_config:main/common/voice_hub_config.cc"
    "test_device_config:main/common/device_config.cc main/common/voice_hub_config.cc main/common/dashboard_service.cc main/common/dashboard_slot.cc main/common/power_policy.cc"
    "test_power_policy:main/common/power_policy.cc"
    # The battery ADC's owner. Driven from real threads under ASan/UBSan
    # against a fake that reproduces the oneshot driver's actual rules, so the
    # crash's shape — a second registration, and a read through a replaced
    # handle — is a use-after-free the sanitiser catches rather than an opinion.
    "test_battery_monitor:main/common/battery_monitor.cc"
    # device_config_service.cc reaches ESP-IDF in exactly two places — the log
    # macros and the NVS Settings wrapper — so tests/host/shims/ stands in for
    # both and the suite compiles the translation unit the firmware links
    # rather than a copy of its dispatch logic.
    "test_device_config_service:main/common/device_config_service.cc main/common/device_config.cc main/common/voice_hub_config.cc main/common/dashboard_service.cc main/common/dashboard_slot.cc main/common/power_policy.cc"
    # Header-only: settings_layout.h carries the Settings page arithmetic and no
    # rawdraw, LVGL or ESP-IDF type, so the geometry is testable without a panel.
    "test_settings_layout:"
    # The build-time feature gates. build_gate_on.cc is a second translation
    # unit that compiles the same header with CONFIG_AUTONOMY_ENABLED set, so
    # both halves of the derivation are a real compile rather than a copy of it.
    "test_build_gates:tests/host/build_gate_on.cc"
    # The other half of the same gate: not what an unconfigured build does, but
    # whether a build that *was* configured finds out. status_caps_gate.cc
    # compiles dashboard_build_config.h first in a translation unit with
    # ESP_PLATFORM set and a stand-in sdkconfig.h on the include path, then runs
    # StatusHandler's capability block over the real device_config.cc. The
    # include path is this suite's alone; see tests/host/target_config/.
    "test_status_capabilities:tests/host/status_caps_gate.cc main/common/device_config.cc main/common/voice_hub_config.cc main/common/dashboard_service.cc main/common/dashboard_slot.cc main/common/power_policy.cc:-I@FW@/tests/host/target_config"
    # The autonomy suites. json_scan.cc is the reader the profile and the
    # forecast parser both walk their documents with, so its refusals are tested
    # on their own before anything builds a schema on top of them. It was folded
    # into autonomy_profile.cc while the profile was its only consumer.
    "test_json_scan:main/common/json_scan.cc"
    "test_autonomy_profile:main/common/autonomy_profile.cc main/common/json_scan.cc main/common/record_slot.cc"
    "test_autonomy_service:main/common/autonomy_service.cc main/common/autonomy_profile.cc main/common/json_scan.cc main/common/record_slot.cc"
    # The one outbound origin, split so each half is driven where it can be.
    "test_openmeteo_parse:main/common/openmeteo_parse.cc main/common/json_scan.cc"
    "test_openmeteo_client:main/common/openmeteo_client.cc main/common/openmeteo_parse.cc main/common/json_scan.cc"
    # The whole-operation deadline, driven against a reproduction of ESP-IDF's
    # own read loops rather than against a fake that honours its budget. Header
    # only, so there is no .cc to link: the policy is the class, and the fake
    # SDK that exercises it lives in the suite.
    "test_http_deadline:"
    # The connect step of the same deadline: bounded name resolution against a
    # reproduction of lwIP's asynchronous resolver contract, driven with real
    # threads so the uncancellable late callback is exercised rather than
    # reasoned about, plus the resolve-then-connect sequencing on a scripted
    # clock. Header only, same reason as above.
    "test_bounded_connect:"
    # The durable forecast cache: the record that makes a wake with no
    # network still able to draw something true.
    "test_weather_cache:main/common/weather_cache.cc main/common/openmeteo_parse.cc main/common/json_scan.cc main/common/record_slot.cc"
    # The arbitration rule and the wake cycle's sequencing. Pure functions of
    # structs, swept over the whole input space. power_policy.cc is linked into
    # the cycle suite because the timeline test drives the real WakeBudget
    # through a whole wake rather than a stand-in for it.
    "test_autonomy_policy:main/common/autonomy_policy.cc"
    "test_autonomy_cycle:main/common/autonomy_cycle.cc main/common/autonomy_policy.cc main/common/autonomy_status.cc main/common/power_policy.cc"
    "test_autonomy_status:main/common/autonomy_status.cc main/common/autonomy_policy.cc"
    # The compositor suite is also the PRODUCER of the shared golden frames the
    # tower's TypeScript mirror is compared against. It runs from FW_DIR so the
    # default fixture path resolves; see the suite's header for regeneration.
    "test_autonomy_compose:main/common/autonomy_compose.cc main/common/autonomy_font_data.cc main/common/autonomy_profile.cc main/common/openmeteo_parse.cc main/common/json_scan.cc main/common/record_slot.cc"
    # The welcome screen's two drawing primitives. First entries in this file
    # that compile anything under main/rawdraw/: both are dependency-free —
    # rawdraw.cc's framebuffer primitives and the vendored qrcodegen — and
    # neither touches font_engine.h, which is what makes that possible.
    "test_qr_render:main/rawdraw/qr_render.cc main/rawdraw/qrcodegen.c main/rawdraw/rawdraw.cc"
    "test_octopus_mark:main/rawdraw/octopus_mark.cc main/rawdraw/rawdraw.cc"
    # The welcome screen's geometry and the two rules that matter most on it:
    # the QR's payload is built from an address, by this code, so there is no
    # input that could put a credential on a panel that keeps its image with the
    # power off; and no line of text is ever wider than the box it goes in,
    # which is what the lot 3 panel got wrong. octopus_mark.cc is linked because
    # the mascot's rectangle is one of the ones swept for that. The *drawing* of
    # text is still not covered — DrawText would pull in the CJK font component
    # and esp_heap_caps.h, which this harness does not link — but the suite does
    # read dashboard_renderer.cc's string literals back out of the source.
    "test_welcome_screen:main/rawdraw/welcome_screen.cc main/rawdraw/qr_render.cc main/rawdraw/qrcodegen.c main/rawdraw/octopus_mark.cc main/rawdraw/rawdraw.cc"
)

# A test that is defined but never registered in main() passes silently by not
# running. Catch that before running anything, and report the real counts so the
# numbers quoted elsewhere come from the code rather than from memory.
python3 - "$FW_DIR" <<'PYEOF'
import re, sys, pathlib
fw = pathlib.Path(sys.argv[1])
total = 0
bad = False
for f in sorted((fw / "tests/host").glob("test_*.cc")):
    t = f.read_text()
    defined = set(re.findall(r'^static void (test_\w+)\(\)', t, re.M))
    registered = set(re.findall(r'RUN\((test_\w+)\)', t))
    orphans = defined - registered
    ghosts = registered - defined
    if orphans:
        print(f"ERROR {f.name}: defined but never RUN(): {sorted(orphans)}")
        bad = True
    if ghosts:
        print(f"ERROR {f.name}: RUN() names with no definition: {sorted(ghosts)}")
        bad = True
    print(f"{f.name}: {len(registered)} test functions registered")
    total += len(registered)
print(f"TOTAL C++ test functions: {total}")
sys.exit(1 if bad else 0)
PYEOF
echo

status=0
for entry in "${SUITES[@]}"; do
    name="${entry%%:*}"
    rest="${entry#*:}"
    srcs="${rest%%:*}"
    extra=""
    [[ "$rest" == *:* ]] && extra="${rest#*:}"
    # Split the extra-flag field on whitespace BEFORE substituting @FW@, so a
    # project path containing spaces survives as one argument per flag.
    extra_flags=()
    for flag in $extra; do
        extra_flags+=("${flag//@FW@/${FW_DIR}}")
    done

    # A .c source is compiled as C, by the C compiler, into its own object.
    #
    # There is exactly one today — main/rawdraw/qrcodegen.c, vendored verbatim
    # from Nayuki — and the point of the split is that it stays that way. Handing
    # it to clang++ happens to work, and would give its symbols C++ linkage,
    # which would not match the `extern "C"` the caller declares them with. More
    # to the point: a vendored file is kept drop-in replaceable on purpose, and
    # a future upstream revision that is valid C but not valid C++ must not
    # break this suite.
    objs=()
    cxx_srcs=()
    for s in $srcs; do
        if [[ "$s" == *.c ]]; then
            obj="${OUT_DIR}/$(basename "${s%.c}").o"
            # shellcheck disable=SC2086
            "$CC" ${extra_flags[@]+"${extra_flags[@]}"} "${CFLAGS[@]}" -c "${FW_DIR}/${s}" -o "$obj"
            objs+=("$obj")
        else
            cxx_srcs+=("${FW_DIR}/${s}")
        fi
    done

    # shellcheck disable=SC2086
    "$CXX" "${SHIM_INCLUDE[@]}" ${extra_flags[@]+"${extra_flags[@]}"} "${CXXFLAGS[@]}" "${FW_DIR}/tests/host/${name}.cc" \
        ${cxx_srcs[@]+"${cxx_srcs[@]}"} ${objs[@]+"${objs[@]}"} \
        -o "${OUT_DIR}/${name}"
    # Run from the project root: the compositor suite reads and writes
    # tests/fixtures/autonomy/ by a path relative to it.
    if ! (cd "$FW_DIR" && "${OUT_DIR}/${name}"); then
        status=1
    fi
    echo
done

if [[ $status -eq 0 ]]; then
    echo "ALL HOST SUITES PASSED"
else
    echo "HOST SUITES FAILED" >&2
fi
exit $status
