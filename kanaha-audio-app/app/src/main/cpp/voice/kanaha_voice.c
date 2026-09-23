/*
 * Kanaha Audio - the voice request path inside the audio service
 * Licensed under the Apache License, Version 2.0
 */

#include "kanaha_voice.h"
#include "kanaha_calc.h"
#include "../speech/audio_speak.h"
#include "../recording/audio_recording.h"

#include <axutil_error_default.h>
#include <axutil_log_default.h>
#include <json-c/json.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KanahaVoice", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KanahaVoice", __VA_ARGS__)
#else
#define LOGI(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define LOGE(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_files[512];
static axutil_env_t *s_env;
static axis2_h2_json_client_t *s_client;
static kr_book_t s_books[KR_MAX_BOOKS];
static kr_file_t s_files_cat[KR_MAX_FILES];
static kr_context_t s_ctx;
static int s_ready;             /* books loaded and catalog fetched */
static kr_session_t s_session;

static const char *OUTCOME[] = { "SILENT", "ASK", "REFUSE", "RUN" };

void kanaha_voice_init(const char *files_dir)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_files, sizeof(s_files), "%s", files_dir ? files_dir : "");
    kr_session_init(&s_session);
    pthread_mutex_unlock(&s_lock);
}

static void fail(char *out, size_t size, const char *why)
{
    json_object *o = json_object_new_object();
    json_object_object_add(o, "success", json_object_new_boolean(0));
    json_object_object_add(o, "error", json_object_new_string(why));
    snprintf(out, size, "%s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
    LOGE("%s", why);
}

/* Create the client from voice.json and this phone's certificates. */
static int open_client(char *err, int err_len)
{
    char path[640], crt[640], key[640], ca[640];
    json_object *cfg, *calcs, *v;
    axis2_h2_json_client_options_t o;
    const char *host = NULL, *name = NULL;
    int port = 0;

    if (!s_env) {
        axutil_allocator_t *a = axutil_allocator_init(NULL);
        snprintf(path, sizeof(path), "%s/voice/kanaha-voice.log", s_files);
        s_env = axutil_env_create_with_error_log(a, axutil_error_create(a),
                                                 axutil_log_create(a, NULL, path));
        if (!s_env) { snprintf(err, (size_t)err_len, "cannot create the Axis2/C environment"); return -1; }
    }
    snprintf(path, sizeof(path), "%s/voice/voice.json", s_files);
    cfg = json_object_from_file(path);
    if (!cfg || !json_object_object_get_ex(cfg, "calcs", &calcs)) {
        snprintf(err, (size_t)err_len, "no calcs phone configured: %s is missing or has no "
                 "\"calcs\" object", path);
        if (cfg) json_object_put(cfg);
        return -1;
    }
    if (json_object_object_get_ex(calcs, "host", &v)) host = json_object_get_string(v);
    if (json_object_object_get_ex(calcs, "port", &v)) port = json_object_get_int(v);
    if (json_object_object_get_ex(calcs, "verify_name", &v)) name = json_object_get_string(v);

    snprintf(crt, sizeof(crt), "%s/apache/ssl/server.crt", s_files);
    snprintf(key, sizeof(key), "%s/apache/ssl/server.key", s_files);
    snprintf(ca, sizeof(ca), "%s/apache/ssl/ca.crt", s_files);
    memset(&o, 0, sizeof(o));
    o.host = host;
    o.port = port;
    o.verify_name = name;
    o.ca_file = ca;
    o.cert_file = crt;
    o.key_file = key;
    s_client = axis2_h2_json_client_create(s_env, &o);
    json_object_put(cfg);           /* the client copied the strings */
    if (!s_client) {
        snprintf(err, (size_t)err_len, "cannot set up the connection to the calcs phone "
                 "(check voice.json and this phone's provisioning; see kanaha-voice.log)");
        return -1;
    }
    return 0;
}

static int ensure_ready(char *err, int err_len)
{
    char path[640];
    int n;
    if (s_ready)
        return 0;
    if (!s_files[0]) { snprintf(err, (size_t)err_len, "voice path not initialised"); return -1; }
    if (!s_client && open_client(err, err_len) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s/voice/kanaha-books.json", s_files);
    n = kc_load_books(path, s_books, KR_MAX_BOOKS, err, err_len);
    if (n < 0)
        return -1;
    s_ctx.books = s_books;
    s_ctx.n_books = n;
    n = kc_fetch_catalog(s_client, s_env, s_files_cat, KR_MAX_FILES, err, err_len);
    if (n < 0)
        return -1;
    s_ctx.files = s_files_cat;
    s_ctx.n_files = n;
    s_ready = 1;
    LOGI("voice path ready: %d book(s), %d file(s) on the calcs phone", s_ctx.n_books, n);
    return 0;
}

static void say_now(const char *text)
{
    audio_speak_result_t spoken;
    const char *dir = audio_recording_get_audio_dir();
    if (text && text[0] && dir && audio_speak_text(text, dir, &spoken) != 0)
        LOGE("could not speak: %.60s", text);
}

int kanaha_voice_request(const char *transcript, int speak, char *out, size_t size)
{
    kr_result_t r;
    kc_result_t x;
    char err[KR_SAY_LEN], answer[KA_ANSWER_LEN], say[KR_SAY_LEN], trace[KA_TRACE_LEN];
    json_object *o;

    pthread_mutex_lock(&s_lock);
    if (ensure_ready(err, sizeof(err)) != 0) {
        pthread_mutex_unlock(&s_lock);
        fail(out, size, err);
        return -1;
    }

    kr_resolve(&s_ctx, &s_session, transcript ? transcript : "", &r);
    LOGI("heard \"%.120s\" -> %s: %.200s", transcript ? transcript : "", OUTCOME[r.outcome], r.say);
    memset(&x, 0, sizeof(x));
    answer[0] = say[0] = trace[0] = '\0';
    if (speak && r.outcome != KR_SILENT)
        say_now(r.say);             /* the read-back, before anything runs */
    if (r.outcome == KR_RUN) {
        kc_execute(s_client, s_env, &r.spec, &x);
        ka_answer(&r.spec, &x, answer, sizeof(answer), say, sizeof(say), trace, sizeof(trace));
        if (!x.ok)
            s_ready = 0;            /* refetch the catalog next time: the phone may have changed */
        if (trace[0])
            LOGI("%s", trace);
        if (speak)
            say_now(say);
    }

    o = json_object_new_object();
    json_object_object_add(o, "success", json_object_new_boolean(1));
    json_object_object_add(o, "outcome", json_object_new_string(OUTCOME[r.outcome]));
    json_object_object_add(o, "readback", json_object_new_string(r.say));
    if (r.outcome == KR_RUN) {
        json_object_object_add(o, "ok", json_object_new_boolean(x.ok));
        json_object_object_add(o, "answer", json_object_new_string(answer));
        json_object_object_add(o, "say", json_object_new_string(say));
        json_object_object_add(o, "trace", json_object_new_string(trace));
        json_object_object_add(o, "tools", json_object_new_string(x.tools));
        json_object_object_add(o, "round_trip_ms", json_object_new_int64(x.round_trip_ms));
    }
    snprintf(out, size, "%s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void kanaha_voice_reset(void)
{
    pthread_mutex_lock(&s_lock);
    kr_session_init(&s_session);
    s_ready = 0;
    if (s_client) {                 /* voice.json may name another phone now */
        axis2_h2_json_client_free(s_client, s_env);
        s_client = NULL;
    }
    pthread_mutex_unlock(&s_lock);
}
