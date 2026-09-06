/*
 * Kanaha Audio
 * LTC (Linear Timecode) Decoder — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Decodes SMPTE LTC from WAV files using libltc (LGPL-compatible).
 * On-device equivalent of ltcdump — returns JSON instead of text.
 *
 * Architecture:
 *   curl → HTTPS/HTTP2+mTLS → Axis2/C → audio_search_service.c
 *     → "decodeLTC" → ltc_decode_wav() → libltc → timecode frames as JSON
 *
 * This enables the workflow:
 *   1. Record LTC from Tentacle Sync via iRig (startRecording, 44.1kHz)
 *   2. Decode timecode on-device (decodeLTC)
 *   3. Use timestamps for video edit sync (parseLTC.sh / parseWithoutLTC.sh)
 */

#include "ltc_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ltc.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioLTC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define LOGD(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)
#endif

/* Maximum frames we'll decode (36 min at 30fps) */
#define MAX_LTC_FRAMES  65536

/* Read buffer size: 1024 samples at a time */
#define READ_BUF_SAMPLES  1024

/**
 * Read WAV header and validate format.
 * Returns sample rate, channels, data offset, data size.
 */
static int read_wav_header(FILE *f, int *sample_rate, int *channels,
                           int *bits_per_sample, long *data_offset,
                           long *data_size) {
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44) return -1;

    /* Verify RIFF/WAVE */
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return -1;

    /* Find "fmt " and "data" chunks — handle non-standard layouts */
    fseek(f, 12, SEEK_SET);
    int found_fmt = 0, found_data = 0;
    int sr = 0, ch = 0, bps = 0;
    long d_offset = 0, d_size = 0;

    while (!found_data) {
        unsigned char chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;

        uint32_t chunk_size = chunk_hdr[4] | (chunk_hdr[5] << 8) |
                              (chunk_hdr[6] << 16) | (chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (chunk_size < 16 || fread(fmt, 1, 16, f) != 16) break;

            uint16_t audio_format = fmt[0] | (fmt[1] << 8);
            if (audio_format != 1) {
                LOGE("Not PCM format (format=%d)", audio_format);
                return -1;
            }
            ch = fmt[2] | (fmt[3] << 8);
            sr = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bps = fmt[14] | (fmt[15] << 8);

            /* Skip remaining fmt chunk data */
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            found_fmt = 1;
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            d_offset = ftell(f);
            d_size = (long)chunk_size;
            found_data = 1;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) return -1;

    *sample_rate = sr;
    *channels = ch;
    *bits_per_sample = bps;
    *data_offset = d_offset;
    *data_size = d_size;
    return 0;
}

int ltc_decode_wav(const char *wav_path, int channel, ltc_decode_result_t *result) {
    memset(result, 0, sizeof(*result));

    if (!wav_path || strstr(wav_path, "..")) {
        snprintf(result->error, sizeof(result->error), "Invalid path specified");
        return -1;
    }

    if (channel < 1) channel = 1;

    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        snprintf(result->error, sizeof(result->error),
                 "Cannot open file: %s", wav_path);
        return -1;
    }

    int sample_rate, channels, bits_per_sample;
    long data_offset, data_size;

    if (read_wav_header(f, &sample_rate, &channels, &bits_per_sample,
                        &data_offset, &data_size) != 0) {
        snprintf(result->error, sizeof(result->error),
                 "Invalid WAV format: %s", wav_path);
        fclose(f);
        return -1;
    }

    if (bits_per_sample != 16) {
        snprintf(result->error, sizeof(result->error),
                 "Unsupported bit depth: %d (need 16)", bits_per_sample);
        fclose(f);
        return -1;
    }

    if (channel > channels) {
        snprintf(result->error, sizeof(result->error),
                 "Channel %d requested but file has %d channel(s)",
                 channel, channels);
        fclose(f);
        return -1;
    }

    /* read_buf below is sized for at most two channels. The channel count
     * comes straight from the file's fmt chunk, so a WAV declaring more
     * channels than that would make fread() write past the end of a stack
     * buffer -- with file contents. Reject anything the buffer cannot hold,
     * and a non-positive sample rate, which the fps estimate divides by. */
    if (channels < 1 || channels > 2) {
        snprintf(result->error, sizeof(result->error),
                 "Unsupported channel count: %d (need 1 or 2)", channels);
        fclose(f);
        return -1;
    }
    if (sample_rate <= 0) {
        snprintf(result->error, sizeof(result->error),
                 "Invalid sample rate: %d", sample_rate);
        fclose(f);
        return -1;
    }

    LOGI("Decoding LTC: %s (%d Hz, %d ch, channel %d)",
         wav_path, sample_rate, channels, channel);

    result->sample_rate = sample_rate;

    /* Allocate frames array */
    result->frames = (ltc_frame_info_t *)calloc(MAX_LTC_FRAMES,
                                                 sizeof(ltc_frame_info_t));
    if (!result->frames) {
        snprintf(result->error, sizeof(result->error), "Memory allocation failed");
        fclose(f);
        return -1;
    }

    /* Create LTC decoder
     * apv = audio frames per video frame ≈ sample_rate / fps
     * Use 0 for auto-detect, queue size 32 */
    LTCDecoder *decoder = ltc_decoder_create(sample_rate / 25, 32);
    if (!decoder) {
        snprintf(result->error, sizeof(result->error), "Failed to create LTC decoder");
        free(result->frames);
        result->frames = NULL;
        fclose(f);
        return -1;
    }

    /* Read and decode */
    fseek(f, data_offset, SEEK_SET);
    int bytes_per_sample = bits_per_sample / 8;
    int frame_size = bytes_per_sample * channels;
    long total_samples = data_size / frame_size;
    result->duration_samples = total_samples;

    int16_t read_buf[READ_BUF_SAMPLES * 2]; /* max 2 channels */
    ltcsnd_sample_t sound_buf[READ_BUF_SAMPLES];
    int64_t samples_read = 0;
    int prev_frame = -1;
    int prev_second = -1;

    while (samples_read < total_samples && result->total_frames < MAX_LTC_FRAMES) {
        int to_read = READ_BUF_SAMPLES;
        if (samples_read + to_read > total_samples)
            to_read = (int)(total_samples - samples_read);

        size_t nread = fread(read_buf, frame_size, to_read, f);
        if (nread == 0) break;

        /* Extract target channel and convert int16 → ltcsnd_sample_t (unsigned 8-bit) */
        for (size_t i = 0; i < nread; i++) {
            int16_t sample = read_buf[i * channels + (channel - 1)];
            /* Convert signed 16-bit to unsigned 8-bit centered at 128 */
            sound_buf[i] = (ltcsnd_sample_t)((sample >> 8) + 128);
        }

        ltc_decoder_write(decoder, sound_buf, nread, samples_read);

        /* Read decoded frames */
        LTCFrameExt frame;
        while (ltc_decoder_read(decoder, &frame) &&
               result->total_frames < MAX_LTC_FRAMES) {
            SMPTETimecode tc;
            ltc_frame_to_time(&tc, &frame.ltc, 0);

            int idx = result->total_frames;
            result->frames[idx].hours = tc.hours;
            result->frames[idx].minutes = tc.mins;
            result->frames[idx].seconds = tc.secs;
            result->frames[idx].frames = tc.frame;
            result->frames[idx].sample_start = frame.off_start;
            result->frames[idx].sample_end = frame.off_end;

            /* Detect discontinuity */
            int cur_frame_num = tc.frame;
            int cur_second = tc.hours * 3600 + tc.mins * 60 + tc.secs;
            if (prev_frame >= 0) {
                if (cur_second == prev_second) {
                    /* Same second: frame must increment by 1 */
                    if (cur_frame_num != prev_frame + 1) {
                        result->frames[idx].discontinuity = 1;
                    }
                } else if (cur_second == prev_second + 1) {
                    /* Next second: frame must reset to 0 */
                    if (cur_frame_num != 0) {
                        result->frames[idx].discontinuity = 1;
                    }
                } else {
                    /* Gap of more than 1 second */
                    result->frames[idx].discontinuity = 1;
                }
            }
            prev_frame = cur_frame_num;
            prev_second = cur_second;

            /* Track first/last timecode */
            if (result->total_frames == 0) {
                snprintf(result->first_tc, sizeof(result->first_tc),
                         "%02d:%02d:%02d:%02d",
                         tc.hours, tc.mins, tc.secs, tc.frame);
            }
            snprintf(result->last_tc, sizeof(result->last_tc),
                     "%02d:%02d:%02d:%02d",
                     tc.hours, tc.mins, tc.secs, tc.frame);

            result->total_frames++;
        }

        samples_read += nread;
    }

    /* Estimate FPS from frame count and time span */
    if (result->total_frames > 1) {
        int64_t span_samples = result->frames[result->total_frames - 1].sample_end -
                               result->frames[0].sample_start;
        if (span_samples > 0) {
            result->fps = (float)(result->total_frames - 1) *
                          sample_rate / (float)span_samples;
            /* Round to nearest standard fps */
            if (result->fps > 29.0f && result->fps < 31.0f)
                result->fps = 30.0f;
            else if (result->fps > 23.5f && result->fps < 25.5f)
                result->fps = 25.0f;
            else if (result->fps > 23.0f && result->fps < 24.5f)
                result->fps = 24.0f;
        }
    }

    ltc_decoder_free(decoder);
    fclose(f);

    result->success = 1;
    LOGI("LTC decode complete: %d frames, %s → %s, %.0f fps",
         result->total_frames, result->first_tc, result->last_tc, result->fps);
    return 0;
}

void ltc_decode_result_free(ltc_decode_result_t *result) {
    if (result && result->frames) {
        free(result->frames);
        result->frames = NULL;
    }
}
