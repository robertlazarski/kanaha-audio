/*
 * Kanaha Audio
 * Axis2/C Audio Search Service Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * This file follows the same architecture as the Axis2/C userguide samples
 * and Kanaha Camera's camera_control_service.c:
 * - audio_search_service_invoke_json_impl() - Entry point (matches server pattern)
 * - whisper_bridge_*() - Speech operations (keyword search, transcription)
 * - yamnet_bridge_*()  - Audio event detection (instruments, applause, silence)
 *
 * DUAL-MODEL ARCHITECTURE:
 *
 * A single curl request dispatches to one of two ML models via the "action" field:
 *
 *   curl → HTTPS/HTTP2+mTLS → Axis2/C → audio_search_service_invoke_json_impl()
 *     → "action":"searchKeywords"    → whisper_bridge → whisper.cpp  (speech)
 *     → "action":"detectAudioEvents" → yamnet_bridge  → TFLite/YAMNet (events)
 *
 * Both models run in-process. No IPC, no serialization overhead. This is
 * possible because ALL dependencies are permissive (MIT/Apache 2.0).
 *
 * KEY DIFFERENCE from Kanaha Camera:
 * Camera uses Intent IPC (fork/exec "am broadcast") because OpenCamera is GPL Java.
 * Kanaha Audio calls both ML models directly — same process, no IPC latency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioService"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define LOGD(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)
#endif

#include "../whisper/whisper_android_bridge.h"
#include "../yamnet/yamnet_bridge.h"
#include "../recording/audio_recording.h"
#include "../recording/audio_tone.h"
#include "../recording/audio_sidecar.h"
#include "../recording/gps_reader.h"
#include "../sftp/audio_sftp.h"
#include "../ltc/ltc_decoder.h"

#include <dirent.h>

/* ========================================================================
 * JSON PARSING HELPERS
 * Minimal JSON helpers for action-based dispatch. These are standard
 * string-search patterns independently implemented for the audio service.
 * No external JSON library needed for the service layer.
 * ======================================================================== */

/**
 * Escape a string for safe embedding in a JSON value.
 * Caller must free the returned string.
 * Returns NULL on allocation failure.
 */
static char *json_escape_string(const char *input) {
    if (!input) return NULL;

    /* Worst case: every char expands to 2 chars (\n -> \\n) */
    size_t len = strlen(input);
    char *escaped = (char *)malloc(len * 2 + 1);
    if (!escaped) return NULL;

    char *dst = escaped;
    for (const char *src = input; *src; src++) {
        switch (*src) {
            case '"':  *dst++ = '\\'; *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
            case '/':  *dst++ = '\\'; *dst++ = '/';  break;
            case '\n': *dst++ = '\\'; *dst++ = 'n';  break;
            case '\r': *dst++ = '\\'; *dst++ = 'r';  break;
            case '\t': *dst++ = '\\'; *dst++ = 't';  break;
            case '\b': *dst++ = '\\'; *dst++ = 'b';  break;
            case '\f': *dst++ = '\\'; *dst++ = 'f';  break;
            default:   *dst++ = *src; break;
        }
    }
    *dst = '\0';
    return escaped;
}

/* Unescape JSON string in place */
static void json_unescape(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
                case '/': *dst++ = '/'; break;
                case 'n': *dst++ = '\n'; break;
                case 'r': *dst++ = '\r'; break;
                case 't': *dst++ = '\t'; break;
                case '"': *dst++ = '"'; break;
                case '\\': *dst++ = '\\'; break;
                default: *dst++ = *src; break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Simple JSON string extraction helper.
 * Handles both "key":"value" and "key": "value" (with whitespace). */
static const char *extract_json_string(const char *json, const char *key,
                                       char *buffer, size_t buffer_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start) return NULL;

    start += strlen(search_key);
    /* Skip whitespace between colon and opening quote */
    while (*start == ' ' || *start == '\t') start++;
    if (*start != '"') return NULL;
    start++;  /* skip opening quote */

    /* Find closing quote, skipping escaped quotes (\\") */
    const char *end = start;
    while (*end) {
        if (*end == '"') break;            /* Unescaped quote = end of value */
        if (*end == '\\' && *(end + 1)) {
            end += 2;                      /* Skip escaped character */
        } else {
            end++;
        }
    }
    if (*end != '"' || (size_t)(end - start) >= buffer_size) return NULL;

    strncpy(buffer, start, end - start);
    buffer[end - start] = '\0';

    json_unescape(buffer);
    return buffer;
}

/**
 * Extract a numeric value from a JSON object.
 * Handles: "key":0.5 and "key": 0.5 (with whitespace).
 * Does NOT handle: "key":"0.5" (string-wrapped numbers).
 * Returns default_val if key not found or value is not a number.
 */
static float extract_json_float(const char *json, const char *key, float default_val) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start) return default_val;

    start += strlen(search_key);
    /* Skip whitespace */
    while (*start == ' ' || *start == '\t') start++;

    /* Check for string value (quoted number) — skip */
    if (*start == '"') return default_val;

    char *endp = NULL;
    float val = strtof(start, &endp);
    if (endp == start) return default_val;
    return val;
}

/**
 * Extract a 64-bit integer from a JSON object.
 *
 * Unlike extract_json_float (which uses strtof with only ~7 digits of
 * precision), this uses strtoll for full int64 precision. Required for
 * Unix epoch millisecond timestamps (13 digits) used by start_at scheduling.
 *
 * Handles: "key":1772039427000 and "key": 1772039427000
 * Returns default_val if key not found.
 */
static int64_t extract_json_int64(const char *json, const char *key, int64_t default_val) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start) return default_val;

    start += strlen(search_key);
    while (*start == ' ' || *start == '\t') start++;

    if (*start == '"') return default_val;

    char *endp = NULL;
    long long val = strtoll(start, &endp, 10);
    if (endp == start) return default_val;
    return (int64_t)val;
}

/**
 * Extract a JSON array of strings into a flat buffer with parameterized element size.
 *
 * This is the generic version of extract_json_string_array(). The caller provides
 * a flat char buffer and the size of each element. Used for YAMNet event arrays
 * (YAMNET_MAX_EVENT_LEN) which differ from whisper keyword arrays (WHISPER_MAX_KEYWORD_LEN).
 *
 * @param json        JSON string to parse
 * @param key         JSON key name (e.g., "events")
 * @param values      Flat buffer: values[0..max_values-1], each value_size bytes
 * @param value_size  Size of each string slot in the buffer
 * @param max_values  Maximum number of strings to extract
 * @return Number of strings extracted
 */
static int extract_json_string_array_sized(const char *json, const char *key,
                                           char *values, int value_size,
                                           int max_values) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":[", key);

    const char *start = strstr(json, search_key);
    if (!start) return 0;

    start += strlen(search_key);
    int count = 0;

    while (*start && *start != ']' && count < max_values) {
        /* Skip whitespace and commas */
        while (*start == ' ' || *start == ',' || *start == '\n' || *start == '\r') start++;
        if (*start == ']') break;

        if (*start == '"') {
            start++;  /* skip opening quote */
            /* Find closing quote, skipping escaped quotes */
            const char *end = start;
            while (*end) {
                if (*end == '"') break;
                if (*end == '\\' && *(end + 1)) { end += 2; } else { end++; }
            }
            if (*end != '"') break;

            size_t len = end - start;
            if ((int)len >= value_size) len = value_size - 1;

            char *dest = values + count * value_size;
            strncpy(dest, start, len);
            dest[len] = '\0';
            json_unescape(dest);

            count++;
            start = end + 1;
        } else {
            break;  /* Unexpected character */
        }
    }

    return count;
}

/*
 * Extract a JSON array of strings.
 * Parses: "key":["val1","val2","val3"]
 * Returns number of strings extracted.
 */
static int extract_json_string_array(const char *json, const char *key,
                                     char values[][WHISPER_MAX_KEYWORD_LEN],
                                     int max_values) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":[", key);

    const char *start = strstr(json, search_key);
    if (!start) return 0;

    start += strlen(search_key);
    int count = 0;

    while (*start && *start != ']' && count < max_values) {
        /* Skip whitespace and commas */
        while (*start == ' ' || *start == ',' || *start == '\n' || *start == '\r') start++;
        if (*start == ']') break;

        if (*start == '"') {
            start++;  /* skip opening quote */
            /* Find closing quote, skipping escaped quotes */
            const char *end = start;
            while (*end) {
                if (*end == '"') break;
                if (*end == '\\' && *(end + 1)) { end += 2; } else { end++; }
            }
            if (*end != '"') break;

            size_t len = end - start;
            if (len >= WHISPER_MAX_KEYWORD_LEN) len = WHISPER_MAX_KEYWORD_LEN - 1;

            strncpy(values[count], start, len);
            values[count][len] = '\0';
            json_unescape(values[count]);

            count++;
            start = end + 1;
        } else {
            break;  /* Unexpected character */
        }
    }

    return count;
}

/**
 * Sanitize a string for safe logging.
 * Replaces control characters (newlines, tabs, etc.) with underscores to
 * prevent log injection attacks where user input could forge log entries.
 */
static void sanitize_for_logging(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    size_t i;
    for (i = 0; i < dst_size - 1 && src[i] != '\0'; i++) {
        dst[i] = (src[i] >= 32 && src[i] <= 126) ? src[i] : '_';
    }
    dst[i] = '\0';
}

/**
 * Safe snprintf into a buffer at an offset.
 *
 * Wraps snprintf to prevent offset from exceeding buffer size.
 * snprintf returns the number of characters that *would* have been written
 * (not the number actually written), so naively doing offset += snprintf(...)
 * can push offset past the buffer end. This helper clamps the offset.
 *
 * Returns the updated offset (always <= size - 1).
 */
__attribute__((format(printf, 4, 5)))
static size_t safe_snprintf(char *buf, size_t size, size_t offset, const char *fmt, ...) {
    if (offset >= size) return offset;  /* Already full */
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + offset, size - offset, fmt, ap);
    va_end(ap);
    if (written > 0) {
        offset += (size_t)written;
        if (offset >= size) offset = size - 1;  /* Clamp to buffer end */
    }
    return offset;
}

/* Create JSON error response helper */
static void create_error_response(char *buffer, size_t size, const char *error) {
    snprintf(buffer, size, "{\"success\":false,\"error\":\"%s\"}", error);
}

/* ========================================================================
 * SERVICE ENTRY POINT - Matches Axis2/C server-side pattern
 * ======================================================================== */

/**
 * JSON Service Entry Point - Processes audio search requests
 *
 * This function follows the same pattern as camera_control_service.c:
 * - Extracts "action" from JSON request
 * - Routes to appropriate audio operation
 * - Returns JSON response
 *
 * The key architectural difference: instead of Intent IPC (fork/exec),
 * operations call whisper_bridge_*() directly — no process boundary.
 *
 * @param json_request  JSON string containing the request
 * @param json_response Buffer for JSON response
 * @param response_size Size of response buffer
 * @return 0 on success, -1 on error
 */
int audio_search_service_invoke_json_impl(
    const char *json_request,
    char *json_response,
    size_t response_size
) {
    LOGI("audio_search_service_invoke_json_impl called");

    if (!json_request || !json_response || response_size == 0) {
        LOGE("Invalid parameters");
        return -1;
    }

    /* Extract the operation from the JSON request.
     *
     * Two callers, two spellings. The MCP server sends "action" because that
     * is what this service has always taken. The HTTP path sends nothing of
     * the sort: the engine puts the operation from the URL into the request as
     * "operation" and refuses a body that names a different one, so the URL is
     * what decides which operation runs. Accept either, "action" first, and do
     * not make one of them an error - dropping "operation" here breaks every
     * HTTPS call even though the engine routed it correctly. */
    char action[64];
    if (!extract_json_string(json_request, "action", action, sizeof(action))
        && !extract_json_string(json_request, "operation", action, sizeof(action))) {
        create_error_response(json_response, response_size, "Missing 'action' parameter");
        return -1;
    }

    /* Sanitize action for logging to prevent log injection via control characters */
    char safe_action[64];
    sanitize_for_logging(action, safe_action, sizeof(safe_action));
    LOGI("Processing audio action: %s", safe_action);

    /* ================================================================
     * searchKeywords - Primary use case
     * Find keyword timestamps in audio file
     * ================================================================ */
    if (strcmp(action, "searchKeywords") == 0) {
        char audio_file[512] = "";
        extract_json_string(json_request, "audio_file", audio_file, sizeof(audio_file));

        if (strlen(audio_file) == 0) {
            create_error_response(json_response, response_size,
                                  "Missing 'audio_file' parameter");
            return -1;
        }

        if (strstr(audio_file, "..")) {
            create_error_response(json_response, response_size,
                                  "Invalid audio_file (path traversal rejected)");
            return -1;
        }

        /* Extract keywords array */
        char keywords[WHISPER_MAX_KEYWORDS][WHISPER_MAX_KEYWORD_LEN];
        int num_keywords = extract_json_string_array(
            json_request, "keywords",
            keywords, WHISPER_MAX_KEYWORDS
        );

        if (num_keywords == 0) {
            create_error_response(json_response, response_size,
                                  "Missing or empty 'keywords' array");
            return -1;
        }

        /* Build keyword pointers array */
        const char *keyword_ptrs[WHISPER_MAX_KEYWORDS];
        for (int i = 0; i < num_keywords; i++) {
            keyword_ptrs[i] = keywords[i];
        }

        LOGI("Searching %d keywords in %s", num_keywords, audio_file);

        /* Optional decoder priming (R2): per call, and cleared afterwards, so
         * one request cannot change what the next one hears. */
        char initial_prompt[512] = "";
        extract_json_string(json_request, "initial_prompt", initial_prompt, sizeof(initial_prompt));
        whisper_bridge_set_initial_prompt(initial_prompt[0] ? initial_prompt : NULL);

        /* Direct function call — no IPC, no process boundary */
        whisper_search_result_t result;
        int rc = whisper_bridge_search_keywords(
            audio_file, keyword_ptrs, num_keywords, &result
        );
        whisper_bridge_set_initial_prompt(NULL);

        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Keyword search failed");
            return -1;
        }

        /* Build matches JSON array */
        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,\"matches\":[");

        for (int i = 0; i < result.num_matches && offset < response_size - 256; i++) {
            if (i > 0) {
                offset = safe_snprintf(json_response, response_size, offset, ",");
            }
            offset = safe_snprintf(json_response, response_size, offset,
                "{\"keyword\":\"%s\",\"start_ms\":%lld,\"end_ms\":%lld,\"confidence\":%.2f}",
                result.matches[i].keyword,
                (long long)result.matches[i].start_ms,
                (long long)result.matches[i].end_ms,
                result.matches[i].confidence);
        }

        safe_snprintf(json_response, response_size, offset,
            "],\"total_matches\":%d,\"audio_duration_ms\":%lld,\"processing_time_ms\":%lld}",
            result.num_matches,
            (long long)result.audio_duration_ms,
            (long long)result.processing_time_ms);
    }
    /* ================================================================
     * transcribe - Full transcription with timestamps
     * ================================================================ */
    else if (strcmp(action, "transcribe") == 0) {
        char audio_file[512] = "";
        extract_json_string(json_request, "audio_file", audio_file, sizeof(audio_file));

        if (strlen(audio_file) == 0) {
            create_error_response(json_response, response_size,
                                  "Missing 'audio_file' parameter");
            return -1;
        }

        if (strstr(audio_file, "..")) {
            create_error_response(json_response, response_size,
                                  "Invalid audio_file (path traversal rejected)");
            return -1;
        }

        /* Optional decoder priming (R2), same contract as searchKeywords:
         * in force for this call only. */
        char initial_prompt[512] = "";
        extract_json_string(json_request, "initial_prompt", initial_prompt, sizeof(initial_prompt));
        whisper_bridge_set_initial_prompt(initial_prompt[0] ? initial_prompt : NULL);

        whisper_transcribe_result_t result;
        memset(&result, 0, sizeof(result));
        int rc = whisper_bridge_transcribe(audio_file, &result);
        whisper_bridge_set_initial_prompt(NULL);

        if (rc != 0) {
            /* Callee (whisper_bridge_transcribe) is responsible for cleanup on failure.
             * memset zeroed result.text before the call, so nothing to free here. */
            create_error_response(json_response, response_size,
                                  "Transcription failed");
            return -1;
        }

        if (!result.text) {
            create_error_response(json_response, response_size,
                                  "Transcription returned NULL text");
            return -1;
        }

        /* Escape the transcription text for safe JSON embedding */
        char *escaped_text = json_escape_string(result.text);
        if (!escaped_text) {
            free(result.text);
            create_error_response(json_response, response_size,
                                  "Memory allocation failed for text escaping");
            return -1;
        }

        /* The escaped text plus the ~160-char JSON wrapper must fit the fixed
         * response buffer. snprintf would otherwise truncate mid-string and
         * emit syntactically invalid JSON; return an explicit error instead. */
        if (strlen(escaped_text) + 256 >= response_size) {
            free(escaped_text);
            free(result.text);
            create_error_response(json_response, response_size,
                "Transcript exceeds response buffer; use searchKeywords or split the audio");
            return -1;
        }
        snprintf(json_response, response_size,
            "{\"success\":true,\"text\":\"%s\",\"num_segments\":%d,"
            "\"audio_duration_ms\":%lld,\"processing_time_ms\":%lld}",
            escaped_text,
            result.num_segments,
            (long long)result.audio_duration_ms,
            (long long)result.processing_time_ms);

        free(escaped_text);
        free(result.text);
    }
    /* ================================================================
     * getStatus - Model loaded, device info, memory usage
     * ================================================================ */
    else if (strcmp(action, "getStatus") == 0 || strcmp(action, "get_status") == 0) {
        char whisper_status[1024];
        int rc = whisper_bridge_get_status(whisper_status, sizeof(whisper_status));

        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to get status");
            return -1;
        }

        /* Include YAMNet readiness, recording state, and GPS in status */
        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,\"whisper\":%s,\"yamnet_ready\":%s,"
            "\"recording_active\":%s",
            whisper_status,
            yamnet_bridge_is_ready() ? "true" : "false",
            audio_recording_is_active() ? "true" : "false");

        if (audio_recording_is_active()) {
            const char *clip = audio_recording_get_clip_name();
            if (clip) {
                offset = safe_snprintf(json_response, response_size, offset,
                    ",\"recording_clip\":\"%s\"", clip);
            }
        }

        /* Include GPS if available.
         * GPS JSON lives at <filesDir>/gps_location.json; audio_dir is
         * <filesDir>/audio — so ../gps_location.json reaches it.
         * Both paths derive from the same filesDir root passed by ApacheService.kt. */
        const char *audio_dir = audio_recording_get_audio_dir();
        if (audio_dir) {
            char gps_path[1024];
            snprintf(gps_path, sizeof(gps_path), "%s/../gps_location.json", audio_dir);
            gps_location_t gps;
            if (gps_reader_read(gps_path, &gps) == 0 && gps.valid) {
                offset = safe_snprintf(json_response, response_size, offset,
                    ",\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f,"
                    "\"accuracy\":%.1f,\"time\":%lld}",
                    gps.latitude, gps.longitude, gps.accuracy,
                    (long long)gps.time_ms);
            }
        }

        safe_snprintf(json_response, response_size, offset, "}");
    }
    /* ================================================================
     * listModels - Available whisper models on device
     * ================================================================ */
    else if (strcmp(action, "listModels") == 0 || strcmp(action, "list_models") == 0) {
        whisper_model_info_t models[16];
        int count = whisper_bridge_list_models(models, 16);

        if (count < 0) {
            create_error_response(json_response, response_size,
                                  "Failed to list models");
            return -1;
        }

        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,\"models\":[");

        for (int i = 0; i < count && offset < response_size - 256; i++) {
            if (i > 0) {
                offset = safe_snprintf(json_response, response_size, offset, ",");
            }
            char *esc_mname = json_escape_string(models[i].name);
            offset = safe_snprintf(json_response, response_size, offset,
                "{\"name\":\"%s\",\"size_bytes\":%lld,\"loaded\":%s}",
                esc_mname ? esc_mname : "",
                (long long)models[i].size_bytes,
                models[i].loaded ? "true" : "false");
            free(esc_mname);
        }

        safe_snprintf(json_response, response_size, offset,
            "],\"total\":%d}", count);
    }
    /* ================================================================
     * loadModel - Load/switch whisper model
     * ================================================================ */
    else if (strcmp(action, "loadModel") == 0 || strcmp(action, "load_model") == 0) {
        char model_name[128] = "";
        extract_json_string(json_request, "model", model_name, sizeof(model_name));

        if (strlen(model_name) == 0) {
            create_error_response(json_response, response_size,
                                  "Missing 'model' parameter");
            return -1;
        }

        int rc = whisper_bridge_load_model(model_name);

        if (rc == 0) {
            snprintf(json_response, response_size,
                "{\"success\":true,\"message\":\"Model loaded\",\"model\":\"%s\"}",
                model_name);
        } else {
            create_error_response(json_response, response_size,
                                  "Failed to load model");
        }
    }
    /* ================================================================
     * listAudioFiles - List processable audio files
     * ================================================================ */
    else if (strcmp(action, "listAudioFiles") == 0 || strcmp(action, "list_audio_files") == 0) {
        char dir[512] = "";
        extract_json_string(json_request, "directory", dir, sizeof(dir));

        /* Only the app's own audio directory may be listed. The previous
         * comment here claimed that already; the code only fell back to the
         * default on ".." and otherwise listed any absolute path, including
         * files/ssh/keys. The parameter is kept for schema compatibility but
         * anything other than the default is refused. */
        static const char *default_dir = "/data/data/org.kanaha.audio/files/audio";
        if (strlen(dir) != 0 && strcmp(dir, default_dir) != 0) {
            create_error_response(json_response, response_size,
                "listAudioFiles: only the app audio directory may be listed");
            return -1;
        }
        snprintf(dir, sizeof(dir), "%s", default_dir);

        int rc = whisper_bridge_list_audio_files(dir, json_response, response_size);

        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to list audio files");
        }
    }
    /* ================================================================
     * detectAudioEvents - Audio event detection via YAMNet
     * Find non-speech audio events: instruments, applause, silence, etc.
     * ================================================================ */
    else if (strcmp(action, "detectAudioEvents") == 0) {
        char audio_file[512] = "";
        extract_json_string(json_request, "audio_file", audio_file, sizeof(audio_file));

        if (strlen(audio_file) == 0) {
            create_error_response(json_response, response_size,
                                  "Missing 'audio_file' parameter");
            return -1;
        }

        if (strstr(audio_file, "..")) {
            create_error_response(json_response, response_size,
                                  "Invalid audio_file (path traversal rejected)");
            return -1;
        }

        /* Check that YAMNet is initialized */
        if (!yamnet_bridge_is_ready()) {
            create_error_response(json_response, response_size,
                                  "YAMNet model not loaded. Initialize with yamnet.tflite first.");
            return -1;
        }

        /* Extract events array */
        char events[YAMNET_MAX_EVENTS][YAMNET_MAX_EVENT_LEN];
        int num_events = extract_json_string_array_sized(
            json_request, "events",
            (char *)events, YAMNET_MAX_EVENT_LEN,
            YAMNET_MAX_EVENTS
        );

        if (num_events == 0) {
            create_error_response(json_response, response_size,
                                  "Missing or empty 'events' array");
            return -1;
        }

        /* Build event pointers array */
        const char *event_ptrs[YAMNET_MAX_EVENTS];
        for (int i = 0; i < num_events; i++) {
            event_ptrs[i] = events[i];
        }

        /* Extract optional parameters */
        float threshold = extract_json_float(json_request, "threshold", 0.5f);

        char mode[32] = "all";
        extract_json_string(json_request, "mode", mode, sizeof(mode));

        float time_range_start = extract_json_float(json_request, "time_range_start", -1.0f);
        float time_range_end = extract_json_float(json_request, "time_range_end", -1.0f);

        LOGI("Detecting %d events in %s (threshold=%.2f, mode=%s)",
             num_events, audio_file, threshold, mode);

        /* Direct function call — same process, no IPC */
        yamnet_detect_result_t result;
        int rc = yamnet_bridge_detect(
            audio_file, event_ptrs, num_events,
            threshold, mode,
            time_range_start, time_range_end,
            &result
        );

        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Audio event detection failed");
            return -1;
        }

        /* Build detections JSON array */
        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,\"detections\":[");

        for (int i = 0; i < result.num_detections && offset < response_size - 256; i++) {
            if (i > 0) {
                offset = safe_snprintf(json_response, response_size, offset, ",");
            }
            offset = safe_snprintf(json_response, response_size, offset,
                "{\"event\":\"%s\",\"start_ms\":%lld,\"end_ms\":%lld,\"confidence\":%.2f}",
                result.detections[i].event,
                (long long)result.detections[i].start_ms,
                (long long)result.detections[i].end_ms,
                result.detections[i].confidence);
        }

        safe_snprintf(json_response, response_size, offset,
            "],\"total_detections\":%d,\"frames_analyzed\":%d,\"processing_time_ms\":%lld}",
            result.num_detections,
            result.frames_analyzed,
            (long long)result.processing_time_ms);
    }
    /* ================================================================
     * startRecording - Record audio from device microphone
     *
     * Opens an AAudio input stream and records to a WAV file.
     * Only one recording can be active at a time. If start_at is
     * specified, a background thread sleeps until that time and then
     * starts recording — the HTTP response returns immediately with
     * "scheduled":true. Scheduling uses clock_nanosleep for
     * kernel-level precision (no message queue jitter).
     *
     * Side effect: writes kanaha_audio_recording_start.json sidecar
     * with recording_start_ms and GPS coordinates (if available).
     * ================================================================ */
    else if (strcmp(action, "startRecording") == 0) {
        char clip_name[AUDIO_RECORDING_MAX_CLIP_NAME] = "";
        extract_json_string(json_request, "clip_name", clip_name, sizeof(clip_name));

        if (strlen(clip_name) == 0) {
            create_error_response(json_response, response_size,
                                  "Missing 'clip_name' parameter");
            return -1;
        }

        int sample_rate = (int)extract_json_float(json_request, "sample_rate", 16000.0f);
        /* Use int64 for epoch timestamps — float only has ~7 digits of
         * precision, which corrupts 13-digit Unix epoch ms values */
        int64_t start_at = extract_json_int64(json_request, "start_at", 0);

        /* Determine if this will be a scheduled (future) or immediate start.
         * audio_recording_start returns immediately for both — scheduled
         * starts spawn a detached thread internally. */
        int scheduled = 0;
        if (start_at > 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            if (start_at > now_ms) scheduled = 1;
        }

        int rc = audio_recording_start(clip_name, sample_rate, start_at);
        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to start recording");
            return -1;
        }

        /* Write sidecar JSON alongside the WAV file (only for immediate starts).
         * For scheduled starts, the sidecar should be written when recording
         * actually begins — but the scheduling thread doesn't have access to
         * the sidecar writer. For now, write it on immediate starts only;
         * scheduled start sidecar can be added when the demo needs it. */
        if (!scheduled) {
            const char *audio_dir = audio_recording_get_audio_dir();
            if (audio_dir) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                audio_sidecar_write(audio_dir, clip_name, now_ms,
                                    sample_rate == 44100 ? 44100 : 16000, 1);
            }
        }

        /* Escape clip_name for safe JSON embedding (may contain \ or ") */
        char *escaped_clip = json_escape_string(clip_name);
        snprintf(json_response, response_size,
            "{\"success\":true,\"clip_name\":\"%s\","
            "\"recording_file\":\"%s.wav\",\"sample_rate\":%d,\"scheduled\":%s}",
            escaped_clip ? escaped_clip : clip_name,
            escaped_clip ? escaped_clip : clip_name,
            sample_rate == 44100 ? 44100 : 16000,
            scheduled ? "true" : "false");
        free(escaped_clip);
    }
    /* ================================================================
     * stopRecording - Stop active recording
     *
     * Stops the AAudio stream, drains the ring buffer to disk,
     * finalizes the WAV header, and returns the recording metadata.
     * The resulting WAV file is immediately available for
     * searchKeywords or detectAudioEvents analysis.
     * ================================================================ */
    else if (strcmp(action, "stopRecording") == 0) {
        const char *clip_name = audio_recording_get_clip_name();
        if (!clip_name) {
            create_error_response(json_response, response_size,
                                  "No active recording");
            return -1;
        }

        /* Copy clip name before stop clears it (stop resets state to IDLE) */
        char saved_clip[AUDIO_RECORDING_MAX_CLIP_NAME + 1];
        strncpy(saved_clip, clip_name, AUDIO_RECORDING_MAX_CLIP_NAME);
        saved_clip[AUDIO_RECORDING_MAX_CLIP_NAME] = '\0';

        int64_t duration_ms = 0;
        int64_t file_size = 0;
        int rc = audio_recording_stop(&duration_ms, &file_size);

        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to stop recording");
            return -1;
        }

        char *escaped_clip = json_escape_string(saved_clip);
        snprintf(json_response, response_size,
            "{\"success\":true,\"clip_name\":\"%s\","
            "\"duration_ms\":%lld,\"file_size_bytes\":%lld}",
            escaped_clip ? escaped_clip : saved_clip,
            (long long)duration_ms, (long long)file_size);
        free(escaped_clip);
    }
    /* ================================================================
     * playTone - Play a sine wave tone through the speaker
     *
     * Launches a detached thread for tone generation, so the HTTP
     * response returns immediately (the tone plays asynchronously).
     * If start_at is specified, the tone thread sleeps until that
     * time before opening the AAudio output stream.
     *
     * Use case: send the same start_at to multiple devices for
     * synchronized audio slate across phone + laptop.
     * ================================================================ */
    else if (strcmp(action, "playTone") == 0) {
        int frequency = (int)extract_json_float(json_request, "frequency", 0.0f);
        int duration_ms = (int)extract_json_float(json_request, "duration_ms", 0.0f);
        int64_t start_at = extract_json_int64(json_request, "start_at", 0);

        if (frequency <= 0) {
            create_error_response(json_response, response_size,
                                  "Missing or invalid 'frequency' parameter");
            return -1;
        }
        if (duration_ms <= 0) {
            create_error_response(json_response, response_size,
                                  "Missing or invalid 'duration_ms' parameter");
            return -1;
        }

        int rc = audio_tone_play(frequency, duration_ms, start_at);
        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to play tone");
            return -1;
        }

        /* Calculate delay for response */
        int64_t delay_ms = 0;
        if (start_at > 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            delay_ms = start_at - now_ms;
            if (delay_ms < 0) delay_ms = 0;
        }

        snprintf(json_response, response_size,
            "{\"success\":true,\"scheduled\":%s,\"fire_at\":%lld,\"delay_ms\":%lld}",
            start_at > 0 ? "true" : "false",
            (long long)start_at, (long long)delay_ms);
    }
    /* ================================================================
     * playAudio - Play a WAV file through the device speaker
     * ================================================================ */
    else if (strcmp(action, "playAudio") == 0) {
        char audio_file[512] = "";
        extract_json_string(json_request, "audio_file", audio_file, sizeof(audio_file));

        if (audio_file[0] == '\0') {
            create_error_response(json_response, response_size,
                                  "Missing required field: audio_file");
            return -1;
        }

        if (strstr(audio_file, "..")) {
            create_error_response(json_response, response_size,
                                  "Invalid audio_file: path traversal not allowed");
            return -1;
        }

        int rc = audio_tone_play_file(audio_file);
        if (rc != 0) {
            create_error_response(json_response, response_size,
                                  "Failed to play audio file");
            return -1;
        }

        {
            char *escaped_file = json_escape_string(audio_file);
            snprintf(json_response, response_size,
                "{\"success\":true,\"audio_file\":\"%s\"}",
                escaped_file ? escaped_file : "");
            free(escaped_file);
        }
    }
    /* ================================================================
     * listRecordings - List WAV files in the audio directory
     *
     * Scans the audio directory for .wav files and returns name,
     * size, and estimated duration. Duration is estimated from file
     * size assuming 16kHz mono 16-bit PCM (the default recording
     * format). Files recorded at 44100 Hz will show shorter
     * estimated durations — this is a known limitation.
     * ================================================================ */
    else if (strcmp(action, "listRecordings") == 0) {
        const char *audio_dir = audio_recording_get_audio_dir();
        if (!audio_dir) {
            create_error_response(json_response, response_size,
                                  "Recording subsystem not initialized");
            return -1;
        }

        DIR *dir = opendir(audio_dir);
        if (!dir) {
            create_error_response(json_response, response_size,
                                  "Cannot open audio directory");
            return -1;
        }

        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,\"recordings\":[");

        int count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && offset < response_size - 512) {
            /* Only list .wav files */
            size_t namelen = strlen(entry->d_name);
            if (namelen < 4 ||
                strcmp(entry->d_name + namelen - 4, ".wav") != 0) {
                continue;
            }

            /* Get file size and read actual sample rate from WAV header */
            char filepath[1024];
            int path_len = snprintf(filepath, sizeof(filepath), "%s/%s", audio_dir, entry->d_name);
            if (path_len < 0 || (size_t)path_len >= sizeof(filepath)) continue; /* path too long */
            struct stat st;
            if (stat(filepath, &st) != 0) continue;

            /* Read sample rate from WAV header (offset 24, 4 bytes LE).
             * Fall back to 16000 if the file is too small or unreadable. */
            int32_t wav_sample_rate = 16000;
            FILE *wav_f = fopen(filepath, "rb");
            if (wav_f && st.st_size >= 44) {
                fseek(wav_f, 24, SEEK_SET);
                if (fread(&wav_sample_rate, 4, 1, wav_f) != 1) {
                    wav_sample_rate = 16000;
                }
                fclose(wav_f);
            } else if (wav_f) {
                fclose(wav_f);
            }
            if (wav_sample_rate <= 0) wav_sample_rate = 16000;

            /* Calculate duration from data bytes and actual sample rate */
            int64_t data_bytes = st.st_size - 44;
            if (data_bytes < 0) data_bytes = 0;
            int64_t est_duration_ms = (data_bytes / 2) * 1000 / wav_sample_rate;

            if (count > 0) {
                offset = safe_snprintf(json_response, response_size, offset, ",");
            }
            char *escaped_name = json_escape_string(entry->d_name);
            offset = safe_snprintf(json_response, response_size, offset,
                "{\"name\":\"%s\",\"size_bytes\":%lld,\"duration_ms\":%lld}",
                escaped_name ? escaped_name : "", (long long)st.st_size,
                (long long)est_duration_ms);
            free(escaped_name);
            count++;
        }
        closedir(dir);

        safe_snprintf(json_response, response_size, offset,
            "],\"total\":%d}", count);
    }
    /* ================================================================
     * LTC (SMPTE timecode) decoding
     * ================================================================ */
    else if (strcmp(action, "decodeLTC") == 0) {
        char audio_file[512] = "";
        extract_json_string(json_request, "audio_file", audio_file, sizeof(audio_file));

        if (audio_file[0] == '\0') {
            create_error_response(json_response, response_size,
                "Missing required field: audio_file");
            return -1;
        }

        int channel = (int)extract_json_float(json_request, "channel", 1.0f);

        ltc_decode_result_t ltc_result;
        int rc = ltc_decode_wav(audio_file, channel, &ltc_result);

        if (rc != 0) {
            char *escaped = json_escape_string(ltc_result.error);
            create_error_response(json_response, response_size,
                escaped ? escaped : ltc_result.error);
            if (escaped) free(escaped);
            ltc_decode_result_free(&ltc_result);
            return -1;
        }

        /* Build JSON response with frames array */
        size_t offset = 0;
        offset = safe_snprintf(json_response, response_size, offset,
            "{\"success\":true,"
            "\"total_frames\":%d,"
            "\"fps\":%.1f,"
            "\"sample_rate\":%d,"
            "\"first_tc\":\"%s\","
            "\"last_tc\":\"%s\","
            "\"frames\":[",
            ltc_result.total_frames,
            ltc_result.fps,
            ltc_result.sample_rate,
            ltc_result.first_tc,
            ltc_result.last_tc);

        for (int i = 0; i < ltc_result.total_frames && offset < response_size - 256; i++) {
            ltc_frame_info_t *fr = &ltc_result.frames[i];
            if (i > 0) {
                offset = safe_snprintf(json_response, response_size, offset, ",");
            }
            offset = safe_snprintf(json_response, response_size, offset,
                "{\"tc\":\"%02d:%02d:%02d:%02d\","
                "\"sample_start\":%lld,"
                "\"sample_end\":%lld"
                "%s}",
                fr->hours, fr->minutes, fr->seconds, fr->frames,
                (long long)fr->sample_start,
                (long long)fr->sample_end,
                fr->discontinuity ? ",\"discontinuity\":true" : "");
        }

        safe_snprintf(json_response, response_size, offset, "]}");

        ltc_decode_result_free(&ltc_result);
    }
    /* ================================================================
     * SFTP file transfer
     * ================================================================ */
    else if (strcmp(action, "sftpTransfer") == 0) {
        char server_id[128] = "";
        char audio_filename[512] = "";
        char destination_folder[512] = "";

        extract_json_string(json_request, "storage_server_id", server_id, sizeof(server_id));
        extract_json_string(json_request, "audio_filename", audio_filename, sizeof(audio_filename));
        extract_json_string(json_request, "destination_folder", destination_folder, sizeof(destination_folder));

        if (server_id[0] == '\0' || audio_filename[0] == '\0' ||
            destination_folder[0] == '\0') {
            create_error_response(json_response, response_size,
                "Missing required field: storage_server_id, audio_filename, or destination_folder");
            return -1;
        }

        /* Reject path traversal in filenames */
        if (strstr(audio_filename, "..") || strchr(audio_filename, '/')) {
            create_error_response(json_response, response_size,
                "Invalid audio_filename: path traversal not allowed");
            return -1;
        }

        const char *audio_dir = audio_recording_get_audio_dir();
        if (!audio_dir) {
            create_error_response(json_response, response_size,
                "Recording subsystem not initialized");
            return -1;
        }

        audio_sftp_result_t sftp_result;
        int rc = audio_sftp_transfer(server_id, audio_filename,
                                     destination_folder, audio_dir, &sftp_result);

        if (rc == 0) {
            snprintf(json_response, response_size,
                "{\"success\":true,"
                "\"operation_id\":\"%s\","
                "\"files_transferred\":%d,"
                "\"bytes_transferred\":%lld,"
                "\"timestamp\":%lld,"
                "\"message\":\"%s\"}",
                sftp_result.operation_id,
                sftp_result.files_transferred,
                (long long)sftp_result.bytes_transferred,
                (long long)sftp_result.timestamp,
                sftp_result.message);
        } else {
            char *escaped_err = json_escape_string(sftp_result.error);
            snprintf(json_response, response_size,
                "{\"success\":false,"
                "\"operation_id\":\"%s\","
                "\"error\":\"%s\","
                "\"timestamp\":%lld}",
                sftp_result.operation_id,
                escaped_err ? escaped_err : sftp_result.error,
                (long long)sftp_result.timestamp);
            if (escaped_err) free(escaped_err);
        }
    }
    /* ================================================================
     * Unknown action
     * ================================================================ */
    else {
        /* The action name is echoed back; keep it to a safe alphabet so a
         * quote or backslash in it cannot break the JSON it lands in. */
        char safe_action[64];
        size_t n = 0;
        for (const char *p = action; *p && n < sizeof(safe_action) - 1; p++) {
            char c = *p;
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-';
            safe_action[n++] = ok ? c : '?';
        }
        safe_action[n] = '\0';
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Unknown action: %s", safe_action);
        create_error_response(json_response, response_size, error_msg);
        return -1;
    }

    LOGD("Response: %s", json_response);
    return 0;
}

/* ========================================================================
 * SERVICE LIFECYCLE FUNCTIONS
 * ======================================================================== */

/**
 * Initialize audio search service.
 * Sets up the whisper bridge for speech operations.
 * YAMNet is initialized separately via yamnet_bridge_init() because
 * its model path differs (yamnet.tflite vs ggml-*.bin directory).
 */
int audio_search_service_init(const char *models_dir) {
    LOGI("audio_search_service_init called");
    int rc = whisper_bridge_init(models_dir);

    /* Initialize recording subsystem.
     * Audio files go to a sibling "audio" directory next to "models". */
    char audio_dir[1024];
    snprintf(audio_dir, sizeof(audio_dir), "%s/../audio", models_dir);
    audio_recording_init(audio_dir);

    /* Initialize SFTP subsystem.
     * SSH config goes to a sibling "ssh" directory next to "models". */
    char ssh_dir[1024];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s/../ssh", models_dir);
    audio_sftp_init(ssh_dir);

    /* Try to initialize YAMNet if the model exists in the same directory.
     * YAMNet init is best-effort — the service works without it (whisper-only). */
    char yamnet_path[1024];
    snprintf(yamnet_path, sizeof(yamnet_path), "%s/yamnet.tflite", models_dir);

    struct stat st;
    if (stat(yamnet_path, &st) == 0) {
        int yrc = yamnet_bridge_init(yamnet_path);
        if (yrc == 0) {
            LOGI("YAMNet bridge initialized from %s", yamnet_path);
        } else {
            LOGI("YAMNet bridge init failed (non-fatal, whisper-only mode)");
        }
    } else {
        LOGI("YAMNet model not found at %s (whisper-only mode)", yamnet_path);
    }

    return rc;
}

/**
 * Cleanup audio search service
 */
void audio_search_service_cleanup(void) {
    LOGI("audio_search_service_cleanup called");
    audio_sftp_cleanup();
    audio_recording_cleanup();
    whisper_bridge_cleanup();
    yamnet_bridge_cleanup();
}

/*
 * ARCHITECTURE: Dual-Model Axis2/C Service
 *
 * This implementation follows the same architecture as the Axis2/C userguide
 * sample services and Kanaha Camera's camera_control_service.c:
 *
 * Server-side:
 *   mod_axis2 -> axis2_json_rpc_msg_recv -> dlsym("*_invoke_json") -> service
 *
 * Android (this file):
 *   HTTP client -> audio_search_service_invoke_json_impl()
 *     -> "searchKeywords"    -> whisper_bridge -> whisper.cpp  (speech-to-text)
 *     -> "detectAudioEvents" -> yamnet_bridge  -> TFLite/YAMNet (audio events)
 *
 * Two ML models, one service, dispatched via the same Axis2/C weak symbol
 * registry. Both models run in-process on the phone.
 *
 * Unlike Kanaha Camera, there is no Intent IPC boundary because all
 * dependencies (whisper.cpp, ggml, TFLite, YAMNet) are permissive
 * C/C++ libraries that link directly into the same process. This eliminates:
 * - ~300 lines of IPC infrastructure (fork/exec, response files, polling)
 * - ~100ms IPC latency per call
 * - TOCTOU race conditions on response files
 * - Shell injection attack surface from am broadcast
 */
