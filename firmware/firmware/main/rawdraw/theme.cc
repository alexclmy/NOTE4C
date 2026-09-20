/**
 * @file theme.cc
 * @brief Semantic 4-color theme system for RawDraw e-paper UI.
 */

#include "theme.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cstring>

namespace rawdraw {
namespace {

constexpr int kTokenCount = static_cast<int>(ThemeToken::Count);
constexpr ThemeId kOnlyRuntimeTheme = ThemeId::Industrial;

ThemeDefinition MakeTheme(ThemeId id, const char* key, const char* name,
                          PaintStyle text_primary,
                          PaintStyle text_secondary,
                          PaintStyle bg_primary,
                          PaintStyle bg_secondary,
                          PaintStyle accent,
                          PaintStyle warning,
                          PaintStyle danger,
                          PaintStyle success,
                          PaintStyle selected,
                          PaintStyle disabled,
                          PaintStyle border,
                          PaintStyle shadow,
                          PaintStyle focus,
                          PaintStyle badge,
                          PaintStyle progress) {
    ThemeDefinition t = {id, key, name, {}};
    t.tokens[static_cast<int>(ThemeToken::TextPrimary)] = text_primary;
    t.tokens[static_cast<int>(ThemeToken::TextSecondary)] = text_secondary;
    t.tokens[static_cast<int>(ThemeToken::BackgroundPrimary)] = bg_primary;
    t.tokens[static_cast<int>(ThemeToken::BackgroundSecondary)] = bg_secondary;
    t.tokens[static_cast<int>(ThemeToken::Accent)] = accent;
    t.tokens[static_cast<int>(ThemeToken::Warning)] = warning;
    t.tokens[static_cast<int>(ThemeToken::Danger)] = danger;
    t.tokens[static_cast<int>(ThemeToken::SuccessLike)] = success;
    t.tokens[static_cast<int>(ThemeToken::Selected)] = selected;
    t.tokens[static_cast<int>(ThemeToken::Disabled)] = disabled;
    t.tokens[static_cast<int>(ThemeToken::Border)] = border;
    t.tokens[static_cast<int>(ThemeToken::Shadow)] = shadow;
    t.tokens[static_cast<int>(ThemeToken::Focus)] = focus;
    t.tokens[static_cast<int>(ThemeToken::Badge)] = badge;
    t.tokens[static_cast<int>(ThemeToken::ProgressFill)] = progress;
    return t;
}

const ThemeDefinition kThemes[] = {
    MakeTheme(
        ThemeId::Industrial, "nintendo_pop", "Nintendo Pop",
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(WHITE, RED, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(WHITE, RED, RED, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(WHITE, RED, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray, 1, RefreshCost::AvoidLargeArea),
        MakePaint(WHITE, RED, BLACK, DitherToken::None, 2, RefreshCost::SmallAccent),
        MakePaint(WHITE, RED, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent),
        MakePaint(WHITE, RED, BLACK, DitherToken::None, 1, RefreshCost::SmallAccent)),
    MakeTheme(
        ThemeId::BrightLemon, "bright_lemon", "Bright Lemon",
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherSoft),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherSoft),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherGold),
        MakePaint(WHITE, RED, RED),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherSoft),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::None, 2),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, BLACK)),
    MakeTheme(
        ThemeId::Console, "console", "Console",
        MakePaint(WHITE, BLACK, WHITE),
        MakePaint(YELLOW, BLACK, WHITE),
        MakePaint(WHITE, BLACK, WHITE, DitherToken::None, 1, RefreshCost::AvoidLargeArea),
        MakePaint(WHITE, BLACK, WHITE, DitherToken::DitherGray, 1, RefreshCost::AvoidLargeArea),
        MakePaint(BLACK, YELLOW, YELLOW),
        MakePaint(BLACK, YELLOW, YELLOW),
        MakePaint(WHITE, RED, RED),
        MakePaint(BLACK, YELLOW, YELLOW),
        MakePaint(BLACK, YELLOW, YELLOW),
        MakePaint(WHITE, BLACK, WHITE, DitherToken::DitherGray),
        MakePaint(WHITE, BLACK, WHITE),
        MakePaint(WHITE, BLACK, WHITE, DitherToken::DitherGray),
        MakePaint(BLACK, YELLOW, YELLOW, DitherToken::None, 2),
        MakePaint(BLACK, YELLOW, YELLOW),
        MakePaint(BLACK, YELLOW, YELLOW)),
    MakeTheme(
        ThemeId::PeachPaper, "peach_paper", "Peach Paper",
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(WHITE, RED, RED),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherSoft),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherPeach, 2),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherPeach),
        MakePaint(BLACK, YELLOW, BLACK, DitherToken::DitherGold)),
    MakeTheme(
        ThemeId::Sticker, "sticker", "Sticker",
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherSoft),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(WHITE, RED, RED),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(WHITE, BLACK, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherGray),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherGray),
        MakePaint(WHITE, BLACK, BLACK, DitherToken::None, 2),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, RED, RED)),
    MakeTheme(
        ThemeId::CandyPop, "candy_pop", "Candy Pop",
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, RED, DitherToken::DitherPeach),
        MakePaint(BLACK, WHITE, BLACK),
        MakePaint(BLACK, WHITE, YELLOW, DitherToken::DitherGold),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(WHITE, RED, RED),
        MakePaint(BLACK, YELLOW, BLACK),
        MakePaint(BLACK, YELLOW, RED),
        MakePaint(BLACK, WHITE, BLACK, DitherToken::DitherLightGray),
        MakePaint(BLACK, WHITE, RED),
        MakePaint(BLACK, WHITE, RED, DitherToken::DitherPeach),
        MakePaint(BLACK, YELLOW, RED, DitherToken::None, 2),
        MakePaint(BLACK, YELLOW, RED),
        MakePaint(WHITE, RED, RED)),
};

const ThemeDefinition& ThemeById(ThemeId id) {
    for (const auto& theme : kThemes) {
        if (theme.id == id) return theme;
    }
    return kThemes[0];
}

bool DitherPixel(DitherToken token, int x, int y) {
    switch (token) {
        case DitherToken::DitherGray:
            return ((x & 1) == 0) && ((y & 1) == 0);
        case DitherToken::DitherLightGray:
            return ((x & 3) == 0) && ((y & 3) == 0);
        case DitherToken::DitherOrange:
        case DitherToken::DitherGold:
            return ((x & 3) != 0) || ((y & 3) != 0);
        case DitherToken::DitherPeach:
            return ((x & 3) == 0) && ((y & 1) == 0);
        case DitherToken::DitherNoise:
            return (((x * 17) ^ (y * 31)) & 7) < 3;
        case DitherToken::DitherSoft:
            return ((x & 7) == 0) && ((y & 3) == 0);
        case DitherToken::None:
        default:
            return false;
    }
}

Color DitherColor(DitherToken token, const PaintStyle& style, int x, int y) {
    if (token == DitherToken::None) return style.bg;
    const bool on = DitherPixel(token, x, y);
    switch (token) {
        case DitherToken::DitherOrange:
            return on ? YELLOW : RED;
        case DitherToken::DitherPeach:
            return on ? RED : WHITE;
        case DitherToken::DitherGold:
            return on ? YELLOW : WHITE;
        case DitherToken::DitherGray:
        case DitherToken::DitherLightGray:
        case DitherToken::DitherNoise:
        case DitherToken::DitherSoft:
        default:
            return on ? style.fg : style.bg;
    }
}

PaintStyle NormalizeForPanel(PaintStyle style) {
#if CONFIG_ZECTRIX_EPD_PANEL_1BPP
    // On the black/white EPD, RED and YELLOW are electrically rendered as
    // black. Keep raw photo pixels untouched in the display driver, but make
    // semantic UI surfaces degrade to readable white cards with black ink.
    if (style.bg == RED || style.bg == YELLOW) {
        style.bg = WHITE;
        if (style.dither == DitherToken::None) {
            style.dither = DitherToken::DitherLightGray;
        }
    }
    if (style.fg == WHITE || style.fg == RED || style.fg == YELLOW) {
        style.fg = BLACK;
    }
    if (style.border == RED || style.border == YELLOW || style.border == WHITE) {
        style.border = BLACK;
    }
    if (style.dither == DitherToken::DitherOrange ||
        style.dither == DitherToken::DitherPeach ||
        style.dither == DitherToken::DitherGold) {
        style.dither = DitherToken::DitherLightGray;
    }
#endif
    return style;
}

}  // namespace

PaintStyle MakePaint(Color fg, Color bg, Color border,
                     DitherToken dither,
                     uint8_t border_width,
                     RefreshCost refresh_cost) {
    PaintStyle s;
    s.fg = fg;
    s.bg = bg;
    s.border = border;
    s.dither = dither;
    s.border_width = border_width;
    s.invert_text = false;
    s.refresh_cost = refresh_cost;
    return s;
}

ThemeManager& ThemeManager::Get() {
    static ThemeManager manager;
    return manager;
}

const ThemeDefinition& ThemeManager::Current() const {
    return ThemeById(current_);
}

const ThemeDefinition& ThemeManager::GetTheme(ThemeId id) const {
    return ThemeById(id);
}

bool ThemeManager::SetTheme(ThemeId id) {
    // Runtime theme switching is intentionally collapsed to Nintendo Pop for
    // now. Older firmware may have persisted keys such as "candy_pop"; keep
    // the old definitions below for future revival, but never activate them
    // until multi-theme UI is explicitly re-enabled.
    current_ = kOnlyRuntimeTheme;
    return true;
}

bool ThemeManager::SetThemeByKey(const char* key) {
    return SetTheme(FromKey(key));
}

PaintStyle ThemeManager::Style(ThemeToken token) const {
    const int i = static_cast<int>(token);
    if (i < 0 || i >= kTokenCount) return NormalizeForPanel(Current().tokens[0]);
    return NormalizeForPanel(Current().tokens[i]);
}

PaintStyle ThemeManager::Component(ComponentRole role) const {
    switch (role) {
        case ComponentRole::ButtonNormal:
            return Style(ThemeToken::Accent);
        case ComponentRole::ButtonSelected:
        case ComponentRole::TodoSelected:
        case ComponentRole::SettingsSelected:
        case ComponentRole::QuickSwitchRow:
            return Style(ThemeToken::Selected);
        case ComponentRole::ButtonDisabled:
        case ComponentRole::TodoCompleted:
            return Style(ThemeToken::Disabled);
        case ComponentRole::ButtonDanger:
        case ComponentRole::TodoOverdue:
            return Style(ThemeToken::Danger);
        case ComponentRole::CardElevated:
            return Style(ThemeToken::BackgroundSecondary);
        case ComponentRole::CardWarning:
            return Style(ThemeToken::Warning);
        case ComponentRole::Progress:
            return Style(ThemeToken::ProgressFill);
        case ComponentRole::Panel:
        case ComponentRole::CardDefault:
        case ComponentRole::TodoNormal:
            return Style(ThemeToken::BackgroundSecondary);
        case ComponentRole::StatusBar:
        case ComponentRole::Modal:
        case ComponentRole::SettingsRow:
        default:
            return Style(ThemeToken::BackgroundPrimary);
    }
}

Color ThemeManager::ColorFor(ThemeToken token) const {
    return Style(token).fg;
}

int ThemeManager::ThemeCount() {
    return 1;
}

ThemeId ThemeManager::ThemeAt(int index) {
    if (index < 0 || index >= ThemeCount()) return ThemeId::Industrial;
    return kThemes[index].id;
}

const char* ThemeManager::Key(ThemeId id) {
    return ThemeById(id).key;
}

const char* ThemeManager::DisplayName(ThemeId id) {
    return ThemeById(id).display_name;
}

ThemeId ThemeManager::FromKey(const char* key, ThemeId fallback) {
    (void)fallback;
    if (!key || key[0] == '\0') return kOnlyRuntimeTheme;
    if (std::strcmp(key, kThemes[0].key) == 0 || std::strcmp(key, "industrial") == 0) {
        return kOnlyRuntimeTheme;
    }
    return kOnlyRuntimeTheme;
}

void DrawStyledRect(uint8_t* fb, int width, const Rect& r, const PaintStyle& style) {
    if (!fb || r.w <= 0 || r.h <= 0) return;
    const PaintStyle panel_style = NormalizeForPanel(style);
    if (panel_style.dither == DitherToken::None) {
        DrawRect(fb, width, r, panel_style.bg);
        return;
    }
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            set_pixel(fb, width, x, y, DitherColor(panel_style.dither, panel_style, x, y));
        }
    }
}

void DrawStyledRoundRect(uint8_t* fb, int width, int height, const Rect& r, int radius,
                         const PaintStyle& style) {
    if (!fb || r.w <= 0 || r.h <= 0) return;
    const PaintStyle panel_style = NormalizeForPanel(style);
    if (panel_style.dither == DitherToken::None) {
        DrawRoundRect(fb, width, height, r, radius, panel_style.bg, panel_style.border, panel_style.border_width);
        return;
    }
    DrawRoundRect(fb, width, height, r, radius, panel_style.bg, panel_style.border, panel_style.border_width);
    const Rect inner = {r.x + static_cast<int>(panel_style.border_width),
                        r.y + static_cast<int>(panel_style.border_width),
                        r.w - static_cast<int>(panel_style.border_width) * 2,
                        r.h - static_cast<int>(panel_style.border_width) * 2};
    DrawStyledRect(fb, width, inner, panel_style);
}

void DrawStyledBorder(uint8_t* fb, int width, const Rect& r, const PaintStyle& style) {
    const PaintStyle panel_style = NormalizeForPanel(style);
    DrawRectBorder(fb, width, r, panel_style.border_width, panel_style.border);
}

void DrawStyledText(uint8_t* fb, int width, int x, int y, const char* text,
                    const lv_font_t* font, const PaintStyle& style,
                    int height_limit) {
    const PaintStyle panel_style = NormalizeForPanel(style);
    DrawText(fb, width, x, y, text, font, panel_style.fg, height_limit);
}

void DrawStyledIcon(uint8_t* fb, int width, int x, int y, const char* icon_code,
                    const lv_font_t* font, const PaintStyle& style) {
    const PaintStyle panel_style = NormalizeForPanel(style);
    DrawIcon(fb, width, x, y, icon_code, font, panel_style.fg);
}

void DrawStyledProgress(uint8_t* fb, int width, const Rect& r, int value_pct,
                        const PaintStyle& style, int radius) {
    const PaintStyle panel_style = NormalizeForPanel(style);
    DrawProgress(fb, width, r, value_pct, panel_style.bg, panel_style.fg, radius);
}

}  // namespace rawdraw
