#include "rawdraw/layout_utils.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace rawdraw {

int CalcBaselineY(const lv_font_t* font, int center_y, int visual_offset) {
    if (!font) return center_y + visual_offset;
    return center_y + static_cast<int>(font->base_line) -
           static_cast<int>(font->line_height) / 2 + visual_offset;
}

int TopYFromBaseline(const lv_font_t* font, int baseline_y) {
    if (!font) return baseline_y;
    return baseline_y - static_cast<int>(font->base_line);
}

int CenterTextTopY(const lv_font_t* font, int box_top, int box_height, int visual_offset) {
    const int center_y = box_top + box_height / 2;
    return TopYFromBaseline(font, CalcBaselineY(font, center_y, visual_offset));
}

TextInkBounds MeasureTextInkBounds(const lv_font_t* font, const char* text) {
    TextInkBounds bounds;
    if (!font || !text || text[0] == '\0') return bounds;

    int min_y = std::numeric_limits<int>::max();
    int max_y = std::numeric_limits<int>::min();
    const char* p = text;
    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;
        if (ch == '\n') continue;

        lv_font_glyph_dsc_t g = {};
        g.resolved_font = font;
        if (!lv_font_get_glyph_dsc(font, &g, ch, 0)) continue;
        if (g.box_h == 0) continue;

        const int gy = static_cast<int>(font->line_height) -
                       static_cast<int>(font->base_line) -
                       static_cast<int>(g.ofs_y) -
                       static_cast<int>(g.box_h);
        min_y = std::min(min_y, gy);
        max_y = std::max(max_y, gy + static_cast<int>(g.box_h) - 1);
    }

    if (min_y == std::numeric_limits<int>::max()) return bounds;
    bounds.valid = true;
    bounds.top = min_y;
    bounds.bottom = max_y;
    bounds.height = max_y - min_y + 1;
    return bounds;
}

int InkCenteredTextTopY(const lv_font_t* font, const char* text, int center_y, int visual_offset) {
    const TextInkBounds bounds = MeasureTextInkBounds(font, text);
    if (!bounds.valid) {
        return TopYFromBaseline(font, CalcBaselineY(font, center_y, visual_offset));
    }
    const int ink_center_from_y = (bounds.top + bounds.bottom) / 2;
    return center_y - ink_center_from_y + visual_offset;
}

int InkCenteredTextTopYInBox(const lv_font_t* font, const char* text,
                             int box_top, int box_height, int visual_offset) {
    return InkCenteredTextTopY(font, text, box_top + box_height / 2, visual_offset);
}

}  // namespace rawdraw
