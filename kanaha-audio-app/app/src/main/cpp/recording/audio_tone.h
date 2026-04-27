/*
 * Kanaha Audio
 * Tone Playback via AAudio NDK — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Pure C tone generation using AAudio output stream.
 * Generates sine wave tones through the device speaker for
 * multi-device synchronization (slate/clap equivalent).
 *
 * Supports start_at scheduling via clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)
 * for kernel-level precision — no message queue jitter.
 *
 * LIMITS:
 *   - Frequency: 1–20000 Hz (audible range)
 *   - Duration: 1–30000 ms (30 seconds max)
 *   - Output sample rate: 44100 Hz (fixed, for speaker quality)
 *   - Amplitude: ~73% of int16 max (headroom to avoid speaker clipping)
 *   - Concurrent tones: each playTone call spawns a detached thread;
 *     multiple tones can play simultaneously (AAudio handles mixing)
 *   - AAudio requires Android API 28+
 */

#ifndef AUDIO_TONE_H
#define AUDIO_TONE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Play a sine wave tone through the device speaker.
 *
 * If start_at_ms is non-zero, the tone is scheduled for that absolute time
 * using clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME). This provides
 * kernel-level scheduling precision for multi-device sync.
 *
 * @param frequency    Tone frequency in Hz (e.g., 1000)
 * @param duration_ms  Tone duration in milliseconds
 * @param start_at_ms  Unix epoch ms to start tone. 0 = start immediately
 * @return 0 on success, -1 on error
 */
int audio_tone_play(int frequency, int duration_ms, int64_t start_at_ms);

/**
 * Play a WAV audio file through the device speaker.
 *
 * Reads PCM samples from a WAV file and plays them via AAudio output.
 * Supports 16-bit mono or stereo WAV at any sample rate.
 *
 * @param wav_path  Path to WAV file
 * @return 0 on success, -1 on error
 */
int audio_tone_play_file(const char *wav_path);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_TONE_H */
