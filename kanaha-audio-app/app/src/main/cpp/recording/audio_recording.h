/*
 * Kanaha Audio
 * Audio Recording via AAudio NDK — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Pure C audio capture using the AAudio NDK API (API 28+).
 * Runs directly from the native httpd process — no JNI, no Intent IPC.
 *
 * Architecture:
 *   AAudio input stream (mic) → data callback → SPSC ring buffer
 *     → writer pthread → WAV file on disk
 *
 * Default: 16kHz mono PCM_I16 (matches whisper.cpp input).
 * Optional: 44100 Hz for high-quality recording.
 *
 * LIMITS:
 *   - Maximum file size: ~4 GB (WAV uses uint32 for data size field)
 *     At 16 kHz mono 16-bit: ~18.6 hours max recording
 *     At 44.1 kHz mono 16-bit: ~6.7 hours max recording
 *     Beyond this limit, data_size is capped and a warning is logged.
 *     For longer recordings, RF64 format would be needed (not implemented).
 *   - Maximum clip name: 128 characters (AUDIO_RECORDING_MAX_CLIP_NAME)
 *   - Clip name must not contain '/' or '..' (path traversal rejected)
 *   - Only one recording can be active at a time (single microphone)
 *   - Sample rates: 16000 Hz (default) or 44100 Hz only
 *   - Ring buffer: ~2 seconds at 16 kHz (32768 frames). If the disk
 *     writer falls behind by >2 seconds, audio frames are dropped.
 *   - start_at scheduling: returns immediately (detached thread sleeps);
 *     recording_is_active() returns false until the scheduled time arrives
 *   - AAudio requires Android API 28+ (Pixel 9 Pro is API 35)
 *   - RECORD_AUDIO runtime permission required (graceful failure if denied)
 */

#ifndef AUDIO_RECORDING_H
#define AUDIO_RECORDING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum clip name length (excluding NUL) */
#define AUDIO_RECORDING_MAX_CLIP_NAME  128

/* Recording state */
typedef enum {
    AUDIO_RECORDING_IDLE = 0,
    AUDIO_RECORDING_ACTIVE,
    AUDIO_RECORDING_STOPPING,
    AUDIO_RECORDING_SCHEDULED   /* start requested; not yet capturing */
} audio_recording_state_t;

/**
 * Initialize the recording subsystem.
 * Must be called once at startup before any recording operations.
 *
 * @param audio_dir  Directory for WAV file output (e.g., /data/data/org.kanaha.audio/files/audio)
 * @return 0 on success, -1 on error
 */
int audio_recording_init(const char *audio_dir);

/**
 * Cleanup the recording subsystem.
 * Stops any active recording and releases resources.
 */
void audio_recording_cleanup(void);

/**
 * Start recording audio from the device microphone.
 *
 * @param clip_name    Name for the recording (used as WAV filename prefix)
 * @param sample_rate  Sample rate in Hz (16000 or 44100). 0 = default (16000)
 * @param start_at_ms  Unix epoch ms to start recording. 0 = start immediately
 * @return 0 on success, -1 on error
 */
int audio_recording_start(const char *clip_name, int sample_rate, int64_t start_at_ms);

/**
 * Stop the active recording.
 * Finalizes the WAV header and closes the file.
 *
 * @param out_duration_ms   Output: recording duration in milliseconds
 * @param out_file_size     Output: final WAV file size in bytes
 * @return 0 on success, -1 on error (e.g., no active recording)
 */
int audio_recording_stop(int64_t *out_duration_ms, int64_t *out_file_size);

/**
 * Check if recording is currently active.
 * @return 1 if recording, 0 if idle
 */
int audio_recording_is_active(void);

/**
 * Get the current clip name (if recording).
 * @return Clip name string, or NULL if not recording
 */
const char *audio_recording_get_clip_name(void);

/**
 * Get the audio output directory path.
 * @return Directory path, or NULL if not initialized
 */
const char *audio_recording_get_audio_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_RECORDING_H */
