/**
 * @file earcon_gen.cc
 * @brief Implementation of the earcon tone generator.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "earcon_gen.h"

#include <math.h>

namespace earcon {

namespace {

/// Ramp length at each end of a note. 5 ms is long enough to remove the click
/// and short enough that the tone still reads as immediate.
constexpr uint32_t kRampMs = 5;

constexpr double kPi = 3.14159265358979323846;

int16_t Clamp16(double value) {
    if (value > 32767.0) return 32767;
    if (value < -32768.0) return -32768;
    return static_cast<int16_t>(value);
}

}  // namespace

Spec SpecFor(audio_ui::Earcon earcon) {
    switch (earcon) {
        case audio_ui::Earcon::kAcknowledge:
            // One short mid tone: "the button did something".
            return Spec{{{880, 80}, {0, 0}, {0, 0}}, 1, 9000};
        case audio_ui::Earcon::kListenStart:
            // Rising pair. Up means opening.
            return Spec{{{660, 70}, {990, 90}, {0, 0}}, 2, 10000};
        case audio_ui::Earcon::kListenStop:
            // The same pair falling, so start and stop cannot be confused.
            return Spec{{{990, 70}, {660, 90}, {0, 0}}, 2, 10000};
        case audio_ui::Earcon::kResponseReady:
            // Three rising notes, clearly distinct from the listen pair.
            return Spec{{{660, 70}, {880, 70}, {1320, 110}}, 3, 10000};
        case audio_ui::Earcon::kError:
            // Low, with a gap, twice. Low and repeated reads as a problem.
            return Spec{{{330, 120}, {0, 60}, {330, 160}}, 3, 11000};
        case audio_ui::Earcon::kMuted:
            // A single low blip. Short, so a muted device is not annoying.
            return Spec{{{392, 90}, {0, 0}, {0, 0}}, 1, 7000};
    }
    return Spec{{{0, 0}, {0, 0}, {0, 0}}, 0, 0};
}

size_t SampleCount(audio_ui::Earcon earcon) {
    const Spec spec = SpecFor(earcon);
    uint32_t total_ms = 0;
    for (uint8_t i = 0; i < spec.note_count; ++i) {
        total_ms += spec.notes[i].duration_ms;
    }
    return static_cast<size_t>(total_ms) * kSampleRate / 1000u;
}

size_t Render(audio_ui::Earcon earcon, int16_t* out, size_t capacity) {
    if (out == nullptr) {
        return 0;
    }
    const size_t needed = SampleCount(earcon);
    if (needed == 0 || capacity < needed) {
        // Partial earcons are worse than none: a truncated tone is a click.
        return 0;
    }

    const Spec spec = SpecFor(earcon);
    const size_t ramp = static_cast<size_t>(kRampMs) * kSampleRate / 1000u;
    size_t written = 0;

    for (uint8_t n = 0; n < spec.note_count; ++n) {
        const Tone& note = spec.notes[n];
        const size_t samples =
            static_cast<size_t>(note.duration_ms) * kSampleRate / 1000u;
        if (note.frequency_hz == 0) {
            for (size_t i = 0; i < samples; ++i) {
                out[written++] = 0;
            }
            continue;
        }
        const double step = 2.0 * kPi * static_cast<double>(note.frequency_hz) /
                            static_cast<double>(kSampleRate);
        for (size_t i = 0; i < samples; ++i) {
            // Linear fade in and out. With a note shorter than two ramps the
            // envelope simply never reaches full amplitude, which is fine.
            double envelope = 1.0;
            if (ramp > 0) {
                if (i < ramp) {
                    envelope = static_cast<double>(i) / static_cast<double>(ramp);
                }
                const size_t from_end = samples - 1 - i;
                if (from_end < ramp) {
                    const double tail =
                        static_cast<double>(from_end) / static_cast<double>(ramp);
                    if (tail < envelope) envelope = tail;
                }
            }
            const double value =
                sin(step * static_cast<double>(i)) * envelope *
                static_cast<double>(spec.amplitude);
            out[written++] = Clamp16(value);
        }
    }
    return written;
}

}  // namespace earcon
