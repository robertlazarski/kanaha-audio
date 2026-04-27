/*
 * Kanaha Audio
 * GPS Location Reader — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Reads GPS location from a JSON file written by the Kotlin LocationService.
 * The LocationService writes the file; this C code reads it.
 *
 * GPS JSON format (written by LocationService.kt):
 * {
 *   "latitude": 21.2969,
 *   "longitude": -157.8171,
 *   "accuracy": 5.0,
 *   "time": 1772039427000,
 *   "provider": "gps"
 * }
 *
 * LIMITS:
 *   - GPS JSON file must be < 1024 bytes (read into stack buffer)
 *   - Location at exactly (0.0, 0.0) is rejected as invalid
 *   - Requires ACCESS_FINE_LOCATION permission (graceful failure if denied)
 *   - LocationService update interval: 5 seconds minimum
 */

#ifndef GPS_READER_H
#define GPS_READER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double latitude;
    double longitude;
    float accuracy;
    int64_t time_ms;     /* Unix epoch ms of GPS fix */
    int valid;           /* 1 if data was read successfully, 0 otherwise */
} gps_location_t;

/**
 * Read the latest GPS location from the shared JSON file.
 *
 * @param gps_json_path  Path to the GPS JSON file (written by LocationService.kt)
 * @param out            Output location struct
 * @return 0 on success, -1 on error (file missing, parse error, etc.)
 */
int gps_reader_read(const char *gps_json_path, gps_location_t *out);

/**
 * Calculate age of GPS fix relative to a reference time.
 *
 * @param gps_time_ms    GPS fix time in Unix epoch ms
 * @param reference_ms   Reference time in Unix epoch ms (e.g., recording start)
 * @return Age in milliseconds, or -1 if invalid
 */
int64_t gps_reader_age_ms(int64_t gps_time_ms, int64_t reference_ms);

#ifdef __cplusplus
}
#endif

#endif /* GPS_READER_H */
