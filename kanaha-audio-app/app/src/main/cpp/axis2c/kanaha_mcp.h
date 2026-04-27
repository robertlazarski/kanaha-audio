/*
 * Kanaha Audio
 * MCP (Model Context Protocol) stdio transport
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Implements JSON-RPC 2.0 over stdin/stdout for audio search operations,
 * following the same MCP pattern as Kanaha Camera's kanaha_mcp.c.
 *
 * Tools exposed:
 *   - searchKeywords   (find keyword timestamps in audio)
 *   - transcribe       (full transcription with word-level timestamps)
 *   - getStatus        (model loaded, device info, memory usage)
 *   - listModels       (available whisper models on device)
 *   - loadModel        (load/switch whisper model)
 *   - listAudioFiles   (list processable audio files)
 *
 * Usage (Claude Desktop claude_desktop_config.json):
 * {
 *   "mcpServers": {
 *     "kanaha-audio": {
 *       "command": "/data/data/org.kanaha.audio/files/kanaha-audio-mcp",
 *       "args": []
 *     }
 *   }
 * }
 *
 * Protocol version: 2024-11-05
 */

#ifndef KANAHA_AUDIO_MCP_H
#define KANAHA_AUDIO_MCP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the MCP JSON-RPC 2.0 stdio loop for audio search.
 *
 * Reads newline-delimited JSON requests from stdin, dispatches to
 * audio_search_service_invoke_json_impl(), and writes JSON-RPC 2.0
 * responses to stdout. Returns when stdin reaches EOF.
 *
 * stdout is reserved for MCP protocol — all logging goes to Android
 * logcat via __android_log_print or to a file.
 */
void kanaha_audio_run_mcp_stdio(void);

#ifdef __cplusplus
}
#endif

#endif /* KANAHA_AUDIO_MCP_H */
