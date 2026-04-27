/*
 * Kanaha Audio
 * SFTP File Transfer — Header
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Transfers WAV recordings from the phone to a storage server via SFTP.
 * Uses libssh2 (BSD license) — pure C, no Java, no Intent IPC.
 *
 * Functionally equivalent to Kanaha Camera's SFTP support but independently
 * implemented. Camera uses JSch (Java) via Intent IPC; Audio uses libssh2 (C)
 * via direct function call — simpler architecture, no IPC latency.
 *
 * Security model matches Camera:
 *   - ed25519 SSH key authentication (no passwords)
 *   - known_hosts server fingerprint verification
 *   - Per-server key files from servers.json configuration
 *
 * LIMITS:
 *   - Maximum file size: limited by available memory for transfer buffer
 *   - Transfer timeout: 20 minutes (matches Camera's timeout)
 *   - Only .wav files from the audio directory are transferable
 *   - server_id must match an entry in servers.json
 *   - SSH private key must exist at <ssh_dir>/keys/<server_id>.key
 */

#ifndef AUDIO_SFTP_H
#define AUDIO_SFTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transfer result */
typedef struct {
    int success;
    int files_transferred;
    int64_t bytes_transferred;
    int64_t timestamp;
    char operation_id[64];
    char error[256];
    char message[256];
} audio_sftp_result_t;

/**
 * Initialize the SFTP subsystem.
 * Must be called once at startup.
 *
 * @param ssh_dir  Directory containing servers.json and keys/ subdirectory
 * @return 0 on success, -1 on error
 */
int audio_sftp_init(const char *ssh_dir);

/**
 * Cleanup the SFTP subsystem.
 */
void audio_sftp_cleanup(void);

/**
 * Transfer audio file(s) to a storage server via SFTP.
 *
 * @param server_id          Server ID from servers.json (e.g., "control1")
 * @param audio_filename     Filename or "*" for all WAV files
 * @param destination_folder Remote directory path
 * @param audio_dir          Local directory containing WAV files
 * @param result             Output: transfer result
 * @return 0 on success, -1 on error
 */
int audio_sftp_transfer(const char *server_id,
                        const char *audio_filename,
                        const char *destination_folder,
                        const char *audio_dir,
                        audio_sftp_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SFTP_H */
