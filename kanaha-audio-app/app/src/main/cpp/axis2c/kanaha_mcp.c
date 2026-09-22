/*
 * Kanaha Audio
 * MCP stdio transport — JSON-RPC 2.0 loop for audio search operations
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Follows the same MCP protocol design pattern established in Kanaha Camera
 * (GPL v3+), but this is an independent implementation under Apache 2.0.
 * No code was copied from the GPL-licensed project.
 *
 * Three required MCP methods:
 *   initialize  — protocol version + server info + capabilities
 *   tools/list  — audio tool catalog with full inputSchema
 *   tools/call  — dispatches to audio_search_service_invoke_json_impl()
 */

#include "kanaha_mcp.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioMCP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, __VA_ARGS__)
#define LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

/* ============================================================================
 * Protocol constants
 * ============================================================================
 */

#define KANAHA_MCP_PROTOCOL_VERSION   "2024-11-05"
#define KANAHA_MCP_SERVER_NAME        "kanaha-audio"
#define KANAHA_MCP_SERVER_VERSION     "1.0.0"

/* JSON-RPC 2.0 error codes */
#define MCP_ERR_PARSE_ERROR       -32700
#define MCP_ERR_INVALID_REQUEST   -32600
#define MCP_ERR_METHOD_NOT_FOUND  -32601
#define MCP_ERR_INVALID_PARAMS    -32602
#define MCP_ERR_INTERNAL_ERROR    -32603

/* Maximum request size */
#define MAX_MCP_REQUEST_BYTES   (1 * 1024 * 1024)  /* 1 MB */
#define MCP_LINE_INITIAL_CAP    4096

/* Response buffer for audio service */
#define AUDIO_RESPONSE_SIZE    65536

/* ============================================================================
 * External: audio service entry point (from audio_search_service.c)
 * ============================================================================
 */

extern int audio_search_service_invoke_json_impl(
    const char *json_request,
    char *json_response,
    size_t response_size);

/* ============================================================================
 * Tool catalog — input schemas for all 14 audio operations
 * ============================================================================
 */

static const char SCHEMA_SEARCH_KEYWORDS[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"audio_file\":{\"type\":\"string\","
            "\"description\":\"Path to audio file (WAV format)\"},"
        "\"keywords\":{\"type\":\"array\","
            "\"items\":{\"type\":\"string\"},"
            "\"description\":\"Array of keyword phrases to search for\"},"
        "\"initial_prompt\":{\"type\":\"string\","
            "\"description\":\"Optional decoder priming: a short line the decoder is conditioned on, "
                "e.g. the tickers you expect. Biases recognition, does not constrain it, and costs "
                "decode budget. Applies to this call only\"}"
    "},"
    "\"required\":[\"audio_file\",\"keywords\"]"
    "}";

static const char SCHEMA_TRANSCRIBE[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"audio_file\":{\"type\":\"string\","
            "\"description\":\"Path to audio file (WAV format)\"},"
        "\"initial_prompt\":{\"type\":\"string\","
            "\"description\":\"Optional decoder priming: a short line the decoder is conditioned on, "
                "e.g. the tickers you expect plus a domain sentence. Biases recognition, does not "
                "constrain it, and costs decode budget. Applies to this call only\"}"
    "},"
    "\"required\":[\"audio_file\"]"
    "}";

static const char SCHEMA_GET_STATUS[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{},"
    "\"required\":[],"
    "\"description\":\"No parameters required. Returns whisper model status and device info.\""
    "}";

static const char SCHEMA_LIST_MODELS[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{},"
    "\"required\":[],"
    "\"description\":\"No parameters required. Lists available whisper ggml models.\""
    "}";

static const char SCHEMA_LOAD_MODEL[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"model\":{\"type\":\"string\","
            "\"description\":\"Model name: tiny, base, small, medium, large\"}"
    "},"
    "\"required\":[\"model\"]"
    "}";

static const char SCHEMA_DETECT_AUDIO_EVENTS[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"audio_file\":{\"type\":\"string\","
            "\"description\":\"Path to audio file (WAV format, 16kHz preferred)\"},"
        "\"events\":{\"type\":\"array\","
            "\"items\":{\"type\":\"string\"},"
            "\"description\":\"AudioSet class names to detect: Saxophone, Piano, Guitar, "
            "Applause, Silence, Drums, Trumpet, etc. (521 classes available)\"},"
        "\"threshold\":{\"type\":\"number\","
            "\"description\":\"Confidence threshold 0.0-1.0 (default 0.5)\"},"
        "\"mode\":{\"type\":\"string\","
            "\"enum\":[\"all\",\"first_onset\",\"last_offset\"],"
            "\"description\":\"Detection mode: all (every match), first_onset (first only), "
            "last_offset (last only). Default: all\"},"
        "\"time_range_start\":{\"type\":\"number\","
            "\"description\":\"Start of search window in seconds (optional)\"},"
        "\"time_range_end\":{\"type\":\"number\","
            "\"description\":\"End of search window in seconds (optional)\"}"
    "},"
    "\"required\":[\"audio_file\",\"events\"]"
    "}";

static const char SCHEMA_LIST_AUDIO_FILES[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"directory\":{\"type\":\"string\","
            "\"description\":\"Directory to scan for audio files. Default: app audio directory\"}"
    "},"
    "\"required\":[]"
    "}";

static const char SCHEMA_START_RECORDING[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"clip_name\":{\"type\":\"string\","
            "\"description\":\"Name for the recording (used as WAV filename prefix)\"},"
        "\"sample_rate\":{\"type\":\"integer\","
            "\"description\":\"Sample rate in Hz: 16000 (default, whisper-compatible) or 44100 (high quality)\"},"
        "\"start_at\":{\"type\":\"integer\","
            "\"description\":\"Unix epoch milliseconds to start recording. 0 or omit = start immediately\"}"
    "},"
    "\"required\":[\"clip_name\"]"
    "}";

static const char SCHEMA_STOP_RECORDING[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{},"
    "\"required\":[],"
    "\"description\":\"No parameters required. Stops the active recording and finalizes the WAV file.\""
    "}";

static const char SCHEMA_SPEAK[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"text\":{\"type\":\"string\","
            "\"description\":\"What to say on this device's speaker, at most 500 characters. "
                "Synthesised in-process; nothing is uploaded and no network is used\"}"
    "},"
    "\"required\":[\"text\"]"
    "}";

static const char SCHEMA_PLAY_TONE[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"frequency\":{\"type\":\"integer\","
            "\"description\":\"Tone frequency in Hz (e.g., 1000)\"},"
        "\"duration_ms\":{\"type\":\"integer\","
            "\"description\":\"Tone duration in milliseconds\"},"
        "\"start_at\":{\"type\":\"integer\","
            "\"description\":\"Unix epoch milliseconds to start tone. 0 or omit = start immediately\"}"
    "},"
    "\"required\":[\"frequency\",\"duration_ms\"]"
    "}";

static const char SCHEMA_PLAY_AUDIO[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"audio_file\":{\"type\":\"string\","
            "\"description\":\"Path to WAV file to play through the device speaker\"}"
    "},"
    "\"required\":[\"audio_file\"]"
    "}";

static const char SCHEMA_DECODE_LTC[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"audio_file\":{\"type\":\"string\","
            "\"description\":\"Path to WAV file containing LTC audio (44.1kHz or 48kHz)\"},"
        "\"channel\":{\"type\":\"integer\","
            "\"description\":\"Audio channel to decode (1-based, default 1)\"}"
    "},"
    "\"required\":[\"audio_file\"]"
    "}";

static const char SCHEMA_SFTP_TRANSFER[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
        "\"storage_server_id\":{\"type\":\"string\","
            "\"description\":\"Server ID from servers.json (e.g. control1)\"},"
        "\"audio_filename\":{\"type\":\"string\","
            "\"description\":\"Filename to transfer, or * for all WAV files\"},"
        "\"destination_folder\":{\"type\":\"string\","
            "\"description\":\"Remote directory path for uploaded files\"}"
    "},"
    "\"required\":[\"storage_server_id\",\"audio_filename\",\"destination_folder\"]"
    "}";

static const char SCHEMA_LIST_RECORDINGS[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{},"
    "\"required\":[],"
    "\"description\":\"No parameters required. Lists WAV recordings in the audio directory.\""
    "}";

/* ============================================================================
 * Tool catalog struct
 * ============================================================================
 */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
} kanaha_mcp_tool_t;

static const kanaha_mcp_tool_t kanaha_mcp_tools[] = {
    {
        "searchKeywords",
        "Search for keyword phrases in an audio file and return timestamps. "
        "Primary use case: finding 'next slide please' in presentation recordings "
        "to automate third-camera priority cuts in video editing. Returns an array "
        "of matches with start_ms, end_ms, and confidence scores.",
        SCHEMA_SEARCH_KEYWORDS
    },
    {
        "transcribe",
        "Transcribe an audio file with word-level timestamps using whisper.cpp. "
        "Returns full text and segment information.",
        SCHEMA_TRANSCRIBE
    },
    {
        "getStatus",
        "Get audio service status: whether a whisper model is loaded, which model, "
        "models directory, and device info.",
        SCHEMA_GET_STATUS
    },
    {
        "listModels",
        "List available whisper ggml models on the device with sizes and "
        "loaded state.",
        SCHEMA_LIST_MODELS
    },
    {
        "loadModel",
        "Load or switch the active whisper model. Only one model can be loaded "
        "at a time. Smaller models (tiny, base) are faster; larger models "
        "(small, medium) are more accurate.",
        SCHEMA_LOAD_MODEL
    },
    {
        "detectAudioEvents",
        "Detect non-speech audio events using YAMNet (Google AudioSet classifier). "
        "Identifies instruments (Saxophone, Piano, Guitar, Drums), events (Applause, "
        "Silence, Cheering), and 521 other AudioSet classes with timestamps. "
        "Use case: detect saxophone intro of A Love Supreme to mark video start time, "
        "detect applause to mark video end time.",
        SCHEMA_DETECT_AUDIO_EVENTS
    },
    {
        "listAudioFiles",
        "List audio files (WAV, MP3, FLAC, OGG, M4A) available for processing.",
        SCHEMA_LIST_AUDIO_FILES
    },
    {
        "startRecording",
        "Start recording audio from the device microphone. Records to WAV format "
        "(16kHz mono by default, matching whisper.cpp input). Supports start_at "
        "scheduling for multi-device sync via kernel-level clock_nanosleep.",
        SCHEMA_START_RECORDING
    },
    {
        "stopRecording",
        "Stop the active recording. Finalizes the WAV header and returns "
        "clip name, duration, and file size.",
        SCHEMA_STOP_RECORDING
    },
    {
        "playTone",
        "Play a sine wave tone through the device speaker. Used for multi-device "
        "synchronization (audio slate). Supports start_at scheduling for "
        "kernel-level precision timing across devices.",
        SCHEMA_PLAY_TONE
    },
    {
        "speak",
        "Say a line of text on this device's speaker. Synthesis runs in-process (flite, "
        "BSD) so nothing is uploaded and no network is touched. Meant for reading a "
        "resolved request back to the person standing next to the phone before anything "
        "runs, which is the only way a spoken confirmation means they agreed with the "
        "specification rather than merely heard a beep. The voice is a 16 kHz diphone "
        "voice: intelligible, not pretty.",
        SCHEMA_SPEAK
    },
    {
        "listRecordings",
        "List WAV recordings in the audio directory with file sizes and "
        "estimated durations.",
        SCHEMA_LIST_RECORDINGS
    },
    {
        "playAudio",
        "Play a WAV audio file through the device speaker. "
        "Use to play back recordings, preview audio, or as a lost phone finder.",
        SCHEMA_PLAY_AUDIO
    },
    {
        "decodeLTC",
        "Decode SMPTE/LTC timecode from a WAV recording. Returns timecode "
        "frames with sample-accurate positions. On-device equivalent of "
        "ltcdump. Use with Tentacle Sync recordings via iRig.",
        SCHEMA_DECODE_LTC
    },
    {
        "sftpTransfer",
        "Transfer audio file(s) to a storage server via SFTP. Uses ed25519 "
        "SSH key authentication. Server configuration from servers.json.",
        SCHEMA_SFTP_TRANSFER
    },
    { NULL, NULL, NULL }  /* sentinel */
};

/* ============================================================================
 * JSON-RPC 2.0 output helpers
 * ============================================================================
 */

static void mcp_write_result(json_object *id_obj, json_object *result_obj)
{
    json_object *response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));

    json_object_get(id_obj);
    json_object_object_add(response, "id", id_obj);
    json_object_object_add(response, "result", result_obj);

    printf("%s\n",
        json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN));
    fflush(stdout);

    json_object_put(response);
}

static void mcp_write_error(json_object *id_obj, int code, const char *message)
{
    json_object *error_obj = json_object_new_object();
    json_object_object_add(error_obj, "code",    json_object_new_int(code));
    json_object_object_add(error_obj, "message", json_object_new_string(message));

    json_object *response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));

    if (id_obj) {
        json_object_get(id_obj);
        json_object_object_add(response, "id", id_obj);
    } else {
        json_object_object_add(response, "id", json_object_new_null());
    }

    json_object_object_add(response, "error", error_obj);

    printf("%s\n",
        json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN));
    fflush(stdout);

    json_object_put(response);
}

/* ============================================================================
 * MCP method handlers
 * ============================================================================
 */

static json_object *mcp_handle_initialize(void)
{
    json_object *result = json_object_new_object();
    json_object_object_add(result, "protocolVersion",
        json_object_new_string(KANAHA_MCP_PROTOCOL_VERSION));

    json_object *capabilities = json_object_new_object();
    json_object_object_add(capabilities, "tools", json_object_new_object());
    json_object_object_add(result, "capabilities", capabilities);

    json_object *server_info = json_object_new_object();
    json_object_object_add(server_info, "name",
        json_object_new_string(KANAHA_MCP_SERVER_NAME));
    json_object_object_add(server_info, "version",
        json_object_new_string(KANAHA_MCP_SERVER_VERSION));
    json_object_object_add(result, "serverInfo", server_info);

    return result;
}

static json_object *mcp_handle_tools_list(void)
{
    json_object *tools_array = json_object_new_array();

    for (const kanaha_mcp_tool_t *tool = kanaha_mcp_tools;
         tool->name != NULL; tool++) {
        json_object *tool_obj = json_object_new_object();

        json_object_object_add(tool_obj, "name",
            json_object_new_string(tool->name));
        json_object_object_add(tool_obj, "description",
            json_object_new_string(tool->description));

        json_object *schema = json_tokener_parse(tool->input_schema_json);
        if (!schema) {
            schema = json_object_new_object();
            json_object_object_add(schema, "type",
                json_object_new_string("object"));
        }
        json_object_object_add(tool_obj, "inputSchema", schema);

        json_object_array_add(tools_array, tool_obj);
    }

    json_object *result = json_object_new_object();
    json_object_object_add(result, "tools", tools_array);
    return result;
}

/**
 * Dispatch tools/call to the audio service.
 *
 * The audio service expects a JSON string with "action" and parameters.
 * MCP sends params.name (tool name) and params.arguments (parameters).
 * We merge them: {"action":"<name>", ...arguments}.
 */
static json_object *mcp_handle_tools_call(
    json_object  *params,
    int          *out_code,
    const char  **out_msg)
{
    *out_code = 0;
    *out_msg  = NULL;

    if (!params || !json_object_is_type(params, json_type_object)) {
        *out_code = MCP_ERR_INVALID_PARAMS;
        *out_msg  = "params must be an object with name and arguments fields";
        return NULL;
    }

    /* Extract tool name */
    json_object *name_obj = NULL;
    if (!json_object_object_get_ex(params, "name", &name_obj)
            || !json_object_is_type(name_obj, json_type_string)) {
        *out_code = MCP_ERR_INVALID_PARAMS;
        *out_msg  = "params.name is required and must be a string";
        return NULL;
    }
    const char *tool_name = json_object_get_string(name_obj);

    /* Verify tool exists in catalog */
    int found = 0;
    for (const kanaha_mcp_tool_t *t = kanaha_mcp_tools; t->name; t++) {
        if (strcmp(t->name, tool_name) == 0) { found = 1; break; }
    }
    if (!found) {
        *out_code = MCP_ERR_METHOD_NOT_FOUND;
        *out_msg  = "Unknown tool. Available: searchKeywords, transcribe, "
                    "getStatus, listModels, loadModel, detectAudioEvents, "
                    "listAudioFiles, startRecording, stopRecording, "
                    "playTone, listRecordings";
        return NULL;
    }

    /* Build the JSON request for audio_search_service_invoke_json_impl.
     * The service expects: {"action":"<name>", "param1":"val1", ...}
     * MCP gives us: params.arguments = {"param1":"val1", ...}
     * We create a new object merging action + arguments. */
    json_object *request = json_object_new_object();
    json_object_object_add(request, "action",
        json_object_new_string(tool_name));

    /* Merge arguments into request */
    json_object *args_obj = NULL;
    if (json_object_object_get_ex(params, "arguments", &args_obj)
            && json_object_is_type(args_obj, json_type_object)) {
        json_object_object_foreach(args_obj, key, val) {
            json_object_get(val);
            json_object_object_add(request, key, val);
        }
    }

    const char *request_str = json_object_to_json_string_ext(
        request, JSON_C_TO_STRING_PLAIN);

    /* Log tool name only — avoid logging full request to prevent
     * leaking file paths or other user data into logcat */
    LOGI("MCP tools/call: invoking tool '%s'", tool_name);

    /* Heap-allocate response buffer */
    char *response_buf = malloc(AUDIO_RESPONSE_SIZE);
    if (!response_buf) {
        *out_code = MCP_ERR_INTERNAL_ERROR;
        *out_msg  = "Failed to allocate response buffer";
        json_object_put(request);
        return NULL;
    }
    memset(response_buf, 0, AUDIO_RESPONSE_SIZE);

    int rc = audio_search_service_invoke_json_impl(
        request_str, response_buf, AUDIO_RESPONSE_SIZE - 1);
    response_buf[AUDIO_RESPONSE_SIZE - 1] = '\0';  /* guarantee null termination */

    json_object_put(request);

    if (rc != 0) {
        free(response_buf);
        *out_code = MCP_ERR_INTERNAL_ERROR;
        *out_msg  = "Audio operation failed; see service logs for details";
        return NULL;
    }

    /* Wrap in MCP content envelope */
    json_object *content_item = json_object_new_object();
    json_object_object_add(content_item, "type",
        json_object_new_string("text"));
    json_object_object_add(content_item, "text",
        json_object_new_string(response_buf));

    json_object *content_array = json_object_new_array();
    json_object_array_add(content_array, content_item);

    json_object *result = json_object_new_object();
    json_object_object_add(result, "content", content_array);

    free(response_buf);
    return result;
}

/* ============================================================================
 * Line reader
 * ============================================================================
 */

static int mcp_read_line(char **buf_inout, size_t *cap_inout, size_t *out_len)
{
    char   *buf = *buf_inout;
    size_t  cap = *cap_inout;
    size_t  len = 0;
    int     c;

    while ((c = getchar()) != EOF) {
        if (c == '\n') {
            if (len > 0 && buf[len - 1] == '\r') len--;
            buf[len] = '\0';
            *out_len = len;
            return 1;
        }

        if (len >= MAX_MCP_REQUEST_BYTES) {
            while ((c = getchar()) != EOF && c != '\n') {}
            mcp_write_error(NULL, MCP_ERR_INVALID_REQUEST,
                "Request exceeds maximum size (1 MB)");
            *out_len = 0;
            return 0;
        }

        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *new_buf = realloc(buf, new_cap);
            if (!new_buf) {
                mcp_write_error(NULL, MCP_ERR_INTERNAL_ERROR,
                    "Failed to grow line buffer");
                while ((c = getchar()) != EOF && c != '\n') {}
                *out_len = 0;
                return 0;
            }
            buf = new_buf;
            cap = new_cap;
            *buf_inout = buf;
            *cap_inout = cap;
        }

        buf[len++] = (char)c;
    }

    *out_len = 0;
    return 0;  /* EOF */
}

/* ============================================================================
 * Public API: stdio loop
 * ============================================================================
 */

void kanaha_audio_run_mcp_stdio(void)
{
    size_t  line_cap = MCP_LINE_INITIAL_CAP;
    char   *line_buf = malloc(line_cap);
    size_t  line_len = 0;

    if (!line_buf) {
        mcp_write_error(NULL, MCP_ERR_INTERNAL_ERROR,
            "Failed to allocate initial line buffer");
        return;
    }

    LOGI("Kanaha Audio MCP stdio server starting (protocol %s)",
        KANAHA_MCP_PROTOCOL_VERSION);

    while (mcp_read_line(&line_buf, &line_cap, &line_len)) {
        if (line_len == 0) continue;

        json_object *req = json_tokener_parse(line_buf);
        if (!req) {
            mcp_write_error(NULL, MCP_ERR_PARSE_ERROR,
                "Parse error: invalid JSON");
            continue;
        }

        /* No "id" = notification — silently consume */
        json_object *id_obj = NULL;
        if (!json_object_object_get_ex(req, "id", &id_obj)) {
            json_object_put(req);
            continue;
        }

        /* Extract method */
        json_object *method_obj = NULL;
        if (!json_object_object_get_ex(req, "method", &method_obj)
                || !json_object_is_type(method_obj, json_type_string)) {
            mcp_write_error(id_obj, MCP_ERR_INVALID_REQUEST,
                "method is required and must be a string");
            json_object_put(req);
            continue;
        }
        const char *method = json_object_get_string(method_obj);

        /* Extract optional params */
        json_object *params_obj = NULL;
        json_object_object_get_ex(req, "params", &params_obj);

        /* Dispatch */
        if (strcmp(method, "initialize") == 0) {
            mcp_write_result(id_obj, mcp_handle_initialize());

        } else if (strcmp(method, "tools/list") == 0) {
            mcp_write_result(id_obj, mcp_handle_tools_list());

        } else if (strcmp(method, "tools/call") == 0) {
            int         err_code = 0;
            const char *err_msg  = NULL;
            json_object *result  = mcp_handle_tools_call(
                params_obj, &err_code, &err_msg);
            if (result) {
                mcp_write_result(id_obj, result);
            } else {
                mcp_write_error(id_obj, err_code,
                    err_msg ? err_msg : "Internal error");
            }

        } else {
            mcp_write_error(id_obj, MCP_ERR_METHOD_NOT_FOUND,
                "Method not found");
        }

        json_object_put(req);
    }

    free(line_buf);
    LOGI("Kanaha Audio MCP stdio server exiting");
}
