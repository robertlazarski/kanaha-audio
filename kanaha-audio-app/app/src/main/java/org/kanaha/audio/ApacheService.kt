/*
 * Kanaha Audio
 * Apache Service Launcher — Starts native HTTP server via ProcessBuilder
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Follows the same pattern as Kanaha Camera's ApacheService:
 * - Extracts native executable from APK lib directory
 * - Launches via ProcessBuilder in a separate process
 * - Passes SSL certificate paths and port as command-line arguments
 *
 * Unlike Kanaha Camera, there is no Intent IPC to manage —
 * all audio processing happens inside the native process via
 * direct whisper.cpp function calls.
 */

package org.kanaha.audio

import android.content.Context
import android.util.Log
import java.io.File

object ApacheService {

    private const val TAG = "KanahaAudioService"
    private var serverProcess: Process? = null

    /**
     * Start the native HTTP server process.
     *
     * The executable (libkanaha_audio_httpd.so) is packaged in the APK's
     * lib/arm64-v8a/ directory and launched via ProcessBuilder.
     */
    @Synchronized
    fun start(context: Context, port: Int = 8443) {
        if (serverProcess != null) {
            Log.w(TAG, "Server already running")
            return
        }

        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        val executable = File(nativeLibDir, "libkanaha_audio_httpd.so")

        if (!executable.exists()) {
            Log.e(TAG, "Native executable not found: ${executable.absolutePath}")
            return
        }

        val sslDir = File(context.filesDir, "ssl")
        val modelsDir = File(context.filesDir, "models")

        // Ensure models directory exists
        modelsDir.mkdirs()

        val args = mutableListOf(
            executable.absolutePath,
            "-p", port.toString(),
            "-d", context.filesDir.absolutePath,
            "-m", modelsDir.absolutePath,
        )

        // Add SSL args if certificates exist
        val certFile = File(sslDir, "server.crt")
        val keyFile = File(sslDir, "server.key")
        val caFile = File(sslDir, "ca.crt")

        if (certFile.exists() && keyFile.exists()) {
            args.addAll(listOf("-c", certFile.absolutePath))
            args.addAll(listOf("-k", keyFile.absolutePath))
            if (caFile.exists()) {
                args.addAll(listOf("-a", caFile.absolutePath))
            }
        }

        // Redact sensitive certificate paths before logging
        val redactedArgs = args.toMutableList()
        val sensitiveFlags = setOf("-c", "-k", "-a")
        var i = 0
        while (i < redactedArgs.size) {
            if (redactedArgs[i] in sensitiveFlags && i + 1 < redactedArgs.size) {
                redactedArgs[i + 1] = "[redacted]"
                i += 2
            } else {
                i++
            }
        }
        Log.i(TAG, "Starting server: ${redactedArgs.joinToString(" ")}")

        try {
            val processBuilder = ProcessBuilder(args)
            processBuilder.redirectErrorStream(true)

            serverProcess = processBuilder.start()
            Log.i(TAG, "Server process started")

            // Start GPS location service (writes JSON for native gps_reader.c)
            LocationService.start(context)

            // Log server output in background thread
            val process = serverProcess
            Thread {
                try {
                    process?.inputStream?.bufferedReader()?.forEachLine { line ->
                        Log.i(TAG, "Server: $line")
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error reading server output", e)
                }
            }.start()

        } catch (e: Exception) {
            Log.e(TAG, "Failed to start server", e)
        }
    }

    /**
     * Stop the native HTTP server process.
     */
    @Synchronized
    fun stop() {
        LocationService.stop()
        serverProcess?.let { process ->
            Log.i(TAG, "Stopping server...")
            process.destroy()
            serverProcess = null
            Log.i(TAG, "Server stopped")
        }
    }
}
