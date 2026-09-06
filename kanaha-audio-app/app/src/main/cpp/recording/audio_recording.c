/*
 * Kanaha Audio
 * Audio Recording via AAudio NDK — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Records audio from the device microphone using the AAudio C API.
 * Writes 16-bit PCM WAV files directly from native code — no JNI needed.
 *
 * Data flow:
 *   Phone microphone (hardware)
 *     → AAudio input stream (AAUDIO_DIRECTION_INPUT, 16kHz mono, PCM_I16)
 *     → Data callback (real-time thread, non-blocking)
 *     → Lock-free SPSC ring buffer (~1 second of audio)
 *     → Writer pthread (reads ring buffer, writes to WAV file via fwrite)
 *     → WAV file on disk (immediately available for whisper/YAMNet)
 *
 * start_at scheduling uses clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)
 * for kernel-level precision — no Java message queue jitter.
 */

#include "audio_recording.h"
#include "../audio_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/log.h>
#define LOG_TAG "AudioRecording"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGD(...) do { fprintf(stderr, "[DEBUG] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/* ========================================================================
 * Lock-free Single-Producer Single-Consumer ring buffer
 *
 * The AAudio data callback (producer) writes frames from the real-time
 * audio thread. The writer pthread (consumer) reads frames and writes
 * to disk. No mutex — only atomic head/tail indices.
 * ======================================================================== */

#define RING_BUFFER_FRAMES  32768  /* ~2 seconds at 16kHz */

typedef struct {
    int16_t samples[RING_BUFFER_FRAMES];
    atomic_uint head;  /* Written by producer (callback) */
    atomic_uint tail;  /* Written by consumer (writer thread) */
} ring_buffer_t;

/**
 * Return number of samples available for reading.
 *
 * Memory ordering: acquire on head ensures we see all samples the
 * producer wrote before advancing head. Relaxed on tail is safe
 * because only the consumer (this thread) writes tail.
 */
static inline unsigned ring_available(const ring_buffer_t *rb) {
    unsigned h = atomic_load_explicit(&rb->head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    return (h - t) % RING_BUFFER_FRAMES;
}

/**
 * Push samples into the ring buffer (called from AAudio callback thread).
 *
 * The caller must check available space first — this function does NOT
 * check for overflow. Relaxed load on head is safe because only the
 * producer (this thread) writes head. Release store ensures the sample
 * data is visible to the consumer before head advances.
 */
static inline void ring_push(ring_buffer_t *rb, const int16_t *data, unsigned count) {
    unsigned h = atomic_load_explicit(&rb->head, memory_order_relaxed);
    for (unsigned i = 0; i < count; i++) {
        rb->samples[(h + i) % RING_BUFFER_FRAMES] = data[i];
    }
    atomic_store_explicit(&rb->head, (h + count) % RING_BUFFER_FRAMES,
                          memory_order_release);
}

/**
 * Pop up to max_count samples from the ring buffer (called from writer thread).
 *
 * Returns the actual number of samples popped (may be less than max_count
 * if fewer are available). Release store on tail makes the freed slots
 * visible to the producer.
 */
static inline unsigned ring_pop(ring_buffer_t *rb, int16_t *data, unsigned max_count) {
    unsigned avail = ring_available(rb);
    if (avail > max_count) avail = max_count;
    unsigned t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    for (unsigned i = 0; i < avail; i++) {
        data[i] = rb->samples[(t + i) % RING_BUFFER_FRAMES];
    }
    atomic_store_explicit(&rb->tail, (t + avail) % RING_BUFFER_FRAMES,
                          memory_order_release);
    return avail;
}

/* ========================================================================
 * Recording state
 *
 * Only one recording can be active at a time (single microphone).
 * State machine: IDLE → ACTIVE → STOPPING → IDLE
 *
 * Thread safety:
 *   - s_state is atomic (read by callback thread, writer thread, and API callers)
 *   - s_ring is lock-free SPSC (producer = callback, consumer = writer)
 *   - All other fields are only accessed from the API caller thread
 *     (HTTP request handler), except s_total_samples_written which is
 *     only written by the writer thread and read after pthread_join.
 * ======================================================================== */

static char s_audio_dir[512] = "";             /* WAV output directory */
static char s_clip_name[AUDIO_RECORDING_MAX_CLIP_NAME + 1] = "";
static char s_wav_path[1024] = "";             /* Full path to current WAV file */
static int  s_sample_rate = 16000;             /* 16000 or 44100 */
static atomic_int s_state = AUDIO_RECORDING_IDLE;

static ring_buffer_t s_ring;                   /* SPSC ring: callback → writer */
static FILE *s_wav_file = NULL;                /* Open WAV file handle */
static int64_t s_total_samples_written = 0;    /* Samples written to disk */
static int64_t s_recording_start_ms = 0;       /* Wall-clock start time */

static pthread_t s_writer_thread;              /* Disk writer thread */
static atomic_int s_writer_running = 0;        /* 1 if writer thread is joinable */
/* Serialises the whole start and stop sequences. Atomics alone cannot make a
 * multi-step init/teardown atomic: a stop landing mid-init saw a half-built
 * ACTIVE state and leaked the stream/thread. The lock closes that window. */
static pthread_mutex_t s_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef __ANDROID__
static AAudioStream *s_stream = NULL;          /* AAudio input (mic) stream */
#endif

/* ========================================================================
 * WAV header writing
 *
 * WAV format (little-endian):
 *   Offset  Size  Field
 *   0       4     "RIFF"
 *   4       4     file_size - 8  (total file size minus RIFF header)
 *   8       4     "WAVE"
 *   12      4     "fmt "
 *   16      4     fmt chunk size (16 for PCM)
 *   20      2     audio format (1 = PCM)
 *   22      2     num channels
 *   24      4     sample rate
 *   28      4     byte rate (sample_rate * channels * bits/8)
 *   32      2     block align (channels * bits/8)
 *   34      2     bits per sample (16)
 *   36      4     "data"
 *   40      4     data chunk size (num_samples * channels * bits/8)
 *   44+     ...   PCM sample data
 *
 * On startRecording, we write a header with data_size=0, then on
 * stopRecording we seek back and patch offsets 4 and 40 with final sizes.
 * ======================================================================== */

static int write_wav_header(FILE *f, int sample_rate, int16_t channels,
                            uint32_t data_size) {
    /* RIFF header — sizes are uint32 in the WAV spec */
    uint32_t file_size = 36 + data_size;
    int32_t fmt_chunk_size = 16;
    int16_t audio_format = 1;  /* PCM */
    int16_t bits_per_sample = 16;
    int32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    int16_t block_align = channels * (bits_per_sample / 8);

    /* Track total bytes written to detect disk-full / I/O errors */
    size_t written = 0;
    written += fwrite("RIFF", 1, 4, f);
    written += fwrite(&file_size, 4, 1, f) * 4;
    written += fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    written += fwrite("fmt ", 1, 4, f);
    written += fwrite(&fmt_chunk_size, 4, 1, f) * 4;
    written += fwrite(&audio_format, 2, 1, f) * 2;
    written += fwrite(&channels, 2, 1, f) * 2;
    written += fwrite(&sample_rate, 4, 1, f) * 4;
    written += fwrite(&byte_rate, 4, 1, f) * 4;
    written += fwrite(&block_align, 2, 1, f) * 2;
    written += fwrite(&bits_per_sample, 2, 1, f) * 2;

    /* data chunk header (size updated on finalize) */
    written += fwrite("data", 1, 4, f);
    written += fwrite(&data_size, 4, 1, f) * 4;

    if (written != 44) {
        LOGE("WAV header write failed: wrote %zu of 44 bytes (disk full?)", written);
        return -1;
    }
    return 0;
}

static int finalize_wav(FILE *f, int sample_rate, int64_t total_samples) {
    /* Cap at WAV's 4GB limit (uint32 data size field).
     * At 16kHz mono 16-bit, this allows ~18.6 hours of recording.
     * At 44.1kHz, ~6.7 hours. Beyond that, would need RF64 format. */
    uint64_t raw_data_size = (uint64_t)total_samples * sizeof(int16_t);
    if (raw_data_size > UINT32_MAX - 36) {
        LOGE("WAV data size exceeds 4GB limit, capping (recording too long)");
        raw_data_size = UINT32_MAX - 36;
    }
    uint32_t data_size = (uint32_t)raw_data_size;
    uint32_t file_size = 36 + data_size;

    /* Update RIFF file size (offset 4) */
    fseek(f, 4, SEEK_SET);
    fwrite(&file_size, 4, 1, f);

    /* Update data chunk size (offset 40) */
    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);

    return 0;
}

/* ========================================================================
 * Writer thread — reads ring buffer, writes to WAV file
 * ======================================================================== */

static void *writer_thread_func(void *arg) {
    (void)arg;
    int16_t buf[4096];  /* 4096 samples = 256ms at 16kHz, fits in stack */

    LOGI("Writer thread started");

    /* Main loop: run while recording is active, or while there's data
     * left to drain after the callback has stopped producing. */
    while (atomic_load(&s_state) == AUDIO_RECORDING_ACTIVE ||
           ring_available(&s_ring) > 0) {

        unsigned count = ring_pop(&s_ring, buf, 4096);
        if (count > 0 && s_wav_file) {
            fwrite(buf, sizeof(int16_t), count, s_wav_file);
            s_total_samples_written += count;
        } else {
            /* No data available — sleep 5ms to avoid busy-wait.
             * At 16kHz, the callback fires every ~5.3ms (85 frames per
             * callback at the default buffer size), so 5ms polling is
             * fast enough to keep up without burning CPU. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
            nanosleep(&ts, NULL);
        }
    }

    /* Drain any remaining samples that arrived between the last check
     * and the state transition to STOPPING. */
    unsigned count;
    while ((count = ring_pop(&s_ring, buf, 4096)) > 0) {
        if (s_wav_file) {
            fwrite(buf, sizeof(int16_t), count, s_wav_file);
            s_total_samples_written += count;
        }
    }

    LOGI("Writer thread finished, total samples: %lld",
         (long long)s_total_samples_written);
    return NULL;
}

/* ========================================================================
 * AAudio error callback — handles device disconnection
 *
 * Called when the audio device is disconnected (e.g., Bluetooth drops,
 * USB audio removed). Sets state to STOPPING so the writer thread
 * drains and exits cleanly, rather than spinning indefinitely.
 * ======================================================================== */

#ifdef __ANDROID__
static void audio_error_callback(AAudioStream *stream, void *userData,
                                 aaudio_result_t error) {
    (void)stream;
    (void)userData;
    LOGE("AAudio error callback: %s", AAudio_convertResultToText(error));
    /* Signal stop — writer thread will drain ring buffer and exit */
    atomic_store(&s_state, AUDIO_RECORDING_STOPPING);
}
#endif

/* ========================================================================
 * AAudio data callback — runs on real-time audio thread
 * ======================================================================== */

#ifdef __ANDROID__
/**
 * AAudio data callback — runs on a high-priority real-time audio thread.
 *
 * CRITICAL: This function must not block, allocate memory, or acquire
 * mutexes. The only safe operations are reading audioData and writing
 * to the lock-free ring buffer.
 *
 * @param audioData  Buffer of PCM_I16 samples captured from the microphone
 * @param numFrames  Number of mono frames (= number of int16_t samples)
 */
static aaudio_data_callback_result_t audio_data_callback(
    AAudioStream *stream, void *userData,
    void *audioData, int32_t numFrames) {
    (void)stream;
    (void)userData;

    /* Relaxed load is sufficient: we don't need to synchronize data with
     * the state change, just observe the flag promptly. */
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != AUDIO_RECORDING_ACTIVE) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    /* Push captured audio into ring buffer.
     * If ring buffer is full, excess frames are silently dropped.
     * At 16kHz with a 32768-sample (~2s) buffer and 5ms writer polling,
     * overflow should not occur under normal conditions. */
    unsigned space = RING_BUFFER_FRAMES - ring_available(&s_ring) - 1;
    unsigned to_push = (unsigned)numFrames;
    if (to_push > space) {
        to_push = space;
        LOGD("Ring buffer overflow: dropped %u frames", (unsigned)numFrames - to_push);
    }
    if (to_push > 0) {
        ring_push(&s_ring, (const int16_t *)audioData, to_push);
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif

/* ========================================================================
 * Public API
 * ======================================================================== */

int audio_recording_init(const char *audio_dir) {
    if (!audio_dir || strlen(audio_dir) == 0) {
        LOGE("audio_recording_init: audio_dir is NULL or empty");
        return -1;
    }

    strncpy(s_audio_dir, audio_dir, sizeof(s_audio_dir) - 1);
    s_audio_dir[sizeof(s_audio_dir) - 1] = '\0';

    /* Ensure audio directory exists */
    mkdir(s_audio_dir, 0755);

    atomic_store(&s_state, AUDIO_RECORDING_IDLE);
    LOGI("Recording subsystem initialized, audio_dir=%s", s_audio_dir);
    return 0;
}

void audio_recording_cleanup(void) {
    int st = atomic_load(&s_state);
    if (st == AUDIO_RECORDING_ACTIVE || st == AUDIO_RECORDING_STOPPING ||
        st == AUDIO_RECORDING_SCHEDULED) {
        int64_t dur, sz;
        audio_recording_stop(&dur, &sz);
    }
    s_audio_dir[0] = '\0';
}

/**
 * Internal: actually open the WAV file, start the ring buffer writer,
 * and begin the AAudio input stream. Called either directly (start_at=0)
 * or from the scheduling thread (start_at>0).
 *
 * Preconditions: caller holds s_lifecycle_lock; s_clip_name, s_wav_path and
 *                s_sample_rate are set; s_state is SCHEDULED.
 * Returns 0 on success (state ACTIVE), -1 on error (state reset to IDLE).
 */
static int recording_start_impl(void) {
    /* Caller holds s_lifecycle_lock and has verified the slot is ours (state
     * SCHEDULED), so the entire start sequence below is atomic with respect to
     * stop(): no concurrent stop can observe a half-initialised ACTIVE state. */

    /* Record actual start time */
    struct timespec start_ts;
    clock_gettime(CLOCK_REALTIME, &start_ts);
    s_recording_start_ms = (int64_t)start_ts.tv_sec * 1000 + start_ts.tv_nsec / 1000000;

    /* Open WAV file and write header (placeholder size, updated on stop) */
    s_wav_file = fopen(s_wav_path, "wb");
    if (!s_wav_file) {
        LOGE("Failed to open WAV file: %s", s_wav_path);
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        return -1;
    }
    write_wav_header(s_wav_file, s_sample_rate, 1, 0);

    /* Reset ring buffer */
    atomic_store(&s_ring.head, 0);
    atomic_store(&s_ring.tail, 0);
    s_total_samples_written = 0;

    /* Now that the file is open and the ring is reset, publish ACTIVE. The
     * lifecycle lock means stop() cannot interleave, so this cannot race. */
    atomic_store(&s_state, AUDIO_RECORDING_ACTIVE);

    /* Start writer thread */
    s_writer_running = 1;
    if (pthread_create(&s_writer_thread, NULL, writer_thread_func, NULL) != 0) {
        LOGE("Failed to create writer thread");
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        fclose(s_wav_file);
        s_wav_file = NULL;
        return -1;
    }

#ifdef __ANDROID__
    /* Create and start AAudio input stream */
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to create AAudio builder: %s", AAudio_convertResultToText(result));
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        s_writer_running = 0;
        pthread_join(s_writer_thread, NULL);
        fclose(s_wav_file);
        s_wav_file = NULL;
        return -1;
    }

    /* Configure AAudio input stream:
     *   - Direction: INPUT (microphone capture)
     *   - Mono: single channel (matches whisper.cpp and YAMNet input)
     *   - PCM_I16: 16-bit signed integers (matches WAV format)
     *   - Low latency: minimizes time between mic capture and callback delivery
     *   - Data callback: audio_data_callback pushes samples into ring buffer */
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, s_sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, audio_data_callback, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, audio_error_callback, NULL);

    result = AAudioStreamBuilder_openStream(builder, &s_stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to open AAudio stream: %s", AAudio_convertResultToText(result));
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        s_writer_running = 0;
        pthread_join(s_writer_thread, NULL);
        fclose(s_wav_file);
        s_wav_file = NULL;
        return -1;
    }

    result = AAudioStream_requestStart(s_stream);
    if (result != AAUDIO_OK) {
        LOGE("Failed to start AAudio stream: %s", AAudio_convertResultToText(result));
        AAudioStream_close(s_stream);
        s_stream = NULL;
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        s_writer_running = 0;
        pthread_join(s_writer_thread, NULL);
        fclose(s_wav_file);
        s_wav_file = NULL;
        return -1;
    }

    LOGI("Recording started: clip=%s, rate=%d, file=%s",
         s_clip_name, s_sample_rate, s_wav_path);
#else
    LOGI("Recording started (non-Android stub): clip=%s, rate=%d, file=%s",
         s_clip_name, s_sample_rate, s_wav_path);
#endif

    return 0;
}

/* ========================================================================
 * Scheduled recording thread
 *
 * When start_at is specified, we spawn this detached thread so the HTTP
 * response returns immediately. The thread sleeps until start_at via
 * clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME), then calls
 * recording_start_impl() to open the stream.
 *
 * State transitions:
 *   audio_recording_start() sets state to AUDIO_RECORDING_IDLE (validated)
 *   → thread sleeps until start_at
 *   → recording_start_impl() sets state to AUDIO_RECORDING_ACTIVE
 *   If impl fails, state returns to IDLE.
 * ======================================================================== */

static void *scheduled_recording_thread(void *arg) {
    int64_t start_at_ms = *(int64_t *)arg;
    free(arg);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

    if (start_at_ms > now_ms) {
        LOGI("Scheduling recording start for %lld ms (delay: %lld ms)",
             (long long)start_at_ms, (long long)(start_at_ms - now_ms));

        struct timespec ts = {
            .tv_sec  = start_at_ms / 1000,
            .tv_nsec = (start_at_ms % 1000) * 1000000
        };
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL);
    }

    /* The start slot was claimed as SCHEDULED when this thread was created.
     * If anything else has happened since (a stop cancelled it, cleanup ran),
     * the state is no longer SCHEDULED and this start must not proceed. */
    /* Serialise with stop(). Re-check under the lock: a stop may have cancelled
     * us (SCHEDULED->IDLE) while we slept. Start under the lock so stop cannot
     * interleave with initialisation. */
    pthread_mutex_lock(&s_lifecycle_lock);
    if (atomic_load(&s_state) != AUDIO_RECORDING_SCHEDULED) {
        pthread_mutex_unlock(&s_lifecycle_lock);
        LOGE("Scheduled recording cancelled (state changed)");
        return NULL;
    }
    int rc = recording_start_impl();
    pthread_mutex_unlock(&s_lifecycle_lock);
    if (rc != 0) {
        LOGE("Scheduled recording failed to start");
    }
    return NULL;
}

int audio_recording_start(const char *clip_name, int sample_rate, int64_t start_at_ms) {
    /* Argument validation touches only the caller's inputs and s_audio_dir
     * (set once at init), so it needs no lock. */
    if (!clip_name || strlen(clip_name) == 0) {
        LOGE("clip_name is required");
        return -1;
    }
    if (strlen(s_audio_dir) == 0) {
        LOGE("Recording subsystem not initialized (call audio_recording_init first)");
        return -1;
    }
    if (strpbrk(clip_name, "\"\\\n\r\t") != NULL) {
        LOGE("clip_name contains an unsafe character");
        return -1;
    }
    if (strstr(clip_name, "..") || strchr(clip_name, '/')) {
        LOGE("Invalid clip_name (path traversal rejected)");
        return -1;
    }

    /* From here on everything touches shared recording state (s_sample_rate,
     * s_clip_name, s_wav_path, s_state), so it is all under the lifecycle lock.
     * Writing the clip name and path before the lock let a second concurrent
     * start() overwrite them while this one was still using them, even though
     * the state check would reject that second call. */
    pthread_mutex_lock(&s_lifecycle_lock);
    if (atomic_load(&s_state) != AUDIO_RECORDING_IDLE) {
        pthread_mutex_unlock(&s_lifecycle_lock);
        LOGE("Recording already active or scheduled");
        return -1;
    }

    /* Set sample rate (default 16kHz for whisper.cpp compatibility) */
    s_sample_rate = (sample_rate == 44100) ? 44100 : 16000;
    strncpy(s_clip_name, clip_name, AUDIO_RECORDING_MAX_CLIP_NAME);
    s_clip_name[AUDIO_RECORDING_MAX_CLIP_NAME] = '\0';
    snprintf(s_wav_path, sizeof(s_wav_path), "%s/%s.wav", s_audio_dir, s_clip_name);

    /* If start_at is in the future, spawn a scheduling thread so the HTTP
     * response returns immediately. clock_nanosleep(TIMER_ABSTIME) gives
     * kernel-level precision -- no message-queue jitter. The spawned thread
     * re-acquires this same lock after it wakes (see scheduled_recording_thread),
     * and blocks on it only until we release below, so there is no deadlock. */
    if (start_at_ms > 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

        if (start_at_ms > now_ms) {
            int64_t *arg = malloc(sizeof(int64_t));
            if (!arg) {
                pthread_mutex_unlock(&s_lifecycle_lock);
                LOGE("Failed to allocate scheduling arg");
                return -1;
            }
            *arg = start_at_ms;
            atomic_store(&s_state, AUDIO_RECORDING_SCHEDULED);

            pthread_t sched_thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            int rc = pthread_create(&sched_thread, &attr, scheduled_recording_thread, arg);
            pthread_attr_destroy(&attr);

            if (rc != 0) {
                LOGE("Failed to create scheduling thread");
                free(arg);
                atomic_store(&s_state, AUDIO_RECORDING_IDLE);  /* release the claim */
                pthread_mutex_unlock(&s_lifecycle_lock);
                return -1;
            }
            pthread_mutex_unlock(&s_lifecycle_lock);
            return 0;  /* Return immediately — recording starts later */
        }
        /* start_at is in the past — fall through to immediate start */
    }

    /* Immediate start (still holding the lock). */
    atomic_store(&s_state, AUDIO_RECORDING_SCHEDULED);
    int rc = recording_start_impl();
    pthread_mutex_unlock(&s_lifecycle_lock);
    return rc;
}

/**
 * Stop sequence (must happen in this order to avoid data loss):
 *   1. Set state to STOPPING (callback sees this and returns STOP)
 *   2. Request AAudio stream stop (waits for callback to finish)
 *   3. Close AAudio stream (releases hardware)
 *   4. Join writer thread (drains remaining ring buffer to disk)
 *   5. Finalize WAV header (seek back, patch data_size and file_size)
 *   6. Set state to IDLE
 */
int audio_recording_stop(int64_t *out_duration_ms, int64_t *out_file_size) {
    /* The lifecycle lock serialises stop against start and against another
     * stop, so no half-initialised state is observable and two teardowns can
     * never run at once. Held across the whole teardown below. */
    pthread_mutex_lock(&s_lifecycle_lock);
    int cur_state = atomic_load(&s_state);

    /* A scheduled-but-not-started recording holds no resources yet, so
     * cancelling it is just moving the state back to IDLE. The scheduler
     * thread -- blocked on this lock, or about to re-check under it -- then
     * sees IDLE and exits without starting anything. */
    if (cur_state == AUDIO_RECORDING_SCHEDULED) {
        atomic_store(&s_state, AUDIO_RECORDING_IDLE);
        pthread_mutex_unlock(&s_lifecycle_lock);
        LOGI("Scheduled recording cancelled before start");
        if (out_duration_ms) *out_duration_ms = 0;
        if (out_file_size) *out_file_size = 0;
        return 0;
    }

    /* STOPPING is accepted as well as ACTIVE. The AAudio error callback sets
     * STOPPING and nothing else, so without this a mic disconnect left the
     * stream open, the WAV header unfinalised, and both start and stop
     * refusing forever. */
    if (cur_state != AUDIO_RECORDING_ACTIVE && cur_state != AUDIO_RECORDING_STOPPING) {
        pthread_mutex_unlock(&s_lifecycle_lock);
        LOGE("No active recording to stop");
        return -1;
    }

    /* Step 1: Signal stop to callback and writer */
    atomic_store(&s_state, AUDIO_RECORDING_STOPPING);

#ifdef __ANDROID__
    if (s_stream) {
        AAudioStream_requestStop(s_stream);
        AAudioStream_close(s_stream);
        s_stream = NULL;
    }
#endif

    /* Wait for writer thread to drain and exit */
    if (s_writer_running) {
        pthread_join(s_writer_thread, NULL);
        s_writer_running = 0;
    }

    /* Finalize WAV header with actual data size.
     * fflush first to ensure all buffered PCM data is written to disk
     * before we seek back to patch the header fields. */
    if (s_wav_file) {
        fflush(s_wav_file);
        finalize_wav(s_wav_file, s_sample_rate, s_total_samples_written);
        fclose(s_wav_file);
        s_wav_file = NULL;
    }

    /* Calculate duration and file size */
    int64_t duration_ms = 0;
    if (s_sample_rate > 0) {
        duration_ms = (s_total_samples_written * 1000) / s_sample_rate;
    }

    int64_t file_size = 0;
    struct stat st;
    if (stat(s_wav_path, &st) == 0) {
        file_size = st.st_size;
    }

    if (out_duration_ms) *out_duration_ms = duration_ms;
    if (out_file_size) *out_file_size = file_size;

    LOGI("Recording stopped: clip=%s, duration=%lld ms, size=%lld bytes, samples=%lld",
         s_clip_name, (long long)duration_ms, (long long)file_size,
         (long long)s_total_samples_written);

    atomic_store(&s_state, AUDIO_RECORDING_IDLE);
    pthread_mutex_unlock(&s_lifecycle_lock);
    return 0;
}

int audio_recording_is_active(void) {
    /* Anything but IDLE means a stop() is meaningful: capturing, winding
     * down after an error, or scheduled and cancellable. */
    return atomic_load(&s_state) != AUDIO_RECORDING_IDLE ? 1 : 0;
}

const char *audio_recording_get_clip_name(void) {
    if (atomic_load(&s_state) != AUDIO_RECORDING_ACTIVE) return NULL;
    return s_clip_name;
}

const char *audio_recording_get_audio_dir(void) {
    if (s_audio_dir[0] == '\0') return NULL;
    return s_audio_dir;
}
