/**
 * @file theme.h
 * @brief Semantic 4-color theme system for RawDraw e-paper UI.
 */

#ifndef RAWDRAW_THEME_H
#define RAWDRAW_THEME_H

#include <stdint.h>
#include "rawdraw.h"
#include "font_engine.h"

namespace rawdraw {

enum class ThemeId : uint8_t {
    Industrial = 0,
    BrightLemon,
    Console,
    PeachPaper,
    Sticker,
    CandyPop,
};

enum class ThemeToken : uint8_t {
    TextPrimary,
    TextSecondary,
    BackgroundPrimary,
    BackgroundSecondary,
    Accent,
    Warning,
    Danger,
    SuccessLike,
    Selected,
    Disabled,
    Border,
    Shadow,
    Focus,
    Badge,
    ProgressFill,
    Count,
};

enum class DitherToken : uint8_t {
    None,
    DitherGray,
    DitherLightGray,
    DitherOrange,
    DitherPeach,
    DitherGold,
    DitherNoise,
    DitherSoft,
};

enum class ComponentRole : uint8_t {
    ButtonNormal,
    ButtonSelected,
    ButtonDisabled,
    ButtonDanger,
    CardDefault,
    CardElevated,
    CardWarning,
    TodoNormal,
    TodoSelected,
    TodoCompleted,
    TodoOverdue,
    Modal,
    Panel,
    StatusBar,
    Progress,
    SettingsRow,
    SettingsSelected,
    QuickSwitchRow,
};

enum class RefreshCost : uint8_t {
    StaticSafe,
    SmallAccent,
    AvoidFrequent,
    AvoidLargeArea,
};

struct PaintStyle {
    Color fg = BLACK;
    Color bg = WHITE;
    Color border = BLACK;
    DitherToken dither = DitherToken::None;
    uint8_t border_width = 1;
    bool invert_text = false;
    RefreshCost refresh_cost = RefreshCost::StaticSafe;
};

struct ThemeDefinition {
    ThemeId id;
    const char* key;
    const char* display_name;
    PaintStyle tokens[static_cast<int>(ThemeToken::Count)];
};

class ThemeManager {
public:
    static ThemeManager& Get();

    ThemeId CurrentId() const { return current_; }
    const ThemeDefinition& Current() const;
    const ThemeDefinition& GetTheme(ThemeId id) const;
    bool SetTheme(ThemeId id);
    bool SetThemeByKey(const char* key);

    PaintStyle Style(ThemeToken token) const;
    PaintStyle Component(ComponentRole role) const;
    Color ColorFor(ThemeToken token) const;

    static int ThemeCount();
    static ThemeId ThemeAt(int index);
    static const char* Key(ThemeId id);
    static const char* DisplayName(ThemeId id);
    static ThemeId FromKey(const char* key, ThemeId fallback = ThemeId::Industrial);

private:
    ThemeManager() = default;
    ThemeId current_ = ThemeId::Industrial;
};

PaintStyle MakePaint(Color fg, Color bg, Color border,
                     DitherToken dither = DitherToken::None,
                     uint8_t border_width = 1,
                     RefreshCost refresh_cost = RefreshCost::StaticSafe);

void DrawStyledRect(uint8_t* fb, int width, const Rect& r, const PaintStyle& style);
void DrawStyledRoundRect(uint8_t* fb, int width, int height, const Rect& r, int radius,
                         const PaintStyle& style);
void DrawStyledBorder(uint8_t* fb, int width, const Rect& r, const PaintStyle& style);
void DrawStyledText(uint8_t* fb, int width, int x, int y, const char* text,
                    const lv_font_t* font, const PaintStyle& style,
                    int height_limit = 300);
void DrawStyledIcon(uint8_t* fb, int width, int x, int y, const char* icon_code,
                    const lv_font_t* font, const PaintStyle& style);
void DrawStyledProgress(uint8_t* fb, int width, const Rect& r, int value_pct,
                        const PaintStyle& style, int radius = -1);

}  // namespace rawdraw

#endif  // RAWDRAW_THEME_H
