/**
 * @file voice_wakeup.h
 * @brief Voice wakeup listener with BOOT button long-press trigger
 *
 * Listens for long-press of BOOT button (>= 1s) to trigger voice recording.
 * Shows a floating overlay with recording status.
 * Displays offline message when no network is available.
 *
 * Usage:
 * 1. VoiceWakeupInit() — initialize state
 * 2. VoiceWakeupRegisterButton() — register with BOOT button (long-press callback)
 * 3. VoiceWakeupTick() — call from main loop to update overlay state
 * 4. VoiceWakeupDraw() — draw overlay to framebuffer when active
 *
 * State machine:
 *   IDLE → BOOT long-press → CHECKING_NETWORK
 *          → online → RECORDING → WAITING_RESPONSE → DONE → IDLE
 *          → offline → OFFLINE_MSG → IDLE
 */

#ifndef RAWDRAW_VOICE_WAKEUP_H
#define RAWDRAW_VOICE_WAKEUP_H

#include <stdint.h>
#include <stdbool.h>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"
#include "refresh.h"

namespace rawdraw {

/**
 * @brief Voice wakeup states
 */
enum class VoiceState {
    IDLE,              ///< Not active
    CHECKING_NETWORK,  ///< Checking network availability
    RECORDING,         ///< Recording audio
    WAITING_RESPONSE,  ///< Waiting for server response
    OFFLINE_MSG,       ///< Showing offline message
    DONE,              ///< Completed, fading out
};

/**
 * @brief Voice wakeup overlay state
 *
 * Maintains current state, overlay text, and refresh tracking.
 */
struct VoiceWakeupState {
    VoiceState state;                     ///< Current state
    RegionRefresh refresh;                ///< Independent refresh counter
    const lv_font_t* font;                ///< Text font
    char overlay_text[64];                ///< Current overlay text
    int64_t state_start_us;               ///< When current state started
    bool visible;                         ///< Overlay is visible
};

/**
 * @brief Overlay position and size constants
 */
constexpr int kVoiceOverlayX       = 140;  ///< Center-ish X (avoid clock zone)
constexpr int kVoiceOverlayY       = 120;  ///< Middle of screen
constexpr int kVoiceOverlayW       = 120;  ///< Width
constexpr int kVoiceOverlayH       = 40;   ///< Height
constexpr int kVoiceOfflineMsgMs   = 3000; ///< Offline message display duration
constexpr int kVoiceDoneFadeMs     = 1500; ///< Done state display duration

/**
 * @brief Initialize voice wakeup state
 *
 * @param state Voice wakeup state pointer
 * @param font Font for overlay text
 */
void VoiceWakeupInit(VoiceWakeupState* state, const lv_font_t* font = &BUILTIN_TEXT_FONT);

/**
 * @brief Transition to recording state
 *
 * Called when BOOT long-press is detected and network is available.
 *
 * @param state Voice wakeup state pointer
 */
void VoiceWakeupStartRecording(VoiceWakeupState* state);

/**
 * @brief Transition to waiting-for-response state
 *
 * @param state Voice wakeup state pointer
 */
void VoiceWakeupWaiting(VoiceWakeupState* state);

/**
 * @brief Transition to offline message state
 *
 * @param state Voice wakeup state pointer
 */
void VoiceWakeupShowOffline(VoiceWakeupState* state);

/**
 * @brief Transition to done state
 *
 * @param state Voice wakeup state pointer
 */
void VoiceWakeupDone(VoiceWakeupState* state);

/**
 * @brief Reset to idle state
 *
 * @param state Voice wakeup state pointer
 */
void VoiceWakeupReset(VoiceWakeupState* state);

/**
 * @brief Check if overlay is currently visible
 */
bool VoiceWakeupIsVisible(const VoiceWakeupState* state);

/**
 * @brief Update state machine (check timeouts, transitions)
 *
 * Call from main loop or timer. Handles auto-transition from
 * OFFLINE_MSG and DONE states back to IDLE.
 *
 * @param state Voice wakeup state pointer
 * @param now_us Current timestamp in microseconds
 */
void VoiceWakeupTick(VoiceWakeupState* state, int64_t now_us);

/**
 * @brief Draw voice wakeup overlay to framebuffer
 *
 * Only draws if visible. Shows state-appropriate text:
 * - RECORDING: "录音中..."
 * - WAITING_RESPONSE: "处理中..."
 * - OFFLINE_MSG: "离线不可用"
 * - DONE: "完成"
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param height Framebuffer height
 * @return true if overlay was drawn, false if hidden
 */
bool VoiceWakeupDraw(uint8_t* fb, int width, int height, VoiceWakeupState* state);

/**
 * @brief Draw using Framebuffer wrapper
 */
bool VoiceWakeupDrawFb(Framebuffer* fb, int screen_width, int screen_height, VoiceWakeupState* state);

/**
 * @brief Get overlay bounding rectangle
 */
Rect VoiceWakeupGetBounds();

/**
 * @brief Get current state as human-readable string
 */
const char* VoiceStateToString(VoiceState state);

}  // namespace rawdraw

#endif  // RAWDRAW_VOICE_WAKEUP_H
