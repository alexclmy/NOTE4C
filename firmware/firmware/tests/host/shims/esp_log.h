/**
 * @file esp_log.h
 * @brief Host shim for the ESP-IDF logging macros.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Exists so a translation unit whose *only* ESP-IDF dependency is a handful of
 * log lines can be compiled and tested on a host. It is deliberately a shim
 * and not a reimplementation: the logs are discarded, because a test that
 * asserted on log text would pin wording rather than behaviour.
 *
 * The arguments are still evaluated for their types by the compiler — the
 * bodies keep the printf-format checking that would otherwise be lost, so a
 * mismatched %s in a log line still fails the build here as it does on device.
 */

#ifndef HOST_SHIM_ESP_LOG_H
#define HOST_SHIM_ESP_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Swallow the message, keeping the format check. Never writes anywhere.
static inline void host_shim_log_discard(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
static inline void host_shim_log_discard(const char* fmt, ...) { (void)fmt; }

#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, fmt, ...) \
    do { (void)(tag); host_shim_log_discard(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) \
    do { (void)(tag); host_shim_log_discard(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) \
    do { (void)(tag); host_shim_log_discard(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) \
    do { (void)(tag); host_shim_log_discard(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) \
    do { (void)(tag); host_shim_log_discard(fmt, ##__VA_ARGS__); } while (0)

#endif  // HOST_SHIM_ESP_LOG_H
