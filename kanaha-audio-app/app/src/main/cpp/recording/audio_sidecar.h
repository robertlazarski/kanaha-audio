/*
 * Kanaha Audio
 * Recording Sidecar JSON — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Writes a sidecar JSON file alongside each WAV recording.
 * Contains recording_start_ms (for trim arithmetic), sample rate,
 * and GPS coordinates (if available from LocationService).
 *
 * Functionally equivalent to Kanaha Camera's kanaha_recording_start.json
 * but independently implemented under Apache 2.0.
 *
 * LIMITS:
 *   - Output path must be writable (within app sandbox)
 *   - GPS fields are omitted if LocationService hasn't written yet
 *   - Atomic write (tmp + rename) requires same-filesystem paths
 *   - clip_name is not JSON-escaped (caller must sanitize)
 */

#ifndef AUDIO_SIDECAR_H
#define AUDIO_SIDECAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write a sidecar JSON file for a recording.
 * Output filename: <audio_dir>/kanaha_audio_recording_start.json
 *
 * Written atomically (write to .tmp then rename) to prevent partial reads.
 *
 * @param audio_dir         Directory containing the WAV file
 * @param clip_name         Recording clip name
 * @param recording_start_ms  Unix epoch ms when recording started
 * @param sample_rate       Sample rate in Hz
 * @param channels          Number of audio channels
 * @return 0 on success, -1 on error
 */
int audio_sidecar_write(const char *audio_dir,
                        const char *clip_name,
                        int64_t recording_start_ms,
                        int sample_rate,
                        int channels);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SIDECAR_H */
