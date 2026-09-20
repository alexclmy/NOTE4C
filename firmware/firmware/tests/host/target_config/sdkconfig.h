/**
 * @file sdkconfig.h
 * @brief Stand-in for the sdkconfig.h ESP-IDF generates, for one host suite.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The real one is written by the build system into <build>/config/ and is
 * roughly 1400 `#define CONFIG_*` lines. Reproducing it is not the point: the
 * point is that it is reachable on the include path as <sdkconfig.h> and is
 * *not* dragged in by anything else, which is exactly the situation
 * main/dashboard_build_config.h has to work in on the device.
 *
 * Only tests/host/test_status_capabilities.cc gets this directory on its
 * include path, and this directory holds exactly this one header. Putting it in
 * tests/host/shims/ would hand it to every suite, where it would shadow nothing
 * today and silently shadow the real thing the day a portable source starts
 * reading a CONFIG_ symbol.
 *
 * The symbol's value matches a device built with the autonomy rollout gate on —
 * `idf.py menuconfig` -> CONFIG_AUTONOMY_ENABLED=y, which is what the lot-2
 * hardware image was built with.
 */

#ifndef HOST_TEST_SDKCONFIG_H
#define HOST_TEST_SDKCONFIG_H

#define CONFIG_AUTONOMY_ENABLED 1

#endif  // HOST_TEST_SDKCONFIG_H
