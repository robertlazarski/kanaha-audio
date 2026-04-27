/*
 * Kanaha Audio
 * LTC (Linear Timecode) Decoder — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Decodes SMPTE LTC timecode from WAV recordings using libltc.
 * Returns timecode frames as JSON — the on-device equivalent of ltcdump.
 *
 * Use case: Record LTC from a Tentacle Sync unit via iRig, then decode
 * the timecode on-device to get frame-accurate timestamps for video editing.
 *
 * LIMITS:
 *   - Input must be mono or stereo WAV (PCM 16-bit)
 *   - Supported sample rates: 44100, 48000 (LTC needs ≥44.1kHz)
 *   - Maximum frames returned: 65536 (about 36 minutes at 30fps)
 *   - libltc handles forward and reverse LTC
 */

#ifndef LTC_DECODER_H
#define LTC_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single decoded LTC frame */
typedef struct {
    int hours;
    int minutes;
    int seconds;
    int frames;
    int64_t sample_start;
    int64_t sample_end;
    int discontinuity;    /* 1 if there was a gap before this frame */
} ltc_frame_info_t;

/* Decode result */
typedef struct {
    int success;
    int total_frames;
    float fps;
    int64_t duration_samples;
    int sample_rate;
    char first_tc[16];    /* "HH:MM:SS:FF" */
    char last_tc[16];     /* "HH:MM:SS:FF" */
    ltc_frame_info_t *frames;  /* Caller must free */
    char error[256];
} ltc_decode_result_t;

/**
 * Decode LTC timecode from a WAV file.
 *
 * @param wav_path   Path to WAV file containing LTC audio
 * @param channel    Audio channel to decode (1-based, default 1)
 * @param result     Output: decode result (caller must free result->frames)
 * @return 0 on success, -1 on error
 */
int ltc_decode_wav(const char *wav_path, int channel, ltc_decode_result_t *result);

/**
 * Free frames allocated by ltc_decode_wav.
 */
void ltc_decode_result_free(ltc_decode_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* LTC_DECODER_H */
