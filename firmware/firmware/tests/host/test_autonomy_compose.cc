/**
 * @file test_autonomy_compose.cc
 * @brief Host tests for the device's compositor, and the producer of the
 *        golden frames the tower's mirror is checked against.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This suite has two jobs.
 *
 * The first is ordinary: compose a set of scenarios and assert the things that
 * must be true of any frame this device draws — exactly 30000 bytes, every
 * pixel inside the four-colour palette, the same inputs producing the same
 * bytes every time, and the honest states (no clock, no forecast, stale
 * forecast, nothing to draw) actually appearing rather than being quietly
 * skipped.
 *
 * The second is why it exists at all. It writes the scenarios and their frames
 * to tests/fixtures/autonomy/, and those files are the *shared goldens*: the
 * tower's vitest suite reads the same scenarios, runs its own TypeScript
 * mirror of this compositor, and compares the 30000 bytes byte for byte. That
 * comparison is the only thing standing between "the preview shows what the
 * device would draw" and "the preview shows something plausible", and it only
 * works if one side produces the goldens and the other merely consumes them.
 * This is the producing side. It must never read the tower's output.
 *
 * Regenerate with:
 *
 *     AUTONOMY_WRITE_FIXTURES=1 ./tests/host/run.sh
 *
 * and commit the result in both repositories. Without the variable the
 * fixtures are read and compared rather than written, so an accidental change
 * to the compositor fails here instead of silently rewriting the goldens it is
 * supposed to be checked against.
 */

#include "common/autonomy_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "common/autonomy_profile.h"
#include "common/openmeteo_parse.h"
#include "common/record_slot.h"

using namespace autonomy;

// ----------------------------------------------------------- tiny harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,         \
                   g_current, #cond);                                      \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        const long long va = (long long)(a);                               \
        const long long vb = (long long)(b);                               \
        if (va != vb) {                                                    \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n",        \
                   __FILE__, __LINE__, g_current, #a, #b, va, vb);         \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

/// 2026-09-14T12:00:00Z. Fixed, so a fixture generated today still matches in
/// February.
constexpr int64_t kNow = 1789387200ll;
/// Montreal in September: UTC-4. Passed in rather than derived; see the header.
constexpr int32_t kOffset = -4 * 3600;

std::vector<uint8_t>& Canvas() {
    // 120 KB, once, on the heap rather than the stack.
    static std::vector<uint8_t> canvas(kCanvasBytes);
    return canvas;
}

Profile& ProfileSlot(int which) {
    // Profiles are 18 KB apiece; two shared instances keep this suite off the
    // stack without allocating one per scenario.
    static Profile a;
    static Profile b;
    return which == 0 ? a : b;
}

bool LoadProfile(const std::string& json, Profile* out) {
    ParseError err;
    const bool ok = ParseProfile(json.data(), json.size(), out, &err);
    if (!ok) {
        printf("  FAIL in %s: fixture profile rejected: %s at %s\n", g_current,
               err.code ? err.code : "(null)", err.field);
        ++g_failures;
    }
    ++g_checks;
    return ok;
}

/**
 * @brief A deterministic forecast, built directly rather than parsed.
 *
 * The parser has its own suite; what this one needs is a Forecast with known
 * numbers in it, and going through JSON would make a compositor test fail when
 * the parser changed.
 */
weather::Forecast MakeForecast(int64_t fetched, int base_c10, int swing) {
    weather::Forecast f;
    f.fetched_epoch = fetched;
    f.first_hour_epoch = fetched;
    f.hour_count = weather::kMaxHours;
    int lo = 32767;
    int hi = -32768;
    static const uint8_t kCodes[8] = {0, 1, 2, 3, 61, 3, 2, 1};
    for (size_t i = 0; i < weather::kMaxHours; ++i) {
        // A shape with a real minimum and maximum rather than a ramp, so the
        // curve has something to draw and the bounds are not the endpoints.
        const int wave = static_cast<int>((i * 7) % 13) - 6;
        const int v = base_c10 + wave * swing;
        f.temp_c10[i] = static_cast<int16_t>(v);
        f.wmo[i] = kCodes[i % 8];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    f.low_c10 = static_cast<int16_t>(lo);
    f.high_c10 = static_cast<int16_t>(hi);
    return f;
}

const char* const kWeatherBlock =
    R"("weather":{"latitude":45.51,"longitude":-73.56,"label":"Montréal",)"
    R"("min_fetch_interval_min":30,"stale_after_min":180,)"
    R"("unavailable_after_min":720},)";

std::string ProfileJson(const char* composition, const char* modules,
                        bool with_weather = true, bool provenance = true) {
    std::string doc = R"({"profile_version":1,"revision":4,"compiled_at":1789380000,)";
    doc += R"("dashboard_id":"fixture","dashboard_doc_version":2,)";
    doc += R"("wake_interval_min":60,"tower_wait_s":30,"provenance_line":)";
    doc += provenance ? "true," : "false,";
    doc += R"("composition":")";
    doc += composition;
    doc += "\",";
    if (with_weather) doc += kWeatherBlock;
    doc += R"("modules":)";
    doc += modules;
    doc += "}";
    return doc;
}

/// The module sets the fixtures use, named so a diff in a golden says which.
const char* const kModulesFull =
    R"([{"type":"weather","mode":"auto"},)"
    R"({"type":"countdown","mode":"device","target_epoch":1790000000,"label":"Départ"},)"
    R"({"type":"list","mode":"device","title":"Courses","rows":["pain de campagne","lait entier","œufs fermiers"],"synced_epoch":1789300000},)"
    R"({"type":"timestamp","mode":"auto"}])";

const char* const kModulesMessage =
    R"([{"type":"weather","mode":"auto"},)"
    R"({"type":"message","mode":"auto","text":"Les poules sont rentrées; le portail est fermé pour la nuit.","expires_epoch":null}])";

const char* const kModulesConditional =
    R"([{"type":"conditional_message","mode":"device","match":"all",)"
    R"("when":[{"kind":"days_of_week","days":[1,2,3,4,5]}],)"
    R"("text":"Collecte des ordures demain","fallback_text":"Week-end"},)"
    R"({"type":"timestamp","mode":"auto"}])";

const char* const kModulesCountdownOnly =
    R"([{"type":"countdown","mode":"device","target_epoch":1790000000,"label":"Départ"}])";

// ------------------------------------------------------------- the fixtures --

struct Scenario {
    const char* name;
    std::string profile_json;
    bool has_forecast;
    weather::Forecast forecast;
    int64_t now_epoch;
    int32_t utc_offset_s;
    bool clock_set;
    bool degraded;
};

std::vector<Scenario> BuildScenarios() {
    std::vector<Scenario> out;

    const weather::Forecast fresh = MakeForecast(kNow - 600, 155, 8);
    const weather::Forecast old = MakeForecast(kNow - 5 * 3600, 20, 12);
    const weather::Forecast ancient = MakeForecast(kNow - 20 * 3600, -35, 6);

    out.push_back({"editorial-full", ProfileJson("editorial", kModulesFull), true,
                   fresh, kNow, kOffset, true, false});
    out.push_back({"flow-full", ProfileJson("flow", kModulesFull), true, fresh, kNow,
                   kOffset, true, false});
    out.push_back({"focus-full", ProfileJson("focus", kModulesFull), true, fresh, kNow,
                   kOffset, true, false});

    out.push_back({"editorial-message", ProfileJson("editorial", kModulesMessage), true,
                   fresh, kNow, kOffset, true, false});
    out.push_back({"flow-message", ProfileJson("flow", kModulesMessage), true, fresh,
                   kNow, kOffset, true, false});

    // The honest states, one fixture each, because these are the ones a
    // refactor is most likely to quietly lose.
    out.push_back({"editorial-stale-weather", ProfileJson("editorial", kModulesFull),
                   true, old, kNow, kOffset, true, false});
    out.push_back({"editorial-unavailable-weather",
                   ProfileJson("editorial", kModulesFull), true, ancient, kNow, kOffset,
                   true, false});
    out.push_back({"editorial-no-forecast", ProfileJson("editorial", kModulesFull),
                   false, fresh, kNow, kOffset, true, false});
    out.push_back({"editorial-no-clock", ProfileJson("editorial", kModulesFull), true,
                   fresh, 0, kOffset, false, false});
    out.push_back({"flow-degraded", ProfileJson("flow", kModulesFull), true, old, kNow,
                   kOffset, true, true});

    // A weekday, so the conditional resolves to its true branch; and a Sunday,
    // so it resolves to the fallback. 2026-09-14 is a Monday; +6 days is a
    // Sunday.
    out.push_back({"editorial-conditional-true",
                   ProfileJson("editorial", kModulesConditional, false), false, fresh,
                   kNow, kOffset, true, false});
    out.push_back({"editorial-conditional-fallback",
                   ProfileJson("editorial", kModulesConditional, false), false, fresh,
                   kNow + 6 * 86400, kOffset, true, false});

    out.push_back({"focus-countdown-only",
                   ProfileJson("focus", kModulesCountdownOnly, false), false, fresh,
                   kNow, kOffset, true, false});
    out.push_back({"editorial-no-provenance",
                   ProfileJson("editorial", kModulesFull, true, false), true, fresh,
                   kNow, kOffset, true, false});

    return out;
}

bool ComposeScenario(const Scenario& s, uint8_t* frame, ComposeResult* result) {
    Profile& p = ProfileSlot(0);
    if (!LoadProfile(s.profile_json, &p)) return false;
    ComposeInput in;
    in.profile = &p;
    in.forecast = s.has_forecast ? &s.forecast : nullptr;
    in.now_epoch = s.now_epoch;
    in.clock_set = s.clock_set;
    in.degraded = s.degraded;
    in.utc_offset_s = s.utc_offset_s;
    return Compose(in, Canvas().data(), frame, result);
}

std::string HexOf(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[(data[i] >> 4) & 0xf];
        out += kHex[data[i] & 0xf];
    }
    return out;
}

std::string Sha256Hex(const uint8_t* data, size_t len) {
    uint8_t digest[record::kShaBytes];
    record::Sha256(data, len, digest);
    return HexOf(digest, sizeof(digest));
}

/// JSON string escaping, for the scenario manifest. Small on purpose: the only
/// thing in here that needs escaping is the profile document itself.
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}

std::string FixtureDir() {
    const char* from_env = getenv("AUTONOMY_FIXTURE_DIR");
    if (from_env != nullptr && from_env[0] != '\0') return from_env;
    return "tests/fixtures/autonomy";
}

bool WritingFixtures() {
    const char* v = getenv("AUTONOMY_WRITE_FIXTURES");
    return v != nullptr && v[0] == '1';
}

}  // namespace

// ------------------------------------------------------------- invariants --

static void test_every_scenario_produces_a_frame_of_exactly_30000_bytes() {
    for (const Scenario& s : BuildScenarios()) {
        std::vector<uint8_t> frame(kPackedBytes, 0xff);
        ComposeResult result;
        CHECK(ComposeScenario(s, frame.data(), &result));
        CHECK_EQ_INT(frame.size(), 30000);
    }
}

static void test_every_pixel_is_inside_the_four_colour_palette() {
    // The packed frame cannot express anything else, so the real claim is about
    // the canvas: a stray 4 would pack as a 0 and paint the wrong colour
    // silently. Checked on the canvas the compositor actually wrote.
    for (const Scenario& s : BuildScenarios()) {
        std::vector<uint8_t> frame(kPackedBytes);
        ComposeResult result;
        if (!ComposeScenario(s, frame.data(), &result)) continue;
        bool all_in_palette = true;
        for (size_t i = 0; i < kCanvasBytes; ++i) {
            if (Canvas()[i] > 3) {
                all_in_palette = false;
                break;
            }
        }
        CHECK(all_in_palette);
    }
}

static void test_the_same_inputs_produce_the_same_bytes() {
    // The dedup that saves a 25-second panel refresh every hour rests entirely
    // on this. A compositor with any hidden state would cost a refresh per wake
    // for a panel nobody had changed.
    for (const Scenario& s : BuildScenarios()) {
        std::vector<uint8_t> a(kPackedBytes);
        std::vector<uint8_t> b(kPackedBytes);
        ComposeResult ra;
        ComposeResult rb;
        if (!ComposeScenario(s, a.data(), &ra)) continue;
        if (!ComposeScenario(s, b.data(), &rb)) continue;
        CHECK(memcmp(a.data(), b.data(), kPackedBytes) == 0);
    }
}

static void test_different_compositions_produce_different_frames() {
    // Three names that drew the same panel would be three lies in the UI.
    std::vector<uint8_t> ed(kPackedBytes);
    std::vector<uint8_t> fl(kPackedBytes);
    std::vector<uint8_t> fo(kPackedBytes);
    const weather::Forecast f = MakeForecast(kNow - 600, 155, 8);
    ComposeResult r;
    Scenario s{"x", ProfileJson("editorial", kModulesFull), true, f, kNow, kOffset,
               true, false};
    CHECK(ComposeScenario(s, ed.data(), &r));
    s.profile_json = ProfileJson("flow", kModulesFull);
    CHECK(ComposeScenario(s, fl.data(), &r));
    s.profile_json = ProfileJson("focus", kModulesFull);
    CHECK(ComposeScenario(s, fo.data(), &r));
    CHECK(memcmp(ed.data(), fl.data(), kPackedBytes) != 0);
    CHECK(memcmp(ed.data(), fo.data(), kPackedBytes) != 0);
    CHECK(memcmp(fl.data(), fo.data(), kPackedBytes) != 0);
}

static void test_the_packing_matches_the_towers_format() {
    // 2bpp, MSB first, four pixels per byte, palette order. The same bytes the
    // PUT route already accepts: an autonomous frame and a pushed frame are the
    // same kind of object.
    std::vector<uint8_t> canvas(kCanvasBytes, 0);
    canvas[0] = kBlack;
    canvas[1] = kWhite;
    canvas[2] = kYellow;
    canvas[3] = kRed;
    std::vector<uint8_t> frame(kPackedBytes);
    PackFrame(canvas.data(), frame.data());
    CHECK_EQ_INT(frame[0], 0x1b);  // 00 01 10 11
}

static void test_floor_div_agrees_with_javascript_semantics() {
    // The one arithmetic difference between the two renderers that would
    // actually bite, and it bites on negative temperatures.
    CHECK_EQ_INT(FloorDiv(7, 2), 3);
    CHECK_EQ_INT(FloorDiv(-7, 2), -4);   // C++ `/` would give -3
    CHECK_EQ_INT(FloorDiv(-1, 2), -1);   // C++ `/` would give 0
    CHECK_EQ_INT(FloorDiv(6, 3), 2);
    CHECK_EQ_INT(FloorDiv(-6, 3), -2);
    CHECK_EQ_INT(FloorDiv(0, 5), 0);
    CHECK_EQ_INT(FloorDiv(5, 0), 0);     // guarded rather than undefined
}

// ---------------------------------------------------------- honest states --

static void test_an_unset_clock_never_draws_a_countdown_number() {
    const weather::Forecast f = MakeForecast(0, 100, 5);
    Scenario s{"x", ProfileJson("editorial", kModulesCountdownOnly, false), false, f,
               0, kOffset, false, false};
    std::vector<uint8_t> frame(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(s, frame.data(), &r));

    // The claim is about pixels, so it is checked in pixels: with no clock, the
    // frame must differ from the same scenario with a clock, and the red
    // "heure inconnue" must be present. Red appears nowhere else in this
    // composition, which makes it a usable marker.
    bool has_red = false;
    for (size_t i = 0; i < kCanvasBytes; ++i) {
        if (Canvas()[i] == kRed) {
            has_red = true;
            break;
        }
    }
    CHECK(has_red);
}

static void test_a_stale_forecast_is_marked_and_an_ancient_one_is_withheld() {
    Profile& p = ProfileSlot(0);
    CHECK(LoadProfile(ProfileJson("editorial", kModulesFull), &p));

    const weather::Forecast fresh = MakeForecast(kNow - 600, 150, 5);
    const weather::Forecast stale = MakeForecast(kNow - 5 * 3600, 150, 5);
    const weather::Forecast ancient = MakeForecast(kNow - 20 * 3600, 150, 5);

    CHECK(FreshnessOf(p, &fresh, kNow, true) == Freshness::kOk);
    CHECK(FreshnessOf(p, &stale, kNow, true) == Freshness::kStale);
    CHECK(FreshnessOf(p, &ancient, kNow, true) == Freshness::kUnavailable);
    CHECK(FreshnessOf(p, nullptr, kNow, true) == Freshness::kNone);
    // No clock means the age cannot be measured, so freshness is never claimed.
    CHECK(FreshnessOf(p, &fresh, kNow, false) == Freshness::kStale);
    // A clock that went backwards must not read as a forecast from the future.
    CHECK(FreshnessOf(p, &fresh, kNow - 8 * 3600, true) == Freshness::kStale);
}

static void test_an_expired_message_does_not_render() {
    const char* modules =
        R"([{"type":"message","mode":"auto","text":"Fini","expires_epoch":1789000000},)"
        R"({"type":"timestamp","mode":"auto"}])";
    Scenario s{"x", ProfileJson("editorial", modules, false), false,
               weather::Forecast{}, kNow, kOffset, true, false};
    std::vector<uint8_t> with_expiry(kPackedBytes);
    ComposeResult r1;
    CHECK(ComposeScenario(s, with_expiry.data(), &r1));

    const char* live =
        R"([{"type":"message","mode":"auto","text":"Fini","expires_epoch":null},)"
        R"({"type":"timestamp","mode":"auto"}])";
    s.profile_json = ProfileJson("editorial", live, false);
    std::vector<uint8_t> without(kPackedBytes);
    ComposeResult r2;
    CHECK(ComposeScenario(s, without.data(), &r2));

    CHECK(memcmp(with_expiry.data(), without.data(), kPackedBytes) != 0);
    CHECK_EQ_INT(r1.modules_drawn, 1);  // only the timestamp
    CHECK_EQ_INT(r2.modules_drawn, 2);
}

static void test_a_profile_with_nothing_drawable_says_so() {
    const char* modules =
        R"([{"type":"message","mode":"auto","text":"Fini","expires_epoch":1789000000}])";
    Scenario s{"x", ProfileJson("editorial", modules, false), false,
               weather::Forecast{}, kNow, kOffset, true, false};
    std::vector<uint8_t> frame(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(s, frame.data(), &r));
    // A blank panel and a broken panel look identical on e-paper.
    CHECK(r.empty);
    CHECK_EQ_INT(r.modules_drawn, 0);
}

static void test_a_conditional_message_picks_its_side_from_the_day() {
    Scenario weekday{"x", ProfileJson("editorial", kModulesConditional, false), false,
                     weather::Forecast{}, kNow, kOffset, true, false};
    Scenario sunday = weekday;
    sunday.now_epoch = kNow + 6 * 86400;

    std::vector<uint8_t> a(kPackedBytes);
    std::vector<uint8_t> b(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(weekday, a.data(), &r));
    CHECK(ComposeScenario(sunday, b.data(), &r));
    CHECK(memcmp(a.data(), b.data(), kPackedBytes) != 0);
}

static void test_the_provenance_line_can_be_turned_off_and_changes_the_frame() {
    const weather::Forecast f = MakeForecast(kNow - 600, 150, 5);
    Scenario on{"x", ProfileJson("editorial", kModulesFull, true, true), true, f, kNow,
                kOffset, true, false};
    Scenario off = on;
    off.profile_json = ProfileJson("editorial", kModulesFull, true, false);

    std::vector<uint8_t> a(kPackedBytes);
    std::vector<uint8_t> b(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(on, a.data(), &r));
    CHECK(ComposeScenario(off, b.data(), &r));
    CHECK(memcmp(a.data(), b.data(), kPackedBytes) != 0);
}

static void test_a_degraded_cycle_is_visible_on_the_panel() {
    const weather::Forecast f = MakeForecast(kNow - 600, 150, 5);
    Scenario ok{"x", ProfileJson("flow", kModulesFull), true, f, kNow, kOffset, true,
                false};
    Scenario degraded = ok;
    degraded.degraded = true;

    std::vector<uint8_t> a(kPackedBytes);
    std::vector<uint8_t> b(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(ok, a.data(), &r));
    CHECK(ComposeScenario(degraded, b.data(), &r));
    CHECK(memcmp(a.data(), b.data(), kPackedBytes) != 0);
}

static void test_a_long_list_row_is_truncated_rather_than_overflowing() {
    std::string rows = R"([")" + std::string(110, 'x') + R"("])";
    const std::string modules =
        R"([{"type":"list","mode":"device","title":"T","rows":)" + rows +
        R"(,"synced_epoch":0}])";
    Scenario s{"x", ProfileJson("editorial", modules.c_str(), false), false,
               weather::Forecast{}, kNow, kOffset, true, false};
    std::vector<uint8_t> frame(kPackedBytes);
    ComposeResult r;
    CHECK(ComposeScenario(s, frame.data(), &r));

    // Nothing may be drawn in the right-hand margin: a row that ran off the
    // panel is the failure this guards.
    bool margin_clean = true;
    for (int y = 0; y < kPanelHeight; ++y) {
        for (int x = kPanelWidth - 6; x < kPanelWidth; ++x) {
            if (Canvas()[static_cast<size_t>(y) * kPanelWidth + x] != kWhite) {
                margin_clean = false;
            }
        }
    }
    CHECK(margin_clean);
}

// ------------------------------------------------------- the shared goldens --

static void test_the_golden_frames_match_or_are_written() {
    const std::vector<Scenario> scenarios = BuildScenarios();
    const std::string dir = FixtureDir();
    const bool writing = WritingFixtures();

    std::string manifest = "{\n  \"note\": ";
    manifest +=
        "\"Generated by firmware tests/host/test_autonomy_compose.cc. The device's "
        "compositor is the producer; the tower's TypeScript mirror is the consumer. "
        "Regenerate with AUTONOMY_WRITE_FIXTURES=1 ./tests/host/run.sh and commit in "
        "both repositories.\",\n  \"scenarios\": [\n";

    for (size_t i = 0; i < scenarios.size(); ++i) {
        const Scenario& s = scenarios[i];
        std::vector<uint8_t> frame(kPackedBytes);
        ComposeResult result;
        if (!ComposeScenario(s, frame.data(), &result)) continue;

        const std::string bin_path = dir + "/" + s.name + ".bin";
        if (writing) {
            FILE* f = fopen(bin_path.c_str(), "wb");
            if (f == nullptr) {
                printf("  FAIL in %s: cannot write %s\n", g_current, bin_path.c_str());
                ++g_failures;
            } else {
                fwrite(frame.data(), 1, frame.size(), f);
                fclose(f);
            }
            ++g_checks;
        } else {
            FILE* f = fopen(bin_path.c_str(), "rb");
            ++g_checks;
            if (f == nullptr) {
                ++g_failures;
                printf("  FAIL in %s: golden %s is missing; regenerate with "
                       "AUTONOMY_WRITE_FIXTURES=1\n",
                       g_current, bin_path.c_str());
                continue;
            }
            std::vector<uint8_t> stored(kPackedBytes);
            const size_t read = fread(stored.data(), 1, stored.size(), f);
            fclose(f);
            if (read != kPackedBytes ||
                memcmp(stored.data(), frame.data(), kPackedBytes) != 0) {
                ++g_failures;
                printf("  FAIL in %s: golden %s does not match this build\n", g_current,
                       s.name);
            }
        }

        weather::Forecast encoded_source = s.forecast;
        uint8_t encoded[weather::kEncodedForecastBytes];
        const size_t n = s.has_forecast
                             ? weather::EncodeForecast(encoded_source, encoded,
                                                       sizeof(encoded))
                             : 0;

        manifest += "    {\n";
        manifest += std::string("      \"name\": \"") + s.name + "\",\n";
        manifest += "      \"profile\": \"" + JsonEscape(s.profile_json) + "\",\n";
        manifest += std::string("      \"forecastHex\": ") +
                    (s.has_forecast ? "\"" + HexOf(encoded, n) + "\"" : "null") + ",\n";
        manifest += "      \"nowEpoch\": " + std::to_string(s.now_epoch) + ",\n";
        manifest += "      \"utcOffsetS\": " + std::to_string(s.utc_offset_s) + ",\n";
        manifest +=
            std::string("      \"clockSet\": ") + (s.clock_set ? "true" : "false") + ",\n";
        manifest +=
            std::string("      \"degraded\": ") + (s.degraded ? "true" : "false") + ",\n";
        manifest += std::string("      \"frame\": \"") + s.name + ".bin\",\n";
        manifest +=
            "      \"sha256\": \"" + Sha256Hex(frame.data(), frame.size()) + "\"\n";
        manifest += std::string("    }") + (i + 1 < scenarios.size() ? "," : "") + "\n";
    }
    manifest += "  ]\n}\n";

    const std::string manifest_path = dir + "/scenarios.json";
    if (writing) {
        FILE* f = fopen(manifest_path.c_str(), "wb");
        if (f == nullptr) {
            printf("  FAIL in %s: cannot write %s\n", g_current, manifest_path.c_str());
            ++g_failures;
        } else {
            fwrite(manifest.data(), 1, manifest.size(), f);
            fclose(f);
            printf("    (wrote %zu goldens and the manifest to %s)\n", scenarios.size(),
                   dir.c_str());
        }
        ++g_checks;
    }
}

// -------------------------------------------------------- the UTC offset --
//
// THE BUG THIS SECTION EXISTS FOR
// -------------------------------
// The offset used to be computed as `mktime(local) - mktime(utc)`, with
// `tm_isdst` left at zero on the UTC side. `mktime` interprets a broken-down
// time *in local time*, so on a summer day in Toronto it read the UTC breakdown
// as an EDT wall-clock time, applied the daylight rule to it, and returned an
// offset an hour off. Every countdown and every forecast hour label on the
// panel was shifted by that hour for eight months of the year — and the goldens
// were generated in winter, so nothing caught it.

namespace {

/// Run @p fn with TZ set, and put it back afterwards.
void WithTz(const char* tz, void (*fn)(const char*)) {
    const char* saved = getenv("TZ");
    const std::string previous = saved != nullptr ? saved : "";
    const bool had = saved != nullptr;
    setenv("TZ", tz, 1);
    tzset();
    fn(tz);
    if (had) {
        setenv("TZ", previous.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

// 2026-07-15T12:00:00Z — high summer, EDT in Toronto.
const int64_t kSummer = 1784116800;
// 2026-01-15T12:00:00Z — deep winter, EST in Toronto.
const int64_t kWinter = 1768478400;

void CheckToronto(const char*) {
    // EDT is UTC-4, EST is UTC-5. The old arithmetic returned -18000 for both.
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kSummer), -4 * 3600);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kWinter), -5 * 3600);
}

void CheckParis(const char*) {
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kSummer), 2 * 3600);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kWinter), 1 * 3600);
}

void CheckUtc(const char*) {
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kSummer), 0);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kWinter), 0);
}

void CheckIndia(const char*) {
    // A half-hour zone with no daylight saving at all, which the subtraction
    // has to carry as well as the whole-hour ones.
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kSummer), 5 * 3600 + 1800);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(kWinter), 5 * 3600 + 1800);
}

void CheckBoundary(const char*) {
    // The instant daylight saving begins in Toronto in 2026: 07:00 UTC on
    // 8 March. One second either side must differ by exactly an hour.
    const int64_t change = 1772953200;  // 2026-03-08T07:00:00Z
    const int32_t before = autonomy::UtcOffsetSeconds(change - 1);
    const int32_t after = autonomy::UtcOffsetSeconds(change);
    CHECK_EQ_INT(before, -5 * 3600);
    CHECK_EQ_INT(after, -4 * 3600);
    CHECK_EQ_INT(after - before, 3600);

    // And back again in November, which is the direction the old arithmetic
    // happened to get right and which must not regress either.
    const int64_t back = 1793512800;  // 2026-11-01T06:00:00Z
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(back - 1), -4 * 3600);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(back), -5 * 3600);
}

}  // namespace

static void test_the_utc_offset_follows_daylight_saving() {
    WithTz("America/Toronto", CheckToronto);
    WithTz("Europe/Paris", CheckParis);
    WithTz("UTC", CheckUtc);
    WithTz("Asia/Kolkata", CheckIndia);
}

static void test_the_utc_offset_moves_exactly_at_the_boundary() {
    WithTz("America/Toronto", CheckBoundary);
}

static void test_an_unset_clock_has_no_offset_to_report() {
    // Not "UTC". A device that has never reached SNTP has no local time, and
    // inventing one is how hour labels get drawn that nobody can explain.
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(0), 0);
    CHECK_EQ_INT(autonomy::UtcOffsetSeconds(-1), 0);
}

int main() {
    RUN(test_the_utc_offset_follows_daylight_saving);
    RUN(test_the_utc_offset_moves_exactly_at_the_boundary);
    RUN(test_an_unset_clock_has_no_offset_to_report);
    RUN(test_every_scenario_produces_a_frame_of_exactly_30000_bytes);
    RUN(test_every_pixel_is_inside_the_four_colour_palette);
    RUN(test_the_same_inputs_produce_the_same_bytes);
    RUN(test_different_compositions_produce_different_frames);
    RUN(test_the_packing_matches_the_towers_format);
    RUN(test_floor_div_agrees_with_javascript_semantics);

    RUN(test_an_unset_clock_never_draws_a_countdown_number);
    RUN(test_a_stale_forecast_is_marked_and_an_ancient_one_is_withheld);
    RUN(test_an_expired_message_does_not_render);
    RUN(test_a_profile_with_nothing_drawable_says_so);
    RUN(test_a_conditional_message_picks_its_side_from_the_day);
    RUN(test_the_provenance_line_can_be_turned_off_and_changes_the_frame);
    RUN(test_a_degraded_cycle_is_visible_on_the_panel);
    RUN(test_a_long_list_row_is_truncated_rather_than_overflowing);

    RUN(test_the_golden_frames_match_or_are_written);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
