/*
 * Kanaha Audio
 * Shared Audio Utilities - Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Shared WAV file reader used by both the whisper bridge (speech) and
 * the YAMNet bridge (audio event detection). Extracted from
 * whisper_android_bridge.c to avoid code duplication.
 */

#ifndef AUDIO_UTIL_H
#define AUDIO_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read a WAV file and convert to float32 mono.
 *
 * Reads a standard WAV file (RIFF/WAVE container) with 16-bit signed
 * integer PCM samples. Converts to float32 in range [-1.0, 1.0].
 * Multi-channel audio is averaged to mono.
 *
 * @param path            Path to the WAV file
 * @param out_samples     Output: number of samples (per channel)
 * @param out_sample_rate Output: sample rate in Hz
 * @return Heap-allocated float32 array (caller must free), or NULL on error
 */
float *read_wav_pcm16(const char *path, int *out_samples, int *out_sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_UTIL_H */
