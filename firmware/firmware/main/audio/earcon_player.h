/**
 * @file earcon_player.h
 * @brief Renders and plays the synthesised earcons on a task of their own.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The tones themselves are arithmetic (main/common/earcon_gen.h) and are host
 * tested. This is the part that cannot be: a FreeRTOS queue, a task, and a
 * call into AudioService.
 *
 * Why a task rather than rendering in the hook
 * --------------------------------------------
 * The audio state machine's hooks run on the button task. The longest earcon
 * is 750 ms of 16 kHz mono, 24000 bytes, and pushing it through the codec
 * means enabling the output, which takes the shared I2C bus lock. None of that
 * belongs on the path that also has to notice the button being released.
 *
 * So Request() does one non-blocking queue send and returns. If the queue is
 * full the tone is dropped and counted. A dropped acknowledgement is a
 * disappointment; a button task blocked behind the amplifier is a device that
 * looks broken.
 *
 * Nothing here has been heard. Whether the speaker makes any sound at all is
 * HG5.1 in docs/HARDWARE-ACCEPTANCE.md.
 */

#ifndef AUDIO_EARCON_PLAYER_H
#define AUDIO_EARCON_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <vector>

#include "common/audio_fsm.h"

class AudioService;

class EarconPlayer {
public:
    EarconPlayer() = default;
    ~EarconPlayer();

    EarconPlayer(const EarconPlayer&) = delete;
    EarconPlayer& operator=(const EarconPlayer&) = delete;

    /// Create the queue and the task. @p service must outlive this object.
    bool Start(AudioService* service);
    void Stop();

    /**
     * @brief Ask for one tone. Safe from any task, never blocks.
     * @return false if the queue was full and the tone was dropped.
     */
    bool Request(audio_ui::Earcon earcon);

    /// How many tones were dropped because the queue was full. For logs.
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static void TaskEntry(void* arg);
    void Run();

    /// Short. A backlog of acknowledgements is not worth playing: by the time
    /// the fourth one comes out the user has pressed the button again.
    static constexpr UBaseType_t kQueueDepth = 4;

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    AudioService* service_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> dropped_{0};
    /// Reused across tones so a tone costs no allocation once running.
    std::vector<int16_t> scratch_;
};

#endif  // AUDIO_EARCON_PLAYER_H
