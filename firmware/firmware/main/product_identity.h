/**
 * @file product_identity.h
 * @brief What this device is, in one place.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Every user-visible and wire-visible claim about the product's name, version
 * and hardware comes from here. One place, because the previous arrangement
 * had the About dialog asserting a vendor name, a model number and a website
 * that were all inherited from upstream and none of which described this
 * device, while the status route reported an upstream version string that will
 * never change when this firmware does.
 *
 * The upstream version is kept, and kept labelled as upstream. It is a real
 * fact about where this code came from and deleting it would make the
 * provenance harder to establish, not easier. What it must not do is stand in
 * for a version of *this* firmware, because it does not move when this
 * firmware moves.
 */

#ifndef PRODUCT_IDENTITY_H
#define PRODUCT_IDENTITY_H

namespace product {

/// The name of the thing on the wall.
constexpr const char kName[] = "Poulailler Terminal";

/**
 * The version of this firmware, which does move when this firmware moves.
 *
 * Deliberately not the upstream PROJECT_VER. Anything that reads a version to
 * decide what the device can do should read the api level and the capability
 * list instead; this string is for a human reading the panel.
 */
constexpr const char kFirmwareVersion[] = "Marvin 0.2";

/**
 * The panel, in the terms that matter to anyone pushing pixels at it.
 *
 * Written with a plain space rather than a middle dot: the rawdraw bitmap
 * fonts on this board are not guaranteed to carry U+00B7, and a separator that
 * renders as an empty box on e-paper is worse than no separator at all.
 */
constexpr const char kHardware[] = "NOTE4C 4-color";

/// The actual board, as named in main/boards/.
constexpr const char kModel[] = "zectrix-s3-epaper-4.2";

/// The panel geometry, stated once so the About page and the API agree.
constexpr const char kPanel[] = "400x300, 4-color BWRY";

/// Where this code was forked from. A provenance fact, labelled as one.
constexpr const char kUpstreamBase[] = "6.5.9";

}  // namespace product

#endif  // PRODUCT_IDENTITY_H
