/*
 * Kanaha Audio
 * SFTP File Transfer — Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Pure C SFTP using libssh2 (BSD license). No Java, no Intent IPC.
 * Transfers WAV recordings from the phone to a storage server.
 *
 * Architecture:
 *   curl → HTTPS/HTTP2+mTLS → Axis2/C → audio_search_service.c
 *     → "sftpTransfer" → audio_sftp_transfer() → libssh2 → SFTP → server
 *
 * Unlike Kanaha Camera (which uses JSch via Intent IPC to Java),
 * this runs entirely in the native httpd process.
 */

#include "audio_sftp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioSFTP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define LOGD(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)
#endif

/* Transfer buffer size: 64 KB */
#define SFTP_BUFFER_SIZE  (64 * 1024)

/* SSH directory containing servers.json and keys/ */
static char s_ssh_dir[512] = {0};
static int s_initialized = 0;

/* Simple JSON string extraction (same pattern as audio_search_service.c) */
static const char *sftp_extract_json_string(const char *json, const char *key,
                                            char *buffer, size_t buffer_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start) return NULL;

    start += strlen(search_key);
    while (*start == ' ' || *start == '\t') start++;
    if (*start != '"') return NULL;
    start++;

    const char *end = start;
    while (*end && *end != '"') end++;
    if (*end != '"' || (size_t)(end - start) >= buffer_size) return NULL;

    strncpy(buffer, start, end - start);
    buffer[end - start] = '\0';
    return buffer;
}

static int sftp_extract_json_int(const char *json, const char *key, int default_val) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start) return default_val;

    start += strlen(search_key);
    while (*start == ' ' || *start == '\t') start++;

    char *endp = NULL;
    long val = strtol(start, &endp, 10);
    if (endp == start) return default_val;
    return (int)val;
}

/* Generate operation ID */
static void generate_operation_id(char *buf, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, size, "op_%ld_%d",
             (long)ts.tv_sec, (int)(ts.tv_nsec % 1000000000));
}

/**
 * Load server configuration from servers.json.
 * Returns 0 on success, fills host/port/username.
 */
static int load_server_config(const char *server_id,
                              char *host, size_t host_size,
                              int *port,
                              char *username, size_t username_size) {
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/servers.json", s_ssh_dir);

    FILE *f = fopen(config_path, "r");
    if (!f) {
        LOGE("Cannot open servers.json: %s", config_path);
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 65536) {
        fclose(f);
        return -1;
    }

    char *json = (char *)malloc(fsize + 1);
    if (!json) { fclose(f); return -1; }
    size_t nread = fread(json, 1, fsize, f);
    fclose(f);
    json[nread] = '\0';

    /* Find the server_id block — look for "server_id": { ... } */
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", server_id);
    char *block = strstr(json, search);
    if (!block) {
        LOGE("Server '%s' not found in servers.json", server_id);
        free(json);
        return -1;
    }

    /* Find the opening brace of this server's config */
    char *brace = strchr(block + strlen(search), '{');
    if (!brace) { free(json); return -1; }

    /* Find closing brace */
    char *end_brace = strchr(brace, '}');
    if (!end_brace) { free(json); return -1; }

    /* Extract from this block */
    size_t block_len = end_brace - brace + 1;
    char *server_json = (char *)malloc(block_len + 1);
    if (!server_json) { free(json); return -1; }
    memcpy(server_json, brace, block_len);
    server_json[block_len] = '\0';

    sftp_extract_json_string(server_json, "host", host, host_size);
    sftp_extract_json_string(server_json, "username", username, username_size);
    *port = sftp_extract_json_int(server_json, "port", 22);

    free(server_json);
    free(json);

    if (host[0] == '\0' || username[0] == '\0') {
        LOGE("Server '%s' missing host or username", server_id);
        return -1;
    }

    LOGI("Server config: %s@%s:%d", username, host, *port);
    return 0;
}

/**
 * Connect to SSH server and authenticate with ed25519 key.
 */
static int ssh_connect(const char *host, int port, const char *username,
                       const char *server_id,
                       int *out_sock, LIBSSH2_SESSION **out_session) {
    /* Resolve hostname */
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        LOGE("DNS resolution failed for %s: %s", host, gai_strerror(rc));
        return -1;
    }

    /* Create socket and connect */
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        LOGE("Socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        LOGE("SSH connection failed to %s:%d: %s", host, port, strerror(errno));
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    LOGI("TCP connected to %s:%d", host, port);

    /* Create SSH session */
    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        LOGE("Failed to create SSH session");
        close(sock);
        return -1;
    }

    /* Set timeout (20 minutes in ms) */
    libssh2_session_set_timeout(session, 20 * 60 * 1000);

    /* SSH handshake */
    rc = libssh2_session_handshake(session, sock);
    if (rc) {
        LOGE("SSH handshake failed: %d", rc);
        libssh2_session_free(session);
        close(sock);
        return -1;
    }

    LOGI("SSH handshake complete");

    /* Verify known_hosts (if file exists) */
    char known_hosts_path[1024];
    snprintf(known_hosts_path, sizeof(known_hosts_path), "%s/known_hosts", s_ssh_dir);

    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
    if (nh) {
        libssh2_knownhost_readfile(nh, known_hosts_path,
                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);

        size_t fingerprint_len;
        int type;
        const char *fingerprint = libssh2_session_hostkey(session,
                                                          &fingerprint_len, &type);
        if (fingerprint) {
            int check = libssh2_knownhost_checkp(nh, host, port,
                                                 fingerprint, fingerprint_len,
                                                 LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                                 LIBSSH2_KNOWNHOST_KEYENC_RAW,
                                                 NULL);
            if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
                LOGI("known_hosts: server verified");
            } else if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
                LOGD("known_hosts: server not in file (proceeding)");
            } else {
                LOGE("known_hosts: server key MISMATCH (check=%d)", check);
                libssh2_knownhost_free(nh);
                libssh2_session_disconnect(session, "Host key mismatch");
                libssh2_session_free(session);
                close(sock);
                return -1;
            }
        }
        libssh2_knownhost_free(nh);
    }

    /* Authenticate with ed25519 private key */
    char key_path[1024];
    snprintf(key_path, sizeof(key_path), "%s/keys/%s.key", s_ssh_dir, server_id);

    struct stat st;
    if (stat(key_path, &st) != 0) {
        LOGE("SSH private key not found: %s", key_path);
        LOGE("Generate key: ssh-keygen -t ed25519 -f %s.key -N \"\"", server_id);
        libssh2_session_disconnect(session, "Key not found");
        libssh2_session_free(session);
        close(sock);
        return -1;
    }

    rc = libssh2_userauth_publickey_fromfile(session, username,
                                              NULL, /* public key (auto-derived) */
                                              key_path,
                                              NULL  /* passphrase (none) */);
    if (rc) {
        char *err_msg = NULL;
        libssh2_session_last_error(session, &err_msg, NULL, 0);
        LOGE("SSH auth failed for %s@%s: %s", username, host,
             err_msg ? err_msg : "unknown");
        libssh2_session_disconnect(session, "Auth failed");
        libssh2_session_free(session);
        close(sock);
        return -1;
    }

    LOGI("SSH authenticated as %s", username);

    *out_sock = sock;
    *out_session = session;
    return 0;
}

/**
 * Transfer a single file via SFTP.
 */
static int sftp_upload_file(LIBSSH2_SESSION *session,
                            const char *local_path,
                            const char *remote_path,
                            int64_t *bytes_out) {
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
    if (!sftp) {
        LOGE("Failed to init SFTP session");
        return -1;
    }

    /* Open local file */
    FILE *local = fopen(local_path, "rb");
    if (!local) {
        LOGE("Cannot open local file: %s", local_path);
        libssh2_sftp_shutdown(sftp);
        return -1;
    }

    /* Get local file size */
    fseek(local, 0, SEEK_END);
    long file_size = ftell(local);
    fseek(local, 0, SEEK_SET);

    /* Open remote file for writing */
    LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);

    if (!remote) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        LOGE("Cannot open remote file %s: SFTP error %lu", remote_path, sftp_err);
        fclose(local);
        libssh2_sftp_shutdown(sftp);
        return -1;
    }

    LOGI("Uploading %s (%ld bytes) -> %s", local_path, file_size, remote_path);

    /* Transfer data */
    char buffer[SFTP_BUFFER_SIZE];
    int64_t total = 0;
    size_t nread;

    while ((nread = fread(buffer, 1, sizeof(buffer), local)) > 0) {
        char *ptr = buffer;
        size_t remaining = nread;

        while (remaining > 0) {
            ssize_t nwritten = libssh2_sftp_write(remote, ptr, remaining);
            if (nwritten < 0) {
                LOGE("SFTP write error: %ld", (long)nwritten);
                libssh2_sftp_close(remote);
                fclose(local);
                libssh2_sftp_shutdown(sftp);
                return -1;
            }
            ptr += nwritten;
            remaining -= nwritten;
            total += nwritten;
        }
    }

    libssh2_sftp_close(remote);
    fclose(local);
    libssh2_sftp_shutdown(sftp);

    *bytes_out = total;
    LOGI("Upload complete: %lld bytes", (long long)total);
    return 0;
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

int audio_sftp_init(const char *ssh_dir) {
    if (!ssh_dir) return -1;

    int rc = libssh2_init(0);
    if (rc) {
        LOGE("libssh2_init failed: %d", rc);
        return -1;
    }

    strncpy(s_ssh_dir, ssh_dir, sizeof(s_ssh_dir) - 1);
    s_initialized = 1;
    LOGI("SFTP subsystem initialized (ssh_dir=%s)", s_ssh_dir);
    return 0;
}

void audio_sftp_cleanup(void) {
    if (s_initialized) {
        libssh2_exit();
        s_initialized = 0;
    }
}

int audio_sftp_transfer(const char *server_id,
                        const char *audio_filename,
                        const char *destination_folder,
                        const char *audio_dir,
                        audio_sftp_result_t *result) {
    memset(result, 0, sizeof(*result));
    generate_operation_id(result->operation_id, sizeof(result->operation_id));

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    result->timestamp = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (!s_initialized) {
        snprintf(result->error, sizeof(result->error), "SFTP not initialized");
        return -1;
    }

    /* Validate user-controlled path components against traversal */
    if (strstr(server_id, "/") || strstr(server_id, "..")) {
        snprintf(result->error, sizeof(result->error), "Invalid server_id: path traversal not allowed");
        return -1;
    }
    if (strcmp(audio_filename, "*") != 0 &&
        (strstr(audio_filename, "/") || strstr(audio_filename, ".."))) {
        snprintf(result->error, sizeof(result->error), "Invalid audio_filename: path traversal not allowed");
        return -1;
    }
    if (strstr(destination_folder, "..")) {
        snprintf(result->error, sizeof(result->error), "Invalid destination_folder: path traversal not allowed");
        return -1;
    }

    /* Load server configuration */
    char host[256] = {0};
    char username[128] = {0};
    int port = 22;

    if (load_server_config(server_id, host, sizeof(host),
                           &port, username, sizeof(username)) != 0) {
        snprintf(result->error, sizeof(result->error),
                 "Storage server not configured: %s", server_id);
        return -1;
    }

    /* Connect and authenticate */
    int sock = -1;
    LIBSSH2_SESSION *session = NULL;

    if (ssh_connect(host, port, username, server_id, &sock, &session) != 0) {
        snprintf(result->error, sizeof(result->error),
                 "SSH connection failed to %s@%s:%d", username, host, port);
        return -1;
    }

    /* Build list of files to transfer */
    int files_transferred = 0;
    int64_t total_bytes = 0;

    if (strcmp(audio_filename, "*") == 0) {
        /* Transfer all .wav files */
        DIR *dir = opendir(audio_dir);
        if (!dir) {
            snprintf(result->error, sizeof(result->error),
                     "Cannot open audio directory: %s", audio_dir);
            libssh2_session_disconnect(session, "Done");
            libssh2_session_free(session);
            close(sock);
            return -1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t namelen = strlen(entry->d_name);
            if (namelen < 4 ||
                strcmp(entry->d_name + namelen - 4, ".wav") != 0) {
                continue;
            }

            char local_path[1024];
            snprintf(local_path, sizeof(local_path), "%s/%s",
                     audio_dir, entry->d_name);

            char remote_path[1024];
            snprintf(remote_path, sizeof(remote_path), "%s/%s",
                     destination_folder, entry->d_name);

            int64_t bytes = 0;
            if (sftp_upload_file(session, local_path, remote_path, &bytes) == 0) {
                files_transferred++;
                total_bytes += bytes;
            } else {
                LOGE("Failed to transfer %s", entry->d_name);
            }
        }
        closedir(dir);
    } else {
        /* Transfer specific file */
        char local_path[1024];
        snprintf(local_path, sizeof(local_path), "%s/%s",
                 audio_dir, audio_filename);

        /* Validate file exists */
        struct stat st;
        if (stat(local_path, &st) != 0) {
            snprintf(result->error, sizeof(result->error),
                     "No files found matching: %s", audio_filename);
            libssh2_session_disconnect(session, "Done");
            libssh2_session_free(session);
            close(sock);
            return -1;
        }

        char remote_path[1024];
        snprintf(remote_path, sizeof(remote_path), "%s/%s",
                 destination_folder, audio_filename);

        int64_t bytes = 0;
        if (sftp_upload_file(session, local_path, remote_path, &bytes) == 0) {
            files_transferred++;
            total_bytes += bytes;
        } else {
            snprintf(result->error, sizeof(result->error),
                     "SFTP upload failed for %s", audio_filename);
            libssh2_session_disconnect(session, "Done");
            libssh2_session_free(session);
            close(sock);
            return -1;
        }
    }

    /* Disconnect */
    libssh2_session_disconnect(session, "Transfer complete");
    libssh2_session_free(session);
    close(sock);

    /* Fill result */
    result->success = 1;
    result->files_transferred = files_transferred;
    result->bytes_transferred = total_bytes;
    snprintf(result->message, sizeof(result->message),
             "Transferred %d file(s), %lld bytes",
             files_transferred, (long long)total_bytes);

    LOGI("%s", result->message);
    return 0;
}
