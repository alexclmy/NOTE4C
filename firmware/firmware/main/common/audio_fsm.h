/**
 * @file audio_fsm.h
 * @brief Portable push-to-talk state machine. No microphone, no ESP-IDF.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The rule this file exists to enforce: **audio never waits for the panel.**
 * A four-color refresh can take tens of seconds, and the driver is allowed to
 * block for minutes (CustomLcdDisplay::kWorstCaseRefreshMs is 3x120 s + 30 s).
 * If any transition here waited on a render hook, holding the button would
 * feel dead for a minute. There is therefore no render hook in Hooks at all,
 * and tests/host/test_audio_fsm.cc asserts that the whole ladder runs with the
 * display never mentioned.
 *
 * The machine is fed from BOOT **press-down**, not from the driver's 1000 ms
 * long-press callback. Arming at the long press could never be hold-to-talk:
 * the microphone would open 1000 ms after the threshold the user was told
 * about. The arm threshold in Config is therefore the only threshold that
 * decides whether a press is a tap or an utterance, and the board swallows the
 * click that follows a press which really recorded (PTT-IMPLEMENTATION-PLAN.md
 * sections 1.2 and 1.3).
 *
 * Three separate gates must all be open before kRecording is reachable, and all
 * three are rechecked at the arm threshold rather than trusted from press-down:
 * the software mute, the compile-time VOICE_PTT_ENABLED, and transport
 * readiness. Recording with nowhere to send is not a feature.
 *
 * Time is injected. There is no clock here, so a five-minute timeout is a test
 * that runs in microseconds.
 */

#ifndef COMMON_AUDIO_FSM_H
#define COMMON_AUDIO_FSM_H

#include <stdint.h>

#include <functional>

namespace audio_ui {

enum class State {
    kIdle = 0,
    /// BOOT held past the arm threshold; the mic has not started yet.
    kPttArmed,
    kRecording,
    /// Uploaded, waiting for the hub. Bounded, cancellable.
    kWaitingResponse,
    kPlaying,
    /// Software mute. Not a hardware cut, and never described as one.
    kMuted,
    kError,
};

/**
 * @brief Why the machine refused, or gave up. Never inferred from the state.
 *
 * kNone means nothing has gone wrong since the last press. Every other value
 * is a distinct thing to tell the user and a distinct thing to log; collapsing
 * "muted" and "no Wi-Fi" into one "unavailable" is how a device comes to
 * suggest a hardware fault when a setting is switched off.
 */
enum class Reason {
    kNone = 0,
    /// Software mute is on. Not a hardware cut, and never described as one.
    kMuted,
    /// VOICE_PTT_ENABLED is off in this build.
    kVoiceDisabled,
    /// No network, or no hub URL and token configured.
    kTransportUnavailable,
    /// Released before min_recording_ms. Nothing was uploaded.
    kUtteranceTooShort,
    /// The hub did not answer inside response_timeout_ms.
    kResponseTimedOut,
    /// The transport reported that it failed.
    kUploadFailed,
};

/// Short synthesized tones. No assets, no network, no speech.
enum class Earcon {
    kAcknowledge = 0,
    kListenStart,
    kListenStop,
    kResponseReady,
    kError,
    kMuted,
};

/// What the LED should be doing. The LED is the only feedback guaranteed to
/// arrive while the finger is still on the button.
enum class Led {
    kOff = 0,
    kArmed,
    kRecording,
    kWaiting,
    kPlaying,
    kError,
};

/// Inputs. Everything that can move the machine, including time passing.
enum class Event {
    kPttPressed = 0,
    kPttReleased,
    /// Universal Back, from the navigation model.
    kCancel,
    /// The hub answered and there is audio to play.
    kResponseReady,
    /**
     * @brief The hub answered, and there is nothing to play.
     *
     * This is the shipped case, not an edge case. `hub/CONTRACTS.md` section
     * 10 is explicit that the v1 wire returns text and no audio, and this
     * firmware has no speech synthesis. Passing such an answer through
     * kPlaying with a playback hook that does nothing would put "Playing" in
     * the log for something that was never played, so it gets its own event
     * and goes straight back to Idle after the response tone.
     */
    kResponseAnswered,
    /// The hub answered that it failed.
    kResponseFailed,
    kPlaybackFinished,
    kMuteToggled,
    /// Time has advanced to Config-defined limits. Fed by Tick().
    kTimeout,
};

struct Config {
    /// How long BOOT must be held before recording starts. Below this a press
    /// is a click and the board lets it through to navigation.
    uint32_t arm_threshold_ms = 300;
    /**
     * @brief Shortest utterance worth uploading, measured from kRecording.
     *
     * A hold of 320 ms captures perhaps two Opus frames of the tail of a
     * button click. Uploading it costs a round trip and a transcription
     * attempt to produce nothing. Below this the recording is discarded and
     * the user is told why, which is different from an upload that failed.
     *
     * On the device this must also exceed `VoiceCapture::kToneGuardMs` (200 ms
     * plus a 20 ms poll), because the microphone is not opened until the
     * listen tone has finished. A minimum below the guard would let a release
     * commit to an upload of a recording that had not started. The glue is
     * written so that even that case reports an empty capture rather than
     * hanging, but the margin is what keeps it from happening.
     */
    uint32_t min_recording_ms = 400;
    /// Hard cap on one utterance.
    uint32_t max_recording_ms = 15000;
    /**
     * @brief How long to wait for the hub before giving up.
     *
     * 25 s, not 20 s, because the transport's own ladder is three 6 s attempts
     * with 1 s backoff between them: 20 s in the worst case
     * (voice_transport.h). A response timeout shorter than the transport's own
     * budget would cut off the last attempt and report a timeout for a request
     * that was still being retried.
     */
    uint32_t response_timeout_ms = 25000;
    /// Hard cap on one response playback.
    uint32_t max_playback_ms = 60000;
    /// How long the Error state is shown before returning to Idle.
    uint32_t error_linger_ms = 2000;
};

/**
 * @brief Side effects, injected in the RenderHandshakeHooks style.
 *
 * Deliberately absent: anything to do with the display. See the file comment.
 * Every hook may be null; the FSM checks before calling, so host tests can
 * install only what they are asserting on.
 */
struct Hooks {
    std::function<void(Earcon)> earcon;
    std::function<void(Led)> led;
    /// Open the codec input. Never called while VOICE_PTT_ENABLED is off,
    /// because the machine cannot reach kRecording in that build.
    std::function<void()> mic_start;
    std::function<void()> mic_stop;
    /// Hand the captured utterance to the transport.
    std::function<void()> upload;
    /// Throw the captured audio away without sending it. Called for a
    /// cancelled recording and for one below min_recording_ms. Distinct from
    /// mic_stop, which only closes the codec input.
    std::function<void()> discard;
    std::function<void()> playback_start;
    std::function<void()> playback_stop;
    /// Abandon an in-flight request at the transport layer.
    std::function<void()> cancel_request;
};

/**
 * @brief The push-to-talk ladder.
 *
 * Idle -> PttArmed -> Recording -> WaitingResponse -> Playing -> Idle,
 * with Cancel able to leave from anywhere, Muted gating entry and playback,
 * and Error lingering briefly before returning to Idle.
 */
class AudioFsm {
public:
    AudioFsm() = default;
    AudioFsm(const Config& config, Hooks hooks)
        : config_(config), hooks_(std::move(hooks)) {}

    void SetConfig(const Config& config) { config_ = config; }
    void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    /**
     * @brief Whether the microphone path is compiled into this build.
     *
     * With voice disabled the machine acknowledges the reserved gesture and
     * says it is unavailable, rather than pretending to listen. It never
     * reaches kRecording, so mic_start can never fire.
     */
    void SetVoiceEnabled(bool enabled) { voice_enabled_ = enabled; }
    bool voice_enabled() const { return voice_enabled_; }

    /**
     * @brief Whether there is somewhere to send an utterance right now.
     *
     * False when Wi-Fi is down, or when no hub URL and token are configured.
     * Checked at the arm threshold alongside the mute, so a device with no hub
     * says so instead of recording fifteen seconds it will then throw away.
     *
     * Clearing it mid-recording does not abandon the utterance: the upload is
     * the transport's problem and it has its own retries. It only prevents new
     * recordings from starting.
     */
    void SetTransportReady(bool ready) { transport_ready_ = ready; }
    bool transport_ready() const { return transport_ready_; }

    /**
     * @brief Software mute. Suppresses recording entry and playback.
     *
     * Not a hardware cut, and never described as one. Persistence is the
     * caller's job and is real: main/application.cc writes NVS namespace
     * "voice", key "muted", and restores it in InitializeAudioFsm() before any
     * gesture can reach this machine. The Settings row "Mute voice (software)"
     * is the control.
     *
     * Muting while a press is armed returns to Idle rather than leaving the
     * machine armed. The arm threshold does recheck the mute, so this is belt
     * and braces rather than the only guard, but it is the difference between
     * a mute that takes effect now and one that takes effect at the next
     * threshold.
     */
    void SetMuted(bool muted, uint64_t now_ms);
    bool muted() const { return muted_; }

    State state() const { return state_; }
    const Config& config() const { return config_; }

    /// Why the last refusal or failure happened. Reset at each press-down.
    Reason reason() const { return reason_; }

    /**
     * @brief True once the current press has crossed the arm threshold.
     *
     * The board reads this on release to decide whether to swallow the click
     * the driver is about to deliver. Without it, a 500 ms hold on the
     * Dashboard would both record an utterance and switch pages
     * (PTT-IMPLEMENTATION-PLAN.md section 1.3).
     *
     * It is set whether the threshold produced a recording or a refusal. A
     * hold that was refused because the device is muted must not fall through
     * to navigation either: the user held the button to talk, got told no, and
     * a page change on top of that is a second, unasked-for outcome.
     *
     * Cleared at the next press-down.
     */
    bool press_consumed() const { return press_consumed_; }

    /// Feed one event. @p now_ms is monotonic milliseconds.
    void Handle(Event event, uint64_t now_ms);

    /// Advance time. Emits the timeout transitions when their deadline passes.
    /// Idempotent: calling it repeatedly with the same clock changes nothing.
    void Tick(uint64_t now_ms);

    /// Milliseconds spent in the current state, for logs and tests.
    uint64_t TimeInState(uint64_t now_ms) const;

private:
    void Enter(State next, uint64_t now_ms);
    void Emit(Earcon earcon);
    void EmitLed(Led led);
    void Fail(Reason reason, uint64_t now_ms);
    /// Refuse to record, with the reason said out loud. Enters kMuted, which is
    /// the "asked and declined" state whatever the gate was.
    void Refuse(Reason reason, uint64_t now_ms);
    /// The single place kRecording is entered. Rechecks all three gates.
    void BeginRecordingOrRefuse(uint64_t now_ms);
    /// Leave kRecording. Uploads, or discards a too-short utterance.
    void FinishRecording(uint64_t now_ms);

    Config config_{};
    Hooks hooks_{};
    State state_ = State::kIdle;
    uint64_t state_since_ms_ = 0;
    bool muted_ = false;
    bool voice_enabled_ = false;
    bool transport_ready_ = false;
    Reason reason_ = Reason::kNone;
    bool press_consumed_ = false;
    /// True while a press is being held, so a repeat press-down is ignored.
    bool ptt_held_ = false;
};

const char* StateName(State state);
const char* EarconName(Earcon earcon);
const char* EventName(Event event);
const char* ReasonName(Reason reason);

}  // namespace audio_ui

#endif  // COMMON_AUDIO_FSM_H
