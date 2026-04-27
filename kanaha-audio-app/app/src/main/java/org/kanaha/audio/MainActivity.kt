/*
 * Kanaha Audio
 * Main Activity — Minimal UI + Runtime Permissions
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Minimal Activity that displays service status and provides
 * controls for starting/stopping the HTTP server. The actual
 * work (whisper.cpp inference, recording, tone) happens in the native layer.
 *
 * Requests runtime permissions for:
 * - RECORD_AUDIO (microphone recording via AAudio)
 * - ACCESS_FINE_LOCATION (GPS for sidecar metadata)
 */

package org.kanaha.audio

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.TextView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : Activity() {

    companion object {
        private const val PERMISSIONS_REQUEST_CODE = 100

        /**
         * Runtime permissions needed for the recording + GPS subsystem:
         *   RECORD_AUDIO        — AAudio microphone capture (audio_recording.c)
         *   ACCESS_FINE_LOCATION — GPS for sidecar metadata (LocationService.kt)
         *   ACCESS_COARSE_LOCATION — Required alongside FINE on API 31+
         *
         * All are "dangerous" permissions that require runtime prompts.
         * The service starts regardless of grant results — denied permissions
         * cause graceful degradation (recording fails, GPS is omitted).
         */
        private val REQUIRED_PERMISSIONS = arrayOf(
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val textView = TextView(this).apply {
            text = "Kanaha Audio\n\n" +
                   "Service: Starting...\n" +
                   "Model: Not loaded\n\n" +
                   "The HTTP server runs in the background.\n" +
                   "Use curl or MCP to interact with the service."
            textSize = 16f
            setPadding(32, 32, 32, 32)
        }
        setContentView(textView)

        // Request permissions, then start server
        val missing = REQUIRED_PERMISSIONS.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            ApacheService.start(this)
        } else {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), PERMISSIONS_REQUEST_CODE)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSIONS_REQUEST_CODE) {
            // Start server regardless — recording/GPS will gracefully degrade if denied
            ApacheService.start(this)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        ApacheService.stop()
    }
}
