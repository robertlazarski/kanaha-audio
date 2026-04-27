/*
 * Kanaha Audio
 * GPS Location Service — Writes location to JSON file for native C reader
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Thin service that requests GPS and network location updates and writes
 * the latest fix to a JSON file. The native C code (gps_reader.c) reads
 * this file to include GPS data in recording sidecar metadata.
 *
 * This is the one piece that requires Kotlin — LocationManager needs the
 * Android framework. Everything else (recording, tone, sidecar) is pure C.
 */

package org.kanaha.audio

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.util.Log
import androidx.core.content.ContextCompat
import java.io.File

object LocationService {

    private const val TAG = "KanahaLocationService"
    private const val GPS_JSON_FILENAME = "gps_location.json"
    private const val MIN_TIME_MS = 5000L    // 5 second minimum between updates
    private const val MIN_DISTANCE_M = 0f    // No minimum distance (we want time updates)

    private var locationManager: LocationManager? = null
    private var outputFile: File? = null

    /**
     * Location listener that writes every fix to disk.
     *
     * onStatusChanged is deprecated since API 29 but must be overridden
     * for compatibility with older devices. The other callbacks are no-ops
     * because we don't need to react to provider enable/disable — we just
     * write whatever fixes arrive.
     */
    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            writeLocation(location)
        }

        @Deprecated("Deprecated in API 29")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
        override fun onProviderEnabled(provider: String) {}
        override fun onProviderDisabled(provider: String) {}
    }

    /**
     * Start listening for location updates from GPS and network providers.
     * Writes updates to <filesDir>/gps_location.json.
     */
    @Synchronized
    fun start(context: Context) {
        if (locationManager != null) {
            Log.w(TAG, "Location service already running")
            return
        }

        // Check permissions
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "ACCESS_FINE_LOCATION permission not granted, skipping location service")
            return
        }

        outputFile = File(context.filesDir, GPS_JSON_FILENAME)
        locationManager = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager

        try {
            // Request from GPS provider (high accuracy, outdoors)
            if (locationManager!!.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                locationManager!!.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER, MIN_TIME_MS, MIN_DISTANCE_M, locationListener
                )
                Log.i(TAG, "Registered GPS_PROVIDER listener")
            }

            // Request from network provider (fallback, indoors)
            if (locationManager!!.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                locationManager!!.requestLocationUpdates(
                    LocationManager.NETWORK_PROVIDER, MIN_TIME_MS, MIN_DISTANCE_M, locationListener
                )
                Log.i(TAG, "Registered NETWORK_PROVIDER listener")
            }

            // Write last known location immediately if available
            val lastKnown = locationManager!!.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: locationManager!!.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            if (lastKnown != null) {
                writeLocation(lastKnown)
                Log.i(TAG, "Wrote last known location")
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "Location permission denied", e)
        }
    }

    /**
     * Stop listening for location updates.
     */
    @Synchronized
    fun stop() {
        locationManager?.removeUpdates(locationListener)
        locationManager = null
        Log.i(TAG, "Location service stopped")
    }

    /**
     * Write location to JSON file (atomic: write .tmp then rename).
     *
     * The native C code (gps_reader.c) reads this file, so the format
     * must match exactly: latitude, longitude, accuracy, time, provider.
     * The "time" field is Location.getTime() — Unix epoch milliseconds
     * from the GPS satellites (not the phone clock).
     */
    private fun writeLocation(location: Location) {
        val file = outputFile ?: return
        val tmpFile = File(file.parent, "${file.name}.tmp")

        try {
            val json = """
                |{
                |  "latitude": ${location.latitude},
                |  "longitude": ${location.longitude},
                |  "accuracy": ${location.accuracy},
                |  "time": ${location.time},
                |  "provider": "${location.provider}"
                |}
            """.trimMargin()

            tmpFile.writeText(json)
            tmpFile.renameTo(file)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to write GPS JSON", e)
        }
    }
}
