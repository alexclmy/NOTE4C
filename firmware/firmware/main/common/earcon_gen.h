/**
 * @file earcon_gen.h
 * @brief Synthesized earcon tones. No assets, no network, no speech.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Six short tones generated on the device at 16 kHz mono signed 16-bit, the
 * format the ES8311 codec is already opened with
 * (main/boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc:82-95).
 *
 * Generated rather than shipped as files because the alternative costs flash
 * this device does not have: partitions/v2/16m.csv allocates all 16 MB with no
 * free space, which is the same constraint that blocks a wake word model
 * (PRODUCT-PLAN.md section 7). A sine and an envelope cost a few hundred bytes
 * of code.
 *
 * Every tone is amplitude-ramped at both ends. An abrupt start or stop on a
 * class-D amplifier is a click, and on this board the amp pin has two owners
 * (GPIO46, hardware gate HG5), so the waveform is kept as gentle as it can be
 * before anyone puts a speaker on it.
 *
 * Nothing here has been heard on hardware. It is host-tested arithmetic.
 */

#ifndef COMMON_EARCON_GEN_H
#define COMMON_EARCON_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "audio_fsm.h"

namespace earcon {

/// The codec's rate. Callers that resample are on their own.
constexpr uint32_t kSampleRate = 16000;

/// Longest earcon, in samples, so callers can size a static buffer.
constexpr size_t kMaxSamples = kSampleRate * 3 / 4;  // 750 ms

struct Tone {
    /// Frequency in Hz. Zero is silence, used for the gap in two-note tones.
    uint16_t frequency_hz;
    uint16_t duration_ms;
};

/// A named earcon: up to three notes played back to back.
struct Spec {
    Tone notes[3];
    uint8_t note_count;
    /// Peak amplitude, 0 to 32767. Kept well below full scale: these are
    /// acknowledgements, not alerts.
    int16_t amplitude;
};

/// The spec for one earcon. Total duration never exceeds kMaxSamples.
Spec SpecFor(audio_ui::Earcon earcon);

/// Samples one earcon occupies at kSampleRate.
size_t SampleCount(audio_ui::Earcon earcon);

/**
 * @brief Render @p earcon into @p out as signed 16-bit mono.
 *
 * @param out      destination, at least SampleCount(earcon) entries.
 * @param capacity entries available in @p out.
 * @return samples written, or 0 if @p out is null or too small. Never writes
 *         past @p capacity and never writes a partial tone.
 */
size_t Render(audio_ui::Earcon earcon, int16_t* out, size_t capacity);

}  // namespace earcon

#endif  // COMMON_EARCON_GEN_H
