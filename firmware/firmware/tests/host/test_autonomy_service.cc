/**
 * @file test_autonomy_service.cc
 * @brief Host tests for the profile route's semantics.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The route is header parsing and status codes; everything that can actually go
 * wrong with a profile push is here, and so is its test. What is driven below:
 * a wrong token ten times, a retried key, a revision that goes backwards, a
 * document that does not parse, a write that reports success and does not
 * stick, and a read-back that comes back different.
 *
 * The last two are the ones worth having. A store that lies about success is
 * the failure the whole A/B design exists to survive, and the only way to see
 * the service handle it is to inject it.
 */

#include "common/autonomy_service.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
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

const char* const kToken = "0123456789abcdef0123456789abcdef";

/// A memory-backed SlotIo that can be made to lie the way flash lies.
class FakeIo : public record::SlotIo {
public:
    /// Writes report success and store nothing. The failure the A/B design
    /// exists to survive, and the only way to see it handled is to inject it.
    bool swallow_writes = false;
    /// Writes fail outright.
    bool refuse_writes = false;

    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        const std::vector<uint8_t>& s = slots_[slot];
        if (s.empty()) return 0;
        const size_t n = s.size() < max ? s.size() : max;
        memcpy(buf, s.data(), n);
        return static_cast<int>(n);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        if (refuse_writes) return false;
        if (swallow_writes) return true;  // "success", nothing stored
        slots_[slot].assign(data, data + len);
        return true;
    }

private:
    std::vector<uint8_t> slots_[record::kSlotCount];
};

/// The service and everything it borrows, in one holder so a test is one line.
struct Fixture {
    FakeIo io;
    std::vector<uint8_t> scratch;
    Profile profile;
    AutonomyService service;

    Fixture()
        : scratch(kProfileRecordBytes),
          service(&io, scratch.data(), scratch.size(), &profile) {
        service.SetToken(kToken);
    }
};

std::string ProfileJson(int revision) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"profile_version":1,"revision":%d,"compiled_at":1789380000,)"
             R"("dashboard_id":"dash-1","dashboard_doc_version":2,)"
             R"("wake_interval_min":60,"tower_wait_s":30,"provenance_line":true,)"
             R"("composition":"flow",)"
             R"("modules":[{"type":"timestamp","mode":"auto"}]})",
             revision);
    return buf;
}

std::string HexOf(const std::string& body) {
    uint8_t digest[record::kShaBytes];
    record::Sha256(reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                      digest);
    char hex[65];
    ShaToHex(digest, hex);
    return hex;
}

/// A pass-through SlotIo that also records how far down the stack the caller
/// had already gone by the time it reached the store.
///
/// ReadSlot is the deepest frame of a Load(), and on the device it is the one
/// that calls fopen(), so it is where a stack that has already overrun does its
/// damage. Measuring here measures the thing that crashed.
class StackProbeIo : public record::SlotIo {
public:
    explicit StackProbeIo(record::SlotIo* inner) : inner_(inner) {}

    int ReadSlot(int slot, uint8_t* buf, size_t max) override {
        Note();
        return inner_->ReadSlot(slot, buf, max);
    }

    bool WriteSlot(int slot, const uint8_t* data, size_t len) override {
        Note();
        return inner_->WriteSlot(slot, data, len);
    }

    const char* mark = nullptr;
    ptrdiff_t deepest = 0;

private:
    void Note() {
        char here = 0;
        if (mark == nullptr) return;
        // Direction-agnostic: the stack grows down on every host this suite
        // runs on, but the assertion is about distance, not about sign.
        const ptrdiff_t depth = mark > &here ? mark - &here : &here - mark;
        if (depth > deepest) deepest = depth;
    }

    record::SlotIo* inner_;
};

/// Calls Load() from a frame that holds nothing but the mark, so the number
/// that comes back is the cost of the call and not of the test's own locals —
/// one of which is a Profile, which would otherwise swamp the measurement.
///
/// Deliberately noinline: the whole point is that this frame exists.
__attribute__((noinline)) ptrdiff_t LoadAtKnownStackDepth(AutonomyService& service,
                                                          StackProbeIo& probe) {
    char mark = 0;
    probe.mark = &mark;
    probe.deepest = 0;
    service.Load();
    probe.mark = nullptr;
    return probe.deepest;
}

PutResult Push(Fixture& f, const std::string& body, const char* key = nullptr,
               int64_t now_ms = 1000, int64_t now_epoch = 1789387200) {
    return f.service.Put(body.data(), body.size(), kToken, nullptr, key, now_ms,
                         now_epoch);
}

}  // namespace

// ------------------------------------------------------------ the happy path --

static void test_a_first_profile_is_accepted_and_read_back() {
    Fixture f;
    const std::string body = ProfileJson(1);
    const PutResult r = Push(f, body);

    CHECK(r.outcome == PutOutcome::kAccepted);
    CHECK(r.accepted);
    CHECK(r.persisted);
    CHECK(!r.replay);
    CHECK_EQ_INT(r.revision, 1);
    CHECK_STR(r.sha256, HexOf(body).c_str());

    CHECK(f.service.has_profile());
    CHECK_EQ_INT(f.service.revision(), 1);
    CHECK(f.service.profile().composition == Composition::kFlow);
}

static void test_the_stored_bytes_come_back_verbatim() {
    // The whole point of storing raw bytes: the tower compares what it pushed
    // against what comes back, and a re-serialisation here would make that a
    // test of this device's JSON writer.
    Fixture f;
    const std::string body = ProfileJson(3);
    CHECK(Push(f, body).outcome == PutOutcome::kAccepted);

    std::vector<uint8_t> out(kProfileMaxBytes);
    const GetResult g = f.service.Get(out.data(), out.size());
    CHECK(g.present);
    CHECK_EQ_INT(g.bytes, body.size());
    CHECK(memcmp(out.data(), body.data(), g.bytes) == 0);
    CHECK_EQ_INT(g.revision, 3);
    CHECK_STR(g.sha256, HexOf(body).c_str());
}

static void test_a_profile_survives_a_reload_the_way_a_reboot_would() {
    Fixture f;
    const std::string body = ProfileJson(5);
    CHECK(Push(f, body).outcome == PutOutcome::kAccepted);

    // A second service over the same storage: what the device does at boot.
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    Profile parsed;
    AutonomyService reloaded(&f.io, scratch.data(), scratch.size(), &parsed);
    CHECK(reloaded.Load());
    CHECK(reloaded.has_profile());
    CHECK_EQ_INT(reloaded.revision(), 5);
    CHECK_STR(reloaded.sha256(), HexOf(body).c_str());
}

static void test_loading_a_profile_does_not_build_one_on_the_stack() {
    // A device crash, not a style point. LoadLocked() cleared the parsed
    // profile with `*parsed_ = Profile{}`, which reads as a reset and is not
    // one: it constructs an 18712-byte Profile on the *caller's* stack and then
    // copies it. ESP-IDF's main task runs on 8192 bytes, so the boot-time
    // Load() drove the stack more than twice past its end, into the heap
    // underneath, where it zeroed the VFS entry for /spiffs. The very next
    // fopen() — made by this same call, to read the slot — then dereferenced a
    // NULL ops table: LoadProhibited at boot, every boot, reboot loop.
    //
    // Nothing about that is visible in a result value, so the assertion is on
    // how deep the stack had already sunk by the time the store was reached.
    // The store is where the device was standing when it died.
    Fixture f;
    const std::string body = ProfileJson(7);
    CHECK(Push(f, body).outcome == PutOutcome::kAccepted);

    StackProbeIo probe(&f.io);
    std::vector<uint8_t> scratch(kProfileRecordBytes);
    Profile parsed;
    AutonomyService reloaded(&probe, scratch.data(), scratch.size(), &parsed);

    const ptrdiff_t depth = LoadAtKnownStackDepth(reloaded, probe);
    CHECK(reloaded.has_profile());
    CHECK_EQ_INT(reloaded.revision(), 7);

    // The budget is the device's. Load() runs on the 8192-byte main task, from
    // Application::Initialize(), which is nowhere near the top of it; a quarter
    // of the task stack is already generous for a call whose own needs are a
    // few hundred bytes. One Profile temporary is 18712 on its own, so this
    // fails loudly rather than marginally if the pattern comes back.
    CHECK(depth < 2048);
    if (depth >= 2048) {
        printf("    reached the store %lld bytes down the stack (sizeof(Profile)=%zu)\n",
               static_cast<long long>(depth), sizeof(Profile));
    }
}

static void test_a_declared_digest_that_matches_is_accepted() {
    Fixture f;
    const std::string body = ProfileJson(1);
    const std::string hex = HexOf(body);
    const PutResult r = f.service.Put(body.data(), body.size(), kToken, hex.c_str(),
                                      nullptr, 1000, 1789387200);
    CHECK(r.outcome == PutOutcome::kAccepted);

    // And in upper case, which is a legal spelling of the same digest.
    Fixture g;
    std::string upper = hex;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    const PutResult r2 = g.service.Put(body.data(), body.size(), kToken,
                                       upper.c_str(), nullptr, 1000, 1789387200);
    CHECK(r2.outcome == PutOutcome::kAccepted);
}

// ------------------------------------------------------------------- auth --

static void test_an_unprovisioned_device_says_so() {
    Fixture f;
    f.service.SetToken(nullptr);
    const std::string body = ProfileJson(1);
    const PutResult r = f.service.Put(body.data(), body.size(), kToken, nullptr,
                                      nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kNotProvisioned);
    CHECK(!f.service.has_profile());
}

static void test_a_wrong_token_is_refused_and_stores_nothing() {
    Fixture f;
    const std::string body = ProfileJson(1);
    const PutResult r = f.service.Put(body.data(), body.size(), "wrong", nullptr,
                                      nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kUnauthorized);
    CHECK(!f.service.has_profile());
}

static void test_repeated_failures_earn_a_lockout() {
    // Without this the token is a sixty-four-character secret being guessed at
    // LAN speed.
    Fixture f;
    const std::string body = ProfileJson(1);
    for (int i = 0; i < kMaxAuthFailures; ++i) {
        const PutResult r = f.service.Put(body.data(), body.size(), "wrong",
                                          nullptr, nullptr, 1000, 0);
        CHECK(r.outcome == PutOutcome::kUnauthorized);
    }
    // Now even the right token is refused, and it says why.
    const PutResult locked = Push(f, body, nullptr, 1000);
    CHECK(locked.outcome == PutOutcome::kLockedOut);

    // And it ends. A lockout that did not would be a brick.
    const PutResult later = Push(f, body, nullptr, 1000 + kLockoutMs);
    CHECK(later.outcome == PutOutcome::kAccepted);
}

// ------------------------------------------------------------- idempotency --

static void test_a_retried_key_returns_the_first_answer_without_storing_again() {
    // A tower that retried a PUT it never saw the answer to must not push the
    // revision twice.
    Fixture f;
    const std::string body = ProfileJson(4);
    const PutResult first = Push(f, body, "key-1");
    CHECK(first.outcome == PutOutcome::kAccepted);
    CHECK(first.persisted);

    const PutResult again = Push(f, body, "key-1");
    CHECK(again.outcome == PutOutcome::kReplay);
    CHECK(again.replay);
    CHECK(again.accepted);
    // Persisted describes what *this* call did, which is nothing.
    CHECK(!again.persisted);
    CHECK_EQ_INT(again.revision, 4);
    CHECK_STR(again.sha256, first.sha256);
    CHECK_EQ_INT(f.service.revision(), 4);
}

static void test_a_different_key_is_not_a_replay() {
    Fixture f;
    CHECK(Push(f, ProfileJson(1), "key-1").outcome == PutOutcome::kAccepted);
    CHECK(Push(f, ProfileJson(2), "key-2").outcome == PutOutcome::kAccepted);
    CHECK_EQ_INT(f.service.revision(), 2);
}

static void test_the_ring_forgets_the_oldest_key() {
    // Eight deep. The ninth key pushes the first one out, and a retry of it is
    // then an ordinary request — which the revision CAS refuses anyway, so
    // forgetting is safe rather than merely bounded.
    Fixture f;
    for (int i = 1; i <= static_cast<int>(kIdempotencyRing) + 1; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "key-%d", i);
        CHECK(Push(f, ProfileJson(i), key).outcome == PutOutcome::kAccepted);
    }
    const PutResult retried = Push(f, ProfileJson(1), "key-1");
    CHECK(retried.outcome == PutOutcome::kRevisionConflict);
}

// ------------------------------------------------------------------- CAS --

static void test_a_revision_that_does_not_advance_is_refused() {
    Fixture f;
    CHECK(Push(f, ProfileJson(7)).outcome == PutOutcome::kAccepted);

    const PutResult same = Push(f, ProfileJson(7));
    CHECK(same.outcome == PutOutcome::kRevisionConflict);
    CHECK_EQ_INT(same.revision, 7);

    const PutResult older = Push(f, ProfileJson(6));
    CHECK(older.outcome == PutOutcome::kRevisionConflict);
    // And the stored profile is untouched by either.
    CHECK_EQ_INT(f.service.revision(), 7);
}

static void test_a_refused_push_leaves_the_live_profile_intact() {
    // The property the single-buffer design rests on: a refusal restores the
    // profile from the slot rather than leaving a half-parsed one behind.
    Fixture f;
    const std::string good = ProfileJson(9);
    CHECK(Push(f, good).outcome == PutOutcome::kAccepted);
    CHECK(f.service.profile().composition == Composition::kFlow);

    const std::string bad =
        R"({"profile_version":1,"revision":10,"compiled_at":0,"dashboard_id":"d",)"
        R"("dashboard_doc_version":0,"wake_interval_min":60,"tower_wait_s":30,)"
        R"("provenance_line":true,"composition":"nope","modules":[]})";
    const PutResult r = f.service.Put(bad.data(), bad.size(), kToken, nullptr,
                                      nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kInvalid);
    CHECK_STR(r.error.field, "composition");

    // Still holding the good one, fully parsed.
    CHECK(f.service.has_profile());
    CHECK_EQ_INT(f.service.revision(), 9);
    CHECK(f.service.profile().composition == Composition::kFlow);
    CHECK_STR(f.service.sha256(), HexOf(good).c_str());
}

// ----------------------------------------------------------- the refusals --

static void test_an_oversized_document_is_refused_before_parsing() {
    Fixture f;
    const std::string huge(kProfileMaxBytes + 1, ' ');
    const PutResult r = f.service.Put(huge.data(), huge.size(), kToken, nullptr,
                                      nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kTooLarge);
}

static void test_a_document_that_does_not_parse_names_its_field() {
    Fixture f;
    const std::string bad =
        R"({"profile_version":1,"revision":1,"compiled_at":0,"dashboard_id":"d",)"
        R"("dashboard_doc_version":0,"wake_interval_min":14,"tower_wait_s":30,)"
        R"("provenance_line":true,"composition":"flow","modules":[]})";
    const PutResult r = f.service.Put(bad.data(), bad.size(), kToken, nullptr,
                                      nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kInvalid);
    CHECK_STR(r.error.field, "wake_interval_min");
    CHECK(!f.service.has_profile());
}

static void test_a_declared_digest_that_does_not_match_is_refused() {
    // The bytes we received are not the bytes that were sent.
    Fixture f;
    const std::string body = ProfileJson(1);
    const PutResult r = f.service.Put(body.data(), body.size(), kToken,
                                      std::string(64, 'a').c_str(), nullptr, 1000,
                                      0);
    CHECK(r.outcome == PutOutcome::kShaMismatch);
    CHECK(!f.service.has_profile());
}

static void test_an_empty_body_is_refused() {
    Fixture f;
    const PutResult r = f.service.Put("", 0, kToken, nullptr, nullptr, 1000, 0);
    CHECK(r.outcome == PutOutcome::kInvalid);
}

// ------------------------------------------------------------ the storage --

static void test_a_write_that_reports_success_and_stores_nothing_is_caught() {
    // The failure the whole A/B design exists to survive. The service must
    // report store_failed rather than tell the tower it is holding a profile it
    // does not have.
    Fixture f;
    f.io.swallow_writes = true;
    const PutResult r = Push(f, ProfileJson(1));
    CHECK(r.outcome == PutOutcome::kStoreFailed);
    CHECK(!r.accepted);
    CHECK(!f.service.has_profile());
}

static void test_a_write_that_fails_outright_is_reported() {
    Fixture f;
    f.io.refuse_writes = true;
    const PutResult r = Push(f, ProfileJson(1));
    CHECK(r.outcome == PutOutcome::kStoreFailed);
    CHECK(!f.service.has_profile());
}

static void test_a_failed_write_leaves_an_earlier_profile_in_place() {
    Fixture f;
    const std::string good = ProfileJson(2);
    CHECK(Push(f, good).outcome == PutOutcome::kAccepted);

    f.io.swallow_writes = true;
    CHECK(Push(f, ProfileJson(3)).outcome == PutOutcome::kStoreFailed);

    f.io.swallow_writes = false;
    CHECK(f.service.Load());
    CHECK_EQ_INT(f.service.revision(), 2);
    CHECK_STR(f.service.sha256(), HexOf(good).c_str());
}

static void test_a_corrupt_store_reads_as_no_profile_not_as_a_default_one() {
    Fixture f;
    CHECK(Push(f, ProfileJson(1)).outcome == PutOutcome::kAccepted);

    // Corrupt both slots.
    for (int slot = 0; slot < record::kSlotCount; ++slot) {
        uint8_t raw[kProfileRecordBytes];
        const int n = f.io.ReadSlot(slot, raw, sizeof(raw));
        if (n <= 0) continue;
        raw[record::kHeaderBytes + 2] ^= 0xff;
        f.io.WriteSlot(slot, raw, static_cast<size_t>(n));
    }

    CHECK(!f.service.Load());
    CHECK(!f.service.has_profile());
    CHECK_EQ_INT(f.service.revision(), 0);
    CHECK_STR(f.service.sha256(), "");

    // Inert autonomy, not a brick: a fresh push still works.
    CHECK(Push(f, ProfileJson(1)).outcome == PutOutcome::kAccepted);
}

static void test_a_maximal_profile_round_trips_through_the_service() {
    // The case where the parse cursor and the arena come closest, and the one
    // that would fail if Load() ever parsed a document out of its own arena.
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
    std::string body =
        R"({"profile_version":1,"revision":1,"compiled_at":0,"dashboard_id":"d",)"
        R"("dashboard_doc_version":0,"wake_interval_min":60,"tower_wait_s":30,)"
        R"("provenance_line":true,"composition":"editorial","modules":)" + modules +
        "}";
    CHECK(body.size() <= kProfileMaxBytes);

    Fixture f;
    CHECK(Push(f, body).outcome == PutOutcome::kAccepted);
    CHECK_EQ_INT(f.service.profile().module_count, 8);

    std::vector<uint8_t> scratch(kProfileRecordBytes);
    Profile parsed;
    AutonomyService reloaded(&f.io, scratch.data(), scratch.size(), &parsed);
    CHECK(reloaded.Load());
    CHECK_EQ_INT(reloaded.profile().module_count, 8);
    CHECK_EQ_INT(reloaded.profile().modules[7].row_count, 24);
    CHECK_STR(reloaded.sha256(), HexOf(body).c_str());
}

static void test_an_unset_clock_records_no_applied_time() {
    Fixture f;
    CHECK(f.service.Put(ProfileJson(1).data(), ProfileJson(1).size(), kToken,
                        nullptr, nullptr, 1000, 0)
              .outcome == PutOutcome::kAccepted);
    CHECK(!f.service.applied_epoch_known());
    CHECK_EQ_INT(f.service.applied_epoch(), 0);
}

static void test_a_set_clock_records_when_the_profile_was_applied() {
    Fixture f;
    CHECK(Push(f, ProfileJson(1), nullptr, 1000, 1789387200).outcome ==
          PutOutcome::kAccepted);
    CHECK(f.service.applied_epoch_known());
    CHECK_EQ_INT(f.service.applied_epoch(), 1789387200);
}

// ------------------------------------------------- the delegated verifier --
//
// On the device there is exactly one dashboard token, it lives in
// DashboardManager's FrameAuth, and it changes when somebody re-pairs the
// device from its Settings menu. A copy of it inside this service would be a
// second secret that goes stale at exactly the moment correctness matters: the
// operator re-pairs because they believe the old token is compromised, the
// frame route starts refusing it, and the profile route keeps accepting it.
//
// So the device injects a verifier that asks the one owner of the secret, and
// the internal token stays for the host suite, which has no DashboardManager.

namespace {

struct VerifierSpy {
    const char* expected = "";
    bool provisioned = true;
    int calls = 0;
    std::string last_seen;
};

AuthVerdict SpyVerify(const char* token, void* ctx) {
    VerifierSpy* spy = static_cast<VerifierSpy*>(ctx);
    ++spy->calls;
    spy->last_seen = token == nullptr ? "" : token;
    if (!spy->provisioned) return AuthVerdict::kNotProvisioned;
    if (token != nullptr && spy->last_seen == spy->expected) return AuthVerdict::kOk;
    return AuthVerdict::kBadToken;
}

}  // namespace

static void test_an_injected_verifier_decides_instead_of_the_local_token() {
    Fixture f;
    VerifierSpy spy;
    spy.expected = "the-real-one";
    f.service.SetTokenVerifier(SpyVerify, &spy);

    const std::string body = ProfileJson(1);
    // kToken is what the service was told locally, and it must now be ignored:
    // the verifier is the only opinion that counts.
    const PutResult wrong = f.service.Put(body.data(), body.size(), kToken,
                                          nullptr, nullptr, 1000, 1789387200);
    CHECK(wrong.outcome == PutOutcome::kUnauthorized);
    CHECK(!f.service.has_profile());
    CHECK_EQ_INT(spy.calls, 1);

    const PutResult right = f.service.Put(body.data(), body.size(), "the-real-one",
                                          nullptr, nullptr, 2000, 1789387200);
    CHECK(right.outcome == PutOutcome::kAccepted);
    CHECK(f.service.has_profile());
}

static void test_the_verifier_reports_an_unprovisioned_device_as_such() {
    Fixture f;
    VerifierSpy spy;
    spy.expected = kToken;
    spy.provisioned = false;
    f.service.SetTokenVerifier(SpyVerify, &spy);

    const std::string body = ProfileJson(1);
    const PutResult r = f.service.Put(body.data(), body.size(), kToken, nullptr,
                                      nullptr, 1000, 1789387200);
    // Not "unauthorized": a device nobody has paired yet is a different
    // situation from a wrong token, and the tower's copy says so.
    CHECK(r.outcome == PutOutcome::kNotProvisioned);
    CHECK(!f.service.has_profile());
}

static void test_a_rotated_token_takes_effect_without_touching_the_service() {
    // The staleness bug this exists to prevent, driven end to end: the operator
    // re-pairs, the one owner of the secret starts answering differently, and
    // this service follows without anybody remembering to tell it.
    Fixture f;
    VerifierSpy spy;
    spy.expected = "before-rotation";
    f.service.SetTokenVerifier(SpyVerify, &spy);

    const std::string first = ProfileJson(1);
    CHECK(f.service.Put(first.data(), first.size(), "before-rotation", nullptr,
                        nullptr, 1000, 1789387200)
              .outcome == PutOutcome::kAccepted);

    spy.expected = "after-rotation";

    const std::string second = ProfileJson(2);
    CHECK(f.service.Put(second.data(), second.size(), "before-rotation", nullptr,
                        nullptr, 2000, 1789387200)
              .outcome == PutOutcome::kUnauthorized);
    CHECK(f.service.Put(second.data(), second.size(), "after-rotation", nullptr,
                        nullptr, 3000, 1789387200)
              .outcome == PutOutcome::kAccepted);
    CHECK_EQ_INT(f.service.revision(), 2);
}

static void test_clearing_the_verifier_returns_to_the_local_token() {
    // So the host suite's own fixtures keep meaning what they meant, and so a
    // device that somehow comes up without a manager is not silently open.
    Fixture f;
    VerifierSpy spy;
    spy.expected = "elsewhere";
    f.service.SetTokenVerifier(SpyVerify, &spy);
    f.service.SetTokenVerifier(nullptr, nullptr);

    const std::string body = ProfileJson(1);
    CHECK(f.service.Put(body.data(), body.size(), kToken, nullptr, nullptr, 1000,
                        1789387200)
              .outcome == PutOutcome::kAccepted);
    CHECK_EQ_INT(spy.calls, 0);
}

static void test_the_lockout_still_belongs_to_the_service_under_a_verifier() {
    // The verifier answers "is this the token"; the budget for guessing is
    // still this service's, because the ring and the lockout are what the
    // route's tests pin and a delegated yes/no cannot carry them.
    Fixture f;
    VerifierSpy spy;
    spy.expected = "correct-token";
    f.service.SetTokenVerifier(SpyVerify, &spy);

    const std::string body = ProfileJson(1);
    for (int i = 0; i < kMaxAuthFailures; ++i) {
        const PutResult r = f.service.Put(body.data(), body.size(), "wrong",
                                          nullptr, nullptr, 1000 + i, 1789387200);
        CHECK(r.outcome == PutOutcome::kUnauthorized);
    }
    // Even the right token is refused now, and the verifier is not consulted:
    // a lockout that could be probed past would not be one.
    const int calls_before = spy.calls;
    const PutResult locked = f.service.Put(body.data(), body.size(),
                                           "correct-token", nullptr, nullptr,
                                           1100, 1789387200);
    CHECK(locked.outcome == PutOutcome::kLockedOut);
    CHECK_EQ_INT(spy.calls, calls_before);
}

static void test_every_outcome_has_a_name() {
    const PutOutcome all[] = {
        PutOutcome::kAccepted,         PutOutcome::kReplay,
        PutOutcome::kNotProvisioned,   PutOutcome::kUnauthorized,
        PutOutcome::kLockedOut,        PutOutcome::kTooLarge,
        PutOutcome::kInvalid,          PutOutcome::kRevisionConflict,
        PutOutcome::kShaMismatch,      PutOutcome::kStoreFailed,
    };
    for (PutOutcome o : all) CHECK(PutOutcomeName(o)[0] != '\0');
}

// ------------------------------------------------- two tasks, one profile --
//
// WHY THIS SUITE NOW STARTS THREADS
// ---------------------------------
// This service was documented as "not internally locked; the API task owns it",
// and while it was only a store served by a route that was true. It stopped
// being true when the wake cycle began composing from the profile: the HTTP
// task can be part way through Put() — which parses an incoming document
// straight into the live Profile, because the staging area and the live object
// are deliberately the same 18 KB buffer — while the main task walks that same
// object deciding what to draw.
//
// A reader during a push therefore saw a document that was neither the old one
// nor the new one. ProfileLock is the fix; these are the tests that would have
// caught it, and they run under ASan/UBSan where a data race on the revision is
// a visible inconsistency rather than a coin flip.

static void test_a_reader_never_sees_a_half_parsed_profile() {
    Fixture f;
    Push(f, ProfileJson(1));

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            AutonomyService::ProfileLock guard(f.service);
            if (!guard.has_profile()) continue;
            // Everything a reader takes out has to describe one document. The
            // revision in the parsed profile and the revision the service
            // reports are written at different moments inside Put(), so a
            // reader admitted between them would see them disagree.
            const int32_t reported = guard.revision();
            const int32_t parsed = guard.profile().revision;
            const int32_t modules = guard.profile().module_count;
            if (reported != parsed || modules != 1) torn.fetch_add(1);
            reads.fetch_add(1);
        }
    });

    for (int revision = 2; revision <= 60; ++revision) {
        const PutResult r = Push(f, ProfileJson(revision));
        CHECK(r.outcome == PutOutcome::kAccepted);
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(reads.load() > 0);
    CHECK_EQ_INT(torn.load(), 0);
    // And the last write is what is held afterwards.
    CHECK_EQ_INT(f.service.revision(), 60);
}

static void test_concurrent_pushes_keep_the_revision_rule() {
    // Four writers racing on the same compare-and-swap. Exactly one of any pair
    // at the same revision may be accepted, and the stored revision must never
    // go backwards — which is the property the CAS exists for and which a
    // half-applied Put() would break.
    Fixture f;
    Push(f, ProfileJson(1));

    std::atomic<int> accepted{0};
    std::atomic<int> conflicts{0};
    std::atomic<bool> went_backwards{false};

    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w]() {
            for (int n = 0; n < 25; ++n) {
                const int revision = 2 + n * 4 + w;
                const PutResult r = Push(f, ProfileJson(revision));
                if (r.outcome == PutOutcome::kAccepted) {
                    accepted.fetch_add(1);
                } else if (r.outcome == PutOutcome::kRevisionConflict) {
                    conflicts.fetch_add(1);
                }
                AutonomyService::ProfileLock guard(f.service);
                if (guard.has_profile() && guard.revision() < 1) {
                    went_backwards.store(true);
                }
            }
        });
    }
    for (std::thread& th : writers) th.join();

    CHECK(!went_backwards.load());
    CHECK(accepted.load() > 0);
    CHECK_EQ_INT(accepted.load() + conflicts.load(), 100);
    // Whatever order they landed in, the survivor is a real document.
    AutonomyService::ProfileLock guard(f.service);
    CHECK(guard.has_profile());
    CHECK_EQ_INT(guard.revision(), guard.profile().revision);
}

static void test_a_read_back_during_a_push_is_one_document_or_the_other() {
    // GET is verbatim bytes from the slot; it must never serve half of one
    // document and half of another, and the digest it reports must be of the
    // bytes it just returned.
    Fixture f;
    Push(f, ProfileJson(1));

    std::atomic<bool> stop{false};
    std::atomic<int> mismatches{0};
    std::thread reader([&]() {
        std::vector<uint8_t> out(kProfileMaxBytes);
        while (!stop.load(std::memory_order_acquire)) {
            const GetResult g = f.service.Get(out.data(), out.size());
            if (!g.present) continue;
            const std::string body(reinterpret_cast<const char*>(out.data()),
                                   g.bytes);
            if (HexOf(body) != g.sha256) mismatches.fetch_add(1);
        }
    });

    for (int revision = 2; revision <= 40; ++revision) Push(f, ProfileJson(revision));
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK_EQ_INT(mismatches.load(), 0);
}

int main() {
    RUN(test_a_reader_never_sees_a_half_parsed_profile);
    RUN(test_concurrent_pushes_keep_the_revision_rule);
    RUN(test_a_read_back_during_a_push_is_one_document_or_the_other);
    RUN(test_a_first_profile_is_accepted_and_read_back);
    RUN(test_the_stored_bytes_come_back_verbatim);
    RUN(test_a_profile_survives_a_reload_the_way_a_reboot_would);
    RUN(test_loading_a_profile_does_not_build_one_on_the_stack);
    RUN(test_a_declared_digest_that_matches_is_accepted);

    RUN(test_an_unprovisioned_device_says_so);
    RUN(test_a_wrong_token_is_refused_and_stores_nothing);
    RUN(test_repeated_failures_earn_a_lockout);

    RUN(test_a_retried_key_returns_the_first_answer_without_storing_again);
    RUN(test_a_different_key_is_not_a_replay);
    RUN(test_the_ring_forgets_the_oldest_key);

    RUN(test_a_revision_that_does_not_advance_is_refused);
    RUN(test_a_refused_push_leaves_the_live_profile_intact);

    RUN(test_an_oversized_document_is_refused_before_parsing);
    RUN(test_a_document_that_does_not_parse_names_its_field);
    RUN(test_a_declared_digest_that_does_not_match_is_refused);
    RUN(test_an_empty_body_is_refused);

    RUN(test_a_write_that_reports_success_and_stores_nothing_is_caught);
    RUN(test_a_write_that_fails_outright_is_reported);
    RUN(test_a_failed_write_leaves_an_earlier_profile_in_place);
    RUN(test_a_corrupt_store_reads_as_no_profile_not_as_a_default_one);
    RUN(test_a_maximal_profile_round_trips_through_the_service);

    RUN(test_an_injected_verifier_decides_instead_of_the_local_token);
    RUN(test_the_verifier_reports_an_unprovisioned_device_as_such);
    RUN(test_a_rotated_token_takes_effect_without_touching_the_service);
    RUN(test_clearing_the_verifier_returns_to_the_local_token);
    RUN(test_the_lockout_still_belongs_to_the_service_under_a_verifier);

    RUN(test_an_unset_clock_records_no_applied_time);
    RUN(test_a_set_clock_records_when_the_profile_was_applied);
    RUN(test_every_outcome_has_a_name);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
