/*
 * Kanaha Audio
 * Tone Playback via AAudio NDK — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Generates and plays sine wave tones through the device speaker using
 * the AAudio C API. Used for multi-device synchronization — the audio
 * equivalent of a film slate/clapper.
 *
 * Supports start_at scheduling via clock_nanosleep(CLOCK_REALTIME,
 * TIMER_ABSTIME) for kernel-level precision. This is more precise than
 * Java's Handler.postDelayed() which goes through a message queue.
 */

#include "audio_tone.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/log.h>
#define LOG_TAG "AudioTone"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Tone output sample rate: 44100 Hz for speaker quality */
#define TONE_SAMPLE_RATE  44100

/* ========================================================================
 * Tone generation state (per-play, passed via callback userData)
 * ======================================================================== */

typedef struct {
    int frequency;
    int total_frames;      /* Total frames to generate */
    int frames_rendered;   /* Frames rendered so far */
    double phase;          /* Current sine phase */
    double phase_increment;
} tone_state_t;

/* ========================================================================
 * AAudio data callback for tone output
 * ======================================================================== */

#ifdef __ANDROID__
static aaudio_data_callback_result_t tone_data_callback(
    AAudioStream *stream, void *userData,
    void *audioData, int32_t numFrames) {
    (void)stream;
    tone_state_t *state = (tone_state_t *)userData;
    int16_t *output = (int16_t *)audioData;

    int remaining = state->total_frames - state->frames_rendered;
    int to_render = (numFrames < remaining) ? numFrames : remaining;

    /* Generate sine wave samples.
     * Amplitude 24000 is ~73% of int16 max (32767) — loud enough to be
     * clearly audible and detectable by another device's microphone,
     * but leaves headroom to avoid clipping on speakers with gain. */
    for (int i = 0; i < to_render; i++) {
        double sample = sin(state->phase) * 24000.0;
        output[i] = (int16_t)sample;
        state->phase += state->phase_increment;
        /* Wrap phase to [0, 2*PI) to prevent floating-point drift
         * over long tones */
        if (state->phase >= 2.0 * M_PI) {
            state->phase -= 2.0 * M_PI;
        }
    }

    /* Zero-fill any remaining frames in this callback.
     * This happens only on the final callback when we've rendered
     * all requested frames but the buffer is larger. */
    for (int i = to_render; i < numFrames; i++) {
        output[i] = 0;
    }

    state->frames_rendered += to_render;

    if (state->frames_rendered >= state->total_frames) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif

/* ========================================================================
 * Thread function for tone playback (runs in background)
 * ======================================================================== */

typedef struct {
    int frequency;
    int duration_ms;
    int64_t start_at_ms;
} tone_thread_args_t;

static void *tone_thread_func(void *arg) {
    tone_thread_args_t *args = (tone_thread_args_t *)arg;

    /* Wait for start_at if specified */
    if (args->start_at_ms > 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

        if (args->start_at_ms > now_ms) {
            LOGI("Scheduling tone for %lld ms (delay: %lld ms)",
                 (long long)args->start_at_ms,
                 (long long)(args->start_at_ms - now_ms));

            struct timespec ts = {
                .tv_sec  = args->start_at_ms / 1000,
                .tv_nsec = (args->start_at_ms % 1000) * 1000000
            };
            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL);
        }
    }

#ifdef __ANDROID__
    tone_state_t state = {
        .frequency = args->frequency,
        .total_frames = (TONE_SAMPLE_RATE * args->duration_ms) / 1000,
        .frames_rendered = 0,
        .phase = 0.0,
        .phase_increment = 2.0 * M_PI * args->frequency / TONE_SAMPLE_RATE
    };

    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to create AAudio builder: %s", AAudio_convertResultToText(result));
        free(args);
        return NULL;
    }

    /* Configure AAudio output stream:
     *   - Direction: OUTPUT (speaker playback)
     *   - 44100 Hz: standard audio quality for speaker output
     *   - Mono: single channel (tone sync doesn't need stereo)
     *   - PCM_I16: matches recording format
     *   - Low latency: minimizes onset delay for sync precision
     *   - state lives on this thread's stack, safe because we join
     *     (via nanosleep + stop) before returning */
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, TONE_SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, tone_data_callback, &state);

    AAudioStream *stream = NULL;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to open tone output stream: %s", AAudio_convertResultToText(result));
        free(args);
        return NULL;
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        LOGE("Failed to start tone stream: %s", AAudio_convertResultToText(result));
        AAudioStream_close(stream);
        free(args);
        return NULL;
    }

    LOGI("Tone playing: %d Hz for %d ms", args->frequency, args->duration_ms);

    /* Wait for tone to complete.
     * Sleep slightly longer than duration to let callback finish. */
    struct timespec wait = {
        .tv_sec  = (args->duration_ms + 100) / 1000,
        .tv_nsec = ((args->duration_ms + 100) % 1000) * 1000000L
    };
    nanosleep(&wait, NULL);

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    LOGI("Tone finished: %d Hz, %d ms", args->frequency, args->duration_ms);
#else
    LOGI("Tone playback (non-Android stub): %d Hz, %d ms",
         args->frequency, args->duration_ms);
#endif

    free(args);
    return NULL;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int audio_tone_play(int frequency, int duration_ms, int64_t start_at_ms) {
    if (frequency <= 0 || frequency > 20000) {
        LOGE("Invalid frequency: %d (must be 1-20000 Hz)", frequency);
        return -1;
    }
    if (duration_ms <= 0 || duration_ms > 30000) {
        LOGE("Invalid duration: %d ms (must be 1-30000)", duration_ms);
        return -1;
    }

    tone_thread_args_t *args = malloc(sizeof(tone_thread_args_t));
    if (!args) {
        LOGE("Failed to allocate tone args");
        return -1;
    }
    args->frequency = frequency;
    args->duration_ms = duration_ms;
    args->start_at_ms = start_at_ms;

    /* Launch tone in a detached thread so the HTTP response returns immediately.
     *
     * Detached (not joinable) because:
     *   1. The caller (HTTP handler) should not block waiting for the tone
     *   2. The tone may be scheduled for a future start_at time
     *   3. The thread owns its args (heap-allocated, freed on exit)
     *
     * Thread lifecycle: sleep until start_at → open AAudio output stream →
     * play tone via callback → sleep for duration → close stream → exit */
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int rc = pthread_create(&thread, &attr, tone_thread_func, args);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        LOGE("Failed to create tone thread");
        free(args);
        return -1;
    }

    return 0;
}

/* ========================================================================
 * WAV File Playback
 * ======================================================================== */

/* State for WAV playback callback */
typedef struct {
    int16_t *samples;
    int32_t total_samples;
    int32_t position;
} wav_playback_state_t;

#ifdef __ANDROID__
static aaudio_data_callback_result_t wav_data_callback(
    AAudioStream *stream, void *userData,
    void *audioData, int32_t numFrames) {
    wav_playback_state_t *state = (wav_playback_state_t *)userData;
    int16_t *output = (int16_t *)audioData;

    for (int32_t i = 0; i < numFrames; i++) {
        if (state->position < state->total_samples) {
            output[i] = state->samples[state->position++];
        } else {
            output[i] = 0;
        }
    }

    if (state->position >= state->total_samples) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif

int audio_tone_play_file(const char *wav_path) {
    if (!wav_path || strstr(wav_path, "..") || strlen(wav_path) == 0) {
        LOGE("Invalid path");
        return -1;
    }

    /* Verify file exists before proceeding */
    struct stat wav_stat;
    if (stat(wav_path, &wav_stat) != 0) {
        LOGE("File not found: %s", wav_path);
        return -1;
    }

    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        LOGE("Cannot open: %s", wav_path);
        return -1;
    }

    /* Read WAV header */
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        LOGE("Invalid WAV: %s", wav_path);
        fclose(f);
        return -1;
    }

    int channels = hdr[22] | (hdr[23] << 8);
    int sample_rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    int bits = hdr[34] | (hdr[35] << 8);

    if (bits != 16) {
        LOGE("Unsupported bit depth: %d", bits);
        fclose(f);
        return -1;
    }

    /* Read all PCM data. The whole file is loaded, so its size is bounded:
     * a caller naming a large file must not be able to exhaust memory. */
    long file_size = (long)wav_stat.st_size;
    if (file_size < 44 || file_size > AUDIO_PLAY_MAX_BYTES) {
        LOGE("WAV size %ld outside [44, %d]", file_size, AUDIO_PLAY_MAX_BYTES);
        fclose(f);
        return -1;
    }
    fseek(f, 44, SEEK_SET);

    long data_bytes = file_size - 44;
    int32_t total_samples = (int32_t)(data_bytes / 2);  /* 16-bit = 2 bytes per sample */

    /* Mix to mono if stereo */
    int32_t mono_samples = (channels == 2) ? total_samples / 2 : total_samples;
    int16_t *samples = (int16_t *)malloc(mono_samples * sizeof(int16_t));
    if (!samples) {
        LOGE("Alloc failed for %d samples", mono_samples);
        fclose(f);
        return -1;
    }

    if (channels == 1) {
        size_t actually_read = fread(samples, 2, mono_samples, f);
        if (actually_read < (size_t)mono_samples) {
            LOGI("Short read: got %zu of %d samples", actually_read, mono_samples);
            mono_samples = (int32_t)actually_read;
        }
    } else {
        /* Read stereo, mix to mono */
        int16_t stereo[2];
        int32_t i;
        for (i = 0; i < mono_samples; i++) {
            if (fread(stereo, 2, 2, f) != 2) break;
            samples[i] = (int16_t)(((int32_t)stereo[0] + stereo[1]) / 2);
        }
        mono_samples = i;  /* actual samples read */
    }
    fclose(f);

    LOGI("Playing WAV: %s (%d Hz, %d ch, %d samples = %.1fs)",
         wav_path, sample_rate, channels, mono_samples,
         (float)mono_samples / sample_rate);

#ifdef __ANDROID__
    wav_playback_state_t state = {
        .samples = samples,
        .total_samples = mono_samples,
        .position = 0
    };

    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to create builder");
        free(samples);
        return -1;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, wav_data_callback, &state);

    AAudioStream *stream = NULL;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to open playback stream: %s", AAudio_convertResultToText(result));
        free(samples);
        return -1;
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        LOGE("Failed to start playback: %s", AAudio_convertResultToText(result));
        AAudioStream_close(stream);
        free(samples);
        return -1;
    }

    /* Wait for playback to finish */
    int duration_ms = (int)((float)mono_samples / sample_rate * 1000) + 200;
    struct timespec wait = {
        .tv_sec  = duration_ms / 1000,
        .tv_nsec = (duration_ms % 1000) * 1000000L
    };
    nanosleep(&wait, NULL);

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    LOGI("Playback finished: %s", wav_path);
#else
    LOGI("Playback (non-Android stub): %s", wav_path);
#endif

    free(samples);
    return 0;
}
