/**
 * @file test_autonomy_profile.cc
 * @brief Host tests for the autonomy profile's closed validation table and for
 *        the A/B profile slot.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These compile main/common/autonomy_profile.cc and record_slot.cc directly, so
 * what is tested is the translation unit the firmware links.
 *
 * Two claims are worth testing hardest here, because both are the kind a
 * reviewer can otherwise only take on trust:
 *
 *  1. **Nothing is stored that cannot be drawn.** Every malformed document
 *     below is refused *by name*, before a byte reaches the slot. The field
 *     path in each expectation is the one the tower shows the owner, so a
 *     refactor that made the message vaguer fails here.
 *
 *  2. **A power cut cannot tear a profile.** The write-cut walk at the bottom
 *     drives a failure at every byte offset of a profile write and asserts that
 *     what loads afterwards is either the old complete profile or the new one,
 *     never a mixture. It is the same discipline test_dashboard_slot.cc applies
 *     to frames, now applied to the record that decides what the panel draws
 *     when the tower is gone.
 */

#include "common/autonomy_profile.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        ++g_checks;                                                        \
        if (strcmp((a), (b)) != 0) {                                       \
            ++g_failures;                                                  \
            printf("  FAIL %s:%d in %s: \"%s\" == \"%s\"\n", __FILE__,     \
                   __LINE__, g_current, (a), (b));                         \
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

/// A minimal but complete profile, as the tower's canonical serialiser emits
/// it. Tests mutate copies of this rather than each spelling out sixteen
/// fields, so what each one is actually about stays visible.
const char* const kMinimal =
    R"({"profile_version":1,"revision":12,"compiled_at":1789500000,)"
    R"("dashboard_id":"dash-1","dashboard_doc_version":7,)"
    R"("wake_interval_min":60,"tower_wait_s":30,"provenance_line":true,)"
    R"("composition":"flow","modules":[{"type":"timestamp","mode":"auto"}]})";

/// The same document with a weather module and the block it requires.
const char* const kWithWeather =
    R"({"profile_version":1,"revision":12,"compiled_at":1789500000,)"
    R"("dashboard_id":"dash-1","dashboard_doc_version":7,)"
    R"("wake_interval_min":60,"tower_wait_s":30,"provenance_line":true,)"
    R"("composition":"flow",)"
    R"("weather":{"latitude":45.51,"longitude":-73.56,"label":"Montreal",)"
    R"("min_fetch_interval_min":30,"stale_after_min":180,)"
    R"("unavailable_after_min":720},)"
    R"("modules":[{"type":"weather","mode":"auto"}]})";

/// Profiles are 18 KB; one shared instance keeps this suite off the stack.
Profile& Scratch() {
    static Profile p;
    return p;
}

bool Accepts(const std::string& doc, ParseError* err = nullptr) {
    ParseError local;
    ParseError* e = err != nullptr ? err : &local;
    return ParseProfile(doc.data(), doc.size(), &Scratch(), e);
}

/// Assert a document is refused, with the exact code and field path the tower
/// will show. Both are part of the contract, so both are asserted.
void ExpectRefusal(const std::string& doc, const char* code, const char* field) {
    ParseError err;
    const bool ok = ParseProfile(doc.data(), doc.size(), &Scratch(), &err);
    ++g_checks;
    if (ok) {
        ++g_failures;
        printf("  FAIL in %s: expected refusal %s at \"%s\", but it was accepted\n",
               g_current, code, field);
        return;
    }
    // By value, not by pointer: most call sites pass one of the exported token
    // constants, but the JSON reader's tokens are private to
    // autonomy_profile.cc and are written out as literals here. The spelling is
    // the contract either way.
    const bool same_code =
        err.code != nullptr && (err.code == code || strcmp(err.code, code) == 0);
    if (!same_code || strcmp(err.field, field) != 0) {
        ++g_failures;
        printf("  FAIL in %s: expected %s at \"%s\", got %s at \"%s\"\n", g_current,
               code, field, err.code ? err.code : "(null)", err.field);
    }
}

/// Replace the first occurrence of @p from with @p to, so a test reads as the
/// one edit it is making rather than as a second copy of the document.
std::string With(const char* base, const std::string& from, const std::string& to) {
    std::string doc(base);
    const size_t at = doc.find(from);
    if (at == std::string::npos) {
        printf("  FAIL in %s: fixture does not contain \"%s\"\n", g_current,
               from.c_str());
        ++g_failures;
        return doc;
    }
    return doc.replace(at, from.size(), to);
}

}  // namespace

// --------------------------------------------------------- the happy paths --

static void test_the_minimal_profile_is_accepted_and_read_back() {
    Profile& p = Scratch();
    CHECK(Accepts(kMinimal));
    CHECK_EQ_INT(p.profile_version, 1);
    CHECK_EQ_INT(p.revision, 12);
    CHECK_EQ_INT(p.compiled_at, 1789500000);
    CHECK_STR(p.Get(p.dashboard_id), "dash-1");
    CHECK_EQ_INT(p.dashboard_doc_version, 7);
    CHECK_EQ_INT(p.wake_interval_min, 60);
    CHECK_EQ_INT(p.tower_wait_s, 30);
    CHECK(p.provenance_line);
    CHECK(p.composition == Composition::kFlow);
    CHECK(!p.has_weather);
    CHECK_EQ_INT(p.module_count, 1);
    CHECK(p.modules[0].type == ModuleType::kTimestamp);
    CHECK(p.modules[0].mode == Mode::kAuto);
}

static void test_a_weather_profile_carries_its_thresholds() {
    Profile& p = Scratch();
    CHECK(Accepts(kWithWeather));
    CHECK(p.has_weather);
    CHECK(p.weather.latitude > 45.50 && p.weather.latitude < 45.52);
    CHECK(p.weather.longitude < -73.55 && p.weather.longitude > -73.57);
    CHECK_STR(p.Get(p.weather.label), "Montreal");
    CHECK_EQ_INT(p.weather.min_fetch_interval_min, 30);
    CHECK_EQ_INT(p.weather.stale_after_min, 180);
    CHECK_EQ_INT(p.weather.unavailable_after_min, 720);
    CHECK(p.WantsWeather());
    CHECK(!p.IsFullyOffline());
}

static void test_a_profile_with_no_weather_is_fully_offline() {
    Profile& p = Scratch();
    CHECK(Accepts(kMinimal));
    CHECK(!p.WantsWeather());
    CHECK(p.IsFullyOffline());
}

static void test_every_module_type_round_trips() {
    const std::string modules =
        R"([{"type":"weather","mode":"auto"},)"
        R"({"type":"countdown","mode":"device","target_epoch":1791000000,"label":"Depart"},)"
        R"({"type":"message","mode":"auto","text":"Bonjour","expires_epoch":null},)"
        R"({"type":"conditional_message","mode":"device","match":"all",)"
        R"("when":[{"kind":"days_of_week","days":[1,2,3]}],)"
        R"("text":"Semaine","fallback_text":null},)"
        R"({"type":"list","mode":"device","title":"Courses","rows":["pain","lait"],)"
        R"("synced_epoch":1789499000},)"
        R"({"type":"timestamp","mode":"auto"}])";
    const std::string doc =
        With(kWithWeather, R"([{"type":"weather","mode":"auto"}])", modules);

    Profile& p = Scratch();
    CHECK(Accepts(doc));
    CHECK_EQ_INT(p.module_count, 6);

    CHECK(p.modules[1].type == ModuleType::kCountdown);
    CHECK(p.modules[1].mode == Mode::kDevice);
    CHECK_EQ_INT(p.modules[1].target_epoch, 1791000000);
    CHECK_STR(p.Get(p.modules[1].label), "Depart");

    CHECK(p.modules[2].type == ModuleType::kMessage);
    CHECK_STR(p.Get(p.modules[2].text), "Bonjour");
    CHECK(!p.modules[2].has_expires);

    CHECK(p.modules[3].type == ModuleType::kConditionalMessage);
    CHECK(p.modules[3].match == Match::kAll);
    CHECK_EQ_INT(p.modules[3].condition_count, 1);
    CHECK(p.modules[3].conditions[0].kind == ConditionKind::kDaysOfWeek);
    CHECK_EQ_INT(p.modules[3].conditions[0].days_mask, 0x0e);  // 1|2|3
    CHECK(!p.modules[3].has_fallback);

    CHECK(p.modules[4].type == ModuleType::kList);
    CHECK_STR(p.Get(p.modules[4].title), "Courses");
    CHECK_EQ_INT(p.modules[4].row_count, 2);
    CHECK_STR(p.Get(p.modules[4].rows[0]), "pain");
    CHECK_STR(p.Get(p.modules[4].rows[1]), "lait");
    CHECK_EQ_INT(p.modules[4].synced_epoch, 1789499000);
}

static void test_all_three_compositions_are_known() {
    for (const char* name : {"editorial", "flow", "focus"}) {
        const std::string doc =
            With(kMinimal, R"("composition":"flow")",
                 std::string(R"("composition":")") + name + "\"");
        CHECK(Accepts(doc));
        CHECK_STR(CompositionName(Scratch().composition), name);
    }
}

static void test_key_order_does_not_matter() {
    // `type` last in the module, and the root fields shuffled. The device must
    // accept a document that says the right thing in a different order: this is
    // why the module parse locates its discriminant rather than assuming it
    // comes first.
    const std::string doc =
        R"({"modules":[{"mode":"device","label":"X","target_epoch":100,"type":"countdown"}],)"
        R"("composition":"focus","provenance_line":false,"tower_wait_s":0,)"
        R"("wake_interval_min":15,"dashboard_doc_version":0,"dashboard_id":"d",)"
        R"("compiled_at":0,"revision":1,"profile_version":1})";
    Profile& p = Scratch();
    CHECK(Accepts(doc));
    CHECK(p.modules[0].type == ModuleType::kCountdown);
    CHECK_STR(p.Get(p.modules[0].label), "X");
    CHECK(!p.provenance_line);
    CHECK_EQ_INT(p.tower_wait_s, 0);
}

static void test_an_expiring_message_and_a_fallback_are_carried() {
    const std::string modules =
        R"([{"type":"message","mode":"auto","text":"Ce soir","expires_epoch":1791000000},)"
        R"({"type":"conditional_message","mode":"device","match":"any",)"
        R"("when":[{"kind":"date_range","from":"2026-12-01","to":null}],)"
        R"("text":"Decembre","fallback_text":"Pas encore"}])";
    const std::string doc =
        With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules);
    Profile& p = Scratch();
    CHECK(Accepts(doc));
    CHECK(p.modules[0].has_expires);
    CHECK_EQ_INT(p.modules[0].expires_epoch, 1791000000);
    CHECK(p.modules[1].match == Match::kAny);
    CHECK(p.modules[1].conditions[0].kind == ConditionKind::kDateRange);
    CHECK(p.modules[1].conditions[0].has_from);
    CHECK(!p.modules[1].conditions[0].has_to);
    CHECK_STR(p.Get(p.modules[1].conditions[0].from), "2026-12-01");
    CHECK(p.modules[1].has_fallback);
    CHECK_STR(p.Get(p.modules[1].fallback_text), "Pas encore");
}

static void test_a_weather_state_condition_needs_the_forecast() {
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all",)"
        R"("when":[{"kind":"weather_state","state":"stale"}],)"
        R"("text":"Meteo ancienne","fallback_text":null}])";
    const std::string doc =
        With(kWithWeather, R"([{"type":"weather","mode":"auto"}])", modules);
    Profile& p = Scratch();
    CHECK(Accepts(doc));
    // No weather *module*, but a condition that asks about the forecast's age.
    // If this reported "fully offline" the wake cycle would skip the fetch and
    // the message would say "stale" on a device that had simply never looked.
    CHECK(p.WantsWeather());
    CHECK(!p.IsFullyOffline());
}

// ------------------------------------------------------------- the refusals --

static void test_an_unknown_root_field_is_refused_by_name() {
    ExpectRefusal(With(kMinimal, R"("composition":"flow")",
                       R"("composition":"flow","wether":1)"),
                  kErrUnknownField, "wether");
}

static void test_an_unknown_weather_field_is_refused_by_name() {
    ExpectRefusal(With(kWithWeather, R"("label":"Montreal")",
                       R"("label":"Montreal","api_key":"secret")"),
                  kErrUnknownField, "weather.api_key");
}

static void test_an_unknown_module_field_is_refused_by_name() {
    ExpectRefusal(With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
                       R"({"type":"timestamp","mode":"auto","url":"http://x"})"),
                  kErrUnknownField, "modules[0].url");
}

static void test_a_field_belonging_to_another_module_type_is_unknown_here() {
    // `rows` is a real field — on a list. On a timestamp it is not a typo the
    // device should tolerate; it means the sender built the wrong module.
    ExpectRefusal(With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
                       R"({"type":"timestamp","mode":"auto","rows":["a"]})"),
                  kErrUnknownField, "modules[0].rows");
}

static void test_a_missing_root_field_is_named() {
    ExpectRefusal(With(kMinimal, R"("tower_wait_s":30,)", ""), kErrMissingField,
                  "tower_wait_s");
}

static void test_a_missing_module_field_is_named() {
    ExpectRefusal(With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
                       R"({"type":"countdown","mode":"auto","label":"X"})"),
                  kErrMissingField, "modules[0]");
}

static void test_a_duplicate_root_field_is_refused() {
    ExpectRefusal(With(kMinimal, R"("revision":12,)", R"("revision":12,"revision":13,)"),
                  kErrDuplicateField, "revision");
}

static void test_a_duplicate_module_field_is_refused() {
    ExpectRefusal(With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
                       R"({"type":"timestamp","mode":"auto","mode":"device"})"),
                  kErrDuplicateField, "modules[0].mode");
}

static void test_a_wake_interval_below_the_floor_is_refused() {
    // 14 minutes is not a preference the device can hold: power_policy's floor
    // is where the saver stops saving anything.
    ExpectRefusal(With(kMinimal, R"("wake_interval_min":60)",
                       R"("wake_interval_min":14)"),
                  kErrOutOfRange, "wake_interval_min");
}

static void test_a_wake_interval_above_the_ceiling_is_refused() {
    ExpectRefusal(With(kMinimal, R"("wake_interval_min":60)",
                       R"("wake_interval_min":1441)"),
                  kErrOutOfRange, "wake_interval_min");
}

static void test_the_wake_bounds_match_the_power_contract() {
    // Stated in two headers; asserted equal here so they cannot drift apart
    // without a test failing.
    CHECK_EQ_INT(kMinWakeIntervalMin, 15);
    CHECK_EQ_INT(kMaxWakeIntervalMin, 1440);
}

static void test_a_tower_wait_past_the_fetch_cap_is_refused() {
    // The ceiling is the fetch phase cap, now 90 s: 90 is accepted, 91 refused.
    CHECK(Accepts(With(kMinimal, R"("tower_wait_s":30)", R"("tower_wait_s":90)")));
    ExpectRefusal(With(kMinimal, R"("tower_wait_s":30)", R"("tower_wait_s":91)"),
                  kErrOutOfRange, "tower_wait_s");
}

static void test_a_future_profile_version_says_so() {
    ExpectRefusal(With(kMinimal, R"("profile_version":1)", R"("profile_version":2)"),
                  kErrUnsupportedVersion, "profile_version");
}

static void test_an_unknown_composition_is_refused() {
    ExpectRefusal(With(kMinimal, R"("composition":"flow")",
                       R"("composition":"atlas")"),
                  kErrBadValue, "composition");
}

static void test_tower_is_not_a_mode_the_device_accepts() {
    // A tower-mode module is one the device is not told about at all. Carrying
    // one would mean the document describes a panel this device is not drawing.
    ExpectRefusal(With(kMinimal, R"("mode":"auto")", R"("mode":"tower")"),
                  kErrBadValue, "modules[0].mode");
}

static void test_an_unknown_module_type_is_refused_as_a_value() {
    ExpectRefusal(With(kMinimal, R"("type":"timestamp")", R"("type":"octopus")"),
                  kErrBadValue, "modules[0].type");
}

static void test_a_weather_module_without_its_block_is_refused() {
    ExpectRefusal(With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
                       R"({"type":"weather","mode":"auto"})"),
                  kErrWeatherMissing, "weather");
}

static void test_coordinates_finer_than_two_decimals_are_refused() {
    // The tower coarsens before it writes. This refuses anything finer so a
    // hand-edited document cannot smuggle a street address past the rounding.
    ExpectRefusal(With(kWithWeather, R"("latitude":45.51)",
                       R"("latitude":45.5123)"),
                  kErrBadValue, "weather.latitude");
    ExpectRefusal(With(kWithWeather, R"("longitude":-73.56)",
                       R"("longitude":-73.5641)"),
                  kErrBadValue, "weather.longitude");
}

static void test_coordinates_at_exactly_two_decimals_are_accepted() {
    CHECK(Accepts(With(kWithWeather, R"("latitude":45.51)", R"("latitude":-0.5)")));
    CHECK(Accepts(With(kWithWeather, R"("latitude":45.51)", R"("latitude":0)")));
    CHECK(Accepts(With(kWithWeather, R"("longitude":-73.56)", R"("longitude":180)")));
}

static void test_a_value_cannot_become_unavailable_before_it_is_stale() {
    ExpectRefusal(With(kWithWeather, R"("unavailable_after_min":720)",
                       R"("unavailable_after_min":120)"),
                  kErrBadValue, "weather.unavailable_after_min");
}

static void test_a_row_longer_than_the_bound_is_refused_not_truncated() {
    std::string rows = R"([""])";
    rows.insert(rows.size() - 2, std::string(121, 'x'));
    const std::string modules =
        R"([{"type":"list","mode":"device","title":"T","rows":)" + rows +
        R"(,"synced_epoch":0}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrTooLong, "modules[0].rows[0]");
}

static void test_a_row_at_exactly_the_bound_is_accepted() {
    std::string rows = R"([""])";
    rows.insert(rows.size() - 2, std::string(120, 'x'));
    const std::string modules =
        R"([{"type":"list","mode":"device","title":"T","rows":)" + rows +
        R"(,"synced_epoch":0}])";
    CHECK(Accepts(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules)));
    CHECK_EQ_INT(Scratch().modules[0].rows[0].len, 120);
}

static void test_more_rows_than_the_cap_are_refused() {
    std::string rows = "[";
    for (int i = 0; i < 25; ++i) {
        if (i) rows += ",";
        rows += "\"r\"";
    }
    rows += "]";
    const std::string modules =
        R"([{"type":"list","mode":"device","title":"T","rows":)" + rows +
        R"(,"synced_epoch":0}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrOutOfRange, "modules[0].rows");
}

static void test_more_modules_than_the_cap_are_refused() {
    std::string modules = "[";
    for (int i = 0; i < 9; ++i) {
        if (i) modules += ",";
        modules += R"({"type":"timestamp","mode":"auto"})";
    }
    modules += "]";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrTooManyModules, "modules");
}

static void test_more_conditions_than_the_cap_are_refused() {
    std::string when = "[";
    for (int i = 0; i < 5; ++i) {
        if (i) when += ",";
        when += R"({"kind":"weather_state","state":"ok"})";
    }
    when += "]";
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all","when":)" +
        when + R"(,"text":"x","fallback_text":null}])";
    ExpectRefusal(With(kWithWeather, R"([{"type":"weather","mode":"auto"}])", modules),
                  kErrOutOfRange, "modules[0].when");
}

static void test_an_empty_condition_list_is_refused() {
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all","when":[],)"
        R"("text":"x","fallback_text":null}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrOutOfRange, "modules[0].when");
}

static void test_an_all_open_date_range_is_refused() {
    // It matches everything, which means the owner asked for a condition and
    // would have got an unconditional message.
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all",)"
        R"("when":[{"kind":"date_range","from":null,"to":null}],)"
        R"("text":"x","fallback_text":null}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrBadValue, "modules[0].when[0]");
}

static void test_a_malformed_date_is_refused() {
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all",)"
        R"("when":[{"kind":"date_range","from":"2026-13-01","to":null}],)"
        R"("text":"x","fallback_text":null}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrBadValue, "modules[0].when[0].from");
}

static void test_a_condition_field_from_another_kind_is_refused() {
    const std::string modules =
        R"([{"type":"conditional_message","mode":"device","match":"all",)"
        R"("when":[{"kind":"weather_state","state":"ok","days":[1]}],)"
        R"("text":"x","fallback_text":null}])";
    ExpectRefusal(With(kWithWeather, R"([{"type":"weather","mode":"auto"}])", modules),
                  kErrUnknownField, "modules[0].when[0].days");
}

static void test_markup_in_panel_text_is_refused() {
    // The device has no markup, so a tag here is either a mistake or an
    // attempt, and both deserve the same answer: refusal, not escaping.
    const std::string modules =
        R"([{"type":"message","mode":"auto","text":"<b>hi</b>","expires_epoch":null}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrBadValue, "modules[0].text");
}

static void test_an_escaped_control_character_in_text_is_refused() {
    // The JSON reader refuses a *literal* newline; this is the escaped form,
    // which is legal JSON and still not something the panel can draw.
    const std::string modules =
        R"([{"type":"message","mode":"auto","text":"a\nb","expires_epoch":null}])";
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrBadValue, "modules[0].text");
}

static void test_accented_text_is_accepted_and_counted_in_utf16() {
    // "é" is two UTF-8 bytes and one UTF-16 unit. The tower's bound counts
    // UTF-16 units, so 120 of these must fit where 240 bytes would not.
    std::string row;
    for (int i = 0; i < 120; ++i) row += "\xc3\xa9";
    const std::string modules =
        R"([{"type":"list","mode":"device","title":"T","rows":[")" + row +
        R"("],"synced_epoch":0}])";
    CHECK(Accepts(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules)));
    CHECK_EQ_INT(Scratch().modules[0].rows[0].len, 240);
}

static void test_malformed_utf8_is_refused() {
    // A lone continuation byte. It reaches here as a literal byte in the
    // document, which the JSON reader passes through and the text engine could
    // not draw.
    std::string modules =
        R"([{"type":"message","mode":"auto","text":"aXb","expires_epoch":null}])";
    modules[modules.find('X')] = static_cast<char>(0x80);
    ExpectRefusal(With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules),
                  kErrBadValue, "modules[0].text");
}

static void test_a_document_over_the_size_bound_is_refused_before_parsing() {
    std::string doc(kProfileMaxBytes + 1, ' ');
    ParseError err;
    CHECK(!ParseProfile(doc.data(), doc.size(), &Scratch(), &err));
    CHECK(err.code == kErrTooLarge);
}

static void test_a_document_that_is_not_json_is_refused() {
    ParseError err;
    const std::string doc = "not json at all";
    CHECK(!ParseProfile(doc.data(), doc.size(), &Scratch(), &err));
    CHECK(err.code != nullptr);
}

static void test_trailing_bytes_after_the_document_are_refused() {
    // The reader's own token, spelled out rather than referenced: the JSON
    // reader is private to autonomy_profile.cc, and this string is what crosses
    // the wire into the tower's copy table. Writing the literal here is what
    // makes a change to that spelling fail a test instead of silently becoming
    // an unrecognised code at the other end.
    ExpectRefusal(std::string(kMinimal) + "{}", "json_trailing_bytes", "");
}

static void test_every_truncation_of_a_valid_profile_is_refused() {
    // Not one prefix of a good document may be mistaken for a good document.
    // Under ASan this also proves no read runs off the end of the buffer.
    const std::string full(kWithWeather);
    for (size_t cut = 0; cut < full.size(); ++cut) {
        ParseError err;
        const bool ok = ParseProfile(full.data(), cut, &Scratch(), &err);
        if (ok) {
            ++g_failures;
            printf("  FAIL in %s: prefix of %zu bytes was accepted\n", g_current, cut);
        }
        ++g_checks;
    }
    CHECK(Accepts(full));
}

static void test_a_maximal_document_fits_the_text_arena() {
    // Eight list modules of a title and 24 rows apiece is the worst case for
    // the arena's terminators, and the reason kProfileTextBytes has headroom
    // over the wire bound. Built here so the margin is measured, not assumed.
    std::string modules = "[";
    for (int m = 0; m < 8; ++m) {
        if (m) modules += ",";
        modules += R"({"type":"list","mode":"device","title":")";
        modules += std::string(80, 'T');
        modules += R"(","rows":[)";
        for (int r = 0; r < 24; ++r) {
            if (r) modules += ",";
            modules += "\"" + std::string(60, 'r') + "\"";
        }
        modules += R"(],"synced_epoch":0})";
    }
    modules += "]";
    const std::string doc =
        With(kMinimal, R"([{"type":"timestamp","mode":"auto"}])", modules);
    CHECK(doc.size() <= kProfileMaxBytes);
    CHECK(Accepts(doc));
    CHECK_EQ_INT(Scratch().module_count, 8);
    CHECK_EQ_INT(Scratch().modules[7].row_count, 24);
    CHECK(Scratch().text_len <= kProfileTextBytes);
}

static void test_a_refused_document_does_not_leave_a_usable_profile() {
    CHECK(Accepts(kWithWeather));
    CHECK_EQ_INT(Scratch().module_count, 1);
    // A refusal clears rather than half-fills. The contract says `out` is
    // unspecified-but-valid on failure, and "valid" has to mean the caller
    // cannot mistake leftovers for a profile.
    ParseError err;
    const std::string bad = With(kMinimal, R"("composition":"flow")",
                                 R"("composition":"nope")");
    CHECK(!ParseProfile(bad.data(), bad.size(), &Scratch(), &err));
    CHECK(Scratch().profile_version != 1 || Scratch().module_count == 0);
}

// ------------------------------------------------------------ profile slot --

namespace {

/**
 * @brief Memory-backed SlotIo that can fail a write part way, the way a power
 *        cut does.
 */
class CuttingSlotIo : public record::SlotIo {
public:
    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        const std::vector<uint8_t>& s = slots_[slot];
        if (s.empty()) return 0;
        const size_t n = s.size() < max ? s.size() : max;
        memcpy(buf, s.data(), n);
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (cut_after_ >= 0 && static_cast<size_t>(cut_after_) < len) {
            // Power went away mid-write: the bytes that made it are there, the
            // rest are not, and the call never returns success.
            slots_[slot].assign(data, data + cut_after_);
            return false;
        }
        slots_[slot].assign(data, data + len);
        return true;
    }

    void CutAfter(int bytes) { cut_after_ = bytes; }
    void NoCut() { cut_after_ = -1; }

private:
    std::vector<uint8_t> slots_[record::kSlotCount];
    int cut_after_ = -1;
};

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace

static void test_a_profile_round_trips_through_its_slot_byte_for_byte() {
    CuttingSlotIo io;
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    record::RecordSlot slot(kProfileSpec, &io, scratch.data(), scratch.size());

    const std::vector<uint8_t> doc = Bytes(kWithWeather);
    CHECK(slot.Store(doc.data(), doc.size(), 1789500000u) ==
          record::StoreResult::kOk);
    CHECK(slot.has_record());

    std::vector<uint8_t> out(kProfileMaxBytes);
    size_t len = 0;
    CHECK(slot.ReadPayload(out.data(), out.size(), &len));
    CHECK_EQ_INT(len, doc.size());
    CHECK(memcmp(out.data(), doc.data(), len) == 0);

    // And the bytes that came back still parse, which is the whole point of
    // keeping them verbatim rather than re-serialising.
    ParseError err;
    CHECK(ParseProfile(reinterpret_cast<const char*>(out.data()), len, &Scratch(),
                       &err));
}

static void test_a_frame_record_is_not_mistaken_for_a_profile() {
    // Same filesystem, same header layout, different magic. A frame in a
    // profile slot must read as "no profile", not as a profile of garbage.
    CuttingSlotIo io;
    std::vector<uint8_t> frame_scratch(record::kHeaderBytes + 30000);
    record::RecordSlot frame(record::RecordSpec{
                                    {'N', '4', 'C', 'D', 'A', 'S', 'H', '1'},
                                    30000, 30000},
                                &io, frame_scratch.data(), frame_scratch.size());
    std::vector<uint8_t> pixels(30000, 0xa5);
    CHECK(frame.Store(pixels.data(), pixels.size(), 0) ==
          record::StoreResult::kOk);

    std::vector<uint8_t> prof_scratch(kProfileRecordBytes);
    record::RecordSlot profile(kProfileSpec, &io, prof_scratch.data(),
                                  prof_scratch.size());
    CHECK(!profile.Load());
    CHECK(!profile.has_record());
}

static void test_a_corrupted_profile_slot_leaves_autonomy_inert() {
    CuttingSlotIo io;
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    record::RecordSlot slot(kProfileSpec, &io, scratch.data(), scratch.size());
    const std::vector<uint8_t> doc = Bytes(kMinimal);
    CHECK(slot.Store(doc.data(), doc.size(), 0) == record::StoreResult::kOk);

    // Flip a byte of the payload. The stored digest no longer matches, so the
    // record is ignored rather than handed to the parser.
    uint8_t raw[kProfileRecordBytes];
    const int n = io.ReadSlot(slot.active_slot(), raw, sizeof(raw));
    CHECK(n > 0);
    raw[record::kHeaderBytes + 5] ^= 0xff;
    io.NoCut();
    CHECK(io.WriteSlot(slot.active_slot(), raw, static_cast<size_t>(n)));

    record::RecordSlot reloaded(kProfileSpec, &io, scratch.data(), scratch.size());
    CHECK(!reloaded.Load());
}

static void test_a_power_cut_at_any_offset_never_tears_a_profile() {
    const std::vector<uint8_t> first = Bytes(kMinimal);
    const std::vector<uint8_t> second = Bytes(kWithWeather);

    const size_t record_len = record::kHeaderBytes + second.size();
    int cuts = 0;
    for (size_t cut = 0; cut < record_len; ++cut) {
        CuttingSlotIo io;
        std::vector<uint8_t> scratch(kProfileRecordBytes);
        record::RecordSlot slot(kProfileSpec, &io, scratch.data(), scratch.size());
        CHECK(slot.Store(first.data(), first.size(), 1) ==
              record::StoreResult::kOk);

        io.CutAfter(static_cast<int>(cut));
        slot.Store(second.data(), second.size(), 2);
        io.NoCut();

        // Reload from scratch, the way a boot after the cut would.
        record::RecordSlot after(kProfileSpec, &io, scratch.data(), scratch.size());
        CHECK(after.Load());
        std::vector<uint8_t> out(kProfileMaxBytes);
        size_t len = 0;
        CHECK(after.ReadPayload(out.data(), out.size(), &len));

        const bool is_first = len == first.size() &&
                              memcmp(out.data(), first.data(), len) == 0;
        const bool is_second = len == second.size() &&
                               memcmp(out.data(), second.data(), len) == 0;
        if (!is_first && !is_second) {
            ++g_failures;
            printf("  FAIL in %s: cut at %zu produced neither profile\n", g_current,
                   cut);
        }
        ++g_checks;

        // And whichever survived still parses. A record that validated but no
        // longer describes a panel would be the worst of both worlds.
        ParseError err;
        CHECK(ParseProfile(reinterpret_cast<const char*>(out.data()), len, &Scratch(),
                           &err));
        ++cuts;
    }
    printf("    (%d write-cut offsets exercised)\n", cuts);
}

static void test_an_oversized_payload_is_refused_by_the_slot() {
    CuttingSlotIo io;
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    record::RecordSlot slot(kProfileSpec, &io, scratch.data(), scratch.size());
    std::vector<uint8_t> too_big(kProfileMaxBytes + 1, '{');
    CHECK(slot.Store(too_big.data(), too_big.size(), 0) ==
          record::StoreResult::kBadLength);
}

static void test_storing_the_same_profile_twice_writes_nothing() {
    // The flash-wear guarantee: a scheduler that re-pushes an unchanged profile
    // must not cost a write.
    CuttingSlotIo io;
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    record::RecordSlot slot(kProfileSpec, &io, scratch.data(), scratch.size());
    const std::vector<uint8_t> doc = Bytes(kMinimal);
    CHECK(slot.Store(doc.data(), doc.size(), 0) == record::StoreResult::kOk);
    const int active = slot.active_slot();
    CHECK(slot.Store(doc.data(), doc.size(), 0) ==
          record::StoreResult::kDuplicate);
    CHECK_EQ_INT(slot.active_slot(), active);
}

// ------------------------------------------------- bounded field reporting --
//
// Every refusal names the field it was about, and that name is built by
// appending a key to a path. Both are bounded but their sum is not, so the
// join has to decide what to drop. It drops the *front*, because "…rules[0].
// kind" tells the owner which control to look at and "modules[0].rul" does
// not — and it marks the drop with a leading ellipsis rather than silently
// presenting a path that was never in the document.

static void test_the_longest_reachable_field_name_is_reported_in_full() {
    // The bound this pins: the deepest path this schema builds, plus a key at
    // the reader's maximum, still fits ParseError::field with room to spare —
    // so no refusal the owner can actually provoke is ever reported under a
    // name that was not in their document.
    //
    // JoinField's eliding branch exists for the case where that stops being
    // true after a schema change. It is not exercised here because no input can
    // reach it today, and a test that pretended otherwise would be testing a
    // document this parser refuses for a different reason first.
    std::string key(60, 'k');
    std::string doc =
        With(kMinimal, R"({"type":"timestamp","mode":"auto"})",
             R"({"type":"conditional_message","mode":"device","text":"x",)"
             R"("when":[{"kind":"days_of_week","days":[1],")" + key + R"(":1}]})");

    ParseError err;
    CHECK(!ParseProfile(doc.data(), doc.size(), &Scratch(), &err));
    CHECK(err.code == kErrUnknownField);

    const size_t len = strlen(err.field);
    CHECK(len < sizeof(err.field));
    // Reported whole: the name ends with the key and begins with the path.
    CHECK(len > key.size());
    CHECK_STR(err.field + (len - key.size()), key.c_str());
    CHECK_STR(err.field, ("modules[0].when[0]." + key).c_str());
}

static void test_a_field_name_that_fits_is_not_marked_as_truncated() {
    // The overwhelmingly common case must be untouched by the above.
    ExpectRefusal(With(kMinimal, R"("type":"timestamp")",
                       R"("type":"timestamp","nope":1)"),
                  kErrUnknownField, "modules[0].nope");
}

int main() {
    RUN(test_the_minimal_profile_is_accepted_and_read_back);
    RUN(test_a_weather_profile_carries_its_thresholds);
    RUN(test_a_profile_with_no_weather_is_fully_offline);
    RUN(test_every_module_type_round_trips);
    RUN(test_all_three_compositions_are_known);
    RUN(test_key_order_does_not_matter);
    RUN(test_an_expiring_message_and_a_fallback_are_carried);
    RUN(test_a_weather_state_condition_needs_the_forecast);

    RUN(test_an_unknown_root_field_is_refused_by_name);
    RUN(test_an_unknown_weather_field_is_refused_by_name);
    RUN(test_an_unknown_module_field_is_refused_by_name);
    RUN(test_a_field_belonging_to_another_module_type_is_unknown_here);
    RUN(test_a_missing_root_field_is_named);
    RUN(test_a_missing_module_field_is_named);
    RUN(test_a_duplicate_root_field_is_refused);
    RUN(test_a_duplicate_module_field_is_refused);
    RUN(test_a_wake_interval_below_the_floor_is_refused);
    RUN(test_a_wake_interval_above_the_ceiling_is_refused);
    RUN(test_the_wake_bounds_match_the_power_contract);
    RUN(test_a_tower_wait_past_the_fetch_cap_is_refused);
    RUN(test_a_future_profile_version_says_so);
    RUN(test_an_unknown_composition_is_refused);
    RUN(test_tower_is_not_a_mode_the_device_accepts);
    RUN(test_an_unknown_module_type_is_refused_as_a_value);
    RUN(test_a_weather_module_without_its_block_is_refused);
    RUN(test_coordinates_finer_than_two_decimals_are_refused);
    RUN(test_coordinates_at_exactly_two_decimals_are_accepted);
    RUN(test_a_value_cannot_become_unavailable_before_it_is_stale);
    RUN(test_a_row_longer_than_the_bound_is_refused_not_truncated);
    RUN(test_a_row_at_exactly_the_bound_is_accepted);
    RUN(test_more_rows_than_the_cap_are_refused);
    RUN(test_more_modules_than_the_cap_are_refused);
    RUN(test_more_conditions_than_the_cap_are_refused);
    RUN(test_an_empty_condition_list_is_refused);
    RUN(test_an_all_open_date_range_is_refused);
    RUN(test_a_malformed_date_is_refused);
    RUN(test_a_condition_field_from_another_kind_is_refused);
    RUN(test_markup_in_panel_text_is_refused);
    RUN(test_an_escaped_control_character_in_text_is_refused);
    RUN(test_accented_text_is_accepted_and_counted_in_utf16);
    RUN(test_malformed_utf8_is_refused);
    RUN(test_a_document_over_the_size_bound_is_refused_before_parsing);
    RUN(test_a_document_that_is_not_json_is_refused);
    RUN(test_trailing_bytes_after_the_document_are_refused);
    RUN(test_every_truncation_of_a_valid_profile_is_refused);
    RUN(test_a_maximal_document_fits_the_text_arena);
    RUN(test_a_refused_document_does_not_leave_a_usable_profile);

    RUN(test_a_profile_round_trips_through_its_slot_byte_for_byte);
    RUN(test_a_frame_record_is_not_mistaken_for_a_profile);
    RUN(test_a_corrupted_profile_slot_leaves_autonomy_inert);
    RUN(test_a_power_cut_at_any_offset_never_tears_a_profile);
    RUN(test_the_longest_reachable_field_name_is_reported_in_full);
    RUN(test_a_field_name_that_fits_is_not_marked_as_truncated);

    RUN(test_an_oversized_payload_is_refused_by_the_slot);
    RUN(test_storing_the_same_profile_twice_writes_nothing);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
