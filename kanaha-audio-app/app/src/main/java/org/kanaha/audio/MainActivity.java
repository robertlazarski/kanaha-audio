/*
 * Kanaha Audio
 * Main Activity
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Minimal UI: requests permissions, starts/stops the AudioService,
 * and warns if Kanaha Camera Control is already using port 8443.
 */

package org.kanaha.audio;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;

import java.net.InetSocketAddress;
import java.net.Socket;

public class MainActivity extends Activity {
    private static final String TAG = "KanahaAudioMain";
    private static final int PERMISSION_REQUEST_CODE = 1001;
    private static final int SERVER_PORT = 8443;

    private TextView statusText;
    private TextView ipText;
    private TextView warningText;
    private Button startButton;
    private boolean serviceRunning = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusText = findViewById(R.id.status_text);
        ipText = findViewById(R.id.ip_text);
        warningText = findViewById(R.id.warning_text);
        startButton = findViewById(R.id.start_button);

        startButton.setOnClickListener(v -> toggleService());

        requestPermissions();
        updateIpAddress();
    }

    @Override
    protected void onResume() {
        super.onResume();
        checkPortConflict();
    }

    private void requestPermissions() {
        String[] permissions = {
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        };

        boolean needRequest = false;
        for (String perm : permissions) {
            if (checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
                needRequest = true;
                break;
            }
        }

        if (needRequest) {
            requestPermissions(permissions, PERMISSION_REQUEST_CODE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == PERMISSION_REQUEST_CODE) {
            for (int i = 0; i < permissions.length; i++) {
                if (results[i] == PackageManager.PERMISSION_GRANTED) {
                    Log.i(TAG, "Permission granted: " + permissions[i]);
                } else {
                    Log.w(TAG, "Permission denied: " + permissions[i]);
                }
            }
        }
    }

    private void toggleService() {
        if (serviceRunning) {
            stopAudioService();
        } else {
            // Check for port conflict before starting
            if (isPortInUse(SERVER_PORT)) {
                warningText.setVisibility(android.view.View.VISIBLE);
                warningText.setText("WARNING: Port " + SERVER_PORT + " is already in use.\n" +
                    "Kanaha Camera Control may be running.\n" +
                    "Stop it first, or both services will conflict.");
                statusText.setText("Status: Cannot start — port conflict");
                return;
            }
            startAudioService();
        }
    }

    private void startAudioService() {
        Log.i(TAG, "Starting AudioService");
        Intent intent = new Intent(this, AudioService.class);
        intent.setAction("org.kanaha.audio.START_SERVER");
        startForegroundService(intent);
        serviceRunning = true;
        statusText.setText("Status: Running on port " + SERVER_PORT);
        startButton.setText("Stop Server");
        warningText.setVisibility(android.view.View.GONE);
    }

    private void stopAudioService() {
        Log.i(TAG, "Stopping AudioService");
        Intent intent = new Intent(this, AudioService.class);
        intent.setAction("org.kanaha.audio.STOP_SERVER");
        startService(intent);
        serviceRunning = false;
        statusText.setText("Status: Stopped");
        startButton.setText("Start Server");
    }

    /**
     * Check if port 8443 is already in use (e.g., by Kanaha Camera Control).
     */
    private void checkPortConflict() {
        new Thread(() -> {
            boolean inUse = isPortInUse(SERVER_PORT);
            runOnUiThread(() -> {
                if (inUse && !serviceRunning) {
                    warningText.setVisibility(android.view.View.VISIBLE);
                    warningText.setText("WARNING: Port " + SERVER_PORT + " is in use.\n" +
                        "Kanaha Camera Control appears to be running.\n" +
                        "Both apps use port 8443 and cannot run simultaneously.");
                } else if (!inUse) {
                    warningText.setVisibility(android.view.View.GONE);
                }
            });
        }).start();
    }

    private boolean isPortInUse(int port) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", port), 500);
            socket.close();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void updateIpAddress() {
        try {
            WifiManager wifiManager = (WifiManager) getApplicationContext()
                .getSystemService(WIFI_SERVICE);
            WifiInfo wifiInfo = wifiManager.getConnectionInfo();
            int ip = wifiInfo.getIpAddress();
            String ipAddr = String.format("%d.%d.%d.%d",
                (ip & 0xff), (ip >> 8 & 0xff),
                (ip >> 16 & 0xff), (ip >> 24 & 0xff));
            if (!"0.0.0.0".equals(ipAddr)) {
                ipText.setText("WiFi: https://" + ipAddr + ":" + SERVER_PORT);
            } else {
                ipText.setText("WiFi: not connected");
            }
        } catch (Exception e) {
            ipText.setText("WiFi: unavailable");
        }
    }
}
