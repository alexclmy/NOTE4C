/**
 * @file voice_capture.h
 * @brief Drains encoded microphone frames into one bounded utterance.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The container and its bounds are portable and host tested
 * (main/common/utterance_buffer.h). This is the glue: a task that pulls Opus
 * packets out of `AudioService`'s send queue while a recording is in progress.
 *
 * Why a task at all
 * -----------------
 * `AudioService`'s send queue holds 100 packets, six seconds at 60 ms a frame.
 * The recording cap is fifteen. Left undrained, the queue fills, the encoder
 * stalls waiting for room, the input task stalls waiting for the encoder, and
 * the utterance is silently truncated at six seconds with nothing anywhere
 * saying so. Draining continuously is what makes the fifteen-second cap real.
 *
 * Why the finish is not synchronous
 * ---------------------------------
 * `AudioFsm`'s mic_stop hook runs on the button task, and at the moment it
 * runs there are still frames inside the encoder. Serialising there would drop
 * the end of every sentence. Instead the task keeps draining for a short
 * settle window after the microphone closes, then finalises the buffer and
 * hands it to the callback. The user interface does not wait for any of this:
 * the state machine is already in kWaitingResponse.
 *
 * None of this has run on hardware. Whether the microphone captures anything
 * at all is HG5.4 in docs/HARDWARE-ACCEPTANCE.md.
 */

#ifndef COMMON_VOICE_CAPTURE_H
#define COMMON_VOICE_CAPTURE_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <vector>

#include "utterance_buffer.h"

class AudioService;

namespace voice {

/// What one finished capture produced.
struct CaptureResult {
    std::vector<uint8_t> body;
    uint32_t duration_ms = 0;
    uint32_t frames = 0;
    /// A cap was reached and frames were dropped. Still worth uploading.
    bool truncated = false;
};

class VoiceCapture {
public:
    VoiceCapture() = default;
    ~VoiceCapture();

    VoiceCapture(const VoiceCapture&) = delete;
    VoiceCapture& operator=(const VoiceCapture&) = delete;

    /// Called on the capture task when a recording has been fully drained.
    /// An empty body means nothing was captured; the caller decides what to
    /// tell the user, and must not treat it as an upload that failed.
    using DoneCallback = std::function<void(CaptureResult&&)>;

    bool Start(AudioService* service, DoneCallback on_done);
    void Stop();

    /// Open the codec input and start accumulating. Safe from the button task.
    void BeginUtterance();
    /// Close the codec input; the callback follows once the encoder has drained.
    void EndUtterance();
    /// Close the codec input and throw the audio away. No callback follows.
    void Discard();

    bool recording() const { return recording_.load(std::memory_order_acquire); }

private:
    enum class Phase : uint8_t {
        kIdle = 0,
        /// Waiting out the listen-start tone before opening the microphone.
        kToneGuard,
        kRecording,
        kSettling,
        kDiscarding,
    };

    static void TaskEntry(void* arg);
    void Run();
    /// Move whatever the encoder has produced into the buffer.
    /// @return how many frames were taken.
    uint32_t DrainOnce();

    /**
     * @brief How long to keep draining after the microphone closes.
     *
     * Two 60 ms frames can be in the encoder when the button comes up, and the
     * Opus task is at a lower priority than the input task, so 300 ms is
     * several times the expected worst case without being long enough to be
     * noticed as latency.
     */
    static constexpr uint32_t kSettleMs = 300;
    static constexpr uint32_t kPollMs = 20;

    /**
     * @brief How long to wait after the listen-start tone before recording.
     *
     * Two problems, one fix. This board has one microphone and no acoustic
     * echo cancellation reference, so a tone playing while the microphone is
     * open is a tone in the recording. And `EnableVoiceProcessing(true)` calls
     * `ResetDecoder()`, which empties the playback queue: opening the input
     * immediately would cut the tone off mid-note, which on a class-D
     * amplifier is a click.
     *
     * The listen-start earcon is 160 ms (`earcon_gen.cc`, 70 plus 90). 200 ms
     * covers it with room for the queue hop. The cost is that the microphone
     * opens about 525 ms after the button goes down rather than 325 ms, and
     * that `audio_ui::Config::min_recording_ms` has to be larger than this or
     * a short hold would upload an empty container.
     */
    static constexpr uint32_t kToneGuardMs = 200;

    AudioService* service_ = nullptr;
    DoneCallback on_done_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> recording_{false};
    std::atomic<Phase> phase_{Phase::kIdle};
    /// Owned by the capture task once running; only it touches the buffer.
    UtteranceBuffer buffer_;
};

}  // namespace voice

#endif  // COMMON_VOICE_CAPTURE_H
