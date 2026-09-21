/*
 * Kanaha Audio
 * Whisper.cpp Android Bridge - Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Thin wrapper around whisper.cpp C API for Android integration.
 * Handles model loading, transcription, and keyword search.
 */

#ifndef WHISPER_ANDROID_BRIDGE_H
#define WHISPER_ANDROID_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of keyword matches returned */
#define WHISPER_MAX_MATCHES 256

/* Maximum keyword length */
#define WHISPER_MAX_KEYWORD_LEN 256

/* Maximum number of keywords in a search */
#define WHISPER_MAX_KEYWORDS 32

/**
 * A single keyword match with timestamp and confidence
 */
typedef struct {
    char keyword[WHISPER_MAX_KEYWORD_LEN];
    int64_t start_ms;
    int64_t end_ms;
    float confidence;
} whisper_match_t;

/**
 * Result of a keyword search operation
 */
typedef struct {
    whisper_match_t matches[WHISPER_MAX_MATCHES];
    int num_matches;
    int64_t audio_duration_ms;
    int64_t processing_time_ms;
} whisper_search_result_t;

/**
 * Result of a transcription operation
 */
typedef struct {
    char *text;           /* Full transcription text (caller must free) */
    int64_t audio_duration_ms;
    int64_t processing_time_ms;
    int num_segments;
} whisper_transcribe_result_t;

/**
 * Model info
 */
typedef struct {
    char name[128];
    char path[512];
    int loaded;
    int64_t size_bytes;
} whisper_model_info_t;

/**
 * Initialize the whisper bridge.
 * Must be called before any other whisper_bridge_* functions.
 *
 * @param models_dir  Path to directory containing ggml model files
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_init(const char *models_dir);

/**
 * Cleanup the whisper bridge and free all resources.
 */
void whisper_bridge_cleanup(void);

/**
 * Load a whisper model by name (e.g., "base", "tiny", "small").
 * Only one model can be loaded at a time; loading a new model
 * unloads the previous one.
 *
 * @param model_name  Model name without path or extension
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_load_model(const char *model_name);

/**
 * Search for keywords in an audio file.
 * The audio file must be WAV format (PCM 16-bit).
 *
 * @param audio_file   Path to the audio file
 * @param keywords     Array of keyword strings to search for
 * @param num_keywords Number of keywords
 * @param result       Output: search results (caller provides storage)
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_search_keywords(
    const char *audio_file,
    const char **keywords,
    int num_keywords,
    whisper_search_result_t *result
);

/**
 * Transcribe an audio file with word-level timestamps.
 *
 * @param audio_file  Path to the audio file
 * @param result      Output: transcription result (caller must free result->text)
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_transcribe(
    const char *audio_file,
    whisper_transcribe_result_t *result
);

/**
 * Set the decoder prompt used by the next transcribe / keyword search (R2).
 *
 * whisper.cpp conditions a decode on this text, which is how tickers like
 * "MSFT" become words it expects to hear. It biases the decoder; it does not
 * constrain it, and it costs decode budget, so keep it short. Pass NULL or ""
 * to clear it. Empty by default: the cue loop's behaviour must not change
 * unless a caller asks for priming.
 */
void whisper_bridge_set_initial_prompt(const char *prompt);

/**
 * The prompt currently in force, or NULL when there is none.
 */
const char *whisper_bridge_get_initial_prompt(void);

/**
 * Get status of the whisper bridge.
 *
 * @param json_buffer  Output buffer for JSON status
 * @param buffer_size  Size of output buffer
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_get_status(char *json_buffer, size_t buffer_size);

/**
 * List available models in the models directory.
 *
 * @param models      Output array of model info structs
 * @param max_models  Maximum number of models to return
 * @return Number of models found, or -1 on failure
 */
int whisper_bridge_list_models(whisper_model_info_t *models, int max_models);

/**
 * List audio files in a directory.
 *
 * @param dir          Directory to scan
 * @param json_buffer  Output buffer for JSON file list
 * @param buffer_size  Size of output buffer
 * @return 0 on success, -1 on failure
 */
int whisper_bridge_list_audio_files(
    const char *dir,
    char *json_buffer,
    size_t buffer_size
);

#ifdef __cplusplus
}
#endif

#endif /* WHISPER_ANDROID_BRIDGE_H */
