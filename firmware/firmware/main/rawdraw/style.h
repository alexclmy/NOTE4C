/**
 * @file style.h
 * @brief Unified style constants for rawdraw UI components
 *
 * Centralizes spacing, sizing, and layout constants used across all
 * rawdraw components (bubble, button, panel, scrollview, progress_bar).
 * Change these values to adjust the overall UI density and appearance.
 */

#ifndef RAWDRAW_STYLE_H
#define RAWDRAW_STYLE_H

namespace Style {

// ============================================================
// Spacing tokens (SM / MD / LG scale)
// ============================================================

constexpr int kSpacingXXS = 2;   ///< Extra-extra-small gap
constexpr int kSpacingXS  = 4;   ///< Extra-small gap
constexpr int kSpacingSM  = 6;   ///< Small gap
constexpr int kSpacingMD  = 8;   ///< Medium gap (default padding)
constexpr int kSpacingLG  = 12;  ///< Large gap
constexpr int kSpacingXL  = 16;  ///< Extra-large gap
constexpr int kSpacingXXL = 24;  ///< Extra-extra-large gap

// ============================================================
// Border & corner radii
// ============================================================

constexpr int kBorderRadiusSM = 4;   ///< Small corners (badges, tags)
constexpr int kBorderRadiusMD = 8;   ///< Default corners (bubbles, cards)
constexpr int kBorderRadiusLG = 12;  ///< Large corners (panels, dialogs)
constexpr int kBorderRadiusXL = 16;  ///< Extra-large (full rounded buttons)
constexpr int kBorderRadiusPill = 999; ///< Pill shape (half-height)

// ============================================================
// Border thickness
// ============================================================

constexpr int kBorderThin   = 1;  ///< Default border
constexpr int kBorderMedium = 2;  ///< Emphasized border
constexpr int kBorderThick  = 3;  ///< Heavy border

// ============================================================
// Component: Bubble (chat messages)
// ============================================================

constexpr int kBubblePadding    = 8;   ///< Internal padding inside bubble
constexpr int kBubbleMargin     = 24;  ///< Distance from screen edge
constexpr int kBubbleMaxWidthPct = 80; ///< Max width as percentage of screen
constexpr int kBubbleRadius     = kBorderRadiusMD;
constexpr int kBubbleLineSpacing = 2;  ///< Extra space between text lines
constexpr int kBubbleGap        = 4;   ///< Vertical gap between consecutive bubbles

// ============================================================
// Component: Button
// ============================================================

constexpr int kButtonPaddingH   = 12;  ///< Horizontal padding
constexpr int kButtonPaddingV   = 8;   ///< Vertical padding
constexpr int kButtonMinWidth   = 40;  ///< Minimum button width
constexpr int kButtonMinHeight  = 40;  ///< Minimum button height
constexpr int kButtonRadius     = kBorderRadiusMD;
constexpr int kButtonIconGap    = 6;   ///< Gap between icon and text

// ============================================================
// Component: Panel
// ============================================================

constexpr int kPanelPadding     = kSpacingMD;
constexpr int kPanelTitleHeight = 28;  ///< Title bar height
constexpr int kPanelRadius      = kBorderRadiusLG;
constexpr int kPanelBorderWidth = kBorderThin;
constexpr int kPanelGap         = kSpacingSM; ///< Gap between sections

// ============================================================
// Component: ScrollView
// ============================================================

constexpr int kScrollbarWidth   = 3;   ///< Scrollbar indicator width
constexpr int kScrollbarMinH    = 20;  ///< Minimum scrollbar thumb height
constexpr int kScrollMargin     = 4;   ///< Margin between content and scrollbar

// ============================================================
// Component: ProgressBar
// ============================================================

constexpr int kProgressHeight   = 8;   ///< Bar height
constexpr int kProgressRadius   = kBorderRadiusPill;
constexpr int kProgressPadding  = 2;   ///< Padding around bar

// ============================================================
// Component: Toggle
// ============================================================

constexpr int kToggleWidth      = 48;  ///< Default track width
constexpr int kToggleHeight     = 24;  ///< Default track height (thumb = h/2)
constexpr int kTogglePadding    = 2;   ///< Thumb inset from track edge
constexpr int kToggleLabelGap   = kSpacingSM; ///< Gap between toggle and label

// ============================================================
// Component: Slider
// ============================================================

constexpr int kSliderHeight     = 24;  ///< Default track + thumb height
constexpr int kSliderTrackH     = 4;   ///< Track bar height
constexpr int kSliderThumbSize  = 8;   ///< Diamond thumb half-size
constexpr int kSliderLabelGap   = kSpacingXS; ///< Label distance from track

// ============================================================
// Component: Card
// ============================================================

constexpr int kCardPadding      = kSpacingMD;
constexpr int kCardRadius       = kBorderRadiusMD;
constexpr int kCardBorderWidth  = kBorderThin;
constexpr int kCardShadowOffset = 2;   ///< Shadow offset pixels
constexpr int kCardTitleHeight  = kPanelTitleHeight;
constexpr int kCardGap          = kSpacingSM; ///< Gap between stacked cards

// ============================================================
// Component: ListItem
// ============================================================

constexpr int kListItemHeight   = 36;  ///< Default item height
constexpr int kListItemPadding  = kSpacingMD;
constexpr int kListItemIconGap  = kSpacingSM; ///< Gap after icon
constexpr int kListItemChevronW = 10;  ///< Chevron reserved width
constexpr int kListItemSepWidth = 1;   ///< Separator line thickness

// ============================================================
// Component: StatusBar
// ============================================================

// Global top menu bar height. Keep this aligned with the Macintosh-style
// dialog titlebar height (`SettingsRenderer` uses 28px for About InkScreen),
// so page titles, time, and status controls share the same visual rhythm.
// Future tuning should start here before changing individual renderers.
constexpr int kStatusBarHeight  = 28;
constexpr int kStatusBarPadding = 4;
constexpr int kStatusBarIconSize = 16; ///< Icon font size
constexpr int kFooterBarHeight = 22;   ///< Bottom footer / hint bar height
constexpr int kFooterBarPadding = 8;   ///< Horizontal padding for footer text
constexpr int kShellDividerThickness = 2; ///< Reusable shell/status/footer divider thickness
constexpr int kSettingsSidebarWidth = 58;
constexpr int kGalleryInfoWidth = 120;

// ============================================================
// Component: TodoList
// ============================================================

constexpr int kTodoItemHeight   = 32;  ///< Height of each todo item
constexpr int kTodoItemPadding  = 8;   ///< Horizontal padding inside item
constexpr int kTodoTextOffset   = 28;  ///< X offset for text after checkbox
constexpr int kTodoMaxVisibleItems = 7; ///< Max items visible without scroll

// ============================================================
// Component: Card (generic card-style containers)
// ============================================================

constexpr int kItemMinHeight   = 28;  ///< Minimum item row height
constexpr int kItemGap         = 4;   ///< Gap between list items
constexpr int kCheckboxSize    = 16;  ///< Checkbox square size (F3: 16x16 per SPEC)

// ============================================================
// Component: Settings items
// ============================================================

constexpr int kIconSize = 20; ///< Icon size in settings items
constexpr int kSettingsViewportInset = 16;   ///< Left/right margin for settings cards
constexpr int kSettingsCardGap = 4;          ///< Gap between stacked settings cards
constexpr int kSettingsCardRadius = kBorderRadiusMD;
constexpr int kSettingsTagHeight = 14;       ///< Floating section tag height
constexpr int kSettingsIconBox = 22;         ///< Leading icon capsule size
constexpr int kSettingsDialogInset = 44;     ///< Global modal inset from screen edge
constexpr int kSettingsDialogRowH = 30;      ///< Info row height inside dialogs
constexpr int kSettingsValueGap = 10;        ///< Minimum gap between label and right-side value
constexpr int kSettingsCardRightReserve = 14; ///< Reserve for scrollbar/air on the right
constexpr int kSettingsCardInnerPad = 10;     ///< Card horizontal inner padding
constexpr int kSettingsContentOffsetY = 1;    ///< Shared vertical nudge from card center line
constexpr int kVisualTextOffset = 1;          ///< Optical +1px nudge: e-paper text looks slightly high at exact math center
constexpr int kModalInset = 36;               ///< Default modal inset from screen edges
constexpr int kModalTitleHeight = 26;         ///< Title strip height for generic modals
constexpr int kModalFooterHeight = 22;        ///< Bottom action strip height for generic modals

// ============================================================
// Screen dimensions (400x300 SSD1683 EPD)
// ============================================================

constexpr int kScreenWidth  = 400;
constexpr int kScreenHeight = 300;

// Usable content area (below status bar, with margins)
constexpr int kContentTop    = kStatusBarHeight + kSpacingSM;
constexpr int kContentBottom = kScreenHeight - kSpacingSM;
constexpr int kContentLeft   = kSpacingSM;
constexpr int kContentRight  = kScreenWidth - kSpacingSM;
constexpr int kContentWidth  = kScreenWidth - 2 * kSpacingSM;
constexpr int kContentHeight = kContentBottom - kContentTop;

// ============================================================
// Corner safe zones (400x300 SSD1683 physical rounded corners)
// ============================================================

constexpr int kCornerSafeInset = 15;  ///< Pixels to inset from corners
// Top corners: y=0, x < 15 or x > 385 are not usable
// Bottom corners: y=295, x < 15 or x > 385 are not usable

// ============================================================
// Font sizes
// ============================================================

constexpr int kFontSizeXS   = 12;  ///< Tiny labels
constexpr int kFontSizeSM   = 16;  ///< Body text / icons
constexpr int kFontSizeMD   = 20;  ///< Headings
constexpr int kFontSizeLG   = 24;  ///< Large headings
constexpr int kFontSizeXL   = 48;  ///< Hero / large icons

}  // namespace Style

#endif  // RAWDRAW_STYLE_H
