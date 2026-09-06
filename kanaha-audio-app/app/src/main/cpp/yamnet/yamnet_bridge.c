/*
 * Kanaha Audio
 * YAMNet TFLite Bridge - Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * TensorFlow Lite C API wrapper for Google's YAMNet audio event classifier.
 * Processes audio in 0.975-second frames (15,600 samples at 16kHz) with
 * 0.5-second hop, classifying each frame into 521 AudioSet classes.
 *
 * This is the second ML model in the dual-model Axis2/C service:
 *   - whisper.cpp: speech-to-text (keyword search, transcription)
 *   - YAMNet:      audio event detection (instruments, applause, silence)
 *
 * Both models run in-process, dispatched by the same HTTP/2 JSON-RPC endpoint.
 *
 * DATA FLOW:
 *
 *   WAV file on disk
 *        |
 *        v
 *   read_wav_pcm16()           -- Shared WAV reader (audio_util.c)
 *        |
 *        v
 *   Frame loop:                -- Slide 0.975s window with 0.5s hop
 *     copy 15600 samples       -- Into TFLite input tensor
 *     TfLiteInterpreterInvoke  -- Run YAMNet inference
 *     read output[521]         -- Class scores for this frame
 *     check target events      -- Compare against threshold
 *        |
 *        v
 *   Detection results          -- Event name, timestamp, confidence
 */

#include "yamnet_bridge.h"
#include "yamnet_class_map.h"
#include "../audio_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "tensorflow/lite/c/c_api.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#define LOG_TAG "YAMNetBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGD(...) do { fprintf(stderr, "[DEBUG] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/* ========================================================================
 * Bridge state — module-level globals
 *
 * Same single-threaded model as the whisper bridge. The server processes
 * one request at a time via the select() accept loop.
 * ======================================================================== */

static TfLiteInterpreter *g_interpreter = NULL;
static TfLiteModel *g_model = NULL;
static int g_initialized = 0;

/* ========================================================================
 * Class name lookup — case-insensitive search through the 521 classes
 *
 * Returns the class index (0-520) or -1 if not found. Uses
 * case-insensitive comparison so users can pass "saxophone" or
 * "Saxophone" or "SAXOPHONE".
 * ======================================================================== */

static int lookup_class_index(const char *event_name) {
    if (!event_name) return -1;

    for (int i = 0; i < YAMNET_CLASS_MAP_NUM_CLASSES; i++) {
        if (strcasecmp(YAMNET_CLASS_NAMES[i], event_name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ========================================================================
 * Initialization / cleanup
 * ======================================================================== */

int yamnet_bridge_init(const char *model_path) {
    if (!model_path) {
        LOGE("yamnet_bridge_init: model_path is NULL");
        return -1;
    }

    /* Verify the model file exists */
    struct stat st;
    if (stat(model_path, &st) != 0) {
        LOGE("YAMNet model file not found: %s", model_path);
        return -1;
    }

    LOGI("Loading YAMNet model: %s (%lld bytes)", model_path, (long long)st.st_size);

    /* Load the TFLite model from file */
    g_model = TfLiteModelCreateFromFile(model_path);
    if (!g_model) {
        LOGE("Failed to load TFLite model: %s", model_path);
        return -1;
    }

    /*
     * Create interpreter with options.
     *
     * 4 threads = ARM64 big.LITTLE safe default for Pixel 9 Pro performance
     * cores without starving the OS. Same thread count as whisper bridge.
     *
     * YAMNet is much lighter than whisper (~3.7 MB vs ~140+ MB for ggml-base),
     * so inference is fast (~10-20ms per frame on Pixel 9 Pro).
     */
    TfLiteInterpreterOptions *options = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(options, 4);

    g_interpreter = TfLiteInterpreterCreate(g_model, options);
    TfLiteInterpreterOptionsDelete(options);  /* Options are copied, safe to delete */

    if (!g_interpreter) {
        LOGE("Failed to create TFLite interpreter");
        TfLiteModelDelete(g_model);
        g_model = NULL;
        return -1;
    }

    /* Allocate tensors — sets up input/output tensor shapes and memory */
    if (TfLiteInterpreterAllocateTensors(g_interpreter) != kTfLiteOk) {
        LOGE("Failed to allocate TFLite tensors");
        TfLiteInterpreterDelete(g_interpreter);
        TfLiteModelDelete(g_model);
        g_interpreter = NULL;
        g_model = NULL;
        return -1;
    }

    /* Log tensor layout for diagnostics.
     *
     * YAMNet TFLite model tensor layout:
     *   Input:  [N] float32 — dynamic-length raw waveform (resized per call)
     *   Output 0: [F][521] float32 — class scores per frame
     *   Output 1: [F][1024] float32 — embeddings (unused by us)
     *   Output 2: [F][64] float32 — log-mel spectrogram (unused by us)
     *
     * The input is dynamic — we resize it to the audio length before each
     * inference call via TfLiteInterpreterResizeInputTensor().
     */
    {
        int n_out = TfLiteInterpreterGetOutputTensorCount(g_interpreter);
        LOGD("YAMNet model: %d output tensors (dynamic input)", n_out);
    }

    g_initialized = 1;
    LOGI("YAMNet bridge initialized successfully");
    return 0;
}

static void yamnet_bridge_cleanup_unlocked(void) {
    LOGI("YAMNet bridge cleanup");

    if (g_interpreter) {
        TfLiteInterpreterDelete(g_interpreter);
        g_interpreter = NULL;
    }
    if (g_model) {
        TfLiteModelDelete(g_model);
        g_model = NULL;
    }
    g_initialized = 0;
}

static int yamnet_bridge_is_ready_unlocked(void) {
    return g_initialized && g_interpreter != NULL;
}

/* ========================================================================
 * Audio event detection
 *
 * ALGORITHM:
 *
 * 1. Read WAV file into float32 mono array (shared read_wav_pcm16).
 *
 * 2. Slide a 0.975-second window (15,600 samples at 16kHz) across the
 *    audio with a 0.5-second hop (8,000 samples). This gives overlapping
 *    frames for smooth temporal coverage.
 *
 * 3. For each frame:
 *    a. Copy 15,600 float32 samples into the TFLite input tensor.
 *    b. Run TfLiteInterpreterInvoke() — YAMNet inference.
 *    c. Read the output tensor: [521] float32 scores (one per class).
 *    d. For each target event, look up its class index and check if
 *       the score >= threshold.
 *    e. If match, record detection with timestamp and confidence.
 *
 * 4. Apply mode filtering:
 *    - "all":          return every matching frame
 *    - "first_onset":  return only the first match per event
 *    - "last_offset":  return only the last match per event
 *
 * TIMESTAMP CALCULATION:
 *   frame_start_ms = frame_idx * hop_ms
 *   frame_end_ms   = frame_start_ms + frame_ms
 *   where hop_ms = 500ms, frame_ms = 975ms
 * ======================================================================== */

static int yamnet_bridge_detect_unlocked(
    const char *audio_file,
    const char **events,
    int num_events,
    float threshold,
    const char *mode,
    float time_range_start,
    float time_range_end,
    yamnet_detect_result_t *result
) {
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (!g_initialized || !g_interpreter) {
        LOGE("YAMNet bridge not ready (initialized=%d, interpreter=%p)",
             g_initialized, (void *)g_interpreter);
        return -1;
    }

    if (!audio_file || !events || num_events <= 0 || !result) {
        LOGE("Invalid parameters for audio event detection");
        return -1;
    }

    /* Security: reject path traversal */
    if (strstr(audio_file, "..")) {
        LOGE("Security: path traversal in audio_file rejected");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    /* Default threshold */
    if (threshold <= 0.0f) threshold = 0.5f;
    if (!mode) mode = YAMNET_MODE_ALL;

    /* -- Resolve target event class indices --
     *
     * Map user-provided event names (e.g., "Saxophone", "Silence") to their
     * positions in YAMNet's 521-class output tensor. This is done once before
     * the frame loop so we only do O(521) string comparisons per event, not
     * per frame. Invalid class names are logged and skipped. */
    int class_indices[YAMNET_MAX_EVENTS];
    const char *valid_event_names[YAMNET_MAX_EVENTS];  /* Parallel array: original name per valid event */
    int valid_events = 0;

    for (int i = 0; i < num_events && i < YAMNET_MAX_EVENTS; i++) {
        int idx = lookup_class_index(events[i]);
        if (idx < 0) {
            LOGE("Unknown AudioSet class: '%s' (skipping)", events[i]);
            continue;
        }
        class_indices[valid_events] = idx;
        valid_event_names[valid_events] = events[i];  /* Track original name alongside index */
        valid_events++;
        LOGD("Event '%s' -> class index %d", events[i], idx);
    }

    if (valid_events == 0) {
        LOGE("No valid event classes specified");
        return -1;
    }

    /* -- Read WAV file into float32 array -- */
    int n_samples = 0;
    int sample_rate = 0;
    float *pcmf32 = read_wav_pcm16(audio_file, &n_samples, &sample_rate);
    if (!pcmf32) {
        LOGE("Failed to read audio file: %s", audio_file);
        return -1;
    }

    LOGI("Detecting %d events in: %s (%d samples, %d Hz, %.1f sec)",
         valid_events, audio_file, n_samples, sample_rate,
         (float)n_samples / sample_rate);

    /*
     * YAMNet expects 16kHz input. If the audio has a different sample rate,
     * we still process it but log a warning. For best results, convert first:
     *   ffmpeg -i input.wav -ar 16000 -ac 1 output.wav
     */
    if (sample_rate != YAMNET_SAMPLE_RATE) {
        LOGI("Warning: audio is %d Hz, YAMNet expects %d Hz. Results may vary.",
             sample_rate, YAMNET_SAMPLE_RATE);
    }

    /* Calculate time range in samples */
    int start_sample = 0;
    int end_sample = n_samples;

    if (time_range_start >= 0.0f) {
        start_sample = (int)(time_range_start * sample_rate);
        if (start_sample >= n_samples) {
            LOGE("time_range_start (%.1f s) is beyond audio length", time_range_start);
            free(pcmf32);
            return -1;
        }
    }
    if (time_range_end >= 0.0f) {
        end_sample = (int)(time_range_end * sample_rate);
        if (end_sample > n_samples) end_sample = n_samples;
    }

    /* The range is fed to TfLiteTensorCopyFromBuffer() as a pointer offset
     * and a byte count. A negative start reads before the buffer; an
     * inverted range produces a negative length. Neither can be allowed
     * through regardless of how the values were derived. */
    if (start_sample < 0 || end_sample > n_samples || end_sample <= start_sample) {
        LOGE("Invalid sample range [%d, %d) for %d samples",
             start_sample, end_sample, n_samples);
        free(pcmf32);
        return -1;
    }

    /*
     * YAMNet TFLite model input/output:
     *
     * This YAMNet model takes the ENTIRE waveform as input (dynamic shape).
     * The model handles its own framing internally (0.975s frames, 0.48s hop).
     *
     * Input:  [N] float32 — raw waveform samples (16kHz expected)
     * Output: [F][521] float32 — class scores per frame (F = number of frames)
     *         [F][1024] float32 — embeddings (unused)
     *         [F][64] float32 — log-mel spectrogram (unused)
     *
     * We feed the audio region [start_sample..end_sample] as a single
     * inference call, then iterate over the output frames.
     */

    /* Calculate input sample count for the requested time range */
    int input_samples = end_sample - start_sample;

    /* Resize input tensor to match waveform length */
    int input_dims[1] = { input_samples };
    if (TfLiteInterpreterResizeInputTensor(g_interpreter, 0, input_dims, 1) != kTfLiteOk) {
        LOGE("Failed to resize YAMNet input tensor to %d samples", input_samples);
        free(pcmf32);
        return -1;
    }

    /* Re-allocate tensors after resize */
    if (TfLiteInterpreterAllocateTensors(g_interpreter) != kTfLiteOk) {
        LOGE("Failed to re-allocate TFLite tensors after resize");
        free(pcmf32);
        return -1;
    }

    /* Copy waveform into input tensor */
    TfLiteTensor *input_tensor = TfLiteInterpreterGetInputTensor(g_interpreter, 0);
    if (!input_tensor) {
        LOGE("Failed to get TFLite input tensor");
        free(pcmf32);
        return -1;
    }

    if (TfLiteTensorCopyFromBuffer(input_tensor, pcmf32 + start_sample,
                                   input_samples * sizeof(float)) != kTfLiteOk) {
        LOGE("Failed to copy waveform to TFLite input tensor (%d samples)", input_samples);
        free(pcmf32);
        return -1;
    }

    free(pcmf32);  /* Audio data now in tensor — free the buffer */

    /* Run single inference on entire waveform */
    if (TfLiteInterpreterInvoke(g_interpreter) != kTfLiteOk) {
        LOGE("TFLite YAMNet invoke failed");
        return -1;
    }

    /*
     * Read output scores from YAMNet.
     *
     * Output tensor 0 shape: [num_frames][521] float32 scores.
     * Each row is one 0.975s frame. The model internally uses a 0.48s hop,
     * so num_frames = (audio_duration - 0.975) / 0.48 + 1.
     *
     * Key class indices for parseLTC.sh use cases:
     *   192 = Saxophone  (detect "A Love Supreme" intro)
     *   62  = Applause   (detect end of presentation)
     *   494 = Silence    (detect gaps between segments)
     */
    const TfLiteTensor *output_tensor =
        TfLiteInterpreterGetOutputTensor(g_interpreter, 0);
    if (!output_tensor) {
        LOGE("Failed to get TFLite output tensor");
        return -1;
    }

    int num_frames = TfLiteTensorDim(output_tensor, 0);
    int num_classes = TfLiteTensorDim(output_tensor, 1);
    const float *all_scores = (const float *)TfLiteTensorData(output_tensor);

    LOGI("YAMNet output: %d frames x %d classes", num_frames, num_classes);

    if (num_classes != YAMNET_NUM_CLASSES) {
        LOGE("Unexpected YAMNet output: %d classes (expected %d)", num_classes, YAMNET_NUM_CLASSES);
        return -1;
    }

    /*
     * YAMNet's internal hop is 0.48 seconds. Frame timestamps:
     *   frame_start_ms = time_range_start_ms + frame_idx * 480
     *   frame_end_ms   = frame_start_ms + 975
     */
    float base_ms = (time_range_start >= 0.0f) ? time_range_start * 1000.0f : 0.0f;
    float yamnet_hop_ms = 480.0f;    /* YAMNet internal hop: 0.48s */
    float yamnet_frame_ms = 975.0f;  /* YAMNet frame duration: 0.975s */

    /*
     * Per-event state for mode filtering:
     *
     * first_found[ev]:     has this event been detected at least once?
     * last_detection[ev]:  most recent detection (for "last_offset" mode)
     */
    int first_found[YAMNET_MAX_EVENTS];
    yamnet_detection_t last_detection[YAMNET_MAX_EVENTS];
    memset(first_found, 0, sizeof(first_found));
    memset(last_detection, 0, sizeof(last_detection));

    /* Iterate over output frames */
    for (int frame_idx = 0; frame_idx < num_frames; frame_idx++) {
        const float *scores = all_scores + frame_idx * num_classes;
        float frame_start_ms_f = base_ms + frame_idx * yamnet_hop_ms;
        float frame_end_ms_f = frame_start_ms_f + yamnet_frame_ms;

        /* Check each target event against threshold */
        for (int ev = 0; ev < valid_events; ev++) {
            int cls_idx = class_indices[ev];
            float score = scores[cls_idx];

            if (score >= threshold) {
                int64_t det_start_ms = (int64_t)(frame_start_ms_f + 0.5f);
                int64_t det_end_ms = (int64_t)(frame_end_ms_f + 0.5f);

                if (strcmp(mode, YAMNET_MODE_FIRST_ONSET) == 0) {
                    /* Only record first detection per event */
                    if (first_found[ev]) continue;
                    first_found[ev] = 1;
                }

                if (strcmp(mode, YAMNET_MODE_LAST_OFFSET) == 0) {
                    /* Track last detection, don't record yet */
                    strncpy(last_detection[ev].event, valid_event_names[ev],
                            sizeof(last_detection[ev].event) - 1);
                    last_detection[ev].event[sizeof(last_detection[ev].event) - 1] = '\0';
                    last_detection[ev].start_ms = det_start_ms;
                    last_detection[ev].end_ms = det_end_ms;
                    last_detection[ev].confidence = score;
                    first_found[ev] = 1;
                    continue;
                }

                /* Record detection (for "all" and "first_onset" modes) */
                if (result->num_detections < YAMNET_MAX_DETECTIONS) {
                    yamnet_detection_t *d =
                        &result->detections[result->num_detections];
                    strncpy(d->event, valid_event_names[ev], sizeof(d->event) - 1);
                    d->event[sizeof(d->event) - 1] = '\0';
                    d->start_ms = det_start_ms;
                    d->end_ms = det_end_ms;
                    d->confidence = score;

                    LOGI("Detection: '%s' at %lld-%lld ms (confidence %.2f)",
                         d->event, (long long)d->start_ms,
                         (long long)d->end_ms, d->confidence);

                    result->num_detections++;
                }
            }
        }
    }

    /* For "last_offset" mode, add the last detection per event */
    if (strcmp(mode, YAMNET_MODE_LAST_OFFSET) == 0) {
        for (int ev = 0; ev < valid_events; ev++) {
            if (first_found[ev] && result->num_detections < YAMNET_MAX_DETECTIONS) {
                result->detections[result->num_detections] = last_detection[ev];
                result->num_detections++;
            }
        }
    }

    result->frames_analyzed = num_frames;

    /* Record processing time */
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    result->processing_time_ms =
        (end_time.tv_sec - start_time.tv_sec) * 1000 +
        (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    LOGI("Audio event detection complete: %d detections in %d frames (%lld ms)",
         result->num_detections, result->frames_analyzed,
         (long long)result->processing_time_ms);

    return 0;
}

/* One TfLiteInterpreter, shared. ResizeInputTensor / AllocateTensors / Invoke
 * are not safe to run concurrently on it, and the server has four worker
 * threads. See the matching note in whisper_android_bridge.c. */
static pthread_mutex_t g_yamnet_lock = PTHREAD_MUTEX_INITIALIZER;

int yamnet_bridge_detect(const char *audio_file, const char **events, int num_events,
                         float threshold, const char *mode,
                         float time_range_start, float time_range_end,
                         yamnet_detect_result_t *result) {
    pthread_mutex_lock(&g_yamnet_lock);
    int rc = yamnet_bridge_detect_unlocked(audio_file, events, num_events, threshold, mode,
                                           time_range_start, time_range_end, result);
    pthread_mutex_unlock(&g_yamnet_lock);
    return rc;
}

void yamnet_bridge_cleanup(void) {
    pthread_mutex_lock(&g_yamnet_lock);
    yamnet_bridge_cleanup_unlocked();
    pthread_mutex_unlock(&g_yamnet_lock);
}

int yamnet_bridge_is_ready(void) {
    pthread_mutex_lock(&g_yamnet_lock);
    int rc = yamnet_bridge_is_ready_unlocked();
    pthread_mutex_unlock(&g_yamnet_lock);
    return rc;
}
