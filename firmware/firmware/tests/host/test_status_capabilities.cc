/**
 * @file test_status_capabilities.cc
 * @brief Host tests for GET /api/v1/dashboard/status: the capability list it
 *        advertises on a device built with the autonomy rollout gate on, and
 *        the memory contract the handler assembles its response under.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS SUITE IS FOR
 * ----------------------
 * The capability list is the whole of the tower's knowledge about what a device
 * can do. A tower that does not read "autonomy.profile.v1" there renders the
 * device as push-only and hides the local-composition controls — correctly, and
 * silently, and identically whether the feature is genuinely absent or merely
 * failed to announce itself. There is no second signal to cross-check against,
 * which makes this one string load-bearing in a way a normal response field is
 * not.
 *
 * The string is produced by devcfg::RenderCapabilitiesJson(), which
 * test_device_config.cc already covers by passing it literals. What no other
 * suite covers is the step before that: the two *macros* StatusHandler hands
 * it. Those come out of main/dashboard_build_config.h, which resolves
 * CONFIG_AUTONOMY_ENABLED with an `#ifdef` — and an `#ifdef` on a symbol that
 * has not arrived yet is false rather than an error. The gate can therefore be
 * 0 on a device whose sdkconfig.h says 1, with the routes still compiled, the
 * string still in .rodata and every existing test still green.
 *
 * So the derivation is compiled here in the environment the device compiles it
 * in — ESP_PLATFORM defined, <sdkconfig.h> on the include path, and nothing in
 * front of the header that might have pulled sdkconfig.h in as a side effect —
 * and the resulting list is compared byte for byte. See status_caps_gate.cc for
 * the fixture, tests/host/target_config/ for the stand-in sdkconfig.h.
 *
 * Related, and deliberately separate: test_build_gates.cc pins the OFF default
 * of the same gate, where the question is what an unconfigured build does
 * rather than how a configured one learns it was configured.
 *
 * THE SECOND HALF: WHERE THE RESPONSE IS ASSEMBLED
 * ------------------------------------------------
 * The same handler also crashed the device. Its four response buffers were
 * locals — 4 544 bytes of them, in a frame the disassembly put at 4 976
 * (`entry a1, 0x1370`) inside an httpd task given 6 144 — and one unauthenticated
 * GET was enough:
 *
 *     assert failed: xTaskPriorityDisinherit tasks.c:5157 (pxTCB->uxMutexesHeld)
 *
 * raised from StatusHandler -> SendJson -> ESP_LOG -> esp_log_impl_unlock ->
 * xQueueGenericSend, then RTC_SW_CPU_RST. The response never left the device.
 *
 * A host cannot link that handler — it reaches httpd, NVS, the panel and the
 * application singleton — and cannot measure an Xtensa frame. What it can do is
 * read the source back and refuse the shape that caused the fault, which is the
 * same thing test_welcome_screen.cc does to dashboard_renderer.cc's copy: a
 * structural claim, checked against the bytes on disk rather than against a
 * memory of having fixed it once. That is what the last four tests here are.
 */

#include <cstdio>
#include <cstring>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

size_t RenderStatusCapabilities(char* out, size_t out_len);
int TargetAutonomyCompiled();
int TargetVoicePttEnabled();
size_t StatusCapabilityBufferBytes();

// ------------------------------------------------------------ mini harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current_test = "";

static void Check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", g_current_test, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define RUN(fn)                                \
    do {                                       \
        g_current_test = #fn;                  \
        const int before = g_failures;         \
        fn();                                  \
        std::printf("%-64s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

/// The bytes the tower parses, on a device built the way the lot-2 hardware
/// image was built. Written out in full rather than assembled from the pieces,
/// because a test that builds the expected string the same way the code does
/// agrees with the code by construction.
static const char kExpected[] =
    "[\"dashboard.frame.v1\",\"dashboard.refresh.v1\",\"dashboard.pair.v1\","
    "\"config.v2\",\"action.restart\",\"action.sleep\",\"power.hybrid.v1\","
    "\"voice.hub.v1\",\"autonomy.profile.v1\"]";

// ------------------------------------------------------------- the wiring --

/**
 * The claim the whole feature's discoverability rests on: a build whose Kconfig
 * says CONFIG_AUTONOMY_ENABLED=y announces autonomy.profile.v1.
 *
 * This fails if the gate silently reads 0 — which is what happens when
 * dashboard_build_config.h is reached before whatever else in the translation
 * unit happened to pull sdkconfig.h in.
 */
static void test_a_kconfig_enabled_build_announces_autonomy() {
    char caps[512] = {};
    const size_t len = RenderStatusCapabilities(caps, sizeof(caps));
    CHECK(len > 0);
    CHECK(std::strstr(caps, "\"autonomy.profile.v1\"") != nullptr);
    if (std::strstr(caps, "\"autonomy.profile.v1\"") == nullptr) {
        std::printf("      AUTONOMY_COMPILED resolved to %d; list was %s\n",
                    TargetAutonomyCompiled(), caps);
    }
}

/**
 * The gate itself, reported separately so a failure above says which of the two
 * steps broke: the macro, or the renderer it is passed to.
 */
static void test_the_build_config_header_obtains_its_own_kconfig() {
    CHECK(TargetAutonomyCompiled() == 1);
}

/// And the exact bytes, so a reordering or a renamed capability is a failure
/// here rather than a surprise in the tower.
static void test_the_status_list_is_the_one_the_tower_parses() {
    char caps[512] = {};
    RenderStatusCapabilities(caps, sizeof(caps));
    CHECK(std::strcmp(caps, kExpected) == 0);
}

/**
 * The route gives the renderer 256 bytes and falls back to `[]` when it does
 * not fit. `[]` parses, so an overflow would not fail the tower's read — it
 * would take every capability away at once and look exactly like a device that
 * supports nothing. The margin is checked rather than assumed.
 */
static void test_the_capability_list_fits_the_buffer_the_route_gives_it() {
    char caps[512] = {};
    const size_t len = RenderStatusCapabilities(caps, sizeof(caps));
    CHECK(std::strcmp(caps, "[]") != 0);
    CHECK(len + 1 <= StatusCapabilityBufferBytes());
    CHECK(len == std::strlen(kExpected));
}

/**
 * Turning autonomy on must not announce the microphone. The two gates answer to
 * different decisions, and the capability list is where a tower would find out
 * if they had been wired together.
 */
static void test_enabling_autonomy_does_not_announce_the_microphone() {
    char caps[512] = {};
    RenderStatusCapabilities(caps, sizeof(caps));
    CHECK(TargetVoicePttEnabled() == 0);
    CHECK(std::strstr(caps, "voice.ptt.v1") == nullptr);
    // voice.hub.v1 is a different thing and is present either way: it is where
    // audio would be *sent*, not whether this device can capture any.
    CHECK(std::strstr(caps, "\"voice.hub.v1\"") != nullptr);
}

// ------------------------------------------- the route's memory contract --

static const char* const kRoutePath = "main/ui/renderers/rawdraw/dashboard_api.cc";

/// The whole route file, or an empty string when it could not be opened. Run
/// from the firmware root; see tests/host/run.sh.
static std::string RouteSource() {
    std::ifstream in(kRoutePath);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * The *code* between the braces of a definition beginning with @p signature:
 * brace-matched rather than line-counted, with comments blanked out and string
 * literals kept.
 *
 * All three of those matter here. The route's format string is made almost
 * entirely of braces, so a naive matcher ends the function in the middle of it.
 * The comments are full of apostrophes — "the guard's scope" — so a matcher
 * that understands character literals but not comments runs off the end of the
 * function and reports the next handler's buffers as this one's. And the
 * literals are kept because two of the assertions below look for `"no_memory"`
 * and `"503 Service Unavailable"`.
 */
static std::string FunctionBodyOf(const std::string& src, const std::string& signature,
                                  bool* found) {
    *found = false;
    const size_t at = src.find(signature);
    if (at == std::string::npos) return std::string();
    const size_t open = src.find('{', at + signature.size() - 1);
    if (open == std::string::npos) return std::string();

    std::string body;
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            body.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            ++i;
            body.push_back(' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            body.push_back(c);
            for (++i; i < src.size() && src[i] != quote; ++i) {
                body.push_back(src[i]);
                if (src[i] == '\\' && i + 1 < src.size()) {
                    body.push_back(src[++i]);
                }
            }
            body.push_back(quote);
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}' && depth - 1 == 0) {
            *found = true;
            return body.substr(1);          // drop the opening brace
        }
        if (c == '}') --depth;
        body.push_back(c);
    }
    return std::string();
}

/// The first integer assigned to `constexpr size_t <name>`, or 0 when absent.
static size_t ConstexprSizeIn(const std::string& src, const std::string& name) {
    const std::regex re("constexpr\\s+size_t\\s+" + name + "\\s*=\\s*([0-9]+)\\s*;");
    std::smatch m;
    if (!std::regex_search(src, m, re)) return 0;
    return static_cast<size_t>(std::stoul(m[1].str()));
}

/// True when @p body declares an array of bytes — `char buf[N]`, `uint8_t
/// buf[kSomething]` — which on this route means an array on the httpd task's
/// stack. The match deliberately does not care how large the bound is: the two
/// functions swept below have no business declaring one at all.
static bool DeclaresAByteArray(const std::string& body, std::string* what) {
    const std::regex re(
        "\\b(?:unsigned\\s+char|signed\\s+char|char|u?int8_t)\\s+\\w+\\s*\\[");
    std::smatch m;
    if (!std::regex_search(body, m, re)) return false;
    *what = m.str();
    return true;
}

/**
 * NOTHING THE STATUS ROUTE WRITES INTO LIVES ON THE STACK.
 *
 * This is the regression test for the crash. `char caps[256]`,
 * `char power_json[640]`, `char autonomy_json[768]` and `char buf[2880]` were
 * locals of StatusHandler; together they were what pushed the frame past the
 * task's limit and corrupted the log mutex's TCB on the way out.
 *
 * Both halves of the route are swept, because moving the buffers into a helper
 * that is called from the same task would not have fixed anything — it is the
 * same stack.
 */
static void test_the_status_response_is_not_assembled_on_the_stack() {
    const std::string src = RouteSource();
    CHECK(!src.empty());

    static const char* const kFunctions[] = {
        "esp_err_t StatusHandler(httpd_req_t* req)",
        "void RenderStatusJson(StatusScratch& scratch)",
    };
    for (const char* sig : kFunctions) {
        bool found = false;
        const std::string body = FunctionBodyOf(src, sig, &found);
        CHECK(found);
        if (!found) {
            std::printf("      could not find the body of: %s\n", sig);
            continue;
        }
        std::string what;
        const bool array = DeclaresAByteArray(body, &what);
        if (array) {
            std::printf("      %s declares \"%s\" on the stack\n", sig, what.c_str());
        }
        CHECK(!array);
    }
}

/**
 * And the block that replaced them is not itself a local.
 *
 * `StatusScratch scratch;` would compile, would read identically, and would put
 * all 4.5 kB straight back on the frame. Every mention of the type in the
 * handler must therefore be a pointer or a `sizeof`.
 */
static void test_the_scratch_block_is_only_ever_reached_through_a_pointer() {
    const std::string src = RouteSource();
    bool found = false;
    const std::string body =
        FunctionBodyOf(src, "esp_err_t StatusHandler(httpd_req_t* req)", &found);
    CHECK(found);

    bool by_pointer = true;
    const std::regex re("\\bStatusScratch(Guard)?\\b\\s*(.)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it) {
        if ((*it)[1].matched) continue;            // StatusScratchGuard
        const std::string tail = (*it)[2].str();
        if (tail.empty()) continue;
        const char next = tail[0];
        // A pointer, a reference, or the closing paren of a sizeof.
        if (next == '*' || next == ')' || next == '&') continue;
        std::printf("      StatusScratch used as \"%s\"\n", it->str().c_str());
        by_pointer = false;
    }
    CHECK(by_pointer);
}

/**
 * The block comes from the heap and is freed however the handler leaves.
 *
 * The free is the destructor of a guard rather than a statement before each
 * `return`, so the claim checked here is that the guard exists, that the
 * handler constructs one, and that its destructor is the thing that frees.
 */
static void test_the_scratch_block_is_heap_allocated_and_always_freed() {
    const std::string src = RouteSource();
    bool found = false;
    const std::string body =
        FunctionBodyOf(src, "esp_err_t StatusHandler(httpd_req_t* req)", &found);
    CHECK(found);
    CHECK(body.find("heap_caps_malloc(sizeof(StatusScratch)") != std::string::npos);
    CHECK(body.find("StatusScratchGuard guard(scratch)") != std::string::npos);

    bool guard_found = false;
    const std::string guard =
        FunctionBodyOf(src, "class StatusScratchGuard", &guard_found);
    CHECK(guard_found);
    CHECK(guard.find("~StatusScratchGuard() { if (p_ != nullptr) heap_caps_free(p_); }")
          != std::string::npos);
}

/**
 * A failed allocation is answered, not crashed and not returned empty.
 *
 * 503 with a bounded JSON body, from SendError's own 256-byte frame. A handler
 * that dereferenced the null instead would turn a transient heap shortage into
 * the same reset the big frame caused.
 */
static void test_a_failed_status_allocation_is_answered_rather_than_crashing() {
    const std::string src = RouteSource();
    bool found = false;
    const std::string body =
        FunctionBodyOf(src, "esp_err_t StatusHandler(httpd_req_t* req)", &found);
    CHECK(found);

    const size_t refusal = body.find("\"no_memory\"");
    const size_t use = body.find("RenderStatusJson(");
    CHECK(refusal != std::string::npos);
    CHECK(use != std::string::npos);
    if (refusal == std::string::npos || use == std::string::npos) return;

    // The refusal happens before anything reads the block, and it is a 503.
    CHECK(refusal < use);
    CHECK(body.rfind("503 Service Unavailable", refusal) != std::string::npos);

    // And it is *this* allocation's null check that leads to it: the nearest
    // preceding test of the pointer, with no further allocation attempt and no
    // use of the block in between. Checking only that a null test appears
    // somewhere above would be satisfied by the PSRAM-to-internal fallback
    // check, and would still pass with the real guard deleted.
    const size_t checked = body.rfind("if (scratch == nullptr)", refusal);
    CHECK(checked != std::string::npos);
    if (checked == std::string::npos) return;
    const std::string between = body.substr(checked, refusal - checked);
    CHECK(between.find("heap_caps_malloc") == std::string::npos);
    CHECK(between.find("RenderStatusJson") == std::string::npos);
}

/**
 * The capability buffer is still the one status_caps_gate.cc reproduces.
 *
 * That fixture hardcodes 256 so it can reproduce the `[]` fallback the route
 * would really take. Moving the buffer into StatusScratch turned the size into
 * a named constant, which is exactly the kind of change that can drift away
 * from a fixture nobody re-reads. Read back out of the route and compared.
 */
static void test_the_route_still_gives_the_renderer_the_buffer_the_fixture_copies() {
    const std::string src = RouteSource();
    const size_t caps_max = ConstexprSizeIn(src, "kStatusCapsMax");
    CHECK(caps_max == StatusCapabilityBufferBytes());
    if (caps_max != StatusCapabilityBufferBytes()) {
        std::printf("      route declares %zu, fixture reproduces %zu\n",
                    caps_max, StatusCapabilityBufferBytes());
    }
    // And the response buffer is still large enough for the blocks that go in
    // it: a truncated status document is unparseable JSON.
    CHECK(ConstexprSizeIn(src, "kStatusBodyMax") >= 2880);
}

int main() {
    std::printf("status capability wiring host tests "
                "(real dashboard_build_config.h + real device_config.cc)\n\n");

    RUN(test_a_kconfig_enabled_build_announces_autonomy);
    RUN(test_the_build_config_header_obtains_its_own_kconfig);
    RUN(test_the_status_list_is_the_one_the_tower_parses);
    RUN(test_the_capability_list_fits_the_buffer_the_route_gives_it);
    RUN(test_enabling_autonomy_does_not_announce_the_microphone);
    RUN(test_the_status_response_is_not_assembled_on_the_stack);
    RUN(test_the_scratch_block_is_only_ever_reached_through_a_pointer);
    RUN(test_the_scratch_block_is_heap_allocated_and_always_freed);
    RUN(test_a_failed_status_allocation_is_answered_rather_than_crashing);
    RUN(test_the_route_still_gives_the_renderer_the_buffer_the_fixture_copies);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
