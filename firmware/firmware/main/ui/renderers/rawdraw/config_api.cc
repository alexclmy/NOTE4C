/**
 * @file config_api.cc
 * @brief Implementation of the /api/v1/config and /api/v1/actions/ routes.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This file is the thinnest layer it can be. It reads headers, parses JSON and
 * maps outcomes onto status codes; every rule about what may change, to what,
 * and under which conditions lives in common/device_config.h, which the host
 * tests compile directly. The split is not tidiness: a rule that only exists
 * inside an httpd handler can only be tested by a device with a network
 * analyser attached to it.
 */

#include "config_api.h"

#include <esp_log.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include <cJSON.h>

#include "common/dashboard_manager.h"
#include "common/device_config_service.h"

namespace rawdraw {

namespace {

const char* kTag = "ConfigApi";

constexpr size_t kHeaderValueMax = 128;
/// A patch is a handful of short fields. Anything larger is a mistake or an
/// attack, and reading it costs memory either way.
constexpr int kMaxBody = 1024;

const char* GetHeader(httpd_req_t* req, const char* name, char* out, size_t out_len) {
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return nullptr;
    }
    if (httpd_req_get_hdr_value_str(req, name, out, out_len) != ESP_OK) {
        return nullptr;
    }
    return out;
}

/**
 * @brief One line per answered request.
 *
 * Same shape and same restraint as the dashboard API's: the method, the path,
 * the status, and whether an X-Auth-Token header was present at all. The token
 * itself, the patch body and the hub URL a patch may carry are never logged —
 * this file reads a credential out of a header on every authenticated call, and
 * a debug line is not a reason for one to reach a serial console.
 */
void LogHttpRequest(httpd_req_t* req, const char* status) {
    if (req == nullptr) return;
    const bool auth_hdr = httpd_req_get_hdr_value_len(req, "X-Auth-Token") > 0;
    ESP_LOGI(kTag, "http: %s %s -> %s (auth_hdr=%d)",
             http_method_str(static_cast<enum http_method>(req->method)),
             req->uri, status, auth_hdr ? 1 : 0);
}

esp_err_t SendJson(httpd_req_t* req, const char* status, const char* json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    LogHttpRequest(req, status);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* code,
                    const char* detail) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    return SendJson(req, status, buf);
}

/// Refuse with the field name attached, so the caller learns which of its
/// fields was wrong rather than that "something" was.
esp_err_t SendFieldError(httpd_req_t* req, const char* status, const char* code,
                         const std::string& field) {
    char buf[256];
    if (field.empty()) {
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", code);
    } else {
        // The name is one of the fixed allowlist strings, or a name the caller
        // sent. Bound it so a long attacker-chosen key cannot push the rest of
        // the object out of the buffer, and neutralise the two characters that
        // would let it break out of the JSON string it is echoed into.
        char safe[64] = {};
        snprintf(safe, sizeof(safe), "%s", field.c_str());
        for (size_t i = 0; i < sizeof(safe) && safe[i] != '\0'; ++i) {
            const unsigned char c = static_cast<unsigned char>(safe[i]);
            if (c == '"' || c == '\\' || c < 0x20) safe[i] = '_';
        }
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", code, safe);
    }
    return SendJson(req, status, buf);
}

/// Shared token check. Uses the v1 FrameAuth and therefore the v1 lockout.
bool Authorised(httpd_req_t* req) {
    auto& mgr = dashboard::DashboardManager::GetInstance();
    char token_buf[kHeaderValueMax];
    const char* token = GetHeader(req, "X-Auth-Token", token_buf, sizeof(token_buf));
    switch (mgr.CheckToken(token)) {
        case dashboard::AuthResult::kNotProvisioned:
            SendError(req, "503 Service Unavailable", "not_provisioned",
                      "no dashboard token installed; pair the device locally first");
            return false;
        case dashboard::AuthResult::kBadToken:
            SendError(req, "401 Unauthorized", "unauthorized", "invalid token");
            return false;
        case dashboard::AuthResult::kLockedOut:
            SendError(req, "429 Too Many Requests", "locked_out",
                      "too many failed authentications; retry later");
            return false;
        case dashboard::AuthResult::kOk:
            return true;
    }
    return false;
}

/// Read a small JSON body into @p out. Answers the caller on failure.
bool ReadSmallBody(httpd_req_t* req, char* out, size_t out_len, int* len_out) {
    if (req->content_len <= 0 || req->content_len > kMaxBody ||
        static_cast<size_t>(req->content_len) >= out_len) {
        SendError(req, "400 Bad Request", "bad_length",
                  "expected a small JSON object");
        return false;
    }
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, out + received, req->content_len - received);
        if (n <= 0) {
            SendError(req, "400 Bad Request", "short_body",
                      "the body ended before its declared length");
            return false;
        }
        received += n;
    }
    out[received] = '\0';
    *len_out = received;
    return true;
}

// ---------------------------------------------------- GET /api/v1/config ---

esp_err_t ConfigGetHandler(httpd_req_t* req) {
    if (!Authorised(req)) return ESP_OK;

    auto& service = devcfg::DeviceConfigService::GetInstance();
    if (!service.initialised()) {
        return SendError(req, "503 Service Unavailable", "unavailable",
                         "the configuration service is not initialised");
    }

    char body[devcfg::kConfigJsonMax];
    const size_t written = devcfg::RenderConfigJson(service.Snapshot(),
                                                    service.Revision(),
                                                    body, sizeof(body));
    if (written == 0) {
        // Refuse rather than send a truncated object. A caller that parsed
        // half a config would act on defaults it was never told about.
        return SendError(req, "500 Internal Server Error", "render_failed",
                         "the configuration did not fit in one response");
    }
    return SendJson(req, "200 OK", body);
}

// -------------------------------------------------- PATCH /api/v1/config ---

/// Lift one JSON member onto the patch, with its type checked against the
/// allowlist rather than guessed from what arrived.
devcfg::PatchError AddMember(devcfg::ConfigPatch* patch, const cJSON* member) {
    const char* name = member->string;
    const devcfg::FieldSpec* spec = devcfg::FindField(name);
    if (spec == nullptr) {
        // Routed through the patch so failed_field() carries the name back to
        // the caller. RejectType answers kUnknownField for a name it has never
        // heard of, which is what this is.
        return patch->RejectType(name);
    }
    switch (spec->kind) {
        case devcfg::ValueKind::kInteger:
            if (!cJSON_IsNumber(member)) return patch->RejectType(name);
            // Whole minutes only. 5.5 is not a slideshow interval, and
            // truncating it silently would apply a value nobody sent.
            if (member->valuedouble != static_cast<double>(member->valueint)) {
                return patch->RejectType(name);
            }
            return patch->AddInteger(name, member->valueint);

        case devcfg::ValueKind::kBoolean:
            // Strict. 1 and "true" are not booleans, and accepting them would
            // mean a client bug silently unmutes a microphone.
            if (!cJSON_IsBool(member)) return patch->RejectType(name);
            return patch->AddBoolean(name, cJSON_IsTrue(member) != 0);

        case devcfg::ValueKind::kString:
            if (!cJSON_IsString(member) || member->valuestring == nullptr) {
                return patch->RejectType(name);
            }
            return patch->AddString(name, member->valuestring);
    }
    return devcfg::PatchError::kUnknownField;
}

esp_err_t ConfigPatchHandler(httpd_req_t* req) {
    if (!Authorised(req)) return ESP_OK;

    auto& service = devcfg::DeviceConfigService::GetInstance();
    if (!service.initialised()) {
        return SendError(req, "503 Service Unavailable", "unavailable",
                         "the configuration service is not initialised");
    }

    char body[kMaxBody + 1] = {};
    int received = 0;
    if (!ReadSmallBody(req, body, sizeof(body), &received)) return ESP_OK;

    devcfg::ConfigPatch patch;
    devcfg::PatchError error = devcfg::PatchError::kNone;
    std::string failed_field;
    {
        cJSON* root = cJSON_ParseWithLength(body, static_cast<size_t>(received));
        if (root == nullptr || !cJSON_IsObject(root)) {
            if (root != nullptr) cJSON_Delete(root);
            return SendError(req, "400 Bad Request", "not_object",
                             "expected a JSON object");
        }

        // A whole, non-negative number or nothing. A fractional revision that
        // truncated into a match would be a compare-and-swap that compared
        // something the caller never sent.
        const cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "expected_revision");
        if (cJSON_IsNumber(revision) && revision->valuedouble >= 0 &&
            revision->valuedouble <= 4294967295.0 &&
            revision->valuedouble ==
                static_cast<double>(static_cast<uint32_t>(revision->valuedouble))) {
            patch.SetExpectedRevision(static_cast<uint32_t>(revision->valuedouble));
        }

        const cJSON* confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
        if (cJSON_IsString(confirm) && confirm->valuestring != nullptr) {
            patch.SetConfirmation(confirm->valuestring);
        }

        const cJSON* set = cJSON_GetObjectItemCaseSensitive(root, "set");
        if (!cJSON_IsObject(set)) {
            cJSON_Delete(root);
            return SendError(req, "400 Bad Request", "no_set_object",
                             "expected an object under \"set\"");
        }

        const cJSON* member = nullptr;
        cJSON_ArrayForEach(member, set) {
            if (member->string == nullptr) continue;
            error = AddMember(&patch, member);
            if (error != devcfg::PatchError::kNone) break;
        }
        if (error != devcfg::PatchError::kNone) {
            failed_field = patch.failed_field();
        }
        cJSON_Delete(root);
    }

    if (error != devcfg::PatchError::kNone) {
        return SendFieldError(req, devcfg::PatchErrorHttpStatus(error),
                              devcfg::PatchErrorName(error), failed_field);
    }

    devcfg::Config applied;
    uint32_t revision = 0;
    error = service.ApplyPatch(patch, &failed_field, &applied, &revision);

    if (error == devcfg::PatchError::kRevisionMismatch) {
        // Hand back the current revision so the loser of the race can re-read
        // and reconcile in one round trip rather than two.
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"revision_mismatch\",\"revision\":%u}",
                 static_cast<unsigned>(revision));
        return SendJson(req, "409 Conflict", buf);
    }
    if (error != devcfg::PatchError::kNone) {
        return SendFieldError(req, devcfg::PatchErrorHttpStatus(error),
                              devcfg::PatchErrorName(error), failed_field);
    }

    char out[devcfg::kConfigJsonMax];
    const size_t written = devcfg::RenderPatchResponseJson(applied, revision, patch,
                                                            out, sizeof(out));
    if (written == 0) {
        // The write happened; only the report did not fit. Say exactly that,
        // rather than a 500 that would invite the caller to retry a change it
        // has already made.
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"applied\":true,\"revision\":%u,"
                 "\"detail\":\"the configuration did not fit; read GET /api/v1/config\"}",
                 static_cast<unsigned>(revision));
        return SendJson(req, "200 OK", buf);
    }
    return SendJson(req, "200 OK", out);
}

// ------------------------------------------------ POST /api/v1/actions/* ---

esp_err_t HandleAction(httpd_req_t* req, devcfg::Action action) {
    if (!Authorised(req)) return ESP_OK;

    auto& service = devcfg::DeviceConfigService::GetInstance();
    if (!service.initialised()) {
        return SendError(req, "503 Service Unavailable", "unavailable",
                         "the configuration service is not initialised");
    }

    char body[kMaxBody + 1] = {};
    int received = 0;
    if (!ReadSmallBody(req, body, sizeof(body), &received)) return ESP_OK;

    std::string confirm;
    bool has_confirm = false;
    {
        cJSON* root = cJSON_ParseWithLength(body, static_cast<size_t>(received));
        if (root == nullptr || !cJSON_IsObject(root)) {
            if (root != nullptr) cJSON_Delete(root);
            return SendError(req, "400 Bad Request", "not_object",
                             "expected a JSON object");
        }
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "confirm");
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            confirm = item->valuestring;
            has_confirm = true;
        }
        cJSON_Delete(root);
    }

    char idem_buf[kHeaderValueMax];
    const char* idem = GetHeader(req, "Idempotency-Key", idem_buf, sizeof(idem_buf));

    bool replay = false;
    const devcfg::ActionError error = service.RequestAction(
        action, has_confirm ? confirm.c_str() : nullptr, idem, &replay);

    if (error != devcfg::ActionError::kNone) {
        const char* status =
            (error == devcfg::ActionError::kNoRunner) ? "503 Service Unavailable"
                                                      : "400 Bad Request";
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "send {\"confirm\":\"%s\"} with an Idempotency-Key header",
                 devcfg::ActionConfirmation(action));
        return SendError(req, status, devcfg::ActionErrorName(error), detail);
    }

    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"%s\",\"scheduled\":%s,\"replay\":%s,\"at_ms\":%u}",
             devcfg::ActionName(action),
             replay ? "false" : "true",
             replay ? "true" : "false",
             static_cast<unsigned>(devcfg::ActionGate::kDelayMs));
    // 202 for one that was scheduled, 200 for a replay: there is nothing
    // further to wait for on a request that was already served.
    return SendJson(req, replay ? "200 OK" : "202 Accepted", buf);
}

esp_err_t RestartHandler(httpd_req_t* req) {
    return HandleAction(req, devcfg::Action::kRestart);
}

esp_err_t SleepHandler(httpd_req_t* req) {
    return HandleAction(req, devcfg::Action::kSleep);
}

}  // namespace

esp_err_t RegisterConfigApi(httpd_handle_t server) {
    if (server == nullptr) return ESP_ERR_INVALID_ARG;

    const httpd_uri_t routes[] = {
        {.uri = "/api/v1/config",          .method = HTTP_GET,   .handler = ConfigGetHandler,   .user_ctx = nullptr},
        {.uri = "/api/v1/config",          .method = HTTP_PATCH, .handler = ConfigPatchHandler, .user_ctx = nullptr},
        {.uri = "/api/v1/actions/restart", .method = HTTP_POST,  .handler = RestartHandler,     .user_ctx = nullptr},
        {.uri = "/api/v1/actions/sleep",   .method = HTTP_POST,  .handler = SleepHandler,       .user_ctx = nullptr},
    };
    static_assert(sizeof(routes) / sizeof(routes[0]) == kConfigApiRouteCount,
                  "kConfigApiRouteCount must match the table");

    for (const auto& r : routes) {
        const esp_err_t err = httpd_register_uri_handler(server, &r);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "failed to register %s: %s", r.uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(kTag, "device API v2 config registered (%d routes)", kConfigApiRouteCount);
    return ESP_OK;
}

}  // namespace rawdraw
