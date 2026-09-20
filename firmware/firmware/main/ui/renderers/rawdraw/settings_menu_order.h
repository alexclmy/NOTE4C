/**
 * @file settings_menu_order.h
 * @brief The on-device Settings menu order, as a single source of truth.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Header-only and free of every rawdraw, LVGL and ESP-IDF type on purpose, in
 * the same spirit as settings_layout.h: that is what lets
 * tests/host/test_settings_layout.cc compile these exact declarations and
 * assert the ordering properties directly.
 *
 * Why this file exists
 * --------------------
 * The Settings item list is built in application.cc, which cannot be compiled
 * on the host, and several rows are updated later through *hard-coded* indices
 * (the Wi-Fi checkbox, the pairing row, the lockdown toggle). Before this file
 * those indices were literals scattered across the translation unit —
 * `kSettingsPairIndex = 12` — and the item order could only be changed by
 * counting rows by hand and hoping every literal was found. A reorder was a
 * bug waiting to happen, and the comments in application.cc said as much:
 * "rows may only be added at the end".
 *
 * This enum is now that order. application.cc pushes its rows in exactly this
 * sequence and derives every hard-coded index from `Index()`, so the order and
 * the indices cannot drift from one another. The host test asserts the
 * properties the owner cares about — chiefly that the actions he reaches for
 * most sit near the top, where each ~20 s e-paper repaint while scrolling is
 * not spent getting to them.
 */

#ifndef RAWDRAW_SETTINGS_MENU_ORDER_H
#define RAWDRAW_SETTINGS_MENU_ORDER_H

namespace rawdraw {
namespace settings_menu {

/**
 * @brief Every row of the on-device Settings menu, in display order.
 *
 * Section headers are rows too, so `Index()` is the row's position in the list
 * the renderer is handed. The order groups the rows into five coherent
 * sections — System, Dashboard, Network, Gallery, About — with the frequent
 * actions pulled to the front of the first two.
 */
enum class Item : int {
    SystemSection = 0,
    Restart,            ///< frequent: the owner's most common recovery action
    PowerSaving,        ///< frequent: enter deep sleep now (the power action)
    DashboardSection,
    PairDashboard,      ///< frequent: open the pairing window
    BlockLegacyWrites,  ///< frequent: the lockdown toggle
    NetworkSection,
    Wifi,
    LanService,
    LanAddress,
    GallerySection,
    SlideshowInterval,
    PhotoTransfer,
    AboutSection,
    Firmware,
    MuteVoice,
    Count,              ///< sentinel; not a row
};

/// The row position of an item in the list handed to the renderer.
constexpr int Index(Item it) { return static_cast<int>(it); }

/// Number of real rows (excluding the Count sentinel).
constexpr int kItemCount = static_cast<int>(Item::Count);

/// Number of section headers in the list. Kept in step with the enum by the
/// host test, which counts the `*Section` members.
constexpr int kSectionCount = 5;

/**
 * @brief The rows a fridge owner reaches for most.
 *
 * These are pulled near the top by the order above so they are reachable
 * without scrolling past every sub-setting. The set is deliberately small:
 * "frequent" is the four the owner named, not "everything useful".
 */
constexpr bool IsFrequentAction(Item it) {
    return it == Item::Restart || it == Item::PowerSaving ||
           it == Item::PairDashboard || it == Item::BlockLegacyWrites;
}

/**
 * @brief The row the frequent actions are guaranteed to sit above.
 *
 * They are all inside the first two sections (System and Dashboard), so every
 * frequent action has a row index strictly less than this. The host test
 * asserts it, which is what turns "we reordered the menu" into a property that
 * cannot silently regress.
 */
constexpr int kFrequentActionRowThreshold = 6;

}  // namespace settings_menu
}  // namespace rawdraw

#endif  // RAWDRAW_SETTINGS_MENU_ORDER_H
