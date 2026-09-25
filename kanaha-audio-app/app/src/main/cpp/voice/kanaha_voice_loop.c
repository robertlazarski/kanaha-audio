/*
 * Kanaha Audio - the voice loop, on the phone
 * Licensed under the Apache License, Version 2.0
 *
 * A port of kanaha-voice-loop.py's calculation path. The rules carried over
 * are the ones that were learned by speaking to it; each is commented where
 * it lives.
 */

#include "kanaha_voice_loop.h"
#include "kanaha_voice.h"
#include "../recording/audio_recording.h"
#include "../recording/audio_tone.h"
#include "../whisper/whisper_android_bridge.h"

#include <json-c/json.h>

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KanahaVoiceLoop", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KanahaVoiceLoop", __VA_ARGS__)
#else
#define LOGI(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define LOGE(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

#define SAMPLE_RATE      16000
#define N_CLIPS          4
#define MAX_FOLLOW_UPS   2
#define THIN_CHARS       25

typedef enum { ST_STOPPED, ST_STARTING, ST_RUNNING, ST_STOPPING } state_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static state_t s_state = ST_STOPPED;
static volatile int s_stop;
static kvl_config_t s_cfg;
static char s_model[32];
static long s_clips, s_triggers, s_requests;
static char s_last_heard[512], s_last_outcome[16], s_last_error[256];

/* The last requests, kept here because logcat on a busy phone rotates them
 * out within minutes: what was heard, what came of it, and what was said. */
#define HISTORY 20
typedef struct { char when[16]; char heard[256]; char outcome[16]; char said[256]; } entry_t;
static entry_t s_hist[HISTORY];
static int s_hist_n, s_hist_next;

/* Things for someone else to act on -- today only the demo's app switch,
 * which the laptop polls for and performs over adb. Numbered, so a poller
 * acts on each once: it remembers the last seq it handled. */
#define EVENTS 8
typedef struct { long seq; char when[16]; char what[32]; } event_t;
static event_t s_events[EVENTS];
static int s_ev_n, s_ev_next;
static long s_ev_seq;

static const char *TRIGGERS[] = { "calculate", "run it", "stress it", "simulate it" };
#define N_TRIGGERS 4

/* Everything the trigger clips are searched for: the calculation triggers,
 * then the phrases that act by themselves, with no dictation window. The
 * camera is the one thing that changes state, so it has its own phrase,
 * never reachable from dictation. "change the demo" is matched without its
 * "Claude, ... to Kanaha Calcs": whisper hears "Claude" as "cloud" and
 * "Kanaha" several ways, and the three words in the middle are enough. */
#define KW_CAMERA_START  N_TRIGGERS
#define KW_CAMERA_STOP   (N_TRIGGERS + 1)
#define KW_SWITCH        (N_TRIGGERS + 2)
static const char *KEYWORDS[] = { "calculate", "run it", "stress it", "simulate it",
                                  "start the cameras please", "stop the cameras please",
                                  "change the demo" };
#define N_KEYWORDS 7

/* Said inside a window, any of these throws the utterance away. Decided here,
 * not by the resolver: a cancel must cost nothing and must be impossible to
 * misread as a request. */
static const char *CANCELS[] = { "cancel", "never mind", "nevermind", "forget it",
                                 "scratch that", "belay that", "ignore that", "stop stop" };
#define N_CANCELS 8

/* The signal vocabulary, told apart across a room without looking:
 * OPEN one high tone, WORKING one short mid tone, DROPPED high falling to
 * low, NOTHING one long low tone. Dropped and nothing are deliberately
 * different: "you cancelled" and "I heard nothing" are different events. */
typedef struct { int hz, ms; } tone_t;
static const tone_t CUE_OPEN[]    = { { 1000, 200 } };
static const tone_t CUE_WORKING[] = { { 600, 90 } };
static const tone_t CUE_DROPPED[] = { { 880, 140 }, { 440, 240 } };
static const tone_t CUE_NOTHING[] = { { 330, 420 } };
/* Rising: the camera is rolling. Falling: it has stopped. Played only once the
 * camera phone has confirmed -- a tone on the request would say "rolling"
 * about a camera that is not. */
static const tone_t CUE_CAM_START[] = { { 660, 120 }, { 990, 180 } };
static const tone_t CUE_CAM_STOP[]  = { { 990, 120 }, { 660, 180 } };
#define CUE(t) cue(t, (int)(sizeof(t) / sizeof(t[0])))

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Sleep in short steps so a stop request is honoured within 100 ms. */
static int nap(double secs)
{
    double end = now_s() + secs;
    while (!s_stop && now_s() < end) {
        double left = end - now_s();
        usleep((useconds_t)((left > 0.1 ? 0.1 : left) * 1e6));
    }
    return !s_stop;
}

/* playTone returns once the tone is scheduled, so the notes of a signal are
 * spaced here, and the call returns only after the last one has sounded --
 * a window that opens under its own tone records the tone. */
static void cue(const tone_t *t, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        audio_tone_play(t[i].hz, t[i].ms, 0);
        usleep((useconds_t)(t[i].ms + 60) * 1000);
    }
}

/* The recorder's directory is written "<files>/models/../audio", and the
 * whisper bridge refuses any path containing "..". Resolved once here, or
 * every search is rejected -- which is what happened for the first two days
 * this ran: 23,685 clips recorded, none searched, and a status that said
 * "running, 0 triggers". */
static void clip_path(const char *clip, char *buf, size_t len)
{
    static char resolved[PATH_MAX];
    const char *dir = audio_recording_get_audio_dir();
    if (!resolved[0] && dir && !realpath(dir, resolved))
        resolved[0] = '\0';
    snprintf(buf, len, "%s/%s.wav", resolved[0] ? resolved : (dir ? dir : "."), clip);
}

/* A clip that has done its job: nothing heard is kept unless asked. */
static void discard(const char *clip)
{
    char path[640];
    if (s_cfg.keep_clips)
        return;
    clip_path(clip, path, sizeof(path));
    unlink(path);
}

static int rec_start(const char *clip)
{
    if (audio_recording_start(clip, SAMPLE_RATE, 0) != 0) {
        LOGE("could not start recording %s", clip);
        snprintf(s_last_error, sizeof(s_last_error), "could not start recording %s", clip);
        return -1;
    }
    return 0;
}

static void rec_stop(void)
{
    int64_t dur = 0, size = 0;
    if (audio_recording_is_active())
        audio_recording_stop(&dur, &size);
}

/* Whisper narrates what it could not transcribe ("[BLANK_AUDIO]", "(beep)")
 * and marks speaker changes (">>"). None of it is speech. */
static void clean(char *s)
{
    char out[1024];
    int o = 0, i, depth = 0;
    for (i = 0; s[i] && o < (int)sizeof(out) - 1; i++) {
        char c = s[i];
        if (c == '[' || c == '(') { depth++; continue; }
        if ((c == ']' || c == ')') && depth > 0) { depth--; continue; }
        if (depth) continue;
        if (c == '>' && s[i + 1] == '>') { i++; continue; }
        if (isspace((unsigned char)c) && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = isspace((unsigned char)c) ? ' ' : c;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
    memcpy(s, out, (size_t)o + 1);
}

static void transcribe(const char *clip, char *out, size_t len)
{
    char path[640];
    whisper_transcribe_result_t r;
    memset(&r, 0, sizeof(r));
    out[0] = '\0';
    clip_path(clip, path, sizeof(path));
    if (whisper_bridge_transcribe(path, NULL, &r) == 0 && r.text) {
        snprintf(out, len, "%s", r.text);
        clean(out);
    }
    free(r.text);
}

static int lower_contains_word(const char *text, const char *phrase)
{
    char low[1100];
    size_t i, o = 1;
    low[0] = ' ';
    for (i = 0; text[i] && o < sizeof(low) - 2; i++) {
        char c = (char)tolower((unsigned char)text[i]);
        low[o++] = (c == ',' || c == '.' || c == '!' || c == '?') ? ' ' : c;
    }
    low[o++] = ' ';
    low[o] = '\0';
    {
        char pat[64];
        snprintf(pat, sizeof(pat), " %s ", phrase);
        return strstr(low, pat) != NULL;
    }
}

static const char *cancelled(const char *text)
{
    int i;
    for (i = 0; i < N_CANCELS; i++)
        if (lower_contains_word(text, CANCELS[i]))
            return CANCELS[i];
    return NULL;
}

/* Remove the trigger words, so the resolver sees the request, not the cue. */
static void strip_triggers(char *text)
{
    int i;
    for (i = 0; i < N_TRIGGERS; i++) {
        size_t tl = strlen(TRIGGERS[i]);
        char *p;
        char low[1024];
        size_t k;
        for (k = 0; text[k] && k < sizeof(low) - 1; k++) low[k] = (char)tolower((unsigned char)text[k]);
        low[k] = '\0';
        if ((p = strstr(low, TRIGGERS[i]))) {
            size_t at = (size_t)(p - low);
            memmove(text + at, text + at + tl, strlen(text + at + tl) + 1);
        }
    }
    /* trim the punctuation and spaces the cut leaves at either end */
    while (*text && strchr(" ,.;:", *text)) memmove(text, text + 1, strlen(text));
    while (*text && strchr(" ,;:", text[strlen(text) - 1])) text[strlen(text) - 1] = '\0';
}

static int trigger_in(const char *clip)
{
    char path[640];
    whisper_search_result_t r;
    int i, j;
    memset(&r, 0, sizeof(r));
    clip_path(clip, path, sizeof(path));
    if (whisper_bridge_search_keywords(path, KEYWORDS, N_KEYWORDS, NULL, &r) != 0) {
        /* A failed search must not look like a quiet room. */
        pthread_mutex_lock(&s_lock);
        snprintf(s_last_error, sizeof(s_last_error), "keyword search failed on %s", path);
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    for (i = 0; i < r.num_matches; i++) {
        for (j = 0; j < N_KEYWORDS; j++) {
            char kw[128];
            size_t k;
            for (k = 0; r.matches[i].keyword[k] && k < sizeof(kw) - 1; k++)
                kw[k] = (char)tolower((unsigned char)r.matches[i].keyword[k]);
            kw[k] = '\0';
            if (strstr(kw, KEYWORDS[j]) && r.matches[i].confidence >= s_cfg.min_confidence) {
                LOGI("[clip %s] trigger '%s' at %.2f", clip, KEYWORDS[j], r.matches[i].confidence);
                return j;
            }
        }
    }
    return -1;
}

/* One window: OPEN, record, WORKING, transcribe. The text, cleaned. */
static void window(const char *clip, char *text, size_t len)
{
    CUE(CUE_OPEN);
    text[0] = '\0';
    if (s_stop || rec_start(clip) != 0)
        return;
    nap(s_cfg.spec_secs);
    rec_stop();
    CUE(CUE_WORKING);
    transcribe(clip, text, len);
}

static void remember_said(const char *heard, const char *outcome, const char *said)
{
    entry_t *e;
    time_t t = time(NULL);
    struct tm tmv;
    pthread_mutex_lock(&s_lock);
    snprintf(s_last_heard, sizeof(s_last_heard), "%s", heard);
    snprintf(s_last_outcome, sizeof(s_last_outcome), "%s", outcome);
    e = &s_hist[s_hist_next];
    localtime_r(&t, &tmv);
    strftime(e->when, sizeof(e->when), "%H:%M:%S", &tmv);
    snprintf(e->heard, sizeof(e->heard), "%s", heard);
    snprintf(e->outcome, sizeof(e->outcome), "%s", outcome);
    snprintf(e->said, sizeof(e->said), "%s", said ? said : "");
    s_hist_next = (s_hist_next + 1) % HISTORY;
    if (s_hist_n < HISTORY) s_hist_n++;
    pthread_mutex_unlock(&s_lock);
}

static void remember(const char *heard, const char *outcome)
{
    remember_said(heard, outcome, NULL);
}

static void post_event(const char *what)
{
    event_t *e;
    time_t t = time(NULL);
    struct tm tmv;
    pthread_mutex_lock(&s_lock);
    e = &s_events[s_ev_next];
    e->seq = ++s_ev_seq;
    localtime_r(&t, &tmv);
    strftime(e->when, sizeof(e->when), "%H:%M:%S", &tmv);
    snprintf(e->what, sizeof(e->what), "%s", what);
    s_ev_next = (s_ev_next + 1) % EVENTS;
    if (s_ev_n < EVENTS) s_ev_n++;
    pthread_mutex_unlock(&s_lock);
    LOGI("[event %ld] %s", s_ev_seq, what);
}

/* A phrase that acts by itself: the camera, or the demo's app switch. */
static void handle_action(int kw)
{
    char err[256];
    rec_stop();
    if (kw == KW_CAMERA_START || kw == KW_CAMERA_STOP) {
        int start = kw == KW_CAMERA_START;
        if (kanaha_voice_camera(start, err, sizeof(err)) == 0) {
            remember_said(KEYWORDS[kw], start ? "CAMERA_ON" : "CAMERA_OFF", NULL);
            if (start) CUE(CUE_CAM_START); else CUE(CUE_CAM_STOP);
        } else {
            remember_said(KEYWORDS[kw], "CAMERA_ERROR", err);
            CUE(CUE_NOTHING);
            kanaha_voice_say(err);
        }
    } else if (kw == KW_SWITCH) {
        post_event("switch-to-calcs");
        remember_said(KEYWORDS[kw], "SWITCH", "Changing the demo to Kanaha Calcs.");
        kanaha_voice_say("Changing the demo to Kanaha Calcs.");
    }
}

/* The request after a trigger in `trigger_clip`; `bridge_clip` was recording
 * when the trigger was found and holds whatever followed the word. */
static void handle_request(const char *trigger_clip, const char *bridge_clip)
{
    char text[1024], extra[1024], json[4096];
    const char *c;
    int turn;

    rec_stop();                         /* the bridge clip ends here */
    window("vl_spec", text, sizeof(text));
    LOGI("[dictation] heard '%s'", text);

    if ((c = cancelled(text))) {
        LOGI("[dictation] '%s' - cancelled, nothing sent", c);
        remember(text, "CANCELLED");
        CUE(CUE_DROPPED);
        return;
    }
    /* Short is not the same as truncated: whisper punctuates an utterance it
     * heard the end of. Only a thin clip with no final punctuation earns the
     * cost of reading the two clips in front of it, which hold the front of a
     * request from a speaker who did not wait for the tone. */
    {
        size_t l = strlen(text);
        int complete = l > 0 && strchr(".?!", text[l - 1]) != NULL;
        if (l == 0 || (l < THIN_CHARS && !complete)) {
            char t1[512], t2[512];
            transcribe(trigger_clip, t1, sizeof(t1));
            transcribe(bridge_clip, t2, sizeof(t2));
            snprintf(extra, sizeof(extra), "%s %s %s", t1, t2, text);
            snprintf(text, sizeof(text), "%s", extra);
            clean(text);
            LOGI("[dictation] thin clip; with the clips in front: '%s'", text);
        }
    }
    strip_triggers(text);
    if ((c = cancelled(text))) {
        remember(text, "CANCELLED");
        CUE(CUE_DROPPED);
        return;
    }
    if (!text[0]) {
        remember("", "NOTHING");
        CUE(CUE_NOTHING);
        return;
    }

    for (turn = 0; turn <= MAX_FOLLOW_UPS && !s_stop; turn++) {
        json_object *r, *o;
        const char *outcome = "?";
        s_requests++;
        kanaha_voice_request(text, 1, json, sizeof(json));   /* speaks read-back and answer */
        r = json_tokener_parse(json);
        if (r && json_object_object_get_ex(r, "outcome", &o))
            outcome = json_object_get_string(o);
        else if (r && json_object_object_get_ex(r, "error", &o)) {
            pthread_mutex_lock(&s_lock);
            snprintf(s_last_error, sizeof(s_last_error), "%s", json_object_get_string(o));
            pthread_mutex_unlock(&s_lock);
            outcome = "ERROR";
        }
        LOGI("[request] '%s' -> %s", text, outcome);
        {
            /* What the room heard: the answer's SAY line, or the question,
             * refusal or read-back when there was no answer. */
            json_object *said = NULL;
            if (!(r && json_object_object_get_ex(r, "say", &said)) &&
                !(r && json_object_object_get_ex(r, "readback", &said)))
                said = NULL;
            remember_said(text, outcome, said ? json_object_get_string(said) : NULL);
        }

        /* The person heard the window open and spoke: silence would read as a
         * fault, so nothing calculable gets the nothing-heard tone. */
        if (strcmp(outcome, "SILENT") == 0 || strcmp(outcome, "ERROR") == 0) {
            CUE(CUE_NOTHING);
            if (r) json_object_put(r);
            return;
        }
        /* A question opens its own answer window. One that waited for the
         * trigger word again could not be answered: "simulation forward",
         * said after the question, was dropped that way in the trials. */
        if (strcmp(outcome, "ASK") != 0 || turn == MAX_FOLLOW_UPS) {
            if (r) json_object_put(r);
            return;
        }
        if (r) json_object_put(r);
        window("vl_answer", text, sizeof(text));
        strip_triggers(text);
        LOGI("[answer] heard '%s'", text);
        if ((c = cancelled(text))) {
            remember(text, "CANCELLED");
            CUE(CUE_DROPPED);
            return;
        }
        if (!text[0]) {
            remember("", "NOTHING");
            CUE(CUE_NOTHING);
            return;
        }
    }
}

static void *loop_main(void *arg)
{
    static const char *CLIPS[N_CLIPS] = { "vl_0", "vl_1", "vl_2", "vl_3" };
    int ci = 0;
    double last_request = -1e9;
    (void)arg;

    if (whisper_bridge_load_model(s_model) != 0) {
        pthread_mutex_lock(&s_lock);
        snprintf(s_last_error, sizeof(s_last_error), "could not load whisper model %s", s_model);
        s_state = ST_STOPPED;
        pthread_mutex_unlock(&s_lock);
        LOGE("could not load whisper model %s", s_model);
        return NULL;
    }
    pthread_mutex_lock(&s_lock);
    s_state = ST_RUNNING;
    pthread_mutex_unlock(&s_lock);
    LOGI("listening: clips %.1f s, windows %.1f s, model %s", s_cfg.clip_secs, s_cfg.spec_secs, s_model);

    if (rec_start(CLIPS[ci]) != 0)
        s_stop = 1;
    while (!s_stop) {
        const char *just;
        int hit;
        if (!nap(s_cfg.clip_secs))
            break;
        /* Pipelined: re-arm first, then search the clip that just closed while
         * the next one records. A serial loop left the microphone off for the
         * whole search, and a trigger said in that gap was simply never heard. */
        rec_stop();
        just = CLIPS[ci];
        ci = (ci + 1) % N_CLIPS;
        if (rec_start(CLIPS[ci]) != 0)
            break;
        s_clips++;
        hit = trigger_in(just);
        if (hit < 0) {
            discard(just);
            continue;
        }
        s_triggers++;
        if (now_s() - last_request < s_cfg.cooldown_secs) {
            LOGI("trigger within cooldown - ignored");
            continue;
        }
        if (hit >= N_TRIGGERS)
            handle_action(hit);
        else
            handle_request(just, CLIPS[ci]);
        last_request = now_s();
        discard(just);
        discard(CLIPS[ci]);
        discard("vl_spec");
        discard("vl_answer");
        /* back to listening */
        ci = (ci + 1) % N_CLIPS;
        if (!s_stop && rec_start(CLIPS[ci]) != 0)
            break;
    }
    rec_stop();
    discard(CLIPS[ci]);
    pthread_mutex_lock(&s_lock);
    s_state = ST_STOPPED;
    pthread_mutex_unlock(&s_lock);
    LOGI("stopped");
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* autostart                                                                 */
/* ------------------------------------------------------------------------ */

/* The audio service initialises on its first request (the adapter's lazy
 * init), so nothing would start the loop until someone sent one. Instead a
 * thread started when this binary loads waits for Apache to come up, runs
 * that same once-only init, and starts the loop if voice/voice.json says
 *
 *   "loop": {"autostart": true, "clip_secs": 6, "spec_secs": 8}
 *
 * It relies on httpd running as one process (-X, which is how AudioService
 * launches it): a thread started before a fork would not reach the child.
 * The app must be in the foreground for the microphone to hear, which it is
 * when the server is started from the app. */
extern void kanaha_audio_ensure_initialised(void);

static void *autostart_main(void *arg)
{
    sigset_t all;
    char path[640], err[256];
    json_object *cfg, *loop, *v;
    kvl_config_t c;
    const char *files;
    (void)arg;

    sigfillset(&all);                   /* Apache's signals are for Apache */
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    sleep(4);

    kanaha_audio_ensure_initialised();
    files = kanaha_voice_files_dir();
    if (!files || !files[0])
        return NULL;
    snprintf(path, sizeof(path), "%s/voice/voice.json", files);
    cfg = json_object_from_file(path);
    if (!cfg)
        return NULL;
    if (!json_object_object_get_ex(cfg, "loop", &loop) ||
        !json_object_object_get_ex(loop, "autostart", &v) || !json_object_get_boolean(v)) {
        json_object_put(cfg);
        return NULL;
    }
    memset(&c, 0, sizeof(c));
    if (json_object_object_get_ex(loop, "clip_secs", &v)) c.clip_secs = json_object_get_double(v);
    if (json_object_object_get_ex(loop, "spec_secs", &v)) c.spec_secs = json_object_get_double(v);
    json_object_put(cfg);

    /* Connect and fetch the catalog first, so a wrong address or a missing
     * book is in the status now, not discovered by the first thing somebody
     * says. It does not stop the loop: a calcs phone that is off at startup
     * may be on by the first request, and the request says so if it is not. */
    {
        char prep[256];
        int prepared = kanaha_voice_prepare(prep, sizeof(prep)) == 0;
        if (kanaha_voice_loop_start(&c, err, sizeof(err)) != 0) {
            pthread_mutex_lock(&s_lock);
            snprintf(s_last_error, sizeof(s_last_error), "autostart: %s", err);
            pthread_mutex_unlock(&s_lock);
            LOGE("autostart: %s", err);
            return NULL;
        }
        if (!prepared) {
            pthread_mutex_lock(&s_lock);
            snprintf(s_last_error, sizeof(s_last_error), "autostart: listening, but %s", prep);
            pthread_mutex_unlock(&s_lock);
            LOGE("autostart: listening, but %s", prep);
        }
    }
    LOGI("autostart: listening");
    return NULL;
}

__attribute__((constructor))
static void autostart_at_load(void)
{
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, autostart_main, NULL);
    pthread_attr_destroy(&attr);
}

int kanaha_voice_loop_start(const kvl_config_t *cfg, char *err, int err_len)
{
    pthread_t t;
    pthread_attr_t attr;

    pthread_mutex_lock(&s_lock);
    if (s_state != ST_STOPPED) {
        snprintf(err, (size_t)err_len, "the voice loop is already %s",
                 s_state == ST_STOPPING ? "stopping" : "running");
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    if (audio_recording_is_active()) {
        snprintf(err, (size_t)err_len, "the microphone is already recording (%s); stop it first",
                 audio_recording_get_clip_name() ? audio_recording_get_clip_name() : "?");
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.clip_secs = (cfg && cfg->clip_secs > 0) ? cfg->clip_secs : 6.0;
    s_cfg.spec_secs = (cfg && cfg->spec_secs > 0) ? cfg->spec_secs : 8.0;
    s_cfg.cooldown_secs = (cfg && cfg->cooldown_secs > 0) ? cfg->cooldown_secs : 4.0;
    s_cfg.min_confidence = (cfg && cfg->min_confidence > 0) ? cfg->min_confidence : 0.5f;
    snprintf(s_model, sizeof(s_model), "%s", (cfg && cfg->model && cfg->model[0]) ? cfg->model : "tiny.en");
    s_cfg.keep_clips = cfg ? cfg->keep_clips : 0;
    s_clips = s_triggers = s_requests = 0;
    s_last_heard[0] = s_last_outcome[0] = s_last_error[0] = '\0';
    s_stop = 0;
    s_state = ST_STARTING;
    pthread_mutex_unlock(&s_lock);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, loop_main, NULL) != 0) {
        pthread_attr_destroy(&attr);
        pthread_mutex_lock(&s_lock);
        s_state = ST_STOPPED;
        pthread_mutex_unlock(&s_lock);
        snprintf(err, (size_t)err_len, "could not start the voice loop thread");
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void kanaha_voice_loop_stop(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_state == ST_STARTING || s_state == ST_RUNNING) {
        s_state = ST_STOPPING;
        s_stop = 1;
    }
    pthread_mutex_unlock(&s_lock);
}

int kanaha_voice_loop_active(void)
{
    int a;
    pthread_mutex_lock(&s_lock);
    a = s_state != ST_STOPPED;
    pthread_mutex_unlock(&s_lock);
    return a;
}

void kanaha_voice_loop_status(char *out, size_t size)
{
    static const char *NAMES[] = { "stopped", "starting", "running", "stopping" };
    json_object *o = json_object_new_object();
    pthread_mutex_lock(&s_lock);
    json_object_object_add(o, "success", json_object_new_boolean(1));
    json_object_object_add(o, "state", json_object_new_string(NAMES[s_state]));
    json_object_object_add(o, "clips", json_object_new_int64(s_clips));
    json_object_object_add(o, "triggers", json_object_new_int64(s_triggers));
    json_object_object_add(o, "requests", json_object_new_int64(s_requests));
    json_object_object_add(o, "last_heard", json_object_new_string(s_last_heard));
    json_object_object_add(o, "last_outcome", json_object_new_string(s_last_outcome));
    json_object_object_add(o, "last_error", json_object_new_string(s_last_error));
    {
        json_object *h = json_object_new_array();
        int i;
        for (i = 0; i < s_hist_n; i++) {
            entry_t *e = &s_hist[(s_hist_next - s_hist_n + i + HISTORY) % HISTORY];
            json_object *x = json_object_new_object();
            json_object_object_add(x, "when", json_object_new_string(e->when));
            json_object_object_add(x, "heard", json_object_new_string(e->heard));
            json_object_object_add(x, "outcome", json_object_new_string(e->outcome));
            json_object_object_add(x, "said", json_object_new_string(e->said));
            json_object_array_add(h, x);
        }
        json_object_object_add(o, "history", h);
    }
    {
        json_object *ev = json_object_new_array();
        int i;
        for (i = 0; i < s_ev_n; i++) {
            event_t *e = &s_events[(s_ev_next - s_ev_n + i + EVENTS) % EVENTS];
            json_object *x = json_object_new_object();
            json_object_object_add(x, "seq", json_object_new_int64(e->seq));
            json_object_object_add(x, "when", json_object_new_string(e->when));
            json_object_object_add(x, "what", json_object_new_string(e->what));
            json_object_array_add(ev, x);
        }
        json_object_object_add(o, "events", ev);
    }
    pthread_mutex_unlock(&s_lock);
    snprintf(out, size, "%s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
}
