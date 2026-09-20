/**
 * @file dashboard_build_config.h
 * @brief Build-time shape of the poulailler dashboard firmware.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#ifndef DASHBOARD_BUILD_CONFIG_H
#define DASHBOARD_BUILD_CONFIG_H

/*
 * The Kconfig symbols this header derives from, fetched here rather than
 * assumed to be in scope already.
 *
 * AUTONOMY_COMPILED below is `#ifdef CONFIG_AUTONOMY_ENABLED`, and an `#ifdef`
 * on a macro that has not been defined yet is not an error — it is silently
 * false. So the value of the gate depends on whether something earlier in the
 * translation unit had already pulled in the generated sdkconfig.h. It does,
 * today: the top-level CMakeLists.txt force-includes compat/cxx_math_compat.h
 * into every C++ source, that includes <math.h>, picolibc's math.h reaches
 * sys/cdefs.h, and ESP-IDF's esp_libc override of sys/cdefs.h includes
 * sdkconfig.h. Four links, none of them put there for this purpose, and the
 * force-include is C++-only — a C translation unit including this header would
 * get the opposite answer with no diagnostic anywhere.
 *
 * That is not a contract, it is a coincidence that currently holds. A header
 * that reads a config symbol should be the thing that obtains it, so this
 * header obtains it. ESP_PLATFORM is defined by ESP-IDF on every target compile
 * and by nothing on the host, where there is no sdkconfig.h to find and the
 * gates fall back to their defaults below.
 *
 * tests/host/test_status_capabilities.cc compiles this header first in a
 * translation unit with no such chain in front of it, and fails if the gate
 * goes back to depending on one.
 */
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

/**
 * DASHBOARD_MINIMAL_UI
 *
 * When 1 (the default), the firmware compiles only the pages this device can
 * actually reach: Dashboard, Gallery, Settings and the AP transfer / Wi-Fi
 * setup screen. The upstream content pages are removed from the build
 * entirely - not merely hidden.
 *
 * Why removal rather than hiding: those pages render Chinese calendrical and
 * almanac concepts, and text fetched from a Chinese weather API. They cannot be
 * translated into an English UI in any honest sense, and they have nothing to
 * do with displaying a chicken-coop dashboard. Leaving them compiled but
 * unreachable would mean either shipping untranslated UI that some future
 * navigation change could expose, or declaring them "out of scope" to make an
 * audit pass. Removing them makes the claim structural: the strings are not in
 * the binary.
 *
 * Removed when this is 1:
 *   Almanac, Calendar, Weather, WeatherDetail, News, LifeBar, YearProgress,
 *   Ebook, Chat, Log, FontDebug, FontMetrics
 *   plus their data sources: weather_api (QWeather), holiday_fetcher
 *   (timor.tech), the calendar and weather-card draw components.
 *
 * Removing the two fetchers removes the upstream's third-party data sources.
 *
 * That used to be the whole story, and this comment used to say so: "the last
 * code paths in this firmware that would contact a third-party service". It is
 * no longer true, and a comment that stayed comfortable while the code changed
 * underneath it would be worse than no comment.
 *
 * The truth now: this firmware contacts **exactly one** third-party service,
 * Open-Meteo, over HTTPS, at a compile-time constant origin, with no API key,
 * for at most one request per wake — and only when the autonomy profile asks
 * for a forecast and the fetch interval is due. It is off by default
 * (`autonomy.enabled` defaults to false), it is bounded (see
 * common/openmeteo_client.h), and there is no field in any document this
 * device accepts that can move where it connects. The NTP pools the clock uses
 * were always there and are unchanged.
 *
 * See THIRD_PARTY_NOTICES.md for the attribution that obligation carries.
 *
 * Set to 0 (`idf.py -DDASHBOARD_MINIMAL_UI=0 build`) to restore the full
 * upstream page set. That build is not English-only and the i18n audit reports
 * it as such rather than pretending otherwise.
 */
#ifndef DASHBOARD_MINIMAL_UI
#define DASHBOARD_MINIMAL_UI 1
#endif

/**
 * VOICE_PTT_ENABLED
 *
 * When 0 (the default, and what this milestone ships), the microphone path is
 * not compiled. The reserved BOOT-long gesture still reaches the portable
 * state machine in main/common/audio_fsm.cc, which acknowledges it and reports
 * that voice is unavailable. Nothing opens the codec input,
 * AudioService::MarkPttStart() stays uncalled, and no audio leaves the device.
 *
 * Why off by default rather than "present but unused": nothing about audio on
 * this board has ever been verified on hardware. The ES8311 pin map, the I2C
 * address, the amplifier polarity and the microphone itself are unvalidated,
 * and GPIO46 has two owners (the codec PA pin and BoardPowerBsp's
 * Audio_AMP_PIN). Those are hardware gates HG1, HG5 and HG6 in
 * docs/HARDWARE-ACCEPTANCE.md. Compiling the capture path in before they pass
 * would mean shipping a feature whose first real test is somebody's living
 * room.
 *
 * Set to 1 (`idf.py -DVOICE_PTT_ENABLED=1 build`) to compile the glue. That
 * build is a compile check only in this milestone: the upload transport and
 * the response path are Milestone B, and no build of this firmware has been
 * hardware-tested for audio.
 */
#ifndef VOICE_PTT_ENABLED
#define VOICE_PTT_ENABLED 0
#endif

/**
 * AUTONOMY_COMPILED
 *
 * The rollout gate for semi-autonomous rendering, from Kconfig
 * (`CONFIG_AUTONOMY_ENABLED`, default n). It is deliberately a *build* switch
 * separate from the `autonomy.enabled` config field:
 *
 *   - This decides whether the feature can be turned on at all. Off, and the
 *     device advertises no `autonomy.profile.v1` capability, brings up no
 *     profile store, answers the profile and local-render routes with 404
 *     `autonomy_unsupported`, and never composes a frame for itself.
 *   - `autonomy.enabled` decides whether it *is* on, at runtime, over the
 *     config API, with the profile kept either way.
 *
 * Two switches because they answer to different people at different times: the
 * first is a decision about which firmware went out, the second is the
 * operator's kill-switch on a device already in the field.
 *
 * What this flag does not claim: the autonomy translation units are compiled
 * and linked whatever it says. It gates reachability, not presence. That is
 * weaker than DASHBOARD_MINIMAL_UI, which removes code from the binary, and
 * the difference is stated here rather than glossed over.
 *
 * What turning it on now permits, stated plainly because it changed with this
 * increment: an autonomous wake cycle that may compose without being asked, and
 * at most one HTTPS request per wake to a single compile-time origin. Both are
 * still behind `autonomy.enabled`, which defaults to off.
 */
#ifndef AUTONOMY_COMPILED
#ifdef CONFIG_AUTONOMY_ENABLED
#define AUTONOMY_COMPILED 1
#else
#define AUTONOMY_COMPILED 0
#endif
#endif

#endif  // DASHBOARD_BUILD_CONFIG_H
