/**
 * @file audio_fsm.cc
 * @brief Implementation of the push-to-talk state machine.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "audio_fsm.h"

namespace audio_ui {

namespace {

Led LedFor(State state) {
    switch (state) {
        case State::kIdle:            return Led::kOff;
        case State::kPttArmed:        return Led::kArmed;
        case State::kRecording:       return Led::kRecording;
        case State::kWaitingResponse: return Led::kWaiting;
        case State::kPlaying:         return Led::kPlaying;
        case State::kMuted:           return Led::kOff;
        case State::kError:           return Led::kError;
    }
    return Led::kOff;
}

}  // namespace

void AudioFsm::Emit(Earcon earcon) {
    if (hooks_.earcon) hooks_.earcon(earcon);
}

void AudioFsm::EmitLed(Led led) {
    if (hooks_.led) hooks_.led(led);
}

void AudioFsm::Enter(State next, uint64_t now_ms) {
    state_ = next;
    state_since_ms_ = now_ms;
    EmitLed(LedFor(next));
}

void AudioFsm::Fail(Reason reason, uint64_t now_ms) {
    reason_ = reason;
    if (hooks_.cancel_request) hooks_.cancel_request();
    Emit(Earcon::kError);
    Enter(State::kError, now_ms);
}

void AudioFsm::Refuse(Reason reason, uint64_t now_ms) {
    reason_ = reason;
    press_consumed_ = true;
    // A muted device and a device with no hub sound different because they are
    // different. Mute is a setting the user chose, so it gets the unobtrusive
    // blip and the kMuted state. No network and no configured hub are faults,
    // so they get the error tone and kError, which lingers long enough to be
    // noticed and then clears itself.
    if (reason == Reason::kMuted || reason == Reason::kVoiceDisabled) {
        Emit(Earcon::kMuted);
        Enter(State::kMuted, now_ms);
        return;
    }
    Emit(Earcon::kError);
    Enter(State::kError, now_ms);
}

void AudioFsm::BeginRecordingOrRefuse(uint64_t now_ms) {
    // The only place kRecording is entered, and therefore the only place
    // mic_start can fire. Every gate is rechecked here rather than trusted
    // from press-down: the press may have started hundreds of milliseconds
    // ago, and a mute or a dropped Wi-Fi connection in between must win.
    if (!voice_enabled_) {
        Refuse(Reason::kVoiceDisabled, now_ms);
        return;
    }
    if (muted_) {
        Refuse(Reason::kMuted, now_ms);
        return;
    }
    if (!transport_ready_) {
        Refuse(Reason::kTransportUnavailable, now_ms);
        return;
    }
    press_consumed_ = true;
    Emit(Earcon::kListenStart);
    Enter(State::kRecording, now_ms);
    if (hooks_.mic_start) hooks_.mic_start();
}

void AudioFsm::FinishRecording(uint64_t now_ms) {
    const uint64_t held = TimeInState(now_ms);
    if (hooks_.mic_stop) hooks_.mic_stop();
    if (held < config_.min_recording_ms) {
        // Long enough to open the microphone, too short to be speech. Discard
        // it here rather than spending a round trip and a transcription on the
        // tail of a button press, and say which of the two happened: this is
        // not an upload that failed.
        reason_ = Reason::kUtteranceTooShort;
        if (hooks_.discard) hooks_.discard();
        Emit(Earcon::kMuted);
        Enter(State::kIdle, now_ms);
        return;
    }
    Emit(Earcon::kListenStop);
    Enter(State::kWaitingResponse, now_ms);
    if (hooks_.upload) hooks_.upload();
}

uint64_t AudioFsm::TimeInState(uint64_t now_ms) const {
    return now_ms >= state_since_ms_ ? now_ms - state_since_ms_ : 0;
}

void AudioFsm::SetMuted(bool muted, uint64_t now_ms) {
    const bool was_muted = muted_;
    muted_ = muted;
    if (!muted_) {
        if (state_ == State::kMuted) {
            Enter(State::kIdle, now_ms);
        }
        return;
    }
    // Muting mid-flight stops what is audible right now. It does not claim to
    // cut power to the amplifier; it is a software mute and is described as
    // one everywhere it is surfaced.
    if (state_ == State::kRecording) {
        if (hooks_.mic_stop) hooks_.mic_stop();
        Emit(Earcon::kMuted);
        Enter(State::kIdle, now_ms);
        return;
    }
    if (state_ == State::kPlaying) {
        if (hooks_.playback_stop) hooks_.playback_stop();
        Emit(Earcon::kMuted);
        Enter(State::kIdle, now_ms);
        return;
    }
    if (state_ == State::kPttArmed) {
        // Armed means BOOT is held and the arm threshold has not elapsed, so
        // the microphone has not been opened yet. Leaving the machine armed
        // here was the whole defect: the kTimeout branch below would fire a
        // moment later and call mic_start() on a device the user had just
        // muted. Dropping to Idle is what makes the mute take effect before
        // the mic can open rather than after.
        Emit(Earcon::kMuted);
        Enter(State::kIdle, now_ms);
        return;
    }
    if (!was_muted) {
        Emit(Earcon::kMuted);
    }
}

void AudioFsm::Handle(Event event, uint64_t now_ms) {
    switch (event) {
        case Event::kPttPressed: {
            if (ptt_held_) {
                // The gesture recogniser already suppresses repeats, but a
                // stuck key or a driver that re-reports the hold must not
                // restart the ladder mid-utterance.
                return;
            }
            ptt_held_ = true;
            if (state_ != State::kIdle && state_ != State::kMuted) {
                return;
            }
            reason_ = Reason::kNone;
            press_consumed_ = false;
            // Press-down always arms, whatever the gates say. Refusing here
            // would sound the muted blip on every BOOT click, because on the
            // Dashboard a BOOT click is quick-switch and this machine sees the
            // press-down of every one of them. The gates are checked at the
            // arm threshold instead, where a deliberate hold has been
            // distinguished from a tap (PTT-IMPLEMENTATION-PLAN.md 1.4).
            Emit(Earcon::kAcknowledge);
            Enter(State::kPttArmed, now_ms);
            return;
        }

        case Event::kPttReleased: {
            ptt_held_ = false;
            if (state_ == State::kMuted) {
                Enter(State::kIdle, now_ms);
                return;
            }
            if (state_ == State::kPttArmed) {
                // Released before the arm threshold: a tap, not an utterance.
                Enter(State::kIdle, now_ms);
                return;
            }
            if (state_ == State::kRecording) {
                FinishRecording(now_ms);
                return;
            }
            return;
        }

        case Event::kCancel: {
            switch (state_) {
                case State::kPttArmed:
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kRecording:
                    if (hooks_.mic_stop) hooks_.mic_stop();
                    // Nothing is uploaded: a cancelled recording is discarded,
                    // not sent and then ignored. The buffer is emptied too,
                    // so the next utterance cannot inherit this one's audio.
                    if (hooks_.discard) hooks_.discard();
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kWaitingResponse:
                    if (hooks_.cancel_request) hooks_.cancel_request();
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kPlaying:
                    if (hooks_.playback_stop) hooks_.playback_stop();
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kError:
                case State::kMuted:
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kIdle:
                    return;
            }
            return;
        }

        case Event::kResponseReady: {
            if (state_ != State::kWaitingResponse) {
                return;
            }
            if (muted_) {
                // The answer arrived, and the device will not speak it. Saying
                // so is the honest option; playing it anyway is not.
                Emit(Earcon::kMuted);
                Enter(State::kIdle, now_ms);
                return;
            }
            Emit(Earcon::kResponseReady);
            Enter(State::kPlaying, now_ms);
            if (hooks_.playback_start) hooks_.playback_start();
            return;
        }

        case Event::kResponseAnswered: {
            if (state_ != State::kWaitingResponse) {
                return;
            }
            if (muted_) {
                // Nothing would have been spoken anyway, but the muted blip is
                // the honest acknowledgement: the answer arrived and the
                // device is saying nothing about it.
                Emit(Earcon::kMuted);
                Enter(State::kIdle, now_ms);
                return;
            }
            Emit(Earcon::kResponseReady);
            Enter(State::kIdle, now_ms);
            return;
        }

        case Event::kResponseFailed: {
            if (state_ != State::kWaitingResponse) {
                return;
            }
            Fail(Reason::kUploadFailed, now_ms);
            return;
        }

        case Event::kPlaybackFinished: {
            if (state_ != State::kPlaying) {
                return;
            }
            Enter(State::kIdle, now_ms);
            return;
        }

        case Event::kMuteToggled: {
            SetMuted(!muted_, now_ms);
            return;
        }

        case Event::kTimeout: {
            // The deadline reached by other means. Same handling as Tick().
            switch (state_) {
                case State::kPttArmed:
                    BeginRecordingOrRefuse(now_ms);
                    return;
                case State::kRecording:
                    // The hard cap. The hold is still in progress, so
                    // ptt_held_ stays true and the release that eventually
                    // arrives finds the machine already waiting.
                    FinishRecording(now_ms);
                    return;
                case State::kWaitingResponse:
                    Fail(Reason::kResponseTimedOut, now_ms);
                    return;
                case State::kPlaying:
                    if (hooks_.playback_stop) hooks_.playback_stop();
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kError:
                case State::kMuted:
                    Enter(State::kIdle, now_ms);
                    return;
                case State::kIdle:
                    return;
            }
            return;
        }
    }
}

void AudioFsm::Tick(uint64_t now_ms) {
    const uint64_t elapsed = TimeInState(now_ms);
    switch (state_) {
        case State::kPttArmed:
            if (elapsed >= config_.arm_threshold_ms) {
                Handle(Event::kTimeout, now_ms);
            }
            return;
        case State::kRecording:
            if (elapsed >= config_.max_recording_ms) {
                Handle(Event::kTimeout, now_ms);
            }
            return;
        case State::kWaitingResponse:
            if (elapsed >= config_.response_timeout_ms) {
                Handle(Event::kTimeout, now_ms);
            }
            return;
        case State::kPlaying:
            if (elapsed >= config_.max_playback_ms) {
                Handle(Event::kTimeout, now_ms);
            }
            return;
        case State::kError:
            if (elapsed >= config_.error_linger_ms) {
                Handle(Event::kTimeout, now_ms);
            }
            return;
        case State::kIdle:
        case State::kMuted:
            return;
    }
}

const char* StateName(State state) {
    switch (state) {
        case State::kIdle:            return "Idle";
        case State::kPttArmed:        return "PttArmed";
        case State::kRecording:       return "Recording";
        case State::kWaitingResponse: return "WaitingResponse";
        case State::kPlaying:         return "Playing";
        case State::kMuted:           return "Muted";
        case State::kError:           return "Error";
    }
    return "unknown";
}

const char* EarconName(Earcon earcon) {
    switch (earcon) {
        case Earcon::kAcknowledge:   return "acknowledge";
        case Earcon::kListenStart:   return "listen_start";
        case Earcon::kListenStop:    return "listen_stop";
        case Earcon::kResponseReady: return "response_ready";
        case Earcon::kError:         return "error";
        case Earcon::kMuted:         return "muted";
    }
    return "unknown";
}

const char* ReasonName(Reason reason) {
    switch (reason) {
        case Reason::kNone:                 return "none";
        case Reason::kMuted:                return "software mute is on";
        case Reason::kVoiceDisabled:        return "voice path not compiled in";
        case Reason::kTransportUnavailable: return "no network or no hub configured";
        case Reason::kUtteranceTooShort:    return "utterance below the minimum length";
        case Reason::kResponseTimedOut:     return "hub did not answer in time";
        case Reason::kUploadFailed:         return "upload failed";
    }
    return "unknown";
}

const char* EventName(Event event) {
    switch (event) {
        case Event::kPttPressed:      return "PttPressed";
        case Event::kPttReleased:     return "PttReleased";
        case Event::kCancel:          return "Cancel";
        case Event::kResponseReady:   return "ResponseReady";
        case Event::kResponseAnswered:return "ResponseAnswered";
        case Event::kResponseFailed:  return "ResponseFailed";
        case Event::kPlaybackFinished:return "PlaybackFinished";
        case Event::kMuteToggled:     return "MuteToggled";
        case Event::kTimeout:         return "Timeout";
    }
    return "unknown";
}

}  // namespace audio_ui
