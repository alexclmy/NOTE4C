/**
 * @file voice_hub_config.h
 * @brief Validation for the hub address and token. Portable, host tested.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These two strings arrive over the network and end up in an HTTP request line
 * and an HTTP header. That is the whole reason this is a separate,
 * host-testable file rather than four `if`s inside the route handler: a
 * newline in the token is a forged header, and an unbounded URL is an
 * unbounded allocation on a device with this much internal RAM.
 *
 * What is deliberately *not* validated here is whether the hub exists, answers,
 * or has that token. None of that can be known without asking it, and pretending
 * otherwise would mean a device that reports itself configured because a string
 * looked plausible.
 */

#ifndef COMMON_VOICE_HUB_CONFIG_H
#define COMMON_VOICE_HUB_CONFIG_H

#include <stddef.h>

#include <string>

namespace voice {

/// Longest hub URL accepted. Generous for `http://host.local:65535`.
constexpr size_t kMaxHubUrlChars = 200;
/// Longest token accepted. The hub mints 32 random bytes as 64 hex characters
/// (hub/terminal_hub.py, TOKEN_BYTES), so this is ample.
constexpr size_t kMaxHubTokenChars = 256;

/// Why a value was refused. The route hands this back so the caller learns
/// what is wrong rather than "bad request".
enum class HubConfigError {
    kNone = 0,
    kEmpty,
    kTooLong,
    kBadScheme,
    kControlCharacter,
    kNoHost,
    /// Only one of the two was given. A URL without a token, or a token
    /// without a URL, configures nothing and would leave the device claiming
    /// to be set up.
    kIncomplete,
};

const char* HubConfigErrorName(HubConfigError error);

/**
 * @brief Check a hub base URL.
 *
 * Accepts `http://host[:port][/path]` and `https://...`. Rejects anything with
 * a control character, anything longer than kMaxHubUrlChars, any other scheme,
 * and a scheme with nothing after it.
 *
 * `https://` is accepted by this validator and by `esp_http_client`, but the
 * hub does not serve TLS and no certificate is pinned, so nothing in this
 * project has exercised that path. See main/common/voice_uploader.h.
 */
HubConfigError ValidateHubUrl(const std::string& url);

/// Check a bearer token: non-empty, bounded, no control characters, no spaces.
/// A space would split the `Authorization` header value; a newline would end it.
HubConfigError ValidateHubToken(const std::string& token);

/**
 * @brief Check a URL and token together, allowing "both empty" as "clear it".
 *
 * @param url_out    normalised URL, trailing slashes removed.
 * @param token_out  the token, unchanged.
 */
HubConfigError ValidateHubPair(const std::string& url, const std::string& token,
                               std::string* url_out, std::string* token_out);

/// Remove trailing slashes so a base and a path never join into a double one.
std::string NormaliseHubUrl(std::string url);

}  // namespace voice

#endif  // COMMON_VOICE_HUB_CONFIG_H
