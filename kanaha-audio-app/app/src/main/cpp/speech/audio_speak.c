/*
 * Kanaha Audio — speech output (the `speak` operation)
 *
 * Says a line of text out of the phone's own speaker. It exists because a
 * read-back nobody hears is not a read-back: the person is standing next to
 * this phone, and anything that speaks from the laptop across the room is the
 * wrong speaker in a real room.
 *
 * WHY FLITE AND NOT ANDROID'S ENGINE. Android's TextToSpeech is a Java API
 * with no C entry point, so using it would mean either JNI or the Intent IPC
 * boundary this app exists to avoid (see the header of audio_search_service.c:
 * every dependency here is permissive C, linked in-process, no IPC). Flite is
 * BSD-licensed C from CMU, so it links like whisper.cpp and TFLite already do
 * and the Apache-2.0 posture of the app is unchanged. The cost is a voice that
 * sounds like 1998; for reading back five tickers and a date range, that is the
 * right trade, and a listener that never depends on a laptop is worth more than
 * a pleasant voice.
 *
 * The synthesised audio is written to a temporary WAV and handed to the
 * existing playback path rather than opened as a second output stream: one
 * AAudio output path, already proven by playTone and playAudio, is easier to
 * reason about than two.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_speak.h"
#include "../recording/audio_tone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include <flite/flite.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioSpeak"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#define LOGE(...) do {} while (0)
#endif

/* Provided by libflite_cmu_us_kal16.a — a 16 kHz diphone voice, which matches
 * the sample rate everything else in this app records and plays at. */
cst_voice *register_cmu_us_kal16(const char *voxdir);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static cst_voice *s_voice = NULL;   /* registered once; flite voices are reusable */

/*
 * Register the voice on first use. Flite's initialisation is not thread safe
 * and the service is multi-threaded under the worker MPM, so it happens under
 * the same lock that serialises speaking.
 */
static int ensure_voice(void) {
    if (s_voice) return 0;
    flite_init();
    s_voice = register_cmu_us_kal16(NULL);
    if (!s_voice) {
        LOGE("Could not register the voice");
        return -1;
    }
    LOGI("Voice registered (cmu_us_kal16, 16 kHz)");
    return 0;
}

int audio_speak_text(const char *text, const char *audio_dir,
                     audio_speak_result_t *result) {
    char wav_path[512];
    char play_path[PATH_MAX];
    cst_wave *wave = NULL;
    int rc = -1;

    if (!text || !text[0] || !audio_dir || !result) return -1;
    memset(result, 0, sizeof(*result));

    if (strlen(text) > AUDIO_SPEAK_MAX_TEXT) {
        LOGE("Text too long: %zu chars (max %d)", strlen(text), AUDIO_SPEAK_MAX_TEXT);
        return -1;
    }

    /* One speaker, one voice, one utterance at a time. Two overlapping calls
     * would talk over each other on the same speaker even if flite allowed it. */
    pthread_mutex_lock(&s_lock);

    if (ensure_voice() != 0) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    wave = flite_text_to_wave(text, s_voice);
    if (!wave) {
        LOGE("Synthesis failed");
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    result->sample_rate = wave->sample_rate;
    result->duration_ms = (wave->sample_rate > 0)
        ? (int)((long long)wave->num_samples * 1000 / wave->sample_rate) : 0;

    /* A fixed name in the app's own audio directory: the file is an artefact of
     * this call, not something a caller names, so there is no path to validate
     * and nothing a caller can point at. */
    snprintf(wav_path, sizeof(wav_path), "%s/.speak.wav", audio_dir);
    if (cst_wave_save_riff(wave, wav_path) != 0) {
        LOGE("Could not write %s", wav_path);
        delete_wave(wave);
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    delete_wave(wave);

    /* The playback helper refuses any path containing "..", which is the right
     * rule for a caller-supplied name. The audio directory this service is
     * initialised with is itself written as "<files>/models/../audio", so the
     * path has to be resolved before it is handed over — the guard stays intact
     * and what reaches it is the real location of a file we just wrote. */
    if (!realpath(wav_path, play_path)) {
        LOGE("Could not resolve %s", wav_path);
        unlink(wav_path);
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    LOGI("Speaking %d ms at %d Hz", result->duration_ms, result->sample_rate);
    rc = audio_tone_play_file(play_path);
    unlink(play_path);

    pthread_mutex_unlock(&s_lock);

    if (rc != 0) {
        LOGE("Playback failed");
        return -1;
    }
    return 0;
}
