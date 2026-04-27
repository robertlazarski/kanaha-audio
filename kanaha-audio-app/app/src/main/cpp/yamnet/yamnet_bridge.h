/*
 * Kanaha Audio
 * YAMNet TFLite Bridge - Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Audio event detection via Google's YAMNet model (TensorFlow Lite).
 * Classifies audio into 521 AudioSet classes including instruments
 * (Saxophone, Piano, Guitar), events (Applause, Silence), and more.
 *
 * This complements the whisper bridge (speech-to-text) to enable
 * dual-model audio analysis through the same Axis2/C service:
 *   - whisper: "When does the presenter say 'next slide please'?"
 *   - yamnet:  "When does the saxophone intro start?"
 */

#ifndef YAMNET_BRIDGE_H
#define YAMNET_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of event detections returned */
#define YAMNET_MAX_DETECTIONS 256

/* Maximum event name length (AudioSet class names) */
#define YAMNET_MAX_EVENT_LEN 128

/* Maximum number of target events in a single query */
#define YAMNET_MAX_EVENTS 32

/*
 * YAMNet model parameters — these match the model's expected input shape.
 *
 * YAMNet processes audio in fixed-length frames. Each frame is 0.975 seconds
 * (15,600 samples at 16kHz). Consecutive frames overlap via a 0.5-second hop,
 * giving smooth temporal coverage. The model outputs 521 class scores per frame.
 *
 * These values are baked into the yamnet.tflite model and cannot be changed.
 */
#define YAMNET_SAMPLE_RATE   16000   /* Expected input sample rate (Hz) */
#define YAMNET_FRAME_SAMPLES 15600   /* 0.975 seconds at 16kHz */
#define YAMNET_HOP_SAMPLES   8000    /* 0.5 second hop between frames */
#define YAMNET_NUM_CLASSES   521     /* AudioSet class count */

/* Detection modes */
#define YAMNET_MODE_ALL          "all"           /* Every matching frame */
#define YAMNET_MODE_FIRST_ONSET  "first_onset"   /* First match only */
#define YAMNET_MODE_LAST_OFFSET  "last_offset"   /* Last match only */

/**
 * A single audio event detection with timestamp and confidence
 */
typedef struct {
    char event[YAMNET_MAX_EVENT_LEN];   /* AudioSet class name */
    int64_t start_ms;                    /* Start time in milliseconds */
    int64_t end_ms;                      /* End time in milliseconds */
    float confidence;                    /* Detection confidence (0.0-1.0) */
} yamnet_detection_t;

/**
 * Result of an audio event detection operation
 */
typedef struct {
    yamnet_detection_t detections[YAMNET_MAX_DETECTIONS];
    int num_detections;
    int frames_analyzed;
    int64_t processing_time_ms;
} yamnet_detect_result_t;

/**
 * Initialize the YAMNet bridge.
 * Loads the TFLite model from the specified path.
 *
 * @param model_path  Path to yamnet.tflite model file
 * @return 0 on success, -1 on failure
 */
int yamnet_bridge_init(const char *model_path);

/**
 * Cleanup the YAMNet bridge and free all resources.
 */
void yamnet_bridge_cleanup(void);

/**
 * Detect audio events in a WAV file.
 *
 * Processes the audio in 0.975-second frames with 0.5-second hop,
 * running YAMNet inference on each frame. Checks target event classes
 * against the confidence threshold.
 *
 * @param audio_file        Path to audio file (WAV format, 16kHz preferred)
 * @param events            Array of AudioSet class names to detect
 * @param num_events        Number of target events
 * @param threshold         Confidence threshold (0.0-1.0), default 0.5
 * @param mode              Detection mode: "all", "first_onset", "last_offset"
 * @param time_range_start  Start of search window in seconds (-1 for beginning)
 * @param time_range_end    End of search window in seconds (-1 for end)
 * @param result            Output: detection results (caller provides storage)
 * @return 0 on success, -1 on failure
 */
int yamnet_bridge_detect(
    const char *audio_file,
    const char **events,
    int num_events,
    float threshold,
    const char *mode,
    float time_range_start,
    float time_range_end,
    yamnet_detect_result_t *result
);

/**
 * Check if the YAMNet bridge is initialized and ready.
 *
 * @return 1 if ready, 0 if not
 */
int yamnet_bridge_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* YAMNET_BRIDGE_H */
