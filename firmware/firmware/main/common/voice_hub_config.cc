/**
 * @file voice_hub_config.cc
 * @brief Implementation of the hub address and token validation.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "voice_hub_config.h"

namespace voice {

namespace {

bool HasControlCharacter(const std::string& value) {
    for (char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        // DEL as well as the C0 range. A newline forges a header line, a NUL
        // truncates the string for anything downstream written in C.
        if (u < 0x20 || u == 0x7f) {
            return true;
        }
    }
    return false;
}

bool StartsWith(const std::string& value, const char* prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= value.size() || value[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* HubConfigErrorName(HubConfigError error) {
    switch (error) {
        case HubConfigError::kNone:             return "ok";
        case HubConfigError::kEmpty:            return "empty";
        case HubConfigError::kTooLong:          return "too long";
        case HubConfigError::kBadScheme:        return "must begin with http:// or https://";
        case HubConfigError::kControlCharacter: return "contains a control character";
        case HubConfigError::kNoHost:           return "no host after the scheme";
        case HubConfigError::kIncomplete:       return "a url and a token are both required";
    }
    return "unknown";
}

std::string NormaliseHubUrl(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

HubConfigError ValidateHubUrl(const std::string& url) {
    if (url.empty()) {
        return HubConfigError::kEmpty;
    }
    if (url.size() > kMaxHubUrlChars) {
        return HubConfigError::kTooLong;
    }
    if (HasControlCharacter(url)) {
        return HubConfigError::kControlCharacter;
    }
    const char* scheme = nullptr;
    if (StartsWith(url, "http://")) {
        scheme = "http://";
    } else if (StartsWith(url, "https://")) {
        scheme = "https://";
    } else {
        return HubConfigError::kBadScheme;
    }
    size_t scheme_len = 0;
    while (scheme[scheme_len] != '\0') ++scheme_len;

    const std::string rest = url.substr(scheme_len);
    if (rest.empty()) {
        return HubConfigError::kNoHost;
    }
    // A leading slash or colon means the authority is missing, which
    // esp_http_client would accept and then fail on at connect time with a
    // message that says nothing about the configuration being wrong.
    if (rest[0] == '/' || rest[0] == ':' || rest[0] == '?' || rest[0] == '#') {
        return HubConfigError::kNoHost;
    }
    // A space anywhere would end the request line early.
    for (char c : rest) {
        if (c == ' ') {
            return HubConfigError::kControlCharacter;
        }
    }
    return HubConfigError::kNone;
}

HubConfigError ValidateHubToken(const std::string& token) {
    if (token.empty()) {
        return HubConfigError::kEmpty;
    }
    if (token.size() > kMaxHubTokenChars) {
        return HubConfigError::kTooLong;
    }
    if (HasControlCharacter(token)) {
        return HubConfigError::kControlCharacter;
    }
    for (char c : token) {
        // The token goes in `Authorization: Bearer <token>`. A space would
        // make everything after it a separate, ignored parameter, and the
        // request would fail with a 401 that looks like a wrong token rather
        // than a malformed one.
        if (c == ' ') {
            return HubConfigError::kControlCharacter;
        }
    }
    return HubConfigError::kNone;
}

HubConfigError ValidateHubPair(const std::string& url, const std::string& token,
                               std::string* url_out, std::string* token_out) {
    const std::string normalised = NormaliseHubUrl(url);
    if (normalised.empty() && token.empty()) {
        // Both empty is the documented way to clear the configuration, and it
        // is a success rather than an error: the device goes back to having no
        // hub, which is the state it ships in.
        if (url_out) url_out->clear();
        if (token_out) token_out->clear();
        return HubConfigError::kNone;
    }
    if (normalised.empty() || token.empty()) {
        return HubConfigError::kIncomplete;
    }
    const HubConfigError url_error = ValidateHubUrl(normalised);
    if (url_error != HubConfigError::kNone) {
        return url_error;
    }
    const HubConfigError token_error = ValidateHubToken(token);
    if (token_error != HubConfigError::kNone) {
        return token_error;
    }
    if (url_out) *url_out = normalised;
    if (token_out) *token_out = token;
    return HubConfigError::kNone;
}

}  // namespace voice
