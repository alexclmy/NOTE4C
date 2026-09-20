# PROVENANCE-MANIFEST — note4c-fw-v1

Produced 2026-09-18 for NOTE4C Lot 1 (executor: Opus; director/reviewer: Fable).

## Status of this manifest

This manifest is **complete and verified for the delta itself**, but it was produced
**outside** a git repository: `git` is refused by this environment's permission layer
(see `../note4c-fable-architecture/lot1-report.md`, section "Refus de permission").
No commit exists yet. The delta below is therefore expressed as a **file-level tree
comparison** between the two source trees that commit 1 and commit 2 are defined to
carry:

| Planned commit | Content source | Realised? |
|---|---|---|
| 1 — `upstream youn-ink-fourcolor-firmware 2bp @ 51812e4a (copy)` | `/Users/marvin/projects/tools/note4c-firmware/upstream/` | NO (git refused) |
| 2 — `as-installed snapshot from dirty worktree, 2026-09-18` | `/Users/marvin/projects/tools/note4c-firmware-increments/` | NO (git refused) |
| 3 — this manifest | this file + `provenance-manifest.json` | files written, not committed |
| 4 — `CONFIG_APP_REPRODUCIBLE_BUILD=y` | patch specified below | NO (git refused) |

See `REPLAY-LOT1.sh` in this directory for the exact, reviewed command sequence that
reconstructs the four commits verbatim once `git` and recursive copy are permitted.

## Handoff addendum — 2026-09-18 (supersedes the "Realised? NO" column above)

The four commits **were realised** later the same day, in a session where `git` was
permitted, by executing `REPLAY-LOT1.sh` (with the amendments commented
`Fable amendment` in that script):

| Planned commit | Realised as |
|---|---|
| 1 — upstream copy | `9cbd354a` |
| 2 — as-installed snapshot | `400172c3` |
| 3 — this manifest | `1fbc5c94` |
| 4 — reproducible build flag | `0fc16745` |

The T4 patch (`CONFIG_APP_REPRODUCIBLE_BUILD=y`) **is applied** in `0fc16745`.
SHA-256 of the build artifacts was also computed (T6, twice per variant, identical):
voice-off `189ae2cfa8ee0f2cd07a231c0225b76828e460216f6670d78b403b8409166173`,
voice-on `f02218486fb77d817baf025a0329be1b60be235fccff2b13d69665d890e1cfaa`
— see `lot1-hashes/`. Per-file hashes remain byte sizes only, as below.
Full run evidence: `lot1-report.md` §EXÉCUTION in the architecture dossier
(`note4c-fable-architecture/`, kept outside this repo); build logs stay local
(`lot1-logs/`, gitignored).

## Baselines

- **Commit 1 baseline (upstream)**: `/Users/marvin/projects/tools/note4c-firmware/upstream/`
  at `51812e4ab3fa80ba7a5a5a274635ca2cf3901a25`, `2026-08-22T14:13:51+08:00`,
  *"docs: remove internal remote reference"*
  (source of truth: `/Users/marvin/projects/tools/note4c-firmware/PROVENANCE.txt`; the
  short SHA `51812e4a` in the Lot 1 brief matches). The SHA could **not** be independently
  re-derived here because `git` is refused.
- **Commit 2 baseline (as-installed)**: `/Users/marvin/projects/tools/note4c-firmware-increments/`
  — a dirty git worktree, read **strictly read-only**. Its `.git` is a 4 KB worktree
  pointer file, not a repository.

## Comparison method

`diff -rq <upstream> <increments>` with exclusions `-x .git -x 'build-*' -x build -x __pycache__ -x managed_components`,
then `find -type f -ls` to expand directory-only differences and to read byte sizes.

Exclusions and why:

| Excluded | Reason |
|---|---|
| `.git` | per brief |
| `firmware/build-hardware-lot1`, `-lot2`, `-lot2-capability-fix`, `-lot3`, `-lot3-final-rc`, `-lot3-network-welcome-fix`, `-lot3-status-stack-fix`, `-lot3-welcome-fix`, `build-source-baseline`, `build-source-baseline-s3` | ESP-IDF build outputs (~3.3 GB), per brief |
| `firmware/managed_components` | 465 MB of ESP-IDF fetched components; **gitignored** by the repo's own `.gitignore` line 43, so it can never appear in commit 1 or 2 |
| `__pycache__` | gitignored; present only in upstream (`firmware/tests/__pycache__`) |

`firmware/build.sh` and `firmware/build_windows.ps1` are **source files**, not build
outputs, and were deliberately kept in scope.

## Delta summary (commit 1 → commit 2)

| Class | Count |
|---|---|
| Added | 74 |
| Modified | 49 |
| Deleted | **0** |
| **Total changed paths** | **123** |

There are **no deletions**: every file present in the upstream baseline (outside the
exclusions above) is still present in the as-installed snapshot.

One caveat on trackability: `firmware/sdkconfig` (listed below as modified) is
**gitignored** (`.gitignore` line 41). It will not enter commit 1 or commit 2 unless
force-added. The tracked delta is therefore 122 paths; the filesystem delta is 123.

## Hashes

**SHA-256 per file could not be computed**: `shasum` is refused by the permission layer
(verbatim refusal recorded in the Lot 1 report). Per the brief's fallback
("*sinon tailles octets*"), **byte sizes** are given instead. No substitute hashing tool
was used, because substituting for a refused capability would be circumventing the refusal.

---

## Added (74) — path, bytes

### firmware (1)
| Path | Bytes |
|---|---|
| `firmware/THIRD_PARTY_NOTICES.md` | 9856 |

### firmware/main/common (29)
| Path | Bytes |
|---|---|
| `firmware/main/common/autonomy_compose.cc` | 45899 |
| `firmware/main/common/autonomy_compose.h` | 9340 |
| `firmware/main/common/autonomy_cycle.cc` | 12178 |
| `firmware/main/common/autonomy_cycle.h` | 13295 |
| `firmware/main/common/autonomy_font_data.cc` | 227633 |
| `firmware/main/common/autonomy_font_data.h` | 2412 |
| `firmware/main/common/autonomy_policy.cc` | 6269 |
| `firmware/main/common/autonomy_policy.h` | 8313 |
| `firmware/main/common/autonomy_profile.cc` | 43951 |
| `firmware/main/common/autonomy_profile.h` | 13319 |
| `firmware/main/common/autonomy_service.cc` | 11585 |
| `firmware/main/common/autonomy_service.h` | 12672 |
| `firmware/main/common/autonomy_status.cc` | 9635 |
| `firmware/main/common/autonomy_status.h` | 6089 |
| `firmware/main/common/battery_monitor.cc` | 6192 |
| `firmware/main/common/battery_monitor.h` | 9983 |
| `firmware/main/common/bounded_connect.h` | 13218 |
| `firmware/main/common/http_deadline.h` | 5318 |
| `firmware/main/common/json_scan.cc` | 17116 |
| `firmware/main/common/json_scan.h` | 10328 |
| `firmware/main/common/openmeteo_client.cc` | 12095 |
| `firmware/main/common/openmeteo_client.h` | 12923 |
| `firmware/main/common/openmeteo_parse.cc` | 22912 |
| `firmware/main/common/openmeteo_parse.h` | 7457 |
| `firmware/main/common/openmeteo_transport_esp.cc` | 25482 |
| `firmware/main/common/record_slot.cc` | 13249 |
| `firmware/main/common/record_slot.h` | 10788 |
| `firmware/main/common/weather_cache.cc` | 2496 |
| `firmware/main/common/weather_cache.h` | 4879 |

### firmware/main/rawdraw (8)
| Path | Bytes |
|---|---|
| `firmware/main/rawdraw/octopus_mark.cc` | 4852 |
| `firmware/main/rawdraw/octopus_mark.h` | 5629 |
| `firmware/main/rawdraw/qr_render.cc` | 4493 |
| `firmware/main/rawdraw/qr_render.h` | 3537 |
| `firmware/main/rawdraw/qrcodegen.c` | 45513 |
| `firmware/main/rawdraw/qrcodegen.h` | 14420 |
| `firmware/main/rawdraw/welcome_screen.cc` | 6443 |
| `firmware/main/rawdraw/welcome_screen.h` | 14132 |

### firmware/tests/fixtures (15, whole directory is new)
| Path | Bytes |
|---|---|
| `firmware/tests/fixtures/autonomy/editorial-conditional-fallback.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-conditional-true.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-full.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-message.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-no-clock.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-no-forecast.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-no-provenance.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-stale-weather.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/editorial-unavailable-weather.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/flow-degraded.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/flow-full.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/flow-message.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/focus-countdown-only.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/focus-full.bin` | 30000 |
| `firmware/tests/fixtures/autonomy/scenarios.json` | 15869 |

> NOTE for whoever replays the commits: the repo `.gitignore` line 60 is `*.bin`.
> These 14 fixtures are test inputs, not build artefacts, and **must** be force-added
> (`git add -f firmware/tests/fixtures`) or the `.gitignore` must be narrowed, otherwise
> the host suites that read them will not be reproducible from the canonical repo.

### firmware/tests/host (21)
| Path | Bytes |
|---|---|
| `firmware/tests/host/build_gate_on.cc` | 1522 |
| `firmware/tests/host/status_caps_gate.cc` | 3781 |
| `firmware/tests/host/target_config/sdkconfig.h` | 1244 |
| `firmware/tests/host/test_autonomy_compose.cc` | 31993 |
| `firmware/tests/host/test_autonomy_cycle.cc` | 39152 |
| `firmware/tests/host/test_autonomy_policy.cc` | 16014 |
| `firmware/tests/host/test_autonomy_profile.cc` | 41226 |
| `firmware/tests/host/test_autonomy_service.cc` | 36967 |
| `firmware/tests/host/test_autonomy_status.cc` | 19052 |
| `firmware/tests/host/test_battery_monitor.cc` | 29167 |
| `firmware/tests/host/test_bounded_connect.cc` | 28476 |
| `firmware/tests/host/test_build_gates.cc` | 4982 |
| `firmware/tests/host/test_http_deadline.cc` | 22889 |
| `firmware/tests/host/test_json_scan.cc` | 23792 |
| `firmware/tests/host/test_octopus_mark.cc` | 14205 |
| `firmware/tests/host/test_openmeteo_client.cc` | 28839 |
| `firmware/tests/host/test_openmeteo_parse.cc` | 22181 |
| `firmware/tests/host/test_qr_render.cc` | 11795 |
| `firmware/tests/host/test_status_capabilities.cc` | 18662 |
| `firmware/tests/host/test_weather_cache.cc` | 13316 |
| `firmware/tests/host/test_welcome_screen.cc` | 45789 |

---

## Modified (49) — path, bytes upstream → bytes as-installed

| Path | Upstream | As-installed | Δ |
|---|---:|---:|---:|
| `firmware/docs/CONFIG_API.md` | 15563 | 18804 | +3241 |
| `firmware/docs/DASHBOARD_API.md` | 9845 | 22204 | +12359 |
| `firmware/docs/HARDWARE-ACCEPTANCE.md` | 13074 | 13053 | -21 |
| `firmware/docs/MANUAL.md` | 16976 | 16978 | +2 |
| `firmware/docs/POWER.md` | 17950 | 29561 | +11611 |
| `firmware/docs/PROVISIONING.md` | 3779 | 3781 | +2 |
| `firmware/docs/STORAGE.md` | 5839 | 10604 | +4765 |
| `firmware/docs/VOICE-FEASIBILITY.md` | 6678 | 6674 | -4 |
| `firmware/main/CMakeLists.txt` | 7055 | 10779 | +3724 |
| `firmware/main/Kconfig.projbuild` | 1432 | 2755 | +1323 |
| `firmware/main/application.cc` | 96119 | 146464 | +50345 |
| `firmware/main/application.h` | 14357 | 25637 | +11280 |
| `firmware/main/boards/zectrix-s3-epaper-4.2/charge_status.cc` | 3649 | 4156 | +507 |
| `firmware/main/boards/zectrix-s3-epaper-4.2/charge_status.h` | 1425 | 1754 | +329 |
| `firmware/main/boards/zectrix-s3-epaper-4.2/custom_lcd_display.cc` | 53572 | 55484 | +1912 |
| `firmware/main/boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc` | 23810 | 26809 | +2999 |
| `firmware/main/common/dashboard_manager.cc` | 21698 | 28943 | +7245 |
| `firmware/main/common/dashboard_manager.h` | 8356 | 13769 | +5413 |
| `firmware/main/common/dashboard_service.cc` | 8833 | 10754 | +1921 |
| `firmware/main/common/dashboard_service.h` | 12471 | 19028 | +6557 |
| `firmware/main/common/dashboard_slot.cc` | 11423 | 11623 | +200 |
| `firmware/main/common/dashboard_slot.h` | 7849 | 9767 | +1918 |
| `firmware/main/common/device_config.cc` | 28005 | 30596 | +2591 |
| `firmware/main/common/device_config.h` | 17157 | 18848 | +1691 |
| `firmware/main/common/device_config_service.cc` | 12585 | 12732 | +147 |
| `firmware/main/common/device_config_service.h` | 7058 | 7477 | +419 |
| `firmware/main/common/nav_model.cc` | 9892 | 10132 | +240 |
| `firmware/main/common/nav_model.h` | 11919 | 13748 | +1829 |
| `firmware/main/common/power_policy.cc` | 20933 | 23685 | +2752 |
| `firmware/main/common/power_policy.h` | 26345 | 35723 | +9378 |
| `firmware/main/common/voice_uploader.h` | 6465 | 6467 | +2 |
| `firmware/main/dashboard_build_config.h` | 3122 | 7088 | +3966 |
| `firmware/main/product_identity.h` | 2245 | 2245 | 0 (same size, different content) |
| `firmware/main/ui/rawdraw_ui_manager.cc` | 84916 | 85935 | +1019 |
| `firmware/main/ui/rawdraw_ui_manager.h` | 24221 | 24670 | +449 |
| `firmware/main/ui/renderers/rawdraw/ap_transfer_server.cc` | 50887 | 58038 | +7151 |
| `firmware/main/ui/renderers/rawdraw/config_api.cc` | 15963 | 16789 | +826 |
| `firmware/main/ui/renderers/rawdraw/dashboard_api.cc` | 24443 | 49448 | +25005 |
| `firmware/main/ui/renderers/rawdraw/dashboard_renderer.cc` | 4639 | 13351 | +8712 |
| `firmware/main/ui/renderers/rawdraw/dashboard_renderer.h` | 2550 | 2940 | +390 |
| `firmware/sdkconfig` *(gitignored)* | 137800 | 137804 | +4 |
| `firmware/sdkconfig.defaults` | 2142 | 2569 | +427 |
| `firmware/tests/host/run.sh` | 4299 | 11728 | +7429 |
| `firmware/tests/host/test_dashboard_service.cc` | 25281 | 39296 | +14015 |
| `firmware/tests/host/test_dashboard_slot.cc` | 21390 | 24895 | +3505 |
| `firmware/tests/host/test_device_config.cc` | 41538 | 47692 | +6154 |
| `firmware/tests/host/test_nav_model.cc` | 27468 | 29544 | +2076 |
| `firmware/tests/host/test_power_policy.cc` | 45485 | 59275 | +13790 |
| `firmware/tests/host/test_voice_hub_config.cc` | 9893 | 9897 | +4 |

## Deleted (0)

None.

---

## Commit 4 patch specification (T4) — NOT applied

Only change authorised for Lot 1. Target file
`firmware/sdkconfig.defaults.esp32s3` (39 lines as-installed) does **not** currently
contain `CONFIG_APP_REPRODUCIBLE_BUILD`. The change is a pure append:

```
+
+# Lot 1: deterministic artefacts (no __DATE__/__TIME__, no absolute build paths)
+CONFIG_APP_REPRODUCIBLE_BUILD=y
```

`firmware/sdkconfig` (the generated, gitignored file) also lacks the symbol. Because it
is a generated artefact and is gitignored, it should be regenerated by the build rather
than hand-edited; do **not** carry a hand-edited `sdkconfig` into the canonical repo.

## Voice-on build mechanism (T6) — located, not exercised

The existing, non-invented mechanism is a CMake option, not a Kconfig symbol:

- `firmware/main/CMakeLists.txt:179` — `option(VOICE_PTT_ENABLED "Compile the push-to-talk microphone path" OFF)`
- `firmware/main/CMakeLists.txt:265` — `VOICE_PTT_ENABLED=$<BOOL:${VOICE_PTT_ENABLED}>`
- `firmware/main/dashboard_build_config.h:108` — documents the invocation verbatim:
  `idf.py -DVOICE_PTT_ENABLED=1 build`
- default when unset: `firmware/main/dashboard_build_config.h:113-114` → `#define VOICE_PTT_ENABLED 0`

So: voice-off = `idf.py build`; voice-on = `idf.py -DVOICE_PTT_ENABLED=1 build`.
