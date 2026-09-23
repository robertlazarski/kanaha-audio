/*
 * Kanaha Audio
 * Whisper.cpp Android Bridge - Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Thin wrapper around whisper.cpp C API. Handles:
 * - Model lifecycle (load/unload)
 * - WAV file reading (16-bit PCM to float32 conversion)
 * - Transcription with token-level timestamps
 * - Keyword matching against transcription tokens
 *
 * Unlike Kanaha Camera (which uses Intent IPC across GPL boundary),
 * this calls whisper.cpp directly — same process, no serialization overhead.
 *
 * DATA FLOW OVERVIEW:
 *
 *   WAV file on disk
 *        |
 *        v
 *   read_wav_pcm16()          -- Shared WAV reader (audio_util.c), also used by YAMNet bridge
 *        |
 *        v
 *   whisper_full()            -- Run whisper.cpp inference (greedy decoding, token timestamps)
 *        |
 *        v
 *   whisper_full_n_segments() -- Get number of text segments
 *   whisper_full_n_tokens()   -- Get tokens per segment (words with timestamps)
 *        |
 *        v
 *   For searchKeywords:       -- Collect all tokens, strip punctuation, lowercase,
 *     sliding window match       then slide a window matching keyword words
 *                                against consecutive tokens. Same algorithm as
 *                                the Python proof-of-concept (test_keyword_search.py).
 *
 *   For transcribe:           -- Concatenate segment texts into one string.
 *
 * WHISPER.CPP TIMESTAMP UNITS:
 *   whisper_token_data.t0 and t1 are in units of 10ms (centiseconds).
 *   We multiply by 10 to convert to milliseconds for the API response.
 *   Example: t0=530 means 5300ms = 5.3 seconds into the audio.
 */

#include "whisper_android_bridge.h"
#include "whisper.h"
#include "../audio_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#define LOG_TAG "WhisperBridge"
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
 * Only one whisper model can be loaded at a time. This matches the
 * Android use case where memory is limited and the phone runs one
 * service. Thread safety is not needed — the server is single-threaded
 * (one request at a time via the select() accept loop).
 * ======================================================================== */

static char g_models_dir[512] = {0};         /* Directory containing ggml-*.bin files */
static char g_current_model[128] = {0};      /* Name of currently loaded model (e.g., "base") */
static int g_initialized = 0;                /* 1 after whisper_bridge_init() succeeds */
static struct whisper_context *g_whisper_ctx = NULL;  /* The loaded whisper model context */


/* ========================================================================
 * WAV file reader — now in audio_util.c (shared with YAMNet bridge)
 *
 * The read_wav_pcm16() function was extracted to audio_util.c so both
 * the whisper bridge and the YAMNet bridge can use the same WAV reader
 * without code duplication. See audio_util.h for the API.
 * ======================================================================== */

/* ========================================================================
 * Initialization / cleanup
 *
 * whisper_bridge_init() just records the models directory. No model is
 * loaded yet — call whisper_bridge_load_model() to load one before
 * running any inference operations.
 * ======================================================================== */

/* whisper.cpp and ggml log through their own callback, which defaults to
 * stderr -- and stderr goes nowhere under Android. Without this, failures
 * inside the library are simply invisible: DTW alignment can fail to
 * initialise and the only symptom is t_dtw coming back as -1 on every token. */

static void whisper_log_to_android(enum ggml_log_level level, const char *text, void *user_data) {
    (void)user_data;
    if (!text || !*text) return;
    int prio = (level == GGML_LOG_LEVEL_ERROR) ? ANDROID_LOG_ERROR
             : (level == GGML_LOG_LEVEL_WARN)  ? ANDROID_LOG_WARN
             : ANDROID_LOG_INFO;
    __android_log_print(prio, "WhisperCpp", "%s", text);
}

int whisper_bridge_init(const char *models_dir) {
    whisper_log_set(whisper_log_to_android, NULL);

    if (!models_dir) {
        LOGE("whisper_bridge_init: models_dir is NULL");
        return -1;
    }

    strncpy(g_models_dir, models_dir, sizeof(g_models_dir) - 1);
    g_models_dir[sizeof(g_models_dir) - 1] = '\0';
    g_initialized = 1;
    g_whisper_ctx = NULL;
    g_current_model[0] = '\0';

    LOGI("Whisper bridge initialized, models_dir: %s", g_models_dir);
    return 0;
}

static void whisper_bridge_cleanup_unlocked(void) {
    LOGI("Whisper bridge cleanup");

    if (g_whisper_ctx) {
        whisper_free(g_whisper_ctx);  /* Release all model memory */
        g_whisper_ctx = NULL;
    }

    g_current_model[0] = '\0';
    g_initialized = 0;
}

/* ========================================================================
 * Model loading
 *
 * Models are ggml-format files named "ggml-<name>.bin" in the models
 * directory. For example, "ggml-base.bin" is loaded by passing "base".
 *
 * Only one model can be loaded at a time. Loading a new model first
 * frees the previous one. On a Pixel 9 Pro (16 GB RAM):
 *   - tiny:   ~390 MB RAM, fastest
 *   - base:   ~500 MB RAM, good balance (recommended for keyword search)
 *   - small:  ~1 GB RAM, better accuracy
 *   - medium: ~2.6 GB RAM, high accuracy but slow
 * ======================================================================== */

/* DTW alignment-head preset for the loaded model. Hardcoding BASE_EN silently
 * degraded token timestamps whenever a different model (tiny.en, small.en, ...)
 * was loaded -- the docs advertise those. Match the preset to the model, and
 * fall back to NONE (DTW off) rather than a wrong preset. */
static enum whisper_alignment_heads_preset dtw_preset_for_model(const char *m) {
    if (!m) return WHISPER_AHEADS_NONE;
    if (!strcmp(m, "tiny.en"))   return WHISPER_AHEADS_TINY_EN;
    if (!strcmp(m, "tiny"))      return WHISPER_AHEADS_TINY;
    if (!strcmp(m, "base.en"))   return WHISPER_AHEADS_BASE_EN;
    if (!strcmp(m, "base"))      return WHISPER_AHEADS_BASE;
    if (!strcmp(m, "small.en"))  return WHISPER_AHEADS_SMALL_EN;
    if (!strcmp(m, "small"))     return WHISPER_AHEADS_SMALL;
    if (!strcmp(m, "medium.en")) return WHISPER_AHEADS_MEDIUM_EN;
    if (!strcmp(m, "medium"))    return WHISPER_AHEADS_MEDIUM;
    if (!strcmp(m, "large-v3-turbo")) return WHISPER_AHEADS_LARGE_V3_TURBO;
    if (!strcmp(m, "large-v3"))  return WHISPER_AHEADS_LARGE_V3;
    if (!strcmp(m, "large-v2"))  return WHISPER_AHEADS_LARGE_V2;
    if (!strcmp(m, "large"))     return WHISPER_AHEADS_LARGE_V1;
    LOGE("No DTW aheads preset for model '%s'; disabling DTW (timestamps degrade)", m);
    return WHISPER_AHEADS_NONE;
}

/* Escape a string into a JSON string literal (out >= 2*len+1); drops controls. */
static const char *wjson_escape(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if (c >= 0x20) { out[j++] = (char)c; }
    }
    out[j] = '\0';
    return out;
}

static int whisper_bridge_load_model_unlocked(const char *model_name) {
    if (!g_initialized) {
        LOGE("Whisper bridge not initialized");
        return -1;
    }

    if (!model_name || strlen(model_name) == 0) {
        LOGE("Invalid model name");
        return -1;
    }

    /* Security: reject path traversal — model name must be a plain name like "base" */
    if (strstr(model_name, "..") || strchr(model_name, '/')) {
        LOGE("Security: path traversal in model name rejected");
        return -1;
    }

    /* Build full path: <models_dir>/ggml-<model_name>.bin */
    char model_path[1024];
    snprintf(model_path, sizeof(model_path), "%s/ggml-%s.bin",
             g_models_dir, model_name);

    /* Verify the file exists before attempting to load (fast fail) */
    struct stat st;
    if (stat(model_path, &st) != 0) {
        LOGE("Model file not found: %s", model_path);
        return -1;
    }

    /* Unload previous model if any — only one model at a time to save RAM */
    if (g_whisper_ctx) {
        LOGI("Unloading previous model: %s", g_current_model);
        whisper_free(g_whisper_ctx);
        g_whisper_ctx = NULL;
    }

    LOGI("Loading model: %s (%lld bytes)", model_path, (long long)st.st_size);

    /*
     * whisper_init_from_file_with_params() reads the entire ggml model into
     * memory and builds the computation graph. This is the most memory-intensive
     * operation — for the "base" model, expect ~500 MB of RAM usage.
     */
    struct whisper_context_params cparams = whisper_context_default_params();

    /* DTW token-level timestamps.
     *
     * Without this, whisper's token t0/t1 are heuristic and effectively useless
     * for cueing: a short phrase inside a long quiet stretch comes back as one
     * segment with the tokens smeared across it -- "next slide please" in 28.9 s
     * of room tone reported 0-16950 ms, a 17-second window.
     *
     * DTW aligns tokens against the encoder's cross-attention instead, which is
     * what makes a match usable as an edit cue. The aheads preset must match the
     * model; we only ship base.en, so it is selected directly rather than
     * inferred. If a different model is ever added, this needs a lookup, and a
     * mismatched preset degrades alignment silently. */
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset = dtw_preset_for_model(model_name);

    /* whisper.cpp silently disables DTW when flash attention is on --
     *   "dtw_token_timestamps is not supported with flash_attn - disabling"
     * logged through its own callback, which is why the log is routed to
     * logcat above. Leaving flash_attn at its default made every token come
     * back with t_dtw = -1 and the coarse heuristic timestamps stood in
     * unnoticed. DTW is the point here, so flash attention gives way. */
    cparams.flash_attn = false;

    g_whisper_ctx = whisper_init_from_file_with_params(model_path, cparams);

    if (!g_whisper_ctx) {
        LOGE("Failed to load whisper model: %s", model_path);
        return -1;
    }

    strncpy(g_current_model, model_name, sizeof(g_current_model) - 1);
    g_current_model[sizeof(g_current_model) - 1] = '\0';

    LOGI("Model loaded successfully: %s", model_name);
    return 0;
}

/* ========================================================================
 * Helper: clean a token's text for keyword matching
 *
 * Whisper tokens come back with leading spaces, trailing punctuation,
 * and mixed case. For example:  " Next"  " slide,"  " please."
 *
 * This function strips whitespace and punctuation from both ends, then
 * lowercases the result so we can do exact word matching:
 *   " Next"    → "next"
 *   " slide,"  → "slide"
 *   " please." → "please"
 *
 * This matches the Python proof-of-concept behavior in test_keyword_search.py
 * where we used: word.strip().rstrip(".,!?;:").lower()
 * ======================================================================== */

static void strip_for_matching(const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) return;

    /* Skip leading whitespace (whisper tokens often start with a space) */
    while (*input && isspace((unsigned char)*input)) input++;

    size_t len = strlen(input);
    if (len >= out_size) len = out_size - 1;

    strncpy(output, input, len);
    output[len] = '\0';

    /* Strip trailing whitespace and punctuation (.,!?;: etc.) */
    while (len > 0 && (isspace((unsigned char)output[len - 1]) ||
                       ispunct((unsigned char)output[len - 1]))) {
        output[--len] = '\0';
    }

    /* Lowercase for case-insensitive matching */
    for (size_t i = 0; i < len; i++) {
        output[i] = tolower((unsigned char)output[i]);
    }
}

/*
 * How much of whisper's encoder window to actually run, for TRANSCRIPTION ONLY.
 *
 * The encoder is a fixed 30-second window: a 12-second clip costs the same as a
 * 30-second one. audio_ctx caps it at the part of the window the audio occupies
 * -- 1500 positions cover 30 s, so 50 per second -- and on this phone that
 * halved a 12-second transcription, 7.6 s to 3.8 s.
 *
 * It is not free. At a 25 % margin that same clip came back with "Portfolio
 * Variants" where the full window heard "Portfolio variance": less context makes
 * the decoder likelier to pick the commoner word. That is tolerable here because
 * a transcript is read by a model resolving intent, which has absorbed worse
 * ("vowel" for "vol", "the whole variance" for "the covariance"). It is NOT
 * tolerable for keyword search, which compares words literally, so that path
 * keeps the whole window.
 *
 * The margin is therefore generous rather than aggressive: half again as much
 * context as the audio needs, never below 768. Zero means "use everything",
 * which is what anything past about twenty seconds gets anyway.
 */
static int audio_ctx_for(int n_samples, int sample_rate) {
    if (n_samples <= 0 || sample_rate <= 0) return 0;
    double seconds = (double)n_samples / (double)sample_rate;
    int ctx = (int)(seconds * 50.0 * 1.5) + 128;
    ctx = (ctx + 63) / 64 * 64;          /* keep it a round number of positions */
    if (ctx < 768) ctx = 768;
    if (ctx >= 1500) return 0;           /* long enough to need all of it */
    return ctx;
}

/* ========================================================================
 * Keyword search — the core operation for parseLTC.sh integration
 *
 * ALGORITHM (sliding window over tokens):
 *
 * 1. Run whisper_full() with token_timestamps=true to get per-token
 *    timestamps (not just per-segment). Each token has t0/t1 in 10ms units.
 *
 * 2. Collect all tokens across all segments into a flat array, stripping
 *    punctuation and lowercasing each word.
 *
 * 3. For each keyword phrase (e.g., "next slide please"), split into
 *    individual words ["next", "slide", "please"].
 *
 * 4. Slide a window of that length across the token array. If all words
 *    match consecutively, record a match with:
 *    - start_ms from the first token's t0
 *    - end_ms from the last token's t1
 *    - confidence = average probability across matched tokens
 *
 * This is the same algorithm proven in test_keyword_search.py, which found
 * "next slide please" at 5,300ms and 13,000ms with 0.89 and 0.99 confidence.
 *
 * WHY TOKEN-LEVEL (not segment-level):
 *   Whisper segments can be very long (the entire 20s test audio was one
 *   segment). Segment-level matching gives a match spanning 0-19400ms,
 *   which is useless. Token-level gives 5300-7760ms — precise enough
 *   for parseLTC.sh third-camera priority windows.
 * ======================================================================== */

static int whisper_bridge_search_keywords_unlocked(
    const char *audio_file,
    const char **keywords,
    int num_keywords,
    const char *initial_prompt,
    whisper_search_result_t *result
) {
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (!g_initialized || !g_whisper_ctx) {
        LOGE("Whisper bridge not ready (initialized=%d, ctx=%p)",
             g_initialized, (void *)g_whisper_ctx);
        return -1;
    }

    if (!audio_file || !keywords || num_keywords <= 0 || !result) {
        LOGE("Invalid parameters for keyword search");
        return -1;
    }

    /*
     * Security: reject path traversal sequences.
     * Note: we allow absolute paths here because audio_search_service.c
     * constructs full paths like /data/data/org.kanaha.audio/files/audio/file.wav.
     * The service layer is the trust boundary — it validates user input before
     * passing paths to the bridge. The bridge's ".." check is defense-in-depth.
     */
    if (strstr(audio_file, "..")) {
        LOGE("Security: path traversal in audio_file rejected");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    /* ── Step 1: Read WAV file into float32 array ────────────── */
    int n_samples = 0;
    int sample_rate = 0;
    float *pcmf32 = read_wav_pcm16(audio_file, &n_samples, &sample_rate);
    if (!pcmf32) {
        LOGE("Failed to read audio file: %s", audio_file);
        return -1;
    }

    result->audio_duration_ms = (int64_t)n_samples * 1000 / sample_rate;

    LOGI("Searching %d keywords in: %s (%d samples, %lld ms)",
         num_keywords, audio_file, n_samples, (long long)result->audio_duration_ms);

    /* ── Step 2: Configure and run whisper inference ──────────── */
    /*
     * WHISPER_SAMPLING_GREEDY: simplest decoding strategy, picks the most
     * likely token at each step. Fast and sufficient for clear speech.
     *
     * token_timestamps=true: enables per-token t0/t1 fields in whisper_token_data.
     * This is what gives us word-level precision instead of segment-level.
     *
     * n_threads=4: ARM64 big.LITTLE — 4 threads is a safe default for the
     * performance cores on a Pixel 9 Pro without starving the OS.
     */
    struct whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime   = false;  /* Don't print to stderr during inference */
    wparams.print_progress   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.token_timestamps = true;   /* CRITICAL: enables per-token t0/t1 */
    wparams.language         = "en";   /* Force English (skip language detection) */
    wparams.n_threads        = 4;
    wparams.initial_prompt   = (initial_prompt && initial_prompt[0]) ? initial_prompt : NULL;
    /* Keyword search deliberately keeps the full encoder window. Matching is
     * exact word against exact word, so a transcript that degrades by one
     * syllable is a trigger that never fires, and nobody can tell it was
     * heard at all. The cost is paid here, where it can be seen. */

    int rc = whisper_full(g_whisper_ctx, wparams, pcmf32, n_samples);
    free(pcmf32);  /* Audio data no longer needed after inference */

    if (rc != 0) {
        LOGE("whisper_full() failed with code %d", rc);
        return -1;
    }

    int n_segments = whisper_full_n_segments(g_whisper_ctx);
    LOGI("Transcription complete: %d segments", n_segments);

    /* ── Step 3: Collect all tokens into a flat array ────────── */
    /*
     * We flatten tokens across all segments because keyword phrases
     * might span a segment boundary (unlikely but possible).
     *
     * Each token_info_t holds:
     *   word: cleaned text (lowercase, no punctuation)
     *   t0:   start time in milliseconds
     *   t1:   end time in milliseconds
     *   p:    probability (0.0 to 1.0)
     */
    typedef struct {
        char word[128];         /* Cleaned word for matching */
        int64_t t0;             /* Start time in milliseconds */
        int64_t t1;             /* End time in milliseconds */
        int64_t t_dtw;          /* DTW-aligned instant, ms; -1 when unavailable */
        float p_sum;            /* Probability summed over the pieces of this word */
        int n_pieces;           /* How many tokens were joined to make it */
        float p;                /* Token probability from whisper */
    } token_info_t;

    int max_tokens = 4096;  /* ~1 hour of speech at 1 token per word */
    token_info_t *tokens = (token_info_t *)calloc(max_tokens, sizeof(token_info_t));
    if (!tokens) {
        LOGE("Failed to allocate token array");
        return -1;
    }

    int total_tokens = 0;

    for (int seg = 0; seg < n_segments; seg++) {
        int n_tokens = whisper_full_n_tokens(g_whisper_ctx, seg);
        for (int tok = 0; tok < n_tokens && total_tokens < max_tokens; tok++) {
            const char *text = whisper_full_get_token_text(g_whisper_ctx, seg, tok);
            if (!text || text[0] == '\0') continue;

            /* Skip special tokens — whisper emits [_BEG_], <|en|>, etc. */
            if (text[0] == '[' || text[0] == '<') continue;

            /* Whisper's vocabulary is sub-word, and it marks a word START with
             * a leading space: "portfolio" arrives as " Port" + "folio", while
             * "Microsoft" happens to be one token. Matching token by token
             * therefore found "microsoft" and never "portfolio" — silently, and
             * only for the longer words, which is the worst way for it to fail.
             * The leading space is the only signal available here, so it is read
             * before the text is cleaned (cleaning strips it). */
            int starts_word = isspace((unsigned char)text[0]);

            /* Get timestamp and probability data for this token */
            whisper_token_data tdata =
                whisper_full_get_token_data(g_whisper_ctx, seg, tok);

            /* Clean the token text: " Next," → "next" */
            char piece[128];
            strip_for_matching(text, piece, sizeof(piece));

            /* Skip if the token was only whitespace/punctuation */
            if (piece[0] == '\0') continue;

            if (!starts_word && total_tokens > 0) {
                /* A continuation: glue it onto the word being built. The word
                 * keeps its first piece's start, takes this piece's end, and
                 * averages probability across the pieces, so a word assembled
                 * from three tokens is not scored by whichever one came last. */
                token_info_t *w = &tokens[total_tokens - 1];
                strncat(w->word, piece, sizeof(w->word) - strlen(w->word) - 1);
                w->t1 = tdata.t1 * 10;
                w->p_sum += tdata.p;
                w->n_pieces++;
                w->p = w->p_sum / w->n_pieces;
                continue;
            }

            strncpy(tokens[total_tokens].word, piece,
                    sizeof(tokens[total_tokens].word) - 1);
            tokens[total_tokens].word[sizeof(tokens[total_tokens].word) - 1] = '\0';

            /* Convert whisper's 10ms units to milliseconds */
            tokens[total_tokens].t0 = tdata.t0 * 10;
            tokens[total_tokens].t1 = tdata.t1 * 10;
            /* t_dtw is -1 when DTW alignment did not produce a value for this
             * token; keep the sentinel rather than scaling it into a real time. */
            tokens[total_tokens].t_dtw = (tdata.t_dtw < 0) ? -1 : tdata.t_dtw * 10;
            tokens[total_tokens].p  = tdata.p;
            tokens[total_tokens].p_sum = tdata.p;
            tokens[total_tokens].n_pieces = 1;

            total_tokens++;
        }
    }

    LOGI("Collected %d words for keyword matching", total_tokens);

    /* ── Step 4: Sliding window keyword phrase matching ───────── */
    /*
     * For each keyword phrase (e.g., "next slide please"):
     *   1. Split into words: ["next", "slide", "please"]
     *   2. For each position i in the token array:
     *      - Check if tokens[i..i+2] match all 3 keyword words
     *      - If yes, record match with timestamps from first/last token
     *
     * This handles multi-word phrases correctly. A 3-word keyword
     * requires 3 consecutive matching tokens.
     */
    for (int kw_idx = 0; kw_idx < num_keywords; kw_idx++) {
        /* Lowercase the entire keyword for comparison */
        char kw_lower[WHISPER_MAX_KEYWORD_LEN];
        strncpy(kw_lower, keywords[kw_idx], sizeof(kw_lower) - 1);
        kw_lower[sizeof(kw_lower) - 1] = '\0';
        for (char *p = kw_lower; *p; p++) *p = tolower((unsigned char)*p);

        /* Split keyword into individual words using strtok_r */
        char *kw_words[32];       /* Max 32 words per keyword phrase */
        int kw_word_count = 0;
        char *saveptr = NULL;
        char *tok = strtok_r(kw_lower, " ", &saveptr);
        while (tok && kw_word_count < 32) {
            kw_words[kw_word_count++] = tok;
            tok = strtok_r(NULL, " ", &saveptr);
        }

        if (kw_word_count == 0) continue;

        /* Slide the window across all collected tokens */
        for (int i = 0; i <= total_tokens - kw_word_count; i++) {
            /* Check if tokens[i..i+kw_word_count-1] match all keyword words */
            int match = 1;
            for (int j = 0; j < kw_word_count; j++) {
                if (strcmp(tokens[i + j].word, kw_words[j]) != 0) {
                    match = 0;
                    break;
                }
            }

            if (match && result->num_matches < WHISPER_MAX_MATCHES) {
                whisper_match_t *m = &result->matches[result->num_matches];

                /* Record the keyword as originally provided (preserving case) */
                strncpy(m->keyword, keywords[kw_idx], sizeof(m->keyword) - 1);
                m->keyword[sizeof(m->keyword) - 1] = '\0';

                /* Timestamp span: first token's start to last token's end.
                 *
                 * Prefer the DTW instants when alignment produced them. The
                 * heuristic t0/t1 smear a short phrase across its whole segment
                 * -- a phrase in room tone came back as a 17-second window --
                 * which is useless as an edit cue. Fall back to t0/t1 when DTW
                 * is unavailable, so behaviour degrades rather than breaking. */
                int64_t dtw_first = tokens[i].t_dtw;
                int64_t dtw_last  = tokens[i + kw_word_count - 1].t_dtw;
                if (dtw_first >= 0 && dtw_last >= dtw_first) {
                    m->start_ms = dtw_first;
                    m->end_ms   = dtw_last;
                }
                else {
                    m->start_ms = tokens[i].t0;
                    m->end_ms   = tokens[i + kw_word_count - 1].t1;
                }

                /* Confidence = average probability across all matched tokens */
                float sum_p = 0.0f;
                for (int j = 0; j < kw_word_count; j++) {
                    sum_p += tokens[i + j].p;
                }
                m->confidence = sum_p / kw_word_count;

                LOGI("Match: '%s' at %lld-%lld ms (confidence %.2f)",
                     m->keyword, (long long)m->start_ms,
                     (long long)m->end_ms, m->confidence);

                result->num_matches++;
            }
        }
    }

    free(tokens);

    /* ── Record processing time ─────────────────────────────── */
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    result->processing_time_ms =
        (end_time.tv_sec - start_time.tv_sec) * 1000 +
        (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    LOGI("Keyword search complete: %d matches in %lld ms",
         result->num_matches, (long long)result->processing_time_ms);

    return 0;
}

/* ========================================================================
 * Full transcription
 *
 * Returns the complete transcription text by concatenating all segment
 * texts with spaces between them. The caller must free result->text.
 *
 * On success (return 0), the caller owns result->text and must free it.
 * On failure (return -1), result->text is NULL — the callee cleans up
 * any partial allocations before returning.
 * ======================================================================== */

static int whisper_bridge_transcribe_unlocked(
    const char *audio_file,
    const char *initial_prompt,
    whisper_transcribe_result_t *result
) {
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (!g_initialized || !g_whisper_ctx) {
        LOGE("Whisper bridge not ready");
        return -1;
    }

    if (!audio_file || !result) {
        LOGE("Invalid parameters for transcription");
        return -1;
    }

    /* Security: reject path traversal (see searchKeywords comment for rationale) */
    if (strstr(audio_file, "..")) {
        LOGE("Security: path traversal in audio_file rejected");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    /* Read WAV file into float32 */
    int n_samples = 0;
    int sample_rate = 0;
    float *pcmf32 = read_wav_pcm16(audio_file, &n_samples, &sample_rate);
    if (!pcmf32) {
        LOGE("Failed to read audio file: %s", audio_file);
        return -1;
    }

    result->audio_duration_ms = (int64_t)n_samples * 1000 / sample_rate;

    LOGI("Transcribing: %s (%d samples, %lld ms)",
         audio_file, n_samples, (long long)result->audio_duration_ms);

    /* Same whisper params as keyword search — token timestamps included
     * so the transcription could be used for further analysis */
    struct whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime   = false;
    wparams.print_progress   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.token_timestamps = true;
    wparams.language         = "en";
    wparams.n_threads        = 4;
    wparams.initial_prompt   = (initial_prompt && initial_prompt[0]) ? initial_prompt : NULL;
    wparams.audio_ctx        = audio_ctx_for(n_samples, sample_rate);

    /* Run inference — this is the expensive step */
    int rc = whisper_full(g_whisper_ctx, wparams, pcmf32, n_samples);
    free(pcmf32);

    if (rc != 0) {
        LOGE("whisper_full() failed with code %d", rc);
        return -1;  /* result->text is NULL (memset above), caller must not free */
    }

    int n_segments = whisper_full_n_segments(g_whisper_ctx);
    result->num_segments = n_segments;

    /* ── Build full text from segments ───────────────────────── */
    /*
     * Segments are the natural sentence-like chunks whisper produces.
     * We concatenate them with spaces. The text buffer grows dynamically
     * (starts at 4KB, doubles when needed).
     */
    size_t text_cap = 4096;
    size_t text_len = 0;
    char *text = (char *)malloc(text_cap);
    if (!text) {
        LOGE("Failed to allocate text buffer");
        return -1;  /* result->text is NULL, caller must not free */
    }
    text[0] = '\0';

    for (int i = 0; i < n_segments; i++) {
        const char *seg_text = whisper_full_get_segment_text(g_whisper_ctx, i);
        if (!seg_text) continue;

        size_t seg_len = strlen(seg_text);

        /* Grow buffer if needed (double until large enough) */
        while (text_len + seg_len + 2 >= text_cap) {
            text_cap *= 2;
            char *new_text = (char *)realloc(text, text_cap);
            if (!new_text) {
                LOGE("Failed to grow text buffer");
                free(text);
                return -1;  /* Clean up — callee responsible on failure */
            }
            text = new_text;
        }

        /* Add space separator between segments */
        if (text_len > 0) {
            text[text_len++] = ' ';
        }
        memcpy(text + text_len, seg_text, seg_len);
        text_len += seg_len;
        text[text_len] = '\0';
    }

    result->text = text;  /* Caller now owns this — must free() */

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    result->processing_time_ms =
        (end_time.tv_sec - start_time.tv_sec) * 1000 +
        (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    LOGI("Transcription complete: %d segments, %zu chars in %lld ms",
         result->num_segments, text_len, (long long)result->processing_time_ms);

    return 0;
}

/* ========================================================================
 * Status — returns JSON with bridge state for the getStatus API
 * ======================================================================== */

static int whisper_bridge_get_status_unlocked(char *json_buffer, size_t buffer_size) {
    if (!json_buffer || buffer_size == 0) return -1;

    snprintf(json_buffer, buffer_size,
        "{"
        "\"initialized\":%s,"
        "\"model_loaded\":%s,"
        "\"current_model\":\"%s\","
        "\"models_dir\":\"%s\""
        "}",
        g_initialized ? "true" : "false",
        g_whisper_ctx ? "true" : "false",
        g_current_model[0] ? g_current_model : "none",
        g_models_dir
    );

    return 0;
}

/* ========================================================================
 * Model listing — scans the models directory for ggml-*.bin files
 *
 * Returns info about each model: name, file path, size, and whether
 * it's the currently loaded model.
 * ======================================================================== */

static int whisper_bridge_list_models_unlocked(whisper_model_info_t *models, int max_models) {
    if (!g_initialized || !models || max_models <= 0) return -1;

    DIR *dir = opendir(g_models_dir);
    if (!dir) {
        LOGE("Cannot open models directory: %s", g_models_dir);
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max_models) {
        /* Only match files named ggml-<something>.bin */
        if (strncmp(entry->d_name, "ggml-", 5) != 0) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".bin") != 0) continue;

        /* Extract model name: "ggml-base.bin" → "base" */
        size_t name_len = ext - (entry->d_name + 5);
        if (name_len == 0 || name_len >= sizeof(models[count].name)) continue;

        strncpy(models[count].name, entry->d_name + 5, name_len);
        models[count].name[name_len] = '\0';

        snprintf(models[count].path, sizeof(models[count].path),
                 "%s/%s", g_models_dir, entry->d_name);

        /* Mark if this model is currently loaded */
        models[count].loaded =
            (g_current_model[0] && strcmp(g_current_model, models[count].name) == 0);

        /* Get file size for display (e.g., "base: 142 MB") */
        struct stat st;
        if (stat(models[count].path, &st) == 0) {
            models[count].size_bytes = st.st_size;
        } else {
            models[count].size_bytes = 0;
        }

        count++;
    }

    closedir(dir);
    LOGI("Found %d whisper models in %s", count, g_models_dir);
    return count;
}

/* ========================================================================
 * Audio file listing — scans a directory for processable audio files
 *
 * Returns a JSON array of files with names and sizes. Supports WAV, MP3,
 * FLAC, OGG, and M4A extensions (though only WAV is currently processed
 * by the bridge — others would need ffmpeg conversion first).
 * ======================================================================== */

int whisper_bridge_list_audio_files(
    const char *dir_path,
    char *json_buffer,
    size_t buffer_size
) {
    if (!dir_path || !json_buffer || buffer_size == 0) return -1;

    /* Security: reject path traversal */
    if (strstr(dir_path, "..")) {
        LOGE("Security: path traversal in dir rejected");
        return -1;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        LOGE("Cannot open audio directory: %s", dir_path);
        return -1;
    }

    /* Build JSON array incrementally using snprintf with offset tracking */
    size_t offset = 0;
    /* Clamp even the prefix: on an absurdly small buffer an unchecked
     * offset += snprintf() would exceed buffer_size and underflow every
     * later buffer_size - offset. */
    {
        int pw = snprintf(json_buffer + offset, buffer_size - offset, "{\"files\":[");
        if (pw < 0 || (size_t)pw >= buffer_size) {
            closedir(dir);   /* opened just above -- do not leak the fd */
            return -1;
        }
        offset += (size_t)pw;
    }

    int first = 1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        /* Match common audio file extensions */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".wav") != 0 && strcmp(ext, ".mp3") != 0 &&
            strcmp(ext, ".flac") != 0 && strcmp(ext, ".ogg") != 0 &&
            strcmp(ext, ".m4a") != 0) continue;

        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0) continue;

        /* Escape first, then size the check from the ESCAPED length -- escaping
         * can up to double a name, so checking the raw length under-reserves.
         * Then clamp the write: snprintf returns the length it *wanted* to
         * write, not what it actually wrote, so an unchecked `offset += snprintf`
         * on truncation would push offset past buffer_size and make the closing
         * snprintf(... buffer_size - offset ...) underflow to a huge size_t and
         * write out of bounds. */
        char esc_name[512];
        wjson_escape(entry->d_name, esc_name, sizeof(esc_name));
        size_t needed = strlen(esc_name) + 48;   /* {"name":"","size_bytes":N} + comma */
        if (offset + needed + 3 >= buffer_size) { /* +3 for "]}" and NUL */
            LOGD("JSON buffer full, truncating file list at %zu bytes", offset);
            break;
        }
        int w = snprintf(json_buffer + offset, buffer_size - offset,
            "%s{\"name\":\"%s\",\"size_bytes\":%lld}",
            first ? "" : ",", esc_name, (long long)st.st_size);
        if (w < 0 || (size_t)w >= buffer_size - offset) {
            LOGD("JSON entry would truncate; stopping file list");
            break;   /* do NOT advance offset past the buffer */
        }
        offset += (size_t)w;
        first = 0;
    }

    closedir(dir);

    snprintf(json_buffer + offset, buffer_size - offset, "]}");
    return 0;
}

/* ========================================================================
 * Locking
 *
 * Everything above assumes one caller at a time -- the comments still say
 * "the server is single-threaded". That was true of the superseded custom
 * server. The shipped server is Apache with ThreadsPerChild 4, and mod_axis2
 * does not serialise service invocations, so two connections can run
 * whisper_full() on the same context at once, and loadModel can
 * whisper_free() a context another thread is still inferring on.
 *
 * One mutex around every entry point that touches g_whisper_ctx or the model
 * bookkeeping. Requests queue; a getStatus waits behind a transcribe. That is
 * the correct trade for a single shared model on a phone.
 * ======================================================================== */

static pthread_mutex_t g_whisper_lock = PTHREAD_MUTEX_INITIALIZER;

int whisper_bridge_load_model(const char *model_name) {
    pthread_mutex_lock(&g_whisper_lock);
    int rc = whisper_bridge_load_model_unlocked(model_name);
    pthread_mutex_unlock(&g_whisper_lock);
    return rc;
}

int whisper_bridge_search_keywords(const char *audio_file, const char **keywords,
                                   int num_keywords, const char *initial_prompt,
                                   whisper_search_result_t *result) {
    pthread_mutex_lock(&g_whisper_lock);
    int rc = whisper_bridge_search_keywords_unlocked(audio_file, keywords, num_keywords,
                                                     initial_prompt, result);
    pthread_mutex_unlock(&g_whisper_lock);
    return rc;
}

int whisper_bridge_transcribe(const char *audio_file, const char *initial_prompt,
                              whisper_transcribe_result_t *result) {
    pthread_mutex_lock(&g_whisper_lock);
    int rc = whisper_bridge_transcribe_unlocked(audio_file, initial_prompt, result);
    pthread_mutex_unlock(&g_whisper_lock);
    return rc;
}

int whisper_bridge_get_status(char *json_buffer, size_t buffer_size) {
    pthread_mutex_lock(&g_whisper_lock);
    int rc = whisper_bridge_get_status_unlocked(json_buffer, buffer_size);
    pthread_mutex_unlock(&g_whisper_lock);
    return rc;
}

int whisper_bridge_list_models(whisper_model_info_t *models, int max_models) {
    pthread_mutex_lock(&g_whisper_lock);
    int rc = whisper_bridge_list_models_unlocked(models, max_models);
    pthread_mutex_unlock(&g_whisper_lock);
    return rc;
}

void whisper_bridge_cleanup(void) {
    pthread_mutex_lock(&g_whisper_lock);
    whisper_bridge_cleanup_unlocked();
    pthread_mutex_unlock(&g_whisper_lock);
}
