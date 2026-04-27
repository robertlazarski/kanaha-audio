/*
 * Kanaha Audio
 * Recording Sidecar JSON — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Writes a sidecar JSON file alongside WAV recordings for trim arithmetic
 * and GPS metadata. Written atomically (write .tmp, rename) to prevent
 * partial reads by concurrent processes.
 *
 * Functionally equivalent to Kanaha Camera's kanaha_recording_start.json
 * but independently implemented. The recording_start_ms field serves
 * the same role — it enables parseWithoutLTC.sh trim arithmetic.
 */

#include "audio_sidecar.h"
#include "gps_reader.h"

#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "AudioSidecar"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

int audio_sidecar_write(const char *audio_dir,
                        const char *clip_name,
                        int64_t recording_start_ms,
                        int sample_rate,
                        int channels) {
    if (!audio_dir || !clip_name) {
        LOGE("audio_sidecar_write: NULL argument");
        return -1;
    }

    /* Build paths */
    char tmp_path[1024];
    char final_path[1024];
    snprintf(tmp_path, sizeof(tmp_path),
             "%s/kanaha_audio_recording_start.json.tmp", audio_dir);
    snprintf(final_path, sizeof(final_path),
             "%s/kanaha_audio_recording_start.json", audio_dir);

    /* Try to read GPS data from the LocationService JSON file.
     *
     * GPS file lives at <filesDir>/gps_location.json, which is one directory
     * above the audio directory (<filesDir>/audio/). Both paths derive from
     * the same filesDir root passed by ApacheService.kt. GPS is optional —
     * the sidecar is valid without it (indoors, permission denied, etc.). */
    char gps_path[1024];
    snprintf(gps_path, sizeof(gps_path), "%s/../gps_location.json", audio_dir);

    gps_location_t gps;
    int has_gps = (gps_reader_read(gps_path, &gps) == 0 && gps.valid);
    int64_t gps_age = -1;
    if (has_gps) {
        gps_age = gps_reader_age_ms(gps.time_ms, recording_start_ms);
    }

    /* Write to temp file */
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        LOGE("Failed to open sidecar tmp file: %s", tmp_path);
        return -1;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"recording_start_ms\": %lld,\n", (long long)recording_start_ms);
    fprintf(f, "  \"clip_name\": \"%s\",\n", clip_name);
    fprintf(f, "  \"sample_rate\": %d,\n", sample_rate);
    fprintf(f, "  \"channels\": %d", channels);

    if (has_gps) {
        fprintf(f, ",\n");
        fprintf(f, "  \"gps_time\": %lld,\n", (long long)gps.time_ms);
        fprintf(f, "  \"gps_age_ms\": %lld,\n", (long long)gps_age);
        fprintf(f, "  \"gps_latitude\": %.6f,\n", gps.latitude);
        fprintf(f, "  \"gps_longitude\": %.6f", gps.longitude);
    }

    fprintf(f, "\n}\n");
    fclose(f);

    /* Atomic rename: ensures readers never see a partial/truncated JSON.
     * rename() on the same filesystem is atomic on Linux/Android. */
    if (rename(tmp_path, final_path) != 0) {
        LOGE("Failed to rename sidecar: %s -> %s", tmp_path, final_path);
        return -1;
    }

    LOGI("Sidecar written: %s (gps=%s)", final_path, has_gps ? "yes" : "no");
    return 0;
}
