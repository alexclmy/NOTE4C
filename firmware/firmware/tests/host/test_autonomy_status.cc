/**
 * @file test_autonomy_status.cc
 * @brief Host tests for the status route's `autonomy` block.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * These read the rendered bytes back, the way test_power_policy.cc does for the
 * power block, because every claim worth making about a status route is a claim
 * about what it says when it does not know something.
 *
 * The searches below are deliberately for *digits in the wrong places*: a
 * refactor that substituted a plausible default for a null would keep every
 * shape assertion passing and quietly start telling the tower that a device
 * which had never run a cycle had just updated successfully.
 */

#include "common/autonomy_status.h"

#include <stdio.h>
#include <stdlib.h>
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

#define RUN(fn)                                                            \
    do {                                                                   \
        g_current = #fn;                                                   \
        const int before = g_failures;                                     \
        fn();                                                              \
        printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
    } while (0)

namespace {

std::string Render(const AutonomyStatus& s) {
    char out[kAutonomyJsonMax];
    const size_t n = RenderAutonomyJson(s, out, sizeof(out));
    if (n == 0) return "";
    return std::string(out, n);
}

bool Has(const std::string& json, const char* needle) {
    return json.find(needle) != std::string::npos;
}

/// A device doing everything: enabled, profiled, one cycle behind it.
AutonomyStatus Busy() {
    AutonomyStatus s;
    s.enabled = true;
    s.profile.present = true;
    memcpy(s.profile.sha256,
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 65);
    s.profile.revision = 12;
    s.profile.profile_version = 1;
    s.profile.applied_epoch = 1789380000;
    s.profile.applied_epoch_known = true;

    s.last_cycle.known = true;
    s.last_cycle.origin = Origin::kLocal;
    s.last_cycle.outcome = CycleOutcome::kUpdated;
    s.last_cycle.rendered_epoch = 1789387200;
    s.last_cycle.rendered_epoch_known = true;
    s.last_cycle.wifi_attempted = true;
    s.last_cycle.wifi_connected = true;
    s.last_cycle.wifi_duration_ms = 4200;
    s.last_cycle.fetch.attempted = true;
    s.last_cycle.fetch.ok = true;
    s.last_cycle.fetch.http_status = 200;
    s.last_cycle.fetch.duration_ms = 1900;
    s.last_cycle.fetch.bytes = 8123;
    s.last_cycle.fetch.fetched_epoch = 1789387100;
    s.last_cycle.fetch.cache_age_s = 100;
    s.last_cycle.fetch.age_known = true;

    s.displayed_origin = Origin::kLocal;
    s.next_wake_epoch = 1789390800;
    s.next_wake_epoch_known = true;
    return s;
}

}  // namespace

// --------------------------------------------------------- the busy device --

static void test_a_working_device_reports_every_field() {
    const std::string json = Render(Busy());
    CHECK(!json.empty());
    CHECK(Has(json, "\"enabled\":true"));
    CHECK(Has(json, "\"present\":true"));
    CHECK(Has(json,
              "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef"
              "0123456789abcdef\""));
    CHECK(Has(json, "\"revision\":12"));
    CHECK(Has(json, "\"profile_version\":1"));
    CHECK(Has(json, "\"applied_epoch\":1789380000"));
    CHECK(Has(json, "\"origin\":\"local\""));
    CHECK(Has(json, "\"outcome\":\"updated\""));
    CHECK(Has(json, "\"rendered_epoch\":1789387200"));
    CHECK(Has(json, "\"connected\":true"));
    CHECK(Has(json, "\"duration_ms\":4200"));
    CHECK(Has(json, "\"http_status\":200"));
    CHECK(Has(json, "\"bytes\":8123"));
    CHECK(Has(json, "\"cache_age_s\":100"));
    CHECK(Has(json, "\"displayed_origin\":\"local\""));
    CHECK(Has(json, "\"next_wake_epoch\":1789390800"));
}

static void test_the_block_is_balanced_json() {
    // Not a parser, but enough to catch an unclosed object, which is the way
    // this kind of hand-rolled renderer usually fails.
    const std::string json = Render(Busy());
    int depth = 0;
    int quotes = 0;
    for (char c : json) {
        if (c == '"') quotes += 1;
        if (c == '{') depth += 1;
        if (c == '}') depth -= 1;
        CHECK(depth >= 0);
    }
    CHECK_EQ_INT(depth, 0);
    CHECK_EQ_INT(quotes % 2, 0);
}

// ----------------------------------------------------- the honest silences --

static void test_a_device_with_no_profile_says_so_rather_than_reporting_an_empty_one() {
    AutonomyStatus s;
    s.enabled = true;
    const std::string json = Render(s);
    CHECK(Has(json, "\"present\":false"));
    CHECK(Has(json, "\"sha256\":\"\""));
    CHECK(Has(json, "\"applied_epoch\":null"));
}

static void test_a_device_that_has_run_no_cycle_reports_null() {
    // Defaulting this to an outcome would tell the tower the last update
    // succeeded on a device that has never updated at all.
    AutonomyStatus s = Busy();
    s.last_cycle = CycleStatus{};
    const std::string json = Render(s);
    CHECK(Has(json, "\"last_cycle\":null"));
    CHECK(!Has(json, "\"outcome\":"));
}

static void test_an_unset_clock_renders_nulls_and_not_numbers() {
    AutonomyStatus s = Busy();
    s.profile.applied_epoch_known = false;
    s.last_cycle.rendered_epoch_known = false;
    s.last_cycle.fetch.age_known = false;
    s.next_wake_epoch_known = false;

    const std::string json = Render(s);
    CHECK(Has(json, "\"applied_epoch\":null"));
    CHECK(Has(json, "\"rendered_epoch\":null"));
    CHECK(Has(json, "\"cache_age_s\":null"));
    CHECK(Has(json, "\"next_wake_epoch\":null"));
    // And, specifically, that the numbers did not survive as numbers anyway.
    CHECK(!Has(json, "\"applied_epoch\":1789380000"));
    CHECK(!Has(json, "\"next_wake_epoch\":1789390800"));
}

static void test_a_fetch_that_got_no_response_is_not_reported_as_a_status_code() {
    // No response at all is a different fact from a 500, and a tower that saw
    // one reported as the other would look for a server problem that was really
    // a router problem.
    AutonomyStatus s = Busy();
    s.last_cycle.fetch.ok = false;
    s.last_cycle.fetch.http_status = -1;
    const std::string json = Render(s);
    CHECK(Has(json, "\"http_status\":null"));
    CHECK(Has(json, "\"attempted\":true"));
    CHECK(Has(json, "\"ok\":false"));
    CHECK(!Has(json, "-1"));
}

static void test_a_cycle_that_never_touched_the_radio_says_so_explicitly() {
    // In Device mode this is the desirable case, not a missing measurement.
    AutonomyStatus s = Busy();
    s.last_cycle.wifi_attempted = false;
    s.last_cycle.fetch = FetchStatus{};
    const std::string json = Render(s);
    CHECK(Has(json, "\"wifi\":{\"connected\":false,\"duration_ms\":0}"));
    CHECK(Has(json, "\"fetch\":{\"weather\":null}"));
}

static void test_degraded_is_a_first_class_outcome() {
    // It must never be folded into "updated": a panel drawn from a three-hour
    // old cache did update, and saying only that would hide the interesting
    // half.
    AutonomyStatus s = Busy();
    s.last_cycle.outcome = CycleOutcome::kDegraded;
    CHECK(Has(Render(s), "\"outcome\":\"degraded\""));

    s.last_cycle.outcome = CycleOutcome::kFailed;
    CHECK(Has(Render(s), "\"outcome\":\"failed\""));

    s.last_cycle.outcome = CycleOutcome::kUnchanged;
    CHECK(Has(Render(s), "\"outcome\":\"unchanged\""));
}

static void test_every_outcome_and_origin_renders_a_known_name() {
    const CycleOutcome outcomes[] = {CycleOutcome::kUpdated, CycleOutcome::kUnchanged,
                                     CycleOutcome::kDegraded, CycleOutcome::kFailed};
    for (CycleOutcome o : outcomes) {
        CHECK(CycleOutcomeName(o)[0] != '\0');
    }
    const Origin origins[] = {Origin::kNone, Origin::kTower, Origin::kLocal,
                              Origin::kUnknown};
    for (Origin o : origins) {
        CHECK(OriginName(o)[0] != '\0');
    }
}

/**
 * WHAT THIS TEST USED TO ASSERT, AND WHY IT WAS WRONG.
 *
 * It pinned `displayed_origin: "tower"` for a device that had displayed
 * nothing, on the reasoning that the tower's ledger would fail to resolve the
 * digest and report an honest "unknown frame". That reasoning does not survive
 * contact with the other states: the same field also said "tower" after a local
 * compose was displayed, after a reboot, and after any refresh completed,
 * because the only thing that ever wrote it was a single assignment at the
 * instant a local submit was accepted.
 *
 * The field is now what its name says, and "unknown" is a value the tower
 * understands rather than one it has to infer from a failed lookup.
 */
static void test_an_unconfirmed_panel_says_unknown_rather_than_tower() {
    AutonomyStatus s = Busy();
    s.displayed_origin = Origin::kUnknown;
    s.stored_origin = Origin::kLocal;
    const std::string json = Render(s);
    CHECK(Has(json, "\"displayed_origin\":\"unknown\""));
    CHECK(!Has(json, "\"displayed_origin\":\"tower\""));
    // And the thing that *is* knowable is reported next to it.
    CHECK(Has(json, "\"stored_origin\":\"local\""));
}

static void test_a_device_holding_nothing_says_none_for_both() {
    AutonomyStatus s = Busy();
    s.displayed_origin = Origin::kNone;
    s.stored_origin = Origin::kNone;
    const std::string json = Render(s);
    CHECK(Has(json, "\"displayed_origin\":\"none\""));
    CHECK(Has(json, "\"stored_origin\":\"none\""));
}

static void test_the_two_origins_are_reported_independently() {
    // The state that made the old single field impossible: the store holds a
    // frame this device composed, and the panel is still showing the one the
    // tower pushed before it, because the refresh has not finished.
    AutonomyStatus s = Busy();
    s.stored_origin = Origin::kLocal;
    s.displayed_origin = Origin::kTower;
    const std::string json = Render(s);
    CHECK(Has(json, "\"stored_origin\":\"local\""));
    CHECK(Has(json, "\"displayed_origin\":\"tower\""));
}

// ------------------------------------------------------------- the buffer --

static void test_the_rendered_block_fits_the_advertised_buffer() {
    // The number in the header is a promise the route relies on. Measured
    // against the largest object this can produce rather than assumed.
    AutonomyStatus s = Busy();
    s.profile.revision = 2147483647;
    s.profile.profile_version = 2147483647;
    s.profile.applied_epoch = 4102444800ll;
    s.last_cycle.rendered_epoch = 4102444800ll;
    s.last_cycle.wifi_duration_ms = 4294967295u;
    s.last_cycle.fetch.duration_ms = 4294967295u;
    s.last_cycle.fetch.bytes = 4294967295u;
    s.last_cycle.fetch.fetched_epoch = 4102444800ll;
    s.last_cycle.fetch.cache_age_s = 4102444800ll;
    s.next_wake_epoch = 4102444800ll;

    char out[kAutonomyJsonMax];
    const size_t n = RenderAutonomyJson(s, out, sizeof(out));
    CHECK(n > 0);
    CHECK(n < kAutonomyJsonMax);
    printf("    (largest rendered block: %zu of %zu bytes)\n", n, kAutonomyJsonMax);
}

static void test_a_too_small_buffer_yields_nothing_not_a_fragment() {
    // A tower that sees null knows it learned nothing; a fragment would fail to
    // parse and take the whole status response down with it.
    const AutonomyStatus s = Busy();
    for (size_t cap = 1; cap < 200; ++cap) {
        char out[256];
        memset(out, 'x', sizeof(out));
        const size_t n = RenderAutonomyJson(s, out, cap);
        CHECK_EQ_INT(n, 0);
        CHECK_EQ_INT(out[0], '\0');
    }
}

static void test_render_refuses_a_null_buffer() {
    const AutonomyStatus s = Busy();
    CHECK_EQ_INT(RenderAutonomyJson(s, nullptr, 100), 0);
}

static void test_a_digest_that_is_not_hex_is_refused_rather_than_emitted() {
    // The only strings this block emits are a hex digest and enum names, so a
    // value needing escapes means something upstream is wrong. Failing loudly
    // beats emitting a broken document.
    AutonomyStatus s = Busy();
    memcpy(s.profile.sha256, "not\"a\"digest", 13);
    char out[kAutonomyJsonMax];
    CHECK_EQ_INT(RenderAutonomyJson(s, out, sizeof(out)), 0);
    CHECK_EQ_INT(out[0], '\0');
}


// -------------------------------------------------- the cross-repo contract --

static void test_the_rendered_samples_are_exported_for_the_tower_to_parse() {
    /**
     * The firmware renders this block; the tower parses it with a zod schema in
     * src/server/device/client.ts. Those are two descriptions of one contract,
     * written in two languages in two repositories, and nothing in either
     * repository would notice them drifting apart.
     *
     * So the samples are exported here and the tower's suite parses them with
     * the real schema. It is the same trick as the golden frames: one side
     * produces, the other consumes, and the disagreement becomes a test
     * failure instead of a field report.
     *
     * Written only under AUTONOMY_WRITE_FIXTURES=1, alongside the goldens.
     */
    const char* write = getenv("AUTONOMY_WRITE_FIXTURES");
    if (write == nullptr || write[0] != '1') return;

    struct Sample {
        const char* name;
        AutonomyStatus status;
    };
    std::vector<Sample> samples;

    samples.push_back({"busy", Busy()});

    AutonomyStatus off;
    samples.push_back({"disabled-and-unprofiled", off});

    AutonomyStatus no_cycle = Busy();
    no_cycle.last_cycle = CycleStatus{};
    samples.push_back({"no-cycle-yet", no_cycle});

    AutonomyStatus no_clock = Busy();
    no_clock.profile.applied_epoch_known = false;
    no_clock.last_cycle.rendered_epoch_known = false;
    no_clock.last_cycle.fetch.age_known = false;
    no_clock.next_wake_epoch_known = false;
    samples.push_back({"clock-unknown", no_clock});

    AutonomyStatus offline = Busy();
    offline.last_cycle.outcome = CycleOutcome::kDegraded;
    offline.last_cycle.wifi_attempted = true;
    offline.last_cycle.wifi_connected = false;
    offline.last_cycle.fetch.ok = false;
    offline.last_cycle.fetch.http_status = -1;
    samples.push_back({"degraded-offline", offline});

    AutonomyStatus offline_never = Busy();
    offline_never.last_cycle.wifi_attempted = false;
    offline_never.last_cycle.fetch = FetchStatus{};
    offline_never.last_cycle.origin = Origin::kLocal;
    samples.push_back({"composed-without-radio", offline_never});

    // The ordinary state after a deep sleep, and the one the first version of
    // this block could not express: the device is holding a frame it composed,
    // and cannot confirm that the frame is the one on the glass, because
    // e-paper keeps its image across the reboot and the coordinator that knows
    // what was drawn does not.
    AutonomyStatus unconfirmed = Busy();
    unconfirmed.stored_origin = Origin::kLocal;
    unconfirmed.displayed_origin = Origin::kUnknown;
    samples.push_back({"display-unconfirmed", unconfirmed});

    // A device that is holding nothing at all. Both origins are "none", which
    // is a different claim from "unknown" and is reported as one.
    AutonomyStatus empty = Busy();
    empty.stored_origin = Origin::kNone;
    empty.displayed_origin = Origin::kNone;
    samples.push_back({"nothing-stored", empty});

    std::string doc = "{\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const std::string json = Render(samples[i].status);
        CHECK(!json.empty());
        doc += std::string("  \"") + samples[i].name + "\": " + json;
        doc += (i + 1 < samples.size() ? ",\n" : "\n");
    }
    doc += "}\n";

    const char* dir_env = getenv("AUTONOMY_FIXTURE_DIR");
    const std::string dir = (dir_env != nullptr && dir_env[0] != '\0')
                                ? dir_env
                                : "tests/fixtures/autonomy";
    const std::string path = dir + "/status-samples.json";
    FILE* f = fopen(path.c_str(), "wb");
    ++g_checks;
    if (f == nullptr) {
        ++g_failures;
        printf("  FAIL in %s: cannot write %s\n", g_current, path.c_str());
        return;
    }
    fwrite(doc.data(), 1, doc.size(), f);
    fclose(f);
    printf("    (wrote %zu status samples to %s)\n", samples.size(), path.c_str());
}

int main() {
    RUN(test_a_working_device_reports_every_field);
    RUN(test_the_block_is_balanced_json);

    RUN(test_a_device_with_no_profile_says_so_rather_than_reporting_an_empty_one);
    RUN(test_a_device_that_has_run_no_cycle_reports_null);
    RUN(test_an_unset_clock_renders_nulls_and_not_numbers);
    RUN(test_a_fetch_that_got_no_response_is_not_reported_as_a_status_code);
    RUN(test_a_cycle_that_never_touched_the_radio_says_so_explicitly);
    RUN(test_degraded_is_a_first_class_outcome);
    RUN(test_every_outcome_and_origin_renders_a_known_name);
    RUN(test_an_unconfirmed_panel_says_unknown_rather_than_tower);
    RUN(test_a_device_holding_nothing_says_none_for_both);
    RUN(test_the_two_origins_are_reported_independently);

    RUN(test_the_rendered_block_fits_the_advertised_buffer);
    RUN(test_a_too_small_buffer_yields_nothing_not_a_fragment);
    RUN(test_render_refuses_a_null_buffer);
    RUN(test_a_digest_that_is_not_hex_is_refused_rather_than_emitted);

    RUN(test_the_rendered_samples_are_exported_for_the_tower_to_parse);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
