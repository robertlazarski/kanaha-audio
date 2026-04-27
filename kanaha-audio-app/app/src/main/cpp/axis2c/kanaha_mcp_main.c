/*
 * Kanaha Audio
 * MCP stdio binary entry point
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Standalone binary that Claude Desktop launches as a subprocess.
 * Reads MCP JSON-RPC 2.0 requests from stdin, dispatches to audio
 * search operations, writes responses to stdout.
 *
 * Build: linked with audio_search_service.c + kanaha_mcp.c + whisper bridge + json-c
 * Install: /data/data/org.kanaha.audio/files/kanaha-audio-mcp
 */

#include "kanaha_mcp.h"

/* External: audio service init (from audio_search_service.c) */
extern int audio_search_service_init(const char *models_dir);
extern void audio_search_service_cleanup(void);

int main(void)
{
    /* Initialize with default models directory */
    audio_search_service_init("/data/data/org.kanaha.audio/files/models");

    kanaha_audio_run_mcp_stdio();

    audio_search_service_cleanup();
    return 0;
}
