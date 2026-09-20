/**
 * @file dashboard_api.h
 * @brief Versioned, authenticated HTTP API for the poulailler dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Routes are added to the HTTP server that already listens on the LAN
 * interface; this opens no new listening socket. Every mutating route requires
 * the device token, and a device that has never been provisioned refuses all of
 * them.
 *
 *   PUT  /api/v1/dashboard/frame    30000 raw bytes, token required
 *   GET  /api/v1/dashboard/frame    read back exactly what is stored
 *   GET  /api/v1/dashboard/status   state, digests, timing split, capabilities
 *   POST /api/v1/dashboard/refresh  repaint the stored frame, token required
 *   POST /api/v1/dashboard/pair     claims a token inside a physical window
 *   POST /api/v1/voice/hub          write-only hub address and token
 *
 * RegisterDashboardApi also registers the API v2 config and action routes from
 * config_api.h on the same server, so the two contracts cannot drift apart by
 * one of them failing to be installed.
 */

#ifndef RAWDRAW_DASHBOARD_API_H
#define RAWDRAW_DASHBOARD_API_H

#include <esp_http_server.h>

namespace rawdraw {

/**
 * @brief Register the /api/v1/dashboard/ routes on @p server.
 * @return ESP_OK when every route was registered.
 */
esp_err_t RegisterDashboardApi(httpd_handle_t server);

/**
 * @brief Gate for the legacy unauthenticated mutating routes.
 *
 * Returns true if the request must be refused because dashboard lockdown is
 * on. The caller has already sent the 403 response in that case.
 *
 * This exists because the pre-existing gallery routes (upload, delete, settings,
 * show, meta, move) accept writes from anyone who can reach the device on the
 * LAN. Adding an authenticated API next to them would not make the device
 * secure while they remain open, so in dashboard mode they are closed.
 */
bool LegacyWriteBlocked(httpd_req_t* req);

}  // namespace rawdraw

#endif  // RAWDRAW_DASHBOARD_API_H
