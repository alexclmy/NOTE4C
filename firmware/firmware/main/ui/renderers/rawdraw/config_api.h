/**
 * @file config_api.h
 * @brief The typed settings and action routes of device API v2.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Registered on the same HTTP server as the v1 dashboard routes. Every route
 * here requires the device token and shares the v1 failure lockout, so these
 * cannot be used to probe the token more cheaply than a frame push can.
 *
 *   GET   /api/v1/config            the whole readable configuration
 *   PATCH /api/v1/config            explicit dotted fields only, with a CAS
 *   POST  /api/v1/actions/restart   confirmation literal plus Idempotency-Key
 *   POST  /api/v1/actions/sleep     the same, for deep sleep
 *
 * What is deliberately absent, and stays absent:
 *
 *   * No Wi-Fi credential route. The physical AP portal is the boundary.
 *   * No OTA route, no gallery mutation route, no raw NVS route.
 *   * No pairing initiation. Pairing still starts with a button press.
 *   * No way to turn lockdown off. It can be raised remotely and lowered only
 *     by somebody holding the device.
 *   * No route that returns a secret. The hub token is reported as a boolean
 *     and nothing here has ever held its value.
 */

#ifndef RAWDRAW_CONFIG_API_H
#define RAWDRAW_CONFIG_API_H

#include <esp_http_server.h>

namespace rawdraw {

/// Number of routes RegisterConfigApi adds, so the server's handler budget can
/// be sized from the code rather than from a comment that goes stale.
constexpr int kConfigApiRouteCount = 4;

/**
 * @brief Register the API v2 config and action routes on @p server.
 * @return ESP_OK when every route was registered.
 */
esp_err_t RegisterConfigApi(httpd_handle_t server);

}  // namespace rawdraw

#endif  // RAWDRAW_CONFIG_API_H
