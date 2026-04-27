/*
 * Kanaha Audio
 * Shared Audio Utilities - Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * WAV file reader shared by both the whisper bridge (speech-to-text) and
 * the YAMNet bridge (audio event detection). Both models need the same
 * float32 mono PCM input — this avoids duplicating the WAV parsing code.
 *
 * Extracted from whisper_android_bridge.c.
 */

#include "audio_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "AudioUtil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGD(...) do { fprintf(stderr, "[DEBUG] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/* ========================================================================
 * WAV file reader
 *
 * Reads a standard WAV file (RIFF/WAVE container) with 16-bit signed
 * integer PCM samples. Converts to float32 in range [-1.0, 1.0].
 *
 * If the file is stereo (or more channels), channels are averaged to
 * produce mono output. Both whisper.cpp and YAMNet expect 16kHz mono.
 * For best results, use ffmpeg to convert before processing:
 *   ffmpeg -i input.mp4 -ar 16000 -ac 1 -c:a pcm_s16le output.wav
 *
 * WAV format reference (bytes from file start):
 *   0-3:   "RIFF"
 *   4-7:   file size - 8
 *   8-11:  "WAVE"
 *   12+:   chunks (fmt, data, LIST, etc.)
 *
 * The fmt chunk contains: audio format (1=PCM), channels, sample rate,
 * byte rate, block align, bits per sample. We only accept format=1 (PCM)
 * and bits_per_sample=16.
 *
 * Returns heap-allocated float32 array (caller must free), or NULL on error.
 * ======================================================================== */

float *read_wav_pcm16(const char *path, int *out_samples, int *out_sample_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("Cannot open WAV file: %s", path);
        return NULL;
    }

    /* -- Verify RIFF/WAVE header -- */
    char riff[4];
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) {
        LOGE("Not a RIFF file: %s", path);
        fclose(f);
        return NULL;
    }

    fseek(f, 4, SEEK_CUR);  /* Skip file size (we don't need it) */

    char wave[4];
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) {
        LOGE("Not a WAVE file: %s", path);
        fclose(f);
        return NULL;
    }

    /* -- Walk chunks looking for "fmt " and "data" -- */
    int16_t num_channels = 0;
    int32_t sample_rate = 0;
    int16_t bits_per_sample = 0;
    int32_t data_size = 0;
    int found_fmt = 0, found_data = 0;

    while (!found_data) {
        char chunk_id[4];
        int32_t chunk_size;

        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            /*
             * fmt chunk layout (minimum 16 bytes):
             *   0-1:   audio format (1 = PCM, 3 = IEEE float, etc.)
             *   2-3:   number of channels
             *   4-7:   sample rate (Hz)
             *   8-11:  byte rate (sample_rate * channels * bits/8)
             *   12-13: block align (channels * bits/8)
             *   14-15: bits per sample
             */
            int16_t audio_format;
            if (fread(&audio_format, 2, 1, f) != 1) break;
            if (fread(&num_channels, 2, 1, f) != 1) break;
            if (fread(&sample_rate, 4, 1, f) != 1) break;
            fseek(f, 6, SEEK_CUR);  /* skip byte_rate (4) + block_align (2) */
            if (fread(&bits_per_sample, 2, 1, f) != 1) break;

            /* Some WAV files have extra fmt bytes (e.g., cbSize for extensible format) */
            int remaining = chunk_size - 16;
            if (remaining > 0) fseek(f, remaining, SEEK_CUR);

            if (audio_format != 1) {
                LOGE("WAV is not PCM format (format=%d, expected 1)", audio_format);
                fclose(f);
                return NULL;
            }
            found_fmt = 1;

        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = 1;

        } else {
            /* Skip unknown/unneeded chunks (LIST, bext, fact, etc.) */
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        LOGE("WAV missing fmt or data chunk: %s", path);
        fclose(f);
        return NULL;
    }

    if (bits_per_sample != 16) {
        LOGE("WAV must be 16-bit PCM (got %d-bit): %s", bits_per_sample, path);
        fclose(f);
        return NULL;
    }

    /* Calculate number of samples per channel */
    int bytes_per_sample = num_channels * (bits_per_sample / 8);
    if (bytes_per_sample == 0 || num_channels == 0) {
        LOGE("Invalid WAV: num_channels=%d, bits_per_sample=%d", num_channels, bits_per_sample);
        fclose(f);
        return NULL;
    }
    int n_samples_per_channel = data_size / bytes_per_sample;

    LOGI("WAV: %d Hz, %d ch, %d samples (%.1f sec)",
         sample_rate, num_channels, n_samples_per_channel,
         (float)n_samples_per_channel / sample_rate);

    /* -- Read raw 16-bit PCM sample data -- */
    int16_t *pcm16 = (int16_t *)malloc(data_size);
    if (!pcm16) {
        LOGE("Failed to allocate %d bytes for PCM data", data_size);
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(pcm16, 1, data_size, f);
    fclose(f);

    if ((int32_t)read_bytes != data_size) {
        LOGE("WAV truncated: expected %d bytes, got %zu", data_size, read_bytes);
        free(pcm16);
        return NULL;
    }

    /* -- Convert int16 -> float32, mix to mono if stereo --
     *
     * Both whisper.cpp and YAMNet expect float32 samples in [-1.0, 1.0].
     * int16 range is [-32768, 32767], so we divide by 32768.0.
     *
     * For multi-channel audio, we average all channels to produce mono.
     * This is the standard downmix approach — good enough for speech and
     * instrument detection. A presentation recording from a phone mic is
     * almost always mono anyway. */
    float *pcmf32 = (float *)malloc(n_samples_per_channel * sizeof(float));
    if (!pcmf32) {
        LOGE("Failed to allocate float32 buffer");
        free(pcm16);
        return NULL;
    }

    for (int i = 0; i < n_samples_per_channel; i++) {
        if (num_channels == 1) {
            pcmf32[i] = (float)pcm16[i] / 32768.0f;
        } else {
            float sum = 0.0f;
            for (int c = 0; c < num_channels; c++) {
                sum += (float)pcm16[i * num_channels + c];
            }
            pcmf32[i] = sum / (32768.0f * num_channels);
        }
    }

    free(pcm16);

    *out_samples = n_samples_per_channel;
    *out_sample_rate = sample_rate;
    return pcmf32;  /* Caller must free() this */
}
