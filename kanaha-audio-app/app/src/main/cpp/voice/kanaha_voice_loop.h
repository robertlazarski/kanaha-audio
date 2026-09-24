/*
 * Kanaha Audio - the voice loop, on the phone
 * Licensed under the Apache License, Version 2.0
 *
 * The listening loop kanaha-voice-loop.py ran from a laptop, as a thread in
 * this process, next to whisper and flite: nothing crosses a cable or adb.
 *
 *   trigger clips, back to back -> keyword search on each finished clip
 *   "calculate" (or "run it", "stress it", "simulate it") above the floor ->
 *     OPEN tone -> one dictation window -> WORKING tone -> transcribe
 *     -> "cancel" drops it (DROPPED tone) -> nothing heard (NOTHING tone)
 *     -> kanaha_voice_request: resolve, call Kanaha Calcs, say the answer
 *     -> a question opens its own answer window, no trigger needed
 *
 * The microphone is stopped for the whole of a request and the trigger
 * rotation restarts afterwards, so the phone never records its own read-back.
 * While the loop runs it owns the microphone: startRecording over HTTP or MCP
 * is refused. The app must be in the foreground for the microphone to hear.
 *
 * Nothing heard is kept: a trigger clip is deleted once it has been searched,
 * the clips a request used once it has been answered, and the clip in progress
 * when the loop stops. keep_clips leaves them on disk, for debugging.
 */

#ifndef KANAHA_VOICE_LOOP_H
#define KANAHA_VOICE_LOOP_H

#include <stddef.h>

typedef struct {
    double clip_secs;       /* trigger clip length; 0 = 6 (the trials' setting) */
    double spec_secs;       /* dictation and answer windows; 0 = 8 */
    double cooldown_secs;   /* after a request, ignore triggers; 0 = 4 */
    float min_confidence;   /* trigger floor; 0 = 0.5 */
    const char *model;      /* whisper model; NULL = "tiny.en" */
    int keep_clips;         /* leave clips on disk after use (debugging) */
} kvl_config_t;

/* Start the loop. 0, or -1 with err set (already running, no model...). */
int kanaha_voice_loop_start(const kvl_config_t *cfg, char *err, int err_len);

/* Ask the loop to stop. It finishes the step it is in (a request that is
 * being answered is answered) and releases the microphone. */
void kanaha_voice_loop_stop(void);

/* Whether the loop holds the microphone (starting, running or stopping). */
int kanaha_voice_loop_active(void);

/* {"state":"running","clips":N,"triggers":N,"requests":N,
 *  "last_heard":"...","last_outcome":"RUN","last_error":"..."} */
void kanaha_voice_loop_status(char *json_out, size_t out_size);

#endif /* KANAHA_VOICE_LOOP_H */
