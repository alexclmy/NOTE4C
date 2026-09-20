/**
 * @file autonomy_compose.cc
 * @brief The device's own compositor.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Portions adapted from eMini Home 0.4.0, (c) 2026 Tomasz Fiedoruk, MIT,
 * commit 05ec313f86b1ecfb6f8ecc692fb1f9ef00e78544:
 *
 *   - the ordered 4x4 Bayer kernel and the `< level` comparison in Dither(),
 *     adapted from home_render.c:45-104 (mix/bayer/pixel). The matrix is the
 *     standard ordered-dither matrix; what is taken from eMini is the decision
 *     to dither on this panel at all, and the shape of the helper.
 *   - the descending auto-fit discipline in FitSize(), adapted in principle
 *     from home_render.c:794-889 (poster_layout). No code is copied: our text
 *     engine is not eMini's, so what carries over is the rule — measure with
 *     the routine that will paint, and walk a size ladder downwards rather
 *     than scaling a bitmap.
 *   - the sparkline-with-dithered-band idea in DrawCurve(), from
 *     forecast_graph. The arithmetic is ours and is entirely integer.
 *
 * See THIRD_PARTY_NOTICES.md. eMini's font, its MET Norway parser, its fetcher
 * and its partitioning are deliberately not used; see the plan's §9.1.
 *
 * Read autonomy_compose.h before editing, particularly the paragraph about why
 * there is no floating point in here. It is not a style preference: this file
 * has a twin in TypeScript and the two are compared byte for byte.
 */

#include "autonomy_compose.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "autonomy_font_data.h"

namespace autonomy {

int32_t FloorDiv(int32_t a, int32_t b) {
    // C++ integer division truncates toward zero; JavaScript's Math.floor
    // rounds toward negative infinity. They agree for positive numerators and
    // disagree for every negative one — which, for a panel that draws
    // temperatures, is every winter.
    if (b == 0) return 0;
    const int32_t q = a / b;
    const int32_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

void PackFrame(const uint8_t* canvas, uint8_t* frame_out) {
    memset(frame_out, 0, kPackedBytes);
    for (size_t i = 0; i < kCanvasBytes; ++i) {
        const uint8_t value = canvas[i] & 0x3u;
        frame_out[i >> 2] = static_cast<uint8_t>(
            frame_out[i >> 2] | (value << (6 - 2 * (i & 3))));
    }
}

namespace {

// ------------------------------------------------------------------ layout --

constexpr int kMargin = 12;
constexpr int kHeaderTop = 8;
constexpr int kHeaderRuleY = 34;
constexpr int kBodyTop = 42;
constexpr int kFooterRuleY = 268;
constexpr int kFooterTop = 276;
constexpr int kBodyBottomWithFooter = kFooterRuleY - 6;
constexpr int kBodyBottomNoFooter = kPanelHeight - kMargin;
constexpr int kContentWidth = kPanelWidth - 2 * kMargin;

// --------------------------------------------------------------- the canvas --

struct Canvas {
    uint8_t* px;

    void Fill(uint8_t c) { memset(px, c, kCanvasBytes); }

    void Plot(int x, int y, uint8_t c) {
        if (x < 0 || x >= kPanelWidth || y < 0 || y >= kPanelHeight) return;
        px[static_cast<size_t>(y) * kPanelWidth + x] = c;
    }

    void Rect(int x, int y, int w, int h, uint8_t c) {
        for (int yy = y; yy < y + h; ++yy) {
            if (yy < 0 || yy >= kPanelHeight) continue;
            for (int xx = x; xx < x + w; ++xx) {
                if (xx < 0 || xx >= kPanelWidth) continue;
                px[static_cast<size_t>(yy) * kPanelWidth + xx] = c;
            }
        }
    }

    void HLine(int x, int y, int w, uint8_t c) { Rect(x, y, w, 1, c); }

    /**
     * @brief Ordered-dither a rectangle between two palette entries.
     *
     * Adapted from eMini Home's mix()/bayer[][]/pixel() (home_render.c:45-104),
     * MIT, (c) 2026 Tomasz Fiedoruk.
     *
     * @param level 0..16. 0 is all background, 16 is all foreground.
     *
     * Ordered rather than error-diffused, for a reason specific to this medium:
     * the frame is hashed to decide whether the panel needs a 25-second refresh
     * at all, and error diffusion makes every pixel depend on its neighbours,
     * so a one-degree change in one corner would alter the whole field and cost
     * a refresh. An ordered kernel is a pure function of (x, y, level), so an
     * unchanged region hashes unchanged.
     */
    void Dither(int x, int y, int w, int h, int level, uint8_t fg, uint8_t bg) {
        static const uint8_t kBayer[4][4] = {
            {0, 8, 2, 10},
            {12, 4, 14, 6},
            {3, 11, 1, 9},
            {15, 7, 13, 5},
        };
        for (int yy = y; yy < y + h; ++yy) {
            if (yy < 0 || yy >= kPanelHeight) continue;
            for (int xx = x; xx < x + w; ++xx) {
                if (xx < 0 || xx >= kPanelWidth) continue;
                const uint8_t threshold = kBayer[yy & 3][xx & 3];
                px[static_cast<size_t>(yy) * kPanelWidth + xx] =
                    (threshold < level) ? fg : bg;
            }
        }
    }
};

// ------------------------------------------------------------------- text --

struct CodepointReader {
    const char* s;
    size_t len;
    size_t pos;
    /// Expansion buffer for fallbacks producing more than one character
    /// ("Œ" becomes "OE"), drained before the next real decode.
    char pending[4];
    uint8_t pending_len;
    uint8_t pending_pos;
};

CodepointReader Reader(const char* s, size_t len) {
    CodepointReader r;
    r.s = s;
    r.len = len;
    r.pos = 0;
    memset(r.pending, 0, sizeof(r.pending));
    r.pending_len = 0;
    r.pending_pos = 0;
    return r;
}

/**
 * @brief Replace characters the atlas subset does not carry.
 *
 * The same table as the tower's FALLBACKS in src/core/font.ts, and it exists
 * for the same reason: typographic punctuation arrives from anything pasted out
 * of a word processor, and dropping it silently turns "aujourd’hui" into
 * "aujourdhui" on the glass.
 */
const char* Fallback(uint32_t cp) {
    switch (cp) {
        case 0x2019: return "'";
        case 0x2018: return "'";
        case 0x201c: return "\"";
        case 0x201d: return "\"";
        case 0x2013: return "-";
        case 0x2014: return "-";
        case 0x2022: return "\xc2\xb7";  // • becomes ·, which the atlas has
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        case 0x0178: return "Y";
        case 0x20ac: return "EUR";
        default: return nullptr;
    }
}

/// Decode one UTF-8 code point, skipping malformed bytes rather than drawing a
/// replacement glyph nobody wrote. Returns 0 at the end.
uint32_t NextRaw(const char* s, size_t len, size_t* pos) {
    while (*pos < len) {
        const unsigned char c = static_cast<unsigned char>(s[*pos]);
        if (c < 0x80) {
            ++*pos;
            return c;
        }
        size_t extra = 0;
        uint32_t cp = 0;
        if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1fu;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0fu;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            ++*pos;
            continue;
        }
        if (*pos + extra >= len) {
            *pos = len;
            return 0;
        }
        for (size_t k = 1; k <= extra; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[*pos + k]) & 0x3fu);
        }
        *pos += extra + 1;
        return cp;
    }
    return 0;
}

/// Next drawable code point, fallbacks applied. 0 when the string is done.
uint32_t NextChar(CodepointReader* r) {
    if (r->pending_pos < r->pending_len) {
        return static_cast<unsigned char>(r->pending[r->pending_pos++]);
    }
    r->pending_len = 0;
    r->pending_pos = 0;
    const uint32_t cp = NextRaw(r->s, r->len, &r->pos);
    if (cp == 0) return 0;
    const char* sub = Fallback(cp);
    if (sub == nullptr) return cp;
    const size_t n = strlen(sub);
    if (n == 1) return static_cast<unsigned char>(sub[0]);
    const size_t keep = n < sizeof(r->pending) ? n : sizeof(r->pending);
    for (size_t i = 0; i < keep; ++i) r->pending[i] = sub[i];
    r->pending_len = static_cast<uint8_t>(keep);
    r->pending_pos = 1;
    return static_cast<unsigned char>(r->pending[0]);
}

int MeasureText(const font::Atlas& atlas, const char* s, size_t len) {
    CodepointReader r = Reader(s, len);
    int width = 0;
    for (uint32_t cp = NextChar(&r); cp != 0; cp = NextChar(&r)) {
        const font::Glyph* g = font::FindGlyph(atlas, cp);
        if (g != nullptr) width += g->advance;
    }
    return width;
}

int MeasureCStr(const font::Atlas& atlas, const char* s) {
    return MeasureText(atlas, s, strlen(s));
}

int LineHeight(const font::Atlas& atlas) { return atlas.ascent + atlas.descent; }

void DrawGlyph(Canvas& c, const font::Glyph& g, int pen, int y_top, uint8_t color) {
    const int origin_x = pen + g.left;
    const int origin_y = y_top + g.top;
    const int stride = (g.width + 7) / 8;
    for (int gy = 0; gy < g.height; ++gy) {
        const int target_y = origin_y + gy;
        if (target_y < 0 || target_y >= kPanelHeight) continue;
        for (int gx = 0; gx < g.width; ++gx) {
            const uint32_t byte_index =
                g.bitmap_offset + static_cast<uint32_t>(gy * stride + (gx >> 3));
            if (byte_index >= font::kGlyphBitsLen) continue;
            const uint8_t byte = font::kGlyphBits[byte_index];
            if (((byte >> (7 - (gx & 7))) & 1u) == 0) continue;
            const int target_x = origin_x + gx;
            if (target_x < 0 || target_x >= kPanelWidth) continue;
            c.px[static_cast<size_t>(target_y) * kPanelWidth + target_x] = color;
        }
    }
}

/**
 * @brief Draw text at a left-ascender origin.
 *
 * x, y is the top of the ascent line, matching the tower's
 * FrameBuffer.drawText exactly, so a layout coordinate means the same thing in
 * both renderers.
 */
int DrawText(Canvas& c, const font::Atlas& atlas, int x, int y_top, const char* s,
             size_t len, uint8_t color) {
    CodepointReader r = Reader(s, len);
    int pen = x;
    for (uint32_t cp = NextChar(&r); cp != 0; cp = NextChar(&r)) {
        const font::Glyph* g = font::FindGlyph(atlas, cp);
        if (g == nullptr) continue;
        DrawGlyph(c, *g, pen, y_top, color);
        pen += g->advance;
    }
    return pen - x;
}

int DrawCStr(Canvas& c, const font::Atlas& atlas, int x, int y_top, const char* s,
             uint8_t color) {
    return DrawText(c, atlas, x, y_top, s, strlen(s), color);
}

int DrawRight(Canvas& c, const font::Atlas& atlas, int right, int y_top,
              const char* s, uint8_t color) {
    return DrawText(c, atlas, right - MeasureCStr(atlas, s), y_top, s, strlen(s),
                    color);
}

int DrawCentred(Canvas& c, const font::Atlas& atlas, int centre, int y_top,
                const char* s, uint8_t color) {
    // FloorDiv rather than `/ 2` so a centred odd width lands on the same pixel
    // in both renderers.
    return DrawText(c, atlas, centre - FloorDiv(MeasureCStr(atlas, s), 2), y_top, s,
                    strlen(s), color);
}

/**
 * @brief Copy at most @p max_w pixels' worth of @p s into @p out, ending in an
 *        ellipsis when it did not fit.
 *
 * Truncation is visible rather than silent: a list row that ran out of room
 * ends in "…", so a reader can tell a short row from a cut one.
 */
void FitText(const font::Atlas& atlas, const char* s, size_t len, int max_w,
             char* out, size_t out_cap) {
    if (out_cap == 0) return;
    out[0] = '\0';
    if (MeasureText(atlas, s, len) <= max_w) {
        const size_t n = len < out_cap - 1 ? len : out_cap - 1;
        memcpy(out, s, n);
        out[n] = '\0';
        return;
    }
    const font::Glyph* dots = font::FindGlyph(atlas, 0x2026);
    const int budget = max_w - (dots != nullptr ? dots->advance : 0);

    size_t pos = 0;
    size_t written = 0;
    int width = 0;
    while (pos < len) {
        CodepointReader probe = Reader(s + pos, len - pos);
        const uint32_t cp = NextChar(&probe);
        if (cp == 0) break;
        const size_t consumed = probe.pos;
        if (consumed == 0) break;
        const font::Glyph* g = font::FindGlyph(atlas, cp);
        const int adv = g != nullptr ? g->advance : 0;
        if (width + adv > budget) break;
        if (written + consumed + 4 >= out_cap) break;
        memcpy(out + written, s + pos, consumed);
        written += consumed;
        width += adv;
        pos += consumed;
    }
    if (dots != nullptr && written + 4 < out_cap) {
        out[written++] = '\xe2';
        out[written++] = '\x80';
        out[written++] = '\xa6';
    }
    out[written] = '\0';
}

/// One wrapped line: an offset and a length into the source string.
struct Line {
    uint16_t off;
    uint16_t len;
};

constexpr int kMaxLines = 16;

/**
 * @brief Greedy word wrap.
 *
 * Greedy rather than balanced because the tower wraps greedily too, and a
 * prettier algorithm here would be a second opinion the golden test reports as
 * a difference.
 */
int WrapText(const font::Atlas& atlas, const char* s, size_t len, int max_w,
             Line* lines, int max_lines) {
    int count = 0;
    size_t line_start = 0;
    size_t pos = 0;
    size_t last_break = 0;
    int width = 0;

    while (count < max_lines) {
        if (pos >= len) {
            if (pos > line_start) {
                lines[count].off = static_cast<uint16_t>(line_start);
                lines[count].len = static_cast<uint16_t>(pos - line_start);
                ++count;
            }
            break;
        }
        const size_t before = pos;
        CodepointReader probe = Reader(s + pos, len - pos);
        const uint32_t cp = NextChar(&probe);
        if (cp == 0) {
            if (pos > line_start) {
                lines[count].off = static_cast<uint16_t>(line_start);
                lines[count].len = static_cast<uint16_t>(pos - line_start);
                ++count;
            }
            break;
        }
        pos += probe.pos;
        if (cp == ' ') last_break = before;

        const font::Glyph* g = font::FindGlyph(atlas, cp);
        const int adv = g != nullptr ? g->advance : 0;

        if (width + adv > max_w && before > line_start) {
            size_t end;
            size_t next;
            if (last_break > line_start) {
                end = last_break;
                next = last_break + 1;  // skip the space itself
            } else {
                // One unbroken word wider than the box. Break mid-word rather
                // than overflow: a row running off the panel is worse than a
                // row broken without a hyphen.
                end = before;
                next = before;
            }
            lines[count].off = static_cast<uint16_t>(line_start);
            lines[count].len = static_cast<uint16_t>(end - line_start);
            ++count;
            line_start = next;
            last_break = next;
            pos = next;
            width = 0;
            continue;
        }
        width += adv;
    }
    return count;
}

/**
 * @brief Pick the largest size on the ladder whose wrapped text fits the box.
 *
 * The descending walk is the discipline adapted from eMini's poster_layout:
 * measure with the same routine that will paint, and step down rather than
 * scale. Never returns null when any atlas exists — the smallest rung is used
 * when nothing fits, and the caller clips.
 */
const font::Atlas* FitSize(const char* s, size_t len, int max_w, int max_h,
                           bool bold, int* line_count_out, Line* lines,
                           int max_lines) {
    const font::Atlas* smallest = nullptr;
    for (size_t i = font::kSizeCount; i-- > 0;) {
        const font::Atlas* atlas = font::Find(font::kSizes[i], bold);
        if (atlas == nullptr) continue;
        smallest = atlas;
        const int n = WrapText(*atlas, s, len, max_w, lines, max_lines);
        if (n > 0 && n * LineHeight(*atlas) <= max_h) {
            *line_count_out = n;
            return atlas;
        }
    }
    *line_count_out =
        smallest != nullptr ? WrapText(*smallest, s, len, max_w, lines, max_lines) : 0;
    return smallest;
}

// -------------------------------------------------------------- civil time --

struct Civil {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int weekday;  ///< 0 = Sunday, as the tower counts
};

/// Inverse of the forecast parser's DaysFromCivil. Same algorithm, same source.
Civil CivilFromEpoch(int64_t epoch) {
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    Civil c;
    c.hour = static_cast<int>(rem / 3600);
    c.minute = static_cast<int>((rem % 3600) / 60);
    // 1970-01-01 was a Thursday, which is weekday 4 counting Sunday as 0.
    int64_t wd = (days + 4) % 7;
    if (wd < 0) wd += 7;
    c.weekday = static_cast<int>(wd);

    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    c.year = static_cast<int>(y);
    c.month = static_cast<int>(m);
    c.day = static_cast<int>(d);
    return c;
}

/// French abbreviations. The panel's content is French, so its furniture is.
const char* const kWeekdayFr[7] = {"dim.", "lun.", "mar.", "mer.",
                                   "jeu.", "ven.", "sam."};
const char* const kMonthFr[12] = {"janv.", "févr.", "mars", "avr.",
                                  "mai",   "juin",  "juil.", "août",
                                  "sept.", "oct.",  "nov.",  "déc."};

void FormatClock(const Civil& c, char* out, size_t cap) {
    snprintf(out, cap, "%02d:%02d", c.hour, c.minute);
}

void FormatDate(const Civil& c, char* out, size_t cap) {
    snprintf(out, cap, "%s %d %s", kWeekdayFr[c.weekday], c.day,
             kMonthFr[c.month - 1]);
}

/// Tenths of a degree as whole degrees, rounding half away from zero so -0.5
/// becomes -1 rather than 0, which is what a reader expects in January.
int RoundTenths(int v) { return v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10); }

// ------------------------------------------------------------------ context --

struct Ctx {
    Canvas canvas;
    const Profile* profile;
    const weather::Forecast* forecast;
    Freshness freshness;
    int64_t now_epoch;
    int32_t utc_offset_s;
    Civil now;
    bool clock_set;
    bool degraded;
    int body_bottom;
};

Civil LocalCivil(const Ctx& ctx, int64_t utc_epoch) {
    return CivilFromEpoch(utc_epoch + ctx.utc_offset_s);
}

bool WeatherDrawable(const Ctx& ctx) {
    return ctx.forecast != nullptr && ctx.forecast->valid() &&
           ctx.freshness != Freshness::kNone &&
           ctx.freshness != Freshness::kUnavailable;
}

/// Index into the cached forecast for the current hour, clamped into range.
int CurrentHourIndex(const Ctx& ctx) {
    if (ctx.forecast == nullptr || ctx.forecast->hour_count == 0) return 0;
    if (!ctx.clock_set) return 0;
    const int32_t delta =
        static_cast<int32_t>(ctx.now_epoch - ctx.forecast->first_hour_epoch);
    int index = FloorDiv(delta, static_cast<int32_t>(weather::kHourSeconds));
    if (index < 0) index = 0;
    if (index >= ctx.forecast->hour_count) index = ctx.forecast->hour_count - 1;
    return index;
}

// ------------------------------------------------------------- condition --

bool ConditionHolds(const Condition& cond, const Ctx& ctx) {
    switch (cond.kind) {
        case ConditionKind::kDaysOfWeek:
            // With no clock there is no day, and guessing one would show a
            // weekday message on a Sunday.
            if (!ctx.clock_set) return false;
            return (cond.days_mask & (1u << ctx.now.weekday)) != 0;
        case ConditionKind::kDateRange: {
            if (!ctx.clock_set) return false;
            const int today = ctx.now.year * 10000 + ctx.now.month * 100 + ctx.now.day;
            if (cond.has_from) {
                const char* f = ctx.profile->Get(cond.from);
                const int from = (f[0] - '0') * 10000000 + (f[1] - '0') * 1000000 +
                                 (f[2] - '0') * 100000 + (f[3] - '0') * 10000 +
                                 (f[5] - '0') * 1000 + (f[6] - '0') * 100 +
                                 (f[8] - '0') * 10 + (f[9] - '0');
                if (today < from) return false;
            }
            if (cond.has_to) {
                const char* t = ctx.profile->Get(cond.to);
                const int to = (t[0] - '0') * 10000000 + (t[1] - '0') * 1000000 +
                               (t[2] - '0') * 100000 + (t[3] - '0') * 10000 +
                               (t[5] - '0') * 1000 + (t[6] - '0') * 100 +
                               (t[8] - '0') * 10 + (t[9] - '0');
                if (today > to) return false;
            }
            return true;
        }
        case ConditionKind::kWeatherState:
            switch (cond.state) {
                case WeatherState::kOk: return ctx.freshness == Freshness::kOk;
                case WeatherState::kStale: return ctx.freshness == Freshness::kStale;
                case WeatherState::kUnavailable:
                    return ctx.freshness == Freshness::kUnavailable ||
                           ctx.freshness == Freshness::kNone;
            }
            return false;
    }
    return false;
}

/// The text a conditional message should draw, or nullptr for nothing.
const char* ResolveConditional(const Module& m, const Ctx& ctx) {
    bool holds = m.match == Match::kAll;
    for (uint8_t i = 0; i < m.condition_count; ++i) {
        const bool one = ConditionHolds(m.conditions[i], ctx);
        holds = (m.match == Match::kAll) ? (holds && one) : (holds || one);
    }
    if (holds) {
        const char* t = ctx.profile->Get(m.text);
        return t[0] != '\0' ? t : nullptr;
    }
    if (!m.has_fallback) return nullptr;
    const char* f = ctx.profile->Get(m.fallback_text);
    return f[0] != '\0' ? f : nullptr;
}

// ------------------------------------------------------------- the blocks --

/// Blocks return the height they used, so a layout can stack them.

int DrawWeatherBlock(Ctx& ctx, int x, int y, int w, bool hero) {
    const font::Atlas* big = font::Find(hero ? 34 : 22, true);
    const font::Atlas* body = font::Find(15, false);
    const font::Atlas* small = font::Find(11, false);
    if (big == nullptr || body == nullptr || small == nullptr) return 0;

    if (!WeatherDrawable(ctx)) {
        // No number at all, rather than the last one we happen to remember.
        DrawCStr(ctx.canvas, *body, x, y, "Météo indisponible", kRed);
        const char* why = ctx.freshness == Freshness::kNone
                              ? "jamais reçue"
                              : "trop ancienne pour être affichée";
        DrawCStr(ctx.canvas, *small, x, y + LineHeight(*body) + 2, why, kBlack);
        return LineHeight(*body) + LineHeight(*small) + 2;
    }

    const int index = CurrentHourIndex(ctx);
    char temp[16];
    snprintf(temp, sizeof(temp), "%d°", RoundTenths(ctx.forecast->temp_c10[index]));
    DrawCStr(ctx.canvas, *big, x, y, temp, kBlack);
    const int big_w = MeasureCStr(*big, temp);

    char range[48];
    snprintf(range, sizeof(range), "%d° / %d°", RoundTenths(ctx.forecast->low_c10),
             RoundTenths(ctx.forecast->high_c10));
    DrawCStr(ctx.canvas, *body, x + big_w + 10, y + 2, range, kBlack);

    char label[64];
    FitText(*small, ctx.profile->Get(ctx.profile->weather.label),
            strlen(ctx.profile->Get(ctx.profile->weather.label)),
            w - big_w - 10, label, sizeof(label));
    DrawCStr(ctx.canvas, *small, x + big_w + 10, y + 2 + LineHeight(*body) + 2, label,
             kBlack);

    int used = LineHeight(*big);
    if (ctx.freshness == Freshness::kStale) {
        // An age beside the value, never a silent old number.
        DrawCStr(ctx.canvas, *small, x + big_w + 10,
                 y + 2 + LineHeight(*body) + 2 + LineHeight(*small) + 1,
                 "relevé ancien", kRed);
        const int stale_bottom = 2 + LineHeight(*body) + 2 + 2 * LineHeight(*small) + 1;
        if (stale_bottom > used) used = stale_bottom;
    }
    return used;
}

/**
 * @brief The twelve-hour temperature curve. Flow's centrepiece.
 *
 * Adapted in spirit from eMini's forecast_graph (home_render.c), MIT: a thin
 * sparkline with the band dithered underneath. The arithmetic is ours and is
 * entirely integer, so both renderers pick the same pixels.
 */
void DrawCurve(Ctx& ctx, int x, int y, int w, int h) {
    if (!WeatherDrawable(ctx)) return;
    const int start = CurrentHourIndex(ctx);
    int hours = ctx.forecast->hour_count - start;
    if (hours > 12) hours = 12;
    if (hours < 2) return;

    int lo = ctx.forecast->temp_c10[start];
    int hi = lo;
    for (int i = 0; i < hours; ++i) {
        const int v = ctx.forecast->temp_c10[start + i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    int span = hi - lo;
    if (span < 10) span = 10;  // a flat day still gets a readable band

    int prev_x = 0;
    int prev_y = 0;
    for (int i = 0; i < hours; ++i) {
        const int v = ctx.forecast->temp_c10[start + i];
        const int px = x + FloorDiv(i * (w - 1), hours - 1);
        const int py = y + h - 1 - FloorDiv((v - lo) * (h - 1), span);
        ctx.canvas.Dither(px, py, 1, y + h - py, 6, kBlack, kWhite);
        if (i > 0) {
            const int dx = px - prev_x;
            for (int k = 0; k <= dx; ++k) {
                const int iy =
                    prev_y + (dx == 0 ? 0 : FloorDiv((py - prev_y) * k, dx));
                ctx.canvas.Plot(prev_x + k, iy, kBlack);
                ctx.canvas.Plot(prev_x + k, iy - 1, kBlack);
            }
        }
        prev_x = px;
        prev_y = py;
    }
    ctx.canvas.HLine(x, y + h, w, kBlack);

    // Endpoints labelled, so the band is a reading rather than a decoration.
    const font::Atlas* small = font::Find(11, false);
    if (small == nullptr) return;
    char lo_s[16];
    char hi_s[16];
    snprintf(lo_s, sizeof(lo_s), "%d°", RoundTenths(lo));
    snprintf(hi_s, sizeof(hi_s), "%d°", RoundTenths(hi));
    DrawCStr(ctx.canvas, *small, x, y + h + 3, lo_s, kBlack);
    DrawRight(ctx.canvas, *small, x + w, y + h + 3, hi_s, kBlack);
}

/// Four hourly markers under the curve: now, +4, +8, +12.
int DrawSlots(Ctx& ctx, int x, int y, int w) {
    const font::Atlas* body = font::Find(15, true);
    const font::Atlas* small = font::Find(11, false);
    if (body == nullptr || small == nullptr || !WeatherDrawable(ctx)) return 0;

    const int start = CurrentHourIndex(ctx);
    const int offsets[4] = {0, 4, 8, 12};
    const int column = FloorDiv(w, 4);
    for (int i = 0; i < 4; ++i) {
        int index = start + offsets[i];
        if (index >= ctx.forecast->hour_count) index = ctx.forecast->hour_count - 1;
        const int centre = x + column * i + FloorDiv(column, 2);

        const Civil at = LocalCivil(
            ctx, ctx.forecast->first_hour_epoch +
                     static_cast<int64_t>(index) * weather::kHourSeconds);
        char hour[8];
        snprintf(hour, sizeof(hour), "%02dh", at.hour);
        DrawCentred(ctx.canvas, *small, centre, y, hour, kBlack);

        char temp[16];
        snprintf(temp, sizeof(temp), "%d°", RoundTenths(ctx.forecast->temp_c10[index]));
        DrawCentred(ctx.canvas, *body, centre, y + LineHeight(*small) + 2, temp,
                    kBlack);
    }
    return LineHeight(*small) + LineHeight(*body) + 2;
}

int DrawCountdownBlock(Ctx& ctx, const Module& m, int x, int y, bool hero) {
    const font::Atlas* big = font::Find(hero ? 34 : 22, true);
    const font::Atlas* label_font = font::Find(11, true);
    if (big == nullptr || label_font == nullptr) return 0;

    int used = 0;
    const char* label = ctx.profile->Get(m.label);
    if (label[0] != '\0') {
        DrawCStr(ctx.canvas, *label_font, x, y, label, kBlack);
        used += LineHeight(*label_font) + 2;
    }

    if (!ctx.clock_set) {
        // Never a fabricated number. A device that has not reached an NTP
        // server does not know how long it is until anything.
        const font::Atlas* mid = font::Find(22, true);
        DrawCStr(ctx.canvas, mid != nullptr ? *mid : *big, x, y + used,
                 "heure inconnue", kRed);
        return used + LineHeight(mid != nullptr ? *mid : *big);
    }

    char text[32];
    const int64_t diff = m.target_epoch - ctx.now_epoch;
    if (diff <= 0) {
        snprintf(text, sizeof(text), "maintenant");
    } else if (diff >= 86400) {
        // Days, at the granularity of the wake interval: this panel is never
        // going to tick, so a number that implies it would is a lie about how
        // often the glass is refreshed.
        snprintf(text, sizeof(text), "J-%d",
                 static_cast<int>(FloorDiv(static_cast<int32_t>(diff), 86400)));
    } else {
        snprintf(text, sizeof(text), "%d h",
                 static_cast<int>(FloorDiv(static_cast<int32_t>(diff), 3600)));
    }
    DrawCStr(ctx.canvas, *big, x, y + used, text, kBlack);
    return used + LineHeight(*big);
}

int DrawTextBlock(Ctx& ctx, const char* title, const char* text, int x, int y, int w,
                  int h) {
    const font::Atlas* head = font::Find(11, true);
    if (head == nullptr || text == nullptr || text[0] == '\0') return 0;
    int used = 0;
    if (title != nullptr && title[0] != '\0') {
        DrawCStr(ctx.canvas, *head, x, y, title, kBlack);
        used += LineHeight(*head) + 2;
    }
    Line lines[kMaxLines];
    int count = 0;
    const font::Atlas* atlas =
        FitSize(text, strlen(text), w, h - used, false, &count, lines, kMaxLines);
    if (atlas == nullptr) return used;
    const int line_h = LineHeight(*atlas);
    for (int i = 0; i < count; ++i) {
        const int ly = y + used + i * line_h;
        if (ly + line_h > y + h) break;
        DrawText(ctx.canvas, *atlas, x, ly, text + lines[i].off, lines[i].len, kBlack);
    }
    return used + count * line_h;
}

int DrawListBlock(Ctx& ctx, const Module& m, int x, int y, int w, int h) {
    const font::Atlas* head = font::Find(11, true);
    const font::Atlas* body = font::Find(15, false);
    const font::Atlas* small = font::Find(11, false);
    if (head == nullptr || body == nullptr || small == nullptr) return 0;

    int used = 0;
    const char* title = ctx.profile->Get(m.title);
    if (title[0] != '\0') {
        DrawCStr(ctx.canvas, *head, x, y, title, kBlack);
        used += LineHeight(*head) + 2;
    }
    const int row_h = LineHeight(*body) + 1;
    const int footer_h = LineHeight(*small) + 2;
    char fitted[512];
    for (uint8_t i = 0; i < m.row_count; ++i) {
        if (used + row_h + footer_h > h) break;
        const char* row = ctx.profile->Get(m.rows[i]);
        ctx.canvas.Rect(x + 1, y + used + FloorDiv(LineHeight(*body), 2) - 1, 3, 3,
                        kBlack);
        FitText(*body, row, strlen(row), w - 10, fitted, sizeof(fitted));
        DrawCStr(ctx.canvas, *body, x + 10, y + used, fitted, kBlack);
        used += row_h;
    }
    // A cached list drawn with no date is a list that looks current forever.
    char synced[64];
    if (m.synced_epoch > 0) {
        const Civil c = LocalCivil(ctx, m.synced_epoch);
        snprintf(synced, sizeof(synced), "synchronisée le %d %s", c.day,
                 kMonthFr[c.month - 1]);
    } else {
        snprintf(synced, sizeof(synced), "liste en cache");
    }
    DrawCStr(ctx.canvas, *small, x, y + used + 2, synced, kBlack);
    return used + footer_h;
}

int DrawTimestampBlock(Ctx& ctx, int x, int y) {
    const font::Atlas* small = font::Find(11, false);
    if (small == nullptr) return 0;
    char text[48];
    if (ctx.clock_set) {
        char clock[8];
        FormatClock(ctx.now, clock, sizeof(clock));
        snprintf(text, sizeof(text), "MAJ %s", clock);
    } else {
        snprintf(text, sizeof(text), "MAJ heure inconnue");
    }
    DrawCStr(ctx.canvas, *small, x, y, text, ctx.clock_set ? kBlack : kRed);
    return LineHeight(*small);
}

// -------------------------------------------------------- chrome and items --

void DrawHeader(Ctx& ctx) {
    const font::Atlas* small = font::Find(11, false);
    const font::Atlas* bold = font::Find(15, true);
    if (small == nullptr || bold == nullptr) return;

    char buf[64];
    if (ctx.clock_set) {
        FormatDate(ctx.now, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "date inconnue");
    }
    DrawCStr(ctx.canvas, *bold, kMargin, kHeaderTop, buf,
             ctx.clock_set ? kBlack : kRed);

    // The right of the header is where the panel admits things.
    if (ctx.degraded) {
        DrawRight(ctx.canvas, *small, kPanelWidth - kMargin, kHeaderTop + 3,
                  "hors ligne", kRed);
    } else if (ctx.clock_set) {
        char clock[8];
        FormatClock(ctx.now, clock, sizeof(clock));
        DrawRight(ctx.canvas, *small, kPanelWidth - kMargin, kHeaderTop + 3, clock,
                  kBlack);
    } else {
        DrawRight(ctx.canvas, *small, kPanelWidth - kMargin, kHeaderTop + 3,
                  "heure inconnue", kRed);
    }
    ctx.canvas.HLine(kMargin, kHeaderRuleY, kContentWidth, kBlack);
}

void DrawFooter(Ctx& ctx) {
    if (!ctx.profile->provenance_line) return;
    const font::Atlas* small = font::Find(11, false);
    if (small == nullptr) return;

    ctx.canvas.HLine(kMargin, kFooterRuleY, kContentWidth, kBlack);

    // "Composé sur l'appareil" is the whole point of the line: a panel composed
    // here from a three-hour-old forecast looks exactly like one the tower
    // pushed a minute ago, and this is the only thing on the glass that says
    // which. The Open-Meteo credit is the CC-BY attribution, on the artefact
    // that uses the data.
    char line[192];
    int n = snprintf(line, sizeof(line), "Composé sur l'appareil");
    if (ctx.forecast != nullptr && WeatherDrawable(ctx)) {
        if (ctx.clock_set) {
            const Civil fetched = LocalCivil(ctx, ctx.forecast->fetched_epoch);
            char clock[8];
            FormatClock(fetched, clock, sizeof(clock));
            n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                          " · Météo Open-Meteo %s", clock);
        } else {
            n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                          " · Météo Open-Meteo");
        }
    }
    char fitted[192];
    FitText(*small, line, strlen(line), kContentWidth, fitted, sizeof(fitted));
    DrawCStr(ctx.canvas, *small, kMargin, kFooterTop, fitted, kBlack);
}

/// One thing that will actually be drawn, resolved before any layout happens.
struct Item {
    const Module* module;
    /// For a conditional message, the side that won. Null for other types.
    const char* resolved_text;
};

int CollectItems(const Ctx& ctx, Item* items, int max_items) {
    int count = 0;
    for (uint8_t i = 0; i < ctx.profile->module_count && count < max_items; ++i) {
        const Module& m = ctx.profile->modules[i];
        switch (m.type) {
            case ModuleType::kWeather:
                // Drawn even when unavailable: the "Météo indisponible" block
                // is content, and a panel that silently omitted the forecast
                // would look like a panel that was never asked for one.
                break;
            case ModuleType::kMessage: {
                if (m.has_expires && ctx.clock_set &&
                    ctx.now_epoch >= m.expires_epoch) {
                    continue;  // an expired message does not render
                }
                if (ctx.profile->Get(m.text)[0] == '\0') continue;
                break;
            }
            case ModuleType::kConditionalMessage: {
                const char* text = ResolveConditional(m, ctx);
                if (text == nullptr) continue;
                items[count].module = &m;
                items[count].resolved_text = text;
                ++count;
                continue;
            }
            case ModuleType::kList:
                if (m.row_count == 0) continue;
                break;
            case ModuleType::kCountdown:
            case ModuleType::kTimestamp:
                break;
        }
        items[count].module = &m;
        items[count].resolved_text = nullptr;
        ++count;
    }
    return count;
}

/// Draw one item at (x, y) in a box, returning the height used.
int DrawItem(Ctx& ctx, const Item& item, int x, int y, int w, int h, bool hero) {
    const Module& m = *item.module;
    switch (m.type) {
        case ModuleType::kWeather:
            return DrawWeatherBlock(ctx, x, y, w, hero);
        case ModuleType::kCountdown:
            return DrawCountdownBlock(ctx, m, x, y, hero);
        case ModuleType::kMessage:
            return DrawTextBlock(ctx, nullptr, ctx.profile->Get(m.text), x, y, w, h);
        case ModuleType::kConditionalMessage:
            return DrawTextBlock(ctx, nullptr, item.resolved_text, x, y, w, h);
        case ModuleType::kList:
            return DrawListBlock(ctx, m, x, y, w, h);
        case ModuleType::kTimestamp:
            return DrawTimestampBlock(ctx, x, y);
    }
    return 0;
}

// ------------------------------------------------------- the compositions --

/**
 * Editorial: typographic. One hero at the top, the rest stacked beneath it at
 * full width, separated by rules. Filiation with eMini's "Print"; the geometry
 * and the content are ours.
 */
int ComposeEditorial(Ctx& ctx, const Item* items, int count) {
    int y = kBodyTop;
    int drawn = 0;
    for (int i = 0; i < count; ++i) {
        const int remaining = ctx.body_bottom - y;
        if (remaining <= 12) break;
        if (i > 0) {
            ctx.canvas.HLine(kMargin, y, kContentWidth, kBlack);
            y += 6;
        }
        const int used = DrawItem(ctx, items[i], kMargin, y, kContentWidth,
                                  ctx.body_bottom - y, i == 0);
        if (used > 0) ++drawn;
        y += used + 8;
    }
    return drawn;
}

/**
 * Flow: data forward. The twelve-hour curve owns the top of the body, the four
 * hourly markers sit under it, and everything else stacks below. Filiation with
 * eMini's "Rhythm".
 */
int ComposeFlow(Ctx& ctx, const Item* items, int count) {
    int y = kBodyTop;
    int drawn = 0;
    bool has_weather = false;
    for (int i = 0; i < count; ++i) {
        if (items[i].module->type == ModuleType::kWeather) has_weather = true;
    }

    if (has_weather && WeatherDrawable(ctx)) {
        const int curve_h = 62;
        DrawCurve(ctx, kMargin, y, kContentWidth, curve_h);
        y += curve_h + 18;
        y += DrawSlots(ctx, kMargin, y, kContentWidth) + 10;
        ctx.canvas.HLine(kMargin, y, kContentWidth, kBlack);
        y += 8;
        ++drawn;
    }

    for (int i = 0; i < count; ++i) {
        if (items[i].module->type == ModuleType::kWeather) {
            if (has_weather && WeatherDrawable(ctx)) continue;
        }
        const int remaining = ctx.body_bottom - y;
        if (remaining <= 12) break;
        const int used =
            DrawItem(ctx, items[i], kMargin, y, kContentWidth, remaining, false);
        if (used > 0) ++drawn;
        y += used + 8;
    }
    return drawn;
}

/**
 * Focus: scenic. A dithered field behind one dominant item, with anything else
 * reduced to a single line at the foot of the body. Filiation with eMini's
 * "Atlas".
 */
int ComposeFocus(Ctx& ctx, const Item* items, int count) {
    if (count == 0) return 0;

    const int field_top = kBodyTop;
    const int field_h = 120;
    // The field's weight follows the sky, so the panel reads differently on a
    // clear day and an overcast one without drawing an icon for it.
    int level = 3;
    if (WeatherDrawable(ctx)) {
        const weather::Condition cond =
            weather::ConditionForWmoCode(ctx.forecast->wmo[CurrentHourIndex(ctx)]);
        switch (cond) {
            case weather::Condition::kSunny: level = 1; break;
            case weather::Condition::kPartlyCloudy: level = 4; break;
            case weather::Condition::kCloudy: level = 7; break;
            case weather::Condition::kFog: level = 6; break;
            case weather::Condition::kRainy: level = 9; break;
            case weather::Condition::kSnowy: level = 5; break;
            case weather::Condition::kThunder: level = 12; break;
            case weather::Condition::kUnknown: level = 3; break;
        }
    }
    ctx.canvas.Dither(kMargin, field_top, kContentWidth, field_h, level, kBlack,
                      kWhite);
    // The hero sits on a cleared plate so the dither never runs under type.
    const int plate_y = field_top + 24;
    const int plate_h = 62;
    ctx.canvas.Rect(kMargin + 8, plate_y, kContentWidth - 16, plate_h, kWhite);

    int drawn = 0;
    const int used = DrawItem(ctx, items[0], kMargin + 18, plate_y + 8,
                              kContentWidth - 36, plate_h - 16, true);
    if (used > 0) ++drawn;

    int y = field_top + field_h + 10;
    for (int i = 1; i < count; ++i) {
        const int remaining = ctx.body_bottom - y;
        if (remaining <= 12) break;
        const int h = DrawItem(ctx, items[i], kMargin, y, kContentWidth, remaining,
                               false);
        if (h > 0) ++drawn;
        y += h + 6;
    }
    return drawn;
}

}  // namespace

Freshness FreshnessOf(const Profile& profile, const weather::Forecast* forecast,
                      int64_t now_epoch, bool clock_set) {
    if (forecast == nullptr || !forecast->valid()) return Freshness::kNone;
    if (!profile.has_weather) return Freshness::kNone;
    if (!clock_set) {
        // Age cannot be measured without a clock. The cache is still drawn,
        // because a forecast from this morning is the best thing the panel has,
        // but it is labelled stale rather than fresh: claiming a freshness we
        // cannot verify is the one thing this product refuses.
        return Freshness::kStale;
    }
    const int64_t age_min = (now_epoch - forecast->fetched_epoch) / 60;
    // A clock that went backwards — SNTP correcting a drifted RTC — must not
    // read as a forecast from the future.
    if (age_min < 0) return Freshness::kStale;
    if (age_min >= profile.weather.unavailable_after_min) return Freshness::kUnavailable;
    if (age_min >= profile.weather.stale_after_min) return Freshness::kStale;
    return Freshness::kOk;
}

bool Compose(const ComposeInput& in, uint8_t* canvas, uint8_t* frame_out,
             ComposeResult* result) {
    if (in.profile == nullptr || canvas == nullptr || frame_out == nullptr) {
        return false;
    }

    ComposeResult local;
    ComposeResult* res = result != nullptr ? result : &local;
    *res = ComposeResult{};

    Ctx ctx;
    ctx.canvas.px = canvas;
    ctx.profile = in.profile;
    ctx.forecast = in.forecast;
    ctx.now_epoch = in.now_epoch;
    ctx.utc_offset_s = in.utc_offset_s;
    ctx.clock_set = in.clock_set;
    ctx.degraded = in.degraded;
    ctx.freshness = FreshnessOf(*in.profile, in.forecast, in.now_epoch, in.clock_set);
    ctx.now = CivilFromEpoch(in.now_epoch + in.utc_offset_s);
    ctx.body_bottom =
        in.profile->provenance_line ? kBodyBottomWithFooter : kBodyBottomNoFooter;
    res->freshness = ctx.freshness;

    ctx.canvas.Fill(kWhite);
    DrawHeader(ctx);

    Item items[kMaxModules];
    const int count = CollectItems(ctx, items, kMaxModules);

    int drawn = 0;
    if (count == 0) {
        // A blank panel and a broken panel look identical on e-paper, so say so.
        const font::Atlas* body = font::Find(15, false);
        if (body != nullptr) {
            DrawCStr(ctx.canvas, *body, kMargin, kBodyTop, "Rien à afficher", kBlack);
            DrawCStr(ctx.canvas, *body, kMargin, kBodyTop + LineHeight(*body) + 4,
                     "en ce moment", kBlack);
        }
        res->empty = true;
    } else {
        switch (in.profile->composition) {
            case Composition::kEditorial: drawn = ComposeEditorial(ctx, items, count); break;
            case Composition::kFlow: drawn = ComposeFlow(ctx, items, count); break;
            case Composition::kFocus: drawn = ComposeFocus(ctx, items, count); break;
        }
    }
    res->modules_drawn = static_cast<uint8_t>(drawn);

    DrawFooter(ctx);
    PackFrame(canvas, frame_out);
    return true;
}

int32_t UtcOffsetSeconds(int64_t epoch) {
    // No clock, no offset. Returning something plausible here is how a device
    // that has never reached SNTP ends up drawing hour labels it invented.
    if (epoch <= 0) return 0;

    const time_t when = static_cast<time_t>(epoch);
    struct tm local {};
    if (localtime_r(&when, &local) == nullptr) return 0;

    // timegm reads the broken-down time as UTC and applies no daylight rule of
    // its own, so the difference is the offset that was in force at `epoch` —
    // including, on the right two Sundays, the hour that moved. It normalises
    // its argument, which is why it gets a copy.
    struct tm as_utc = local;
    const time_t local_as_utc = timegm(&as_utc);
    if (local_as_utc == static_cast<time_t>(-1)) return 0;

    return static_cast<int32_t>(static_cast<int64_t>(local_as_utc) - epoch);
}

}  // namespace autonomy
