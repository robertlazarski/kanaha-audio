/*
 * Kanaha Audio
 * GPS Location Reader — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Reads GPS location from a JSON file written by the Kotlin LocationService.
 * Uses simple string parsing (same pattern as audio_search_service.c) —
 * no external JSON library needed.
 */

#include "gps_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "GpsReader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/**
 * Extract a numeric (double) value from a JSON string by key.
 * Handles: "key":123.456 and "key": 123.456 (with whitespace).
 * Returns default_val if key not found.
 */
static double extract_double(const char *json, const char *key, double default_val) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *start = strstr(json, search);
    if (!start) return default_val;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    char *endp = NULL;
    double val = strtod(start, &endp);
    if (endp == start) return default_val;
    return val;
}

/**
 * Read and parse the GPS JSON file written by LocationService.kt.
 *
 * File format:
 *   {"latitude":21.2969,"longitude":-157.8171,"accuracy":5.0,
 *    "time":1772039427000,"provider":"gps"}
 *
 * The file is written atomically (tmp + rename) by LocationService,
 * so we should never see a partial write. If we do (power loss, etc.),
 * the JSON parse will fail gracefully and return -1.
 *
 * Returns 0 and sets out->valid=1 on success, or -1 on error.
 * Returning -1 when the file doesn't exist is normal (LocationService
 * hasn't written yet, or GPS permission was denied).
 */
int gps_reader_read(const char *gps_json_path, gps_location_t *out) {
    if (!gps_json_path || !out) return -1;

    memset(out, 0, sizeof(*out));
    out->valid = 0;

    FILE *f = fopen(gps_json_path, "r");
    if (!f) {
        return -1;  /* File not found — normal if LocationService hasn't written yet */
    }

    /* Read entire file into stack buffer. GPS JSON is ~150 bytes,
     * so 1024 is generous. */
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n == 0) {
        LOGE("GPS JSON file is empty: %s", gps_json_path);
        return -1;
    }
    buf[n] = '\0';

    out->latitude  = extract_double(buf, "latitude", 0.0);
    out->longitude = extract_double(buf, "longitude", 0.0);
    out->accuracy  = (float)extract_double(buf, "accuracy", -1.0);
    out->time_ms   = (int64_t)extract_double(buf, "time", 0.0);

    /* Validate: time must be positive, and at least one of lat/lon must
     * be non-zero. A fix at (0,0) in the Gulf of Guinea is theoretically
     * possible but practically indicates an uninitialized value. */
    if (out->time_ms > 0 && (out->latitude != 0.0 || out->longitude != 0.0)) {
        out->valid = 1;
        return 0;
    }

    LOGE("GPS JSON has invalid values: lat=%.6f lon=%.6f time=%lld",
         out->latitude, out->longitude, (long long)out->time_ms);
    return -1;
}

/**
 * Calculate how stale a GPS fix is relative to a reference time.
 *
 * Example: if GPS time is 1000 and recording started at 2200,
 * the fix is 1200ms old — which is included in the sidecar JSON
 * so downstream tools know the GPS accuracy context.
 */

int64_t gps_reader_age_ms(int64_t gps_time_ms, int64_t reference_ms) {
    if (gps_time_ms <= 0 || reference_ms <= 0) return -1;
    int64_t age = reference_ms - gps_time_ms;
    return (age >= 0) ? age : -age;
}
