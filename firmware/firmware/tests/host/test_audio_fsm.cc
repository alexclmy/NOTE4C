/**
 * @file test_audio_fsm.cc
 * @brief Host tests for the real audio_fsm and earcon_gen translation units.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Time is injected, so a twenty-second response timeout is asserted in
 * microseconds. No audio is produced, no device is touched, and the recorder
 * hooks are fakes that only count calls.
 */

#include "common/audio_fsm.h"
#include "common/earcon_gen.h"
#include "common/voice_transport.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace audio_ui;

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
        std::printf("%-46s %s\n", #fn,         \
                    (g_failures == before) ? "ok" : "FAILED"); \
    } while (0)

static void CheckState(State actual, State expected, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] expected state %s, got %s\n",
                    g_current_test, line, StateName(expected), StateName(actual));
    }
}

#define CHECK_STATE(fsm, expected) CheckState((fsm).state(), (expected), __LINE__)

// -------------------------------------------------------------- fake hooks --

/**
 * @brief Records every side effect in order.
 *
 * The order matters as much as the counts: an acknowledge after the listen
 * tone, or a mic started before the tone that tells the user it is listening,
 * are both wrong in ways a count alone would miss.
 */
struct Recorder {
    std::vector<std::string> calls;
    std::vector<Earcon> earcons;
    std::vector<Led> leds;
    int mic_starts = 0;
    int mic_stops = 0;
    int uploads = 0;
    int playback_starts = 0;
    int playback_stops = 0;
    int cancels = 0;
    int discards = 0;

    Hooks MakeHooks() {
        Hooks h;
        h.earcon = [this](Earcon e) {
            earcons.push_back(e);
            calls.push_back(std::string("earcon:") + EarconName(e));
        };
        h.led = [this](Led l) {
            leds.push_back(l);
            calls.push_back("led");
        };
        h.mic_start = [this]() { ++mic_starts; calls.push_back("mic_start"); };
        h.mic_stop = [this]() { ++mic_stops; calls.push_back("mic_stop"); };
        h.upload = [this]() { ++uploads; calls.push_back("upload"); };
        h.discard = [this]() { ++discards; calls.push_back("discard"); };
        h.playback_start = [this]() { ++playback_starts; calls.push_back("playback_start"); };
        h.playback_stop = [this]() { ++playback_stops; calls.push_back("playback_stop"); };
        h.cancel_request = [this]() { ++cancels; calls.push_back("cancel_request"); };
        return h;
    }

    bool SawEarcon(Earcon e) const {
        for (Earcon seen : earcons) {
            if (seen == e) return true;
        }
        return false;
    }

    /// Index of the first call equal to @p name, or -1.
    int IndexOf(const char* name) const {
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i] == name) return static_cast<int>(i);
        }
        return -1;
    }
};

static Config TestConfig() {
    Config c;
    c.arm_threshold_ms = 300;
    c.min_recording_ms = 400;
    c.max_recording_ms = 15000;
    c.response_timeout_ms = 25000;
    c.max_playback_ms = 60000;
    c.error_linger_ms = 2000;
    return c;
}

/// A machine with voice compiled in and somewhere to send, which is the shape
/// the shipped build has once the mute is off. Each gate is asserted
/// separately, so the helper opens all of them.
static void MakeEnabled(AudioFsm& fsm, Recorder& rec) {
    fsm.SetConfig(TestConfig());
    fsm.SetHooks(rec.MakeHooks());
    fsm.SetVoiceEnabled(true);
    fsm.SetTransportReady(true);
}

/// Drive the machine to Recording. Returns the clock after arming.
static uint64_t DriveToRecording(AudioFsm& fsm, uint64_t now = 1000) {
    fsm.Handle(Event::kPttPressed, now);
    fsm.Tick(now + 300);
    return now + 300;
}

/// Recording, held past min_recording_ms so a release really uploads.
static uint64_t DriveToUploadableRelease(AudioFsm& fsm, uint64_t now = 1000) {
    const uint64_t recording_at = DriveToRecording(fsm, now);
    return recording_at + 500;
}

// ------------------------------------------------------------- arm / cancel --

static void test_starts_idle() {
    AudioFsm fsm;
    CHECK_STATE(fsm, State::kIdle);
    CHECK(!fsm.muted());
    CHECK(!fsm.voice_enabled());
}

static void test_press_arms_and_acknowledges() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    CHECK_STATE(fsm, State::kPttArmed);
    CHECK(rec.SawEarcon(Earcon::kAcknowledge));
    CHECK(rec.mic_starts == 0);
}

static void test_arm_threshold_starts_recording() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Tick(1299);
    CHECK_STATE(fsm, State::kPttArmed);
    CHECK(rec.mic_starts == 0);
    fsm.Tick(1300);
    CHECK_STATE(fsm, State::kRecording);
    CHECK(rec.mic_starts == 1);
}

static void test_release_before_the_threshold_is_a_tap() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kPttReleased, 1100);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_starts == 0);
    CHECK(rec.uploads == 0);
}

static void test_release_while_recording_uploads() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm, 1000);
    fsm.Handle(Event::kPttReleased, t + 2000);
    CHECK_STATE(fsm, State::kWaitingResponse);
    CHECK(rec.mic_stops == 1);
    CHECK(rec.uploads == 1);
    CHECK(rec.SawEarcon(Earcon::kListenStop));
}

static void test_cancel_while_armed_returns_to_idle() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kCancel, 1100);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.uploads == 0);
}

static void test_cancel_while_recording_discards_the_utterance() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kCancel, t + 500);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_stops == 1);
    // Cancelled means never sent, not sent and then ignored.
    CHECK(rec.uploads == 0);
}

static void test_duplicate_press_does_not_restart_the_ladder() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    const size_t before = rec.calls.size();
    fsm.Handle(Event::kPttPressed, t + 100);
    CHECK_STATE(fsm, State::kRecording);
    CHECK(rec.calls.size() == before);
    CHECK(rec.mic_starts == 1);
}

static void test_press_after_release_arms_again() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kPttReleased, 1100);
    fsm.Handle(Event::kPttPressed, 2000);
    CHECK_STATE(fsm, State::kPttArmed);
}

// -------------------------------------------------------------- max duration --

static void test_max_recording_duration_stops_by_itself() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Tick(t + 14999);
    CHECK_STATE(fsm, State::kRecording);
    fsm.Tick(t + 15000);
    CHECK_STATE(fsm, State::kWaitingResponse);
    CHECK(rec.mic_stops == 1);
    CHECK(rec.uploads == 1);
}

static void test_release_after_the_max_duration_changes_nothing() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Tick(t + 15000);
    CHECK_STATE(fsm, State::kWaitingResponse);
    fsm.Handle(Event::kPttReleased, t + 16000);
    CHECK_STATE(fsm, State::kWaitingResponse);
    CHECK(rec.uploads == 1);
}

// -------------------------------------------------------- response handling --

static void test_response_timeout_goes_to_error_then_idle() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 1000);
    const uint64_t waiting_since = t + 1000;
    fsm.Tick(waiting_since + 24999);
    CHECK_STATE(fsm, State::kWaitingResponse);
    fsm.Tick(waiting_since + 25000);
    CHECK_STATE(fsm, State::kError);
    CHECK(rec.SawEarcon(Earcon::kError));
    CHECK(rec.cancels == 1);
    CHECK(fsm.reason() == Reason::kResponseTimedOut);
    // The error is shown briefly, then the device goes quiet by itself.
    fsm.Tick(waiting_since + 25000 + 1999);
    CHECK_STATE(fsm, State::kError);
    fsm.Tick(waiting_since + 25000 + 2000);
    CHECK_STATE(fsm, State::kIdle);
}

/// The response timeout must outlast the transport's own retry ladder, or the
/// state machine reports a timeout for an upload that is still being retried.
/// Both numbers live in headers, so this catches an edit to either one.
static void test_the_response_timeout_outlasts_the_transport_ladder() {
    const Config c = TestConfig();
    const voice::TransportConfig t{};
    const uint64_t worst_case_ms =
        static_cast<uint64_t>(t.max_attempts) * t.attempt_timeout_ms +
        static_cast<uint64_t>(t.max_attempts - 1) * t.backoff_ms;
    CHECK(worst_case_ms <= t.total_deadline_ms);
    CHECK(c.response_timeout_ms > t.total_deadline_ms);
}

static void test_explicit_failure_reports_an_error() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseFailed, t + 800);
    CHECK_STATE(fsm, State::kError);
    CHECK(rec.SawEarcon(Earcon::kError));
}

static void test_response_ready_starts_playback() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseReady, t + 900);
    CHECK_STATE(fsm, State::kPlaying);
    CHECK(rec.playback_starts == 1);
    CHECK(rec.SawEarcon(Earcon::kResponseReady));
    fsm.Handle(Event::kPlaybackFinished, t + 3000);
    CHECK_STATE(fsm, State::kIdle);
}

static void test_playback_is_cancellable_by_back() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseReady, t + 900);
    fsm.Handle(Event::kCancel, t + 1200);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.playback_stops == 1);
}

static void test_playback_has_a_hard_cap() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseReady, t + 900);
    fsm.Tick(t + 900 + 60000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.playback_stops == 1);
}

static void test_cancel_while_waiting_abandons_the_request() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kCancel, t + 700);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.cancels == 1);
}

static void test_late_response_after_cancel_is_ignored() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kCancel, t + 700);
    fsm.Handle(Event::kResponseReady, t + 5000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.playback_starts == 0);
}

// -------------------------------------------------------------------- mute --

static void test_mute_blocks_recording_entry() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.SetMuted(true, 500);
    // Turning the mute on is itself acknowledged. Clear that, so what is
    // asserted below is what the press produced.
    rec.earcons.clear();
    rec.calls.clear();
    fsm.Handle(Event::kPttPressed, 1000);
    // Press-down arms whatever the gates say; the refusal happens at the arm
    // threshold, so a BOOT click on a muted device stays silent about it.
    CHECK_STATE(fsm, State::kPttArmed);
    CHECK(!rec.SawEarcon(Earcon::kMuted));
    fsm.Tick(2000);
    CHECK_STATE(fsm, State::kMuted);
    CHECK(rec.mic_starts == 0);
    CHECK(rec.SawEarcon(Earcon::kMuted));
    CHECK(fsm.reason() == Reason::kMuted);
    fsm.Handle(Event::kPttReleased, 2100);
    CHECK_STATE(fsm, State::kIdle);
}

/// The whole point of moving the refusal to the arm threshold: on the
/// Dashboard a BOOT click is quick-switch, and this machine sees the press-down
/// of every one of them. A muted blip on each would be unusable.
static void test_a_tap_on_a_muted_device_makes_no_muted_sound() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.SetMuted(true, 500);
    rec.earcons.clear();
    rec.calls.clear();
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kPttReleased, 1150);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(!rec.SawEarcon(Earcon::kMuted));
    CHECK(!fsm.press_consumed());
    CHECK(rec.mic_starts == 0);
}

static void test_mute_suppresses_playback_of_an_arrived_response() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.SetMuted(true, t + 600);
    fsm.Handle(Event::kResponseReady, t + 900);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.playback_starts == 0);
}

static void test_muting_mid_recording_stops_the_microphone() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.SetMuted(true, t + 400);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_stops == 1);
    CHECK(rec.uploads == 0);
}

static void test_muting_mid_playback_stops_it() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseReady, t + 900);
    fsm.SetMuted(true, t + 1000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.playback_stops == 1);
}

static void test_mute_toggle_round_trips() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    CHECK(!fsm.muted());
    fsm.Handle(Event::kMuteToggled, 100);
    CHECK(fsm.muted());
    fsm.Handle(Event::kMuteToggled, 200);
    CHECK(!fsm.muted());
    fsm.Handle(Event::kPttPressed, 300);
    CHECK_STATE(fsm, State::kPttArmed);
}

static void test_muting_while_armed_never_opens_the_microphone() {
    // The regression this file exists for. Muting used to be handled only for
    // Recording and Playing, so a mute arriving between the press and the arm
    // threshold left the machine armed, and the arm timeout then opened the
    // codec input on a device the user had just muted.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    CHECK_STATE(fsm, State::kPttArmed);
    fsm.SetMuted(true, 1100);
    CHECK_STATE(fsm, State::kIdle);
    // Well past the arm threshold: the timeout must find nothing to arm.
    fsm.Tick(9000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_starts == 0);
    CHECK(rec.SawEarcon(Earcon::kMuted));
}

static void test_the_mute_gesture_while_armed_also_disarms() {
    // The same property reached through the event rather than the setter, so
    // the Settings row and a future mute button are both covered.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    CHECK_STATE(fsm, State::kPttArmed);
    fsm.Handle(Event::kMuteToggled, 1050);
    CHECK(fsm.muted());
    CHECK_STATE(fsm, State::kIdle);
    fsm.Handle(Event::kTimeout, 2000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_starts == 0);
}

static void test_the_arm_timeout_rechecks_voice_enabled_before_recording() {
    // The reachable half of the recheck added to the kPttArmed timeout branch.
    // The muted half is now unreachable through the public API, because
    // SetMuted() leaves Armed before the timeout can fire; it is kept as
    // defence in depth for the Milestone B callers that will drive this
    // machine from more than one task.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    CHECK_STATE(fsm, State::kPttArmed);
    // The shipped configuration: the mic path is not compiled in.
    fsm.SetVoiceEnabled(false);
    fsm.Handle(Event::kTimeout, 2000);
    CHECK_STATE(fsm, State::kMuted);
    CHECK(rec.mic_starts == 0);
}

static void test_muting_while_armed_still_releases_cleanly() {
    // The finger is still on the button when the mute lands. Releasing it must
    // not leave the machine in a state that a later press cannot escape.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.SetMuted(true, 1100);
    fsm.Handle(Event::kPttReleased, 1200);
    CHECK_STATE(fsm, State::kIdle);
    fsm.SetMuted(false, 1300);
    fsm.Handle(Event::kPttPressed, 1400);
    CHECK_STATE(fsm, State::kPttArmed);
    fsm.Tick(1400 + fsm.config().arm_threshold_ms);
    CHECK_STATE(fsm, State::kRecording);
    CHECK(rec.mic_starts == 1);
}

static void test_unmuting_leaves_the_muted_acknowledgement() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.SetMuted(true, 100);
    fsm.Handle(Event::kPttPressed, 200);
    fsm.Tick(200 + fsm.config().arm_threshold_ms);
    CHECK_STATE(fsm, State::kMuted);
    fsm.SetMuted(false, 700);
    CHECK_STATE(fsm, State::kIdle);
}

// ---------------------------------------------------- voice compiled out --

static void test_voice_disabled_never_opens_the_microphone() {
    // This is the shipped configuration for Milestone A. The reserved gesture
    // is acknowledged as unavailable; nothing listens.
    AudioFsm fsm;
    Recorder rec;
    fsm.SetConfig(TestConfig());
    fsm.SetHooks(rec.MakeHooks());
    fsm.SetTransportReady(true);
    CHECK(!fsm.voice_enabled());
    fsm.Handle(Event::kPttPressed, 1000);
    for (uint64_t t = 1000; t < 60000; t += 250) {
        fsm.Tick(t);
    }
    CHECK_STATE(fsm, State::kMuted);
    CHECK(fsm.reason() == Reason::kVoiceDisabled);
    CHECK(rec.mic_starts == 0);
    CHECK(rec.uploads == 0);
    CHECK(rec.playback_starts == 0);
    CHECK(rec.SawEarcon(Earcon::kMuted));
    CHECK(!rec.SawEarcon(Earcon::kListenStart));
}

static void test_voice_disabled_release_returns_to_idle() {
    AudioFsm fsm;
    Recorder rec;
    fsm.SetConfig(TestConfig());
    fsm.SetHooks(rec.MakeHooks());
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kPttReleased, 1500);
    CHECK_STATE(fsm, State::kIdle);
}

// ------------------------------------------------------ transport readiness --

static void test_no_transport_refuses_rather_than_recording() {
    // Recording fifteen seconds the device has nowhere to send is not a
    // feature. The refusal sounds like an error, not like a mute, because a
    // dropped Wi-Fi connection is a fault and the mute is a choice.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.SetTransportReady(false);
    fsm.Handle(Event::kPttPressed, 1000);
    CHECK_STATE(fsm, State::kPttArmed);
    fsm.Tick(1300);
    CHECK_STATE(fsm, State::kError);
    CHECK(fsm.reason() == Reason::kTransportUnavailable);
    CHECK(rec.SawEarcon(Earcon::kError));
    CHECK(!rec.SawEarcon(Earcon::kMuted));
    CHECK(rec.mic_starts == 0);
    // It clears itself, like every other error.
    fsm.Tick(1300 + 2000);
    CHECK_STATE(fsm, State::kIdle);
}

static void test_losing_the_transport_between_press_and_threshold_is_caught() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.SetTransportReady(false);
    fsm.Tick(1300);
    CHECK_STATE(fsm, State::kError);
    CHECK(rec.mic_starts == 0);
}

static void test_losing_the_transport_mid_recording_does_not_abandon_it() {
    // The upload has its own retries and its own deadline. Dropping the
    // utterance here would throw away speech the transport might well deliver
    // a second later, and would do it silently.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.SetTransportReady(false);
    fsm.Tick(t + 100);
    CHECK_STATE(fsm, State::kRecording);
    fsm.Handle(Event::kPttReleased, t + 600);
    CHECK_STATE(fsm, State::kWaitingResponse);
    CHECK(rec.uploads == 1);
}

// ------------------------------------------------- minimum utterance length --

static void test_a_too_short_utterance_is_discarded_not_uploaded() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    // Past the 300 ms arm threshold, inside the 400 ms minimum.
    fsm.Handle(Event::kPttReleased, t + 399);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.mic_stops == 1);
    CHECK(rec.uploads == 0);
    CHECK(rec.discards == 1);
    CHECK(fsm.reason() == Reason::kUtteranceTooShort);
    // It still consumed the press: the user held the button to talk, and the
    // page must not also change under them.
    CHECK(fsm.press_consumed());
}

static void test_exactly_the_minimum_length_uploads() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToUploadableRelease(fsm);
    fsm.Handle(Event::kPttReleased, t - 100);
    CHECK_STATE(fsm, State::kWaitingResponse);
    CHECK(rec.uploads == 1);
    CHECK(rec.discards == 0);
}

// ------------------------------------------- an answer with nothing to play --

static void test_an_answer_with_no_audio_never_enters_playing() {
    // The shipped case: hub/CONTRACTS.md section 10 returns text and no audio,
    // and this firmware cannot speak. "Playing" in a log for something that
    // was never played is the kind of small lie this project does not tell.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 600);
    CHECK_STATE(fsm, State::kWaitingResponse);
    fsm.Handle(Event::kResponseAnswered, t + 900);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(rec.SawEarcon(Earcon::kResponseReady));
    CHECK(rec.playback_starts == 0);
    CHECK(rec.playback_stops == 0);
}

static void test_an_answer_with_no_audio_respects_the_mute() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 600);
    fsm.SetMuted(true, t + 700);
    rec.earcons.clear();
    fsm.Handle(Event::kResponseAnswered, t + 900);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(!rec.SawEarcon(Earcon::kResponseReady));
    CHECK(rec.SawEarcon(Earcon::kMuted));
}

static void test_an_answer_outside_waiting_is_ignored() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kResponseAnswered, 1000);
    CHECK_STATE(fsm, State::kIdle);
    CHECK(!rec.SawEarcon(Earcon::kResponseReady));
}

// --------------------------------------------------------- press consumption --

static void test_a_tap_leaves_the_press_available_to_navigation() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Handle(Event::kPttReleased, 1200);
    CHECK(!fsm.press_consumed());
}

static void test_a_hold_that_recorded_consumes_the_press() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    CHECK(fsm.press_consumed());
    fsm.Handle(Event::kPttReleased, t + 600);
    CHECK(fsm.press_consumed());
}

static void test_the_next_press_clears_the_consumption_flag() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 600);
    fsm.Handle(Event::kCancel, t + 700);
    CHECK(fsm.press_consumed());
    fsm.Handle(Event::kPttPressed, t + 1000);
    CHECK(!fsm.press_consumed());
    CHECK(fsm.reason() == Reason::kNone);
}

// -------------------------------------------------------- ordering and time --

static void test_earcons_are_emitted_in_order() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    fsm.Handle(Event::kResponseReady, t + 900);
    fsm.Handle(Event::kPlaybackFinished, t + 2000);

    const Earcon expected[] = {Earcon::kAcknowledge, Earcon::kListenStart,
                               Earcon::kListenStop, Earcon::kResponseReady};
    CHECK(rec.earcons.size() == 4);
    if (rec.earcons.size() == 4) {
        for (size_t i = 0; i < 4; ++i) {
            CHECK(rec.earcons[i] == expected[i]);
        }
    }
}

static void test_the_listen_tone_precedes_opening_the_microphone() {
    // The user hears "listening" and only then is the mic live, so the tone
    // cannot be captured as the first thing in the recording.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    DriveToRecording(fsm);
    const int tone = rec.IndexOf("earcon:listen_start");
    const int mic = rec.IndexOf("mic_start");
    CHECK(tone >= 0);
    CHECK(mic >= 0);
    CHECK(tone < mic);
}

static void test_the_microphone_stops_before_the_upload() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    const uint64_t t = DriveToRecording(fsm);
    fsm.Handle(Event::kPttReleased, t + 500);
    const int mic_stop = rec.IndexOf("mic_stop");
    const int upload = rec.IndexOf("upload");
    CHECK(mic_stop >= 0);
    CHECK(upload >= 0);
    CHECK(mic_stop < upload);
}

static void test_no_transition_waits_on_the_display() {
    // The load-bearing property of this file. The hook struct has no render
    // member at all, so the whole ladder is driven here with nothing but the
    // clock: if a transition ever came to depend on a refresh completing,
    // there would be no way to express it and this test would not compile.
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    uint64_t now = 1000;
    fsm.Handle(Event::kPttPressed, now);
    now += 300;
    fsm.Tick(now);
    CHECK_STATE(fsm, State::kRecording);
    now += 1000;
    fsm.Handle(Event::kPttReleased, now);
    CHECK_STATE(fsm, State::kWaitingResponse);
    now += 100;
    fsm.Handle(Event::kResponseReady, now);
    CHECK_STATE(fsm, State::kPlaying);
    now += 100;
    fsm.Handle(Event::kPlaybackFinished, now);
    CHECK_STATE(fsm, State::kIdle);
    // A panel refresh taking three minutes in the middle of this would have
    // changed none of it.
}

static void test_tick_is_idempotent() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Tick(1300);
    CHECK_STATE(fsm, State::kRecording);
    const size_t after_first = rec.calls.size();
    fsm.Tick(1300);
    fsm.Tick(1300);
    CHECK(rec.calls.size() == after_first);
    CHECK(rec.mic_starts == 1);
}

static void test_time_in_state_is_monotonic_and_safe() {
    AudioFsm fsm;
    Recorder rec;
    MakeEnabled(fsm, rec);
    fsm.Handle(Event::kPttPressed, 5000);
    CHECK(fsm.TimeInState(5000) == 0);
    CHECK(fsm.TimeInState(5250) == 250);
    // A clock that goes backwards must not underflow into a huge duration.
    CHECK(fsm.TimeInState(4000) == 0);
}

static void test_null_hooks_are_safe() {
    // Every hook is optional so a caller can install only what it needs.
    AudioFsm fsm;
    fsm.SetConfig(TestConfig());
    fsm.SetVoiceEnabled(true);
    fsm.SetTransportReady(true);
    fsm.Handle(Event::kPttPressed, 1000);
    fsm.Tick(1300);
    fsm.Handle(Event::kPttReleased, 2000);
    fsm.Handle(Event::kResponseReady, 2100);
    fsm.Handle(Event::kPlaybackFinished, 3000);
    CHECK_STATE(fsm, State::kIdle);
}

static void test_state_and_event_names_are_total() {
    CHECK(std::strcmp(StateName(State::kWaitingResponse), "WaitingResponse") == 0);
    CHECK(std::strcmp(StateName(State::kMuted), "Muted") == 0);
    CHECK(std::strcmp(EventName(Event::kPttReleased), "PttReleased") == 0);
    CHECK(std::strcmp(EarconName(Earcon::kResponseReady), "response_ready") == 0);
}

// ----------------------------------------------------------------- earcons --

static void test_every_earcon_renders() {
    const Earcon all[] = {Earcon::kAcknowledge, Earcon::kListenStart,
                          Earcon::kListenStop, Earcon::kResponseReady,
                          Earcon::kError, Earcon::kMuted};
    std::vector<int16_t> buffer(earcon::kMaxSamples);
    for (Earcon e : all) {
        const size_t expected = earcon::SampleCount(e);
        CHECK(expected > 0);
        CHECK(expected <= earcon::kMaxSamples);
        const size_t written = earcon::Render(e, buffer.data(), buffer.size());
        CHECK(written == expected);
    }
}

static void test_earcons_fit_the_documented_maximum() {
    const Earcon all[] = {Earcon::kAcknowledge, Earcon::kListenStart,
                          Earcon::kListenStop, Earcon::kResponseReady,
                          Earcon::kError, Earcon::kMuted};
    for (Earcon e : all) {
        // 750 ms at 16 kHz. Anything longer stops being an earcon.
        CHECK(earcon::SampleCount(e) <= earcon::kMaxSamples);
    }
}

static void test_render_refuses_a_short_buffer() {
    std::vector<int16_t> buffer(4);
    CHECK(earcon::Render(Earcon::kAcknowledge, buffer.data(), buffer.size()) == 0);
    CHECK(earcon::Render(Earcon::kAcknowledge, nullptr, 100000) == 0);
}

static void test_earcons_start_and_end_near_silence() {
    // The anti-click property. A tone that starts at full amplitude is a pop,
    // and this board's amplifier pin has two owners (hardware gate HG5).
    const Earcon all[] = {Earcon::kAcknowledge, Earcon::kListenStart,
                          Earcon::kListenStop, Earcon::kResponseReady,
                          Earcon::kError, Earcon::kMuted};
    std::vector<int16_t> buffer(earcon::kMaxSamples);
    for (Earcon e : all) {
        const size_t n = earcon::Render(e, buffer.data(), buffer.size());
        CHECK(n > 0);
        if (n == 0) continue;
        CHECK(std::abs(static_cast<int>(buffer[0])) < 200);
        CHECK(std::abs(static_cast<int>(buffer[n - 1])) < 200);
    }
}

static void test_earcons_stay_inside_their_amplitude() {
    std::vector<int16_t> buffer(earcon::kMaxSamples);
    const Earcon all[] = {Earcon::kAcknowledge, Earcon::kListenStart,
                          Earcon::kListenStop, Earcon::kResponseReady,
                          Earcon::kError, Earcon::kMuted};
    for (Earcon e : all) {
        const earcon::Spec spec = earcon::SpecFor(e);
        const size_t n = earcon::Render(e, buffer.data(), buffer.size());
        for (size_t i = 0; i < n; ++i) {
            CHECK(std::abs(static_cast<int>(buffer[i])) <= spec.amplitude);
        }
    }
}

static void test_listen_start_and_stop_are_distinguishable() {
    // Rising versus falling. If they were the same the user could not tell
    // whether the device had started or stopped listening.
    const earcon::Spec start = earcon::SpecFor(Earcon::kListenStart);
    const earcon::Spec stop = earcon::SpecFor(Earcon::kListenStop);
    CHECK(start.note_count == 2);
    CHECK(stop.note_count == 2);
    CHECK(start.notes[0].frequency_hz < start.notes[1].frequency_hz);
    CHECK(stop.notes[0].frequency_hz > stop.notes[1].frequency_hz);
}

static void test_error_earcon_contains_a_gap() {
    const earcon::Spec spec = earcon::SpecFor(Earcon::kError);
    bool has_silence = false;
    for (uint8_t i = 0; i < spec.note_count; ++i) {
        if (spec.notes[i].frequency_hz == 0 && spec.notes[i].duration_ms > 0) {
            has_silence = true;
        }
    }
    CHECK(has_silence);
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("audio_fsm host tests (real firmware translation units)\n\n");

    RUN(test_starts_idle);
    RUN(test_press_arms_and_acknowledges);
    RUN(test_arm_threshold_starts_recording);
    RUN(test_release_before_the_threshold_is_a_tap);
    RUN(test_release_while_recording_uploads);
    RUN(test_cancel_while_armed_returns_to_idle);
    RUN(test_cancel_while_recording_discards_the_utterance);
    RUN(test_duplicate_press_does_not_restart_the_ladder);
    RUN(test_press_after_release_arms_again);

    RUN(test_max_recording_duration_stops_by_itself);
    RUN(test_release_after_the_max_duration_changes_nothing);

    RUN(test_response_timeout_goes_to_error_then_idle);
    RUN(test_the_response_timeout_outlasts_the_transport_ladder);
    RUN(test_explicit_failure_reports_an_error);
    RUN(test_response_ready_starts_playback);
    RUN(test_playback_is_cancellable_by_back);
    RUN(test_playback_has_a_hard_cap);
    RUN(test_cancel_while_waiting_abandons_the_request);
    RUN(test_late_response_after_cancel_is_ignored);

    RUN(test_mute_blocks_recording_entry);
    RUN(test_a_tap_on_a_muted_device_makes_no_muted_sound);
    RUN(test_mute_suppresses_playback_of_an_arrived_response);
    RUN(test_muting_mid_recording_stops_the_microphone);
    RUN(test_muting_mid_playback_stops_it);
    RUN(test_mute_toggle_round_trips);
    RUN(test_muting_while_armed_never_opens_the_microphone);
    RUN(test_the_mute_gesture_while_armed_also_disarms);
    RUN(test_the_arm_timeout_rechecks_voice_enabled_before_recording);
    RUN(test_muting_while_armed_still_releases_cleanly);
    RUN(test_unmuting_leaves_the_muted_acknowledgement);

    RUN(test_voice_disabled_never_opens_the_microphone);
    RUN(test_voice_disabled_release_returns_to_idle);
    RUN(test_no_transport_refuses_rather_than_recording);
    RUN(test_losing_the_transport_between_press_and_threshold_is_caught);
    RUN(test_losing_the_transport_mid_recording_does_not_abandon_it);
    RUN(test_a_too_short_utterance_is_discarded_not_uploaded);
    RUN(test_exactly_the_minimum_length_uploads);
    RUN(test_an_answer_with_no_audio_never_enters_playing);
    RUN(test_an_answer_with_no_audio_respects_the_mute);
    RUN(test_an_answer_outside_waiting_is_ignored);
    RUN(test_a_tap_leaves_the_press_available_to_navigation);
    RUN(test_a_hold_that_recorded_consumes_the_press);
    RUN(test_the_next_press_clears_the_consumption_flag);

    RUN(test_earcons_are_emitted_in_order);
    RUN(test_the_listen_tone_precedes_opening_the_microphone);
    RUN(test_the_microphone_stops_before_the_upload);
    RUN(test_no_transition_waits_on_the_display);
    RUN(test_tick_is_idempotent);
    RUN(test_time_in_state_is_monotonic_and_safe);
    RUN(test_null_hooks_are_safe);
    RUN(test_state_and_event_names_are_total);

    RUN(test_every_earcon_renders);
    RUN(test_earcons_fit_the_documented_maximum);
    RUN(test_render_refuses_a_short_buffer);
    RUN(test_earcons_start_and_end_near_silence);
    RUN(test_earcons_stay_inside_their_amplitude);
    RUN(test_listen_start_and_stop_are_distinguishable);
    RUN(test_error_earcon_contains_a_gap);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
