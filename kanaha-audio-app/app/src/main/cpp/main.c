/*
 * Kanaha Audio - Native Executable Entry Point
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * This is the main() entry point for the standalone HTTP server executable.
 * It is launched via ProcessBuilder from ApacheService.java.
 *
 * Usage: ./kanaha-audio-httpd [-p port] [-d repo_path] [-m models_dir]
 *
 * ARCHITECTURE: No JNI - Runs as separate native process
 * Unlike Kanaha Camera, audio operations are direct function calls
 * (whisper.cpp is C/C++, same process — no Intent IPC needed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioMain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#endif

/* External function from apache_httpd_android.c */
extern int apache_httpd_main(int argc, char *argv[]);
extern void apache_httpd_signal_shutdown(void);

/* External: audio service init (from audio_search_service.c) */
extern int audio_search_service_init(const char *models_dir);
extern void audio_search_service_cleanup(void);

/* Signal handler for graceful shutdown */
static void signal_handler(int signum) {
    LOGI("Received signal %d, shutting down...", signum);
    apache_httpd_signal_shutdown();
}

/**
 * Main entry point for the Kanaha Audio HTTP server executable.
 *
 * This executable is launched via ProcessBuilder from ApacheService.java
 * and runs in its own process, separate from the Android Java layer.
 *
 * Unlike Kanaha Camera, all operations (whisper.cpp inference) happen
 * in-process — no Intent IPC to a Java layer.
 */
int main(int argc, char *argv[]) {
    LOGI("=== Kanaha Audio Server Starting ===");

#ifdef __ANDROID__
    LOGI("Process ID: %d", getpid());
#endif

    /* Set up signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Parse -m flag for models directory */
    const char *models_dir = "/data/data/org.kanaha.audio/files/models";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            models_dir = argv[i + 1];
        }
    }

    /* Initialize audio search service (loads whisper bridge) */
    if (audio_search_service_init(models_dir) != 0) {
        LOGE("Failed to initialize audio search service");
        return 1;
    }

    /* Start the HTTP server */
    int result = apache_httpd_main(argc, argv);

    /* Cleanup */
    audio_search_service_cleanup();

    LOGI("=== Kanaha Audio Server Exited (code: %d) ===", result);
    return result;
}
