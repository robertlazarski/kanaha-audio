/*
 * Kanaha Audio
 * Axis2/C Static Service Adapter — Bridges framework to application
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * This file provides the STRONG symbol that overrides the WEAK symbol
 * in axis2_json_rpc_msg_recv.c at link time. This is the integration
 * point between the Axis2/C framework and the application service.
 *
 * Architecture (from HTTP2_ANDROID.md):
 *
 *   [Axis2/C Framework]                  [This File]                [Application]
 *          |                                  |                          |
 *   mod_axis2 receives                 Strong symbol:              Service impl:
 *   HTTP/2 JSON-RPC request     audio_search_service_         audio_search_service_
 *          |                    invoke_json(env, json_obj)    invoke_json_impl(str, str, size)
 *          v                                  |                          |
 *   axis2_json_rpc_msg_recv                   |                          |
 *          |                                  v                          v
 *   android_static_service_lookup     Converts json_object* -----> char* string
 *   ("AudioSearchService")            to/from char* strings  <----- char* response
 *          |                                  |
 *          v                                  |
 *   Calls function pointer ---------> This function runs
 *   (strong overrides weak)
 *
 * Without this adapter, the weak stub in Axis2/C returns NULL,
 * and the framework responds with "service not available."
 *
 * With this adapter linked (via --whole-archive), HTTP/2 JSON-RPC
 * requests flow through the full Axis2/C dispatch chain into
 * whisper.cpp inference.
 */

#ifdef __ANDROID__

#include <json-c/json.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#ifdef AXIS2_BUILD
#include <axutil_env.h>
#else
/* Minimal typedef when building without full Axis2/C headers */
typedef struct axutil_env axutil_env_t;
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioAdapter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#endif

/* External: service lifecycle (from audio_search_service.c).
 * Under Path B the old main() is gone, so nothing initialises the service
 * subsystems -- whisper, recording, SFTP and YAMNet -- and every request that
 * touches them fails with "not initialized". Apache forks its child before any
 * request arrives, so we initialise lazily on first dispatch rather than at
 * load time, and exactly once per process. */
extern int audio_search_service_init(const char *models_dir);

static pthread_once_t kanaha_audio_init_once = PTHREAD_ONCE_INIT;

static void kanaha_audio_init_locked(void)
{
    const char *models_dir = getenv("KANAHA_AUDIO_MODELS");
    if (!models_dir || !*models_dir) {
        LOGE("KANAHA_AUDIO_MODELS is unset -- recording, whisper and SFTP "
             "will report 'not initialized'. AudioService.java sets it.");
        return;
    }
    LOGI("Initialising audio service subsystems from %s", models_dir);
    audio_search_service_init(models_dir);
}

/* The same once-only initialisation the first request runs, for code that
 * must start before any request arrives (the voice loop's autostart). */
void kanaha_audio_ensure_initialised(void)
{
    pthread_once(&kanaha_audio_init_once, kanaha_audio_init_locked);
}

/* External: application service implementation (from audio_search_service.c) */
extern int audio_search_service_invoke_json_impl(
    const char *json_request,
    char *json_response,
    size_t response_size);

/*
 * STRONG SYMBOL — overrides the weak stub in axis2_json_rpc_msg_recv.c
 *
 * This is the function that Axis2/C calls when a request arrives for
 * the AudioSearchService. The framework resolves this at link time:
 *
 *   1. Axis2/C core has:  __attribute__((weak)) audio_search_service_invoke_json() -> NULL
 *   2. This file has:     audio_search_service_invoke_json() -> actual implementation
 *   3. Linker picks:      strong symbol wins
 *
 * The adapter converts between Axis2/C's json_object* API and the
 * application's char* API, keeping the service implementation free
 * of Axis2/C header dependencies.
 */
json_object *audio_search_service_invoke_json(
    const axutil_env_t *env,
    json_object *json_request)
{
    (void)env;  /* Not needed — Android services use action-based dispatch */

    pthread_once(&kanaha_audio_init_once, kanaha_audio_init_locked);

    if (!json_request) {
        LOGE("audio_search_service_invoke_json: NULL request");
        return NULL;
    }

    /* Convert json_object* to char* for the _impl function */
    const char *request_str = json_object_to_json_string_ext(
        json_request, JSON_C_TO_STRING_PLAIN);

    if (!request_str) {
        LOGE("audio_search_service_invoke_json: failed to serialize request");
        return NULL;
    }

    LOGI("Adapter dispatching: %.80s%s",
         request_str, strlen(request_str) > 80 ? "..." : "");

    /* Call the application service implementation */
    const size_t response_buffer_size = 65536;
    char *response_buffer = (char *)malloc(response_buffer_size);
    if (!response_buffer) {
        LOGE("Failed to allocate response buffer");
        return NULL;
    }
    memset(response_buffer, 0, response_buffer_size);

    int rc = audio_search_service_invoke_json_impl(
        request_str, response_buffer, response_buffer_size);

    if (rc != 0 && response_buffer[0] == '\0') {
        LOGE("Service returned error with no response");
        free(response_buffer);
        return NULL;
    }

    /* Convert char* response back to json_object* for Axis2/C */
    json_object *response = json_tokener_parse(response_buffer);
    free(response_buffer);
    if (!response) {
        LOGE("Failed to parse service response as JSON");
        return NULL;
    }

    LOGI("Adapter returning response");
    return response;
}

#endif /* __ANDROID__ */
