/*
 * Kanaha Audio — speech output (the `speak` operation)
 *
 * Says a line of text out of this phone's speaker, synthesised in-process by
 * flite (BSD, CMU). See audio_speak.c for why this is flite rather than
 * Android's Java engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_SPEAK_H
#define AUDIO_SPEAK_H

/* Longest utterance accepted. A read-back is a sentence; anything longer is
 * either a mistake or a way to occupy the speaker for minutes. */
#define AUDIO_SPEAK_MAX_TEXT 500

/* audio_speak_text: another call is speaking; nothing was said. */
#define AUDIO_SPEAK_BUSY (-2)

typedef struct {
    int duration_ms;    /* length of the synthesised audio */
    int sample_rate;    /* the voice's rate, 16000 for cmu_us_kal16 */
} audio_speak_result_t;

/**
 * Synthesise `text` and play it on this device. Blocks until the utterance
 * finishes, so a caller that must not block should run it on its own thread.
 * Returns AUDIO_SPEAK_BUSY, without waiting, if another call is speaking.
 *
 * @param text       what to say, at most AUDIO_SPEAK_MAX_TEXT characters
 * @param audio_dir  the app's audio directory; a temporary WAV is written and
 *                   deleted there
 * @param result     filled in on success
 * @return 0 on success, -1 on failure
 */
int audio_speak_text(const char *text, const char *audio_dir,
                     audio_speak_result_t *result);

#endif /* AUDIO_SPEAK_H */
