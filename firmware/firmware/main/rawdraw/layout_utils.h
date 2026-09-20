#pragma once

#include "rawdraw/font_engine.h"

namespace rawdraw {

struct TextInkBounds {
    bool valid = false;
    int top = 0;
    int bottom = 0;
    int height = 0;
};

// Returns the baseline y for visually centering text around a known component
// center line. All rawdraw renderers should use this helper instead of local
// copies so font nudge fixes happen in one place.
int CalcBaselineY(const lv_font_t* font, int center_y, int visual_offset = 0);

// Converts a baseline y back to the top y expected by DrawText().
int TopYFromBaseline(const lv_font_t* font, int baseline_y);

// Returns a DrawText() top y for centering text inside a box at box_top.
int CenterTextTopY(const lv_font_t* font, int box_top, int box_height, int visual_offset = 0);

// Measures the actual black-pixel vertical bounds DrawText() will produce when
// called with y=0. This accounts for glyph box_h/ofs_y, which line_height-only
// centering cannot see.
TextInkBounds MeasureTextInkBounds(const lv_font_t* font, const char* text);

// Returns a DrawText() top y that centers the actual glyph ink, not merely the
// font line box. Use this for buttons, table rows, pills, input boxes, and
// compact modal rows.
int InkCenteredTextTopY(const lv_font_t* font, const char* text,
                        int center_y, int visual_offset = 0);

int InkCenteredTextTopYInBox(const lv_font_t* font, const char* text,
                             int box_top, int box_height, int visual_offset = 0);

}  // namespace rawdraw
