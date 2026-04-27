/*
 * Kanaha Audio
 * Audio Server Foreground Service
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Launches the kanaha-audio-httpd native binary as a foreground service.
 * The native process handles all audio operations (whisper.cpp, YAMNet,
 * AAudio recording, LTC decode, SFTP) — no JNI needed.
 *
 * This service exists solely to:
 *   1. Provide RECORD_AUDIO permission context for the native process
 *   2. Keep the process alive via foreground service + wake lock
 *   3. Detect port conflicts with Kanaha Camera Control
 *
 * Architecture:
 *   MainActivity → startForegroundService → AudioService
 *     → ProcessBuilder launches kanaha-audio-httpd
 *     → Native process handles all HTTP/2+mTLS requests
 */

package org.kanaha.audio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.InetSocketAddress;
import java.net.Socket;

public class AudioService extends Service {
    private static final String TAG = "KanahaAudioService";
    private static final String CHANNEL_ID = "kanaha_audio_service";
    private static final int NOTIFICATION_ID = 2001;
    private static final int SERVER_PORT = 8443;

    private PowerManager.WakeLock wakeLock;
    private Process serverProcess;
    private boolean isRunning = false;

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "AudioService created");
        createNotificationChannel();
        acquireWakeLock();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Must call startForeground immediately
        startForeground(NOTIFICATION_ID, createNotification("Starting..."));

        if (intent != null) {
            String action = intent.getAction();
            if ("org.kanaha.audio.START_SERVER".equals(action)) {
                startServer();
            } else if ("org.kanaha.audio.STOP_SERVER".equals(action)) {
                stopServer();
                stopSelf();
            }
        }

        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "AudioService destroying");
        stopServer();
        releaseWakeLock();
        super.onDestroy();
    }

    private void startServer() {
        if (isRunning) {
            Log.i(TAG, "Server already running");
            return;
        }

        // Check for port conflict with Kanaha Camera Control
        if (isPortInUse(SERVER_PORT)) {
            Log.e(TAG, "Port " + SERVER_PORT + " already in use — " +
                  "Kanaha Camera Control may be running. Cannot start.");
            updateNotification("ERROR: Port " + SERVER_PORT + " in use (Camera Control?)");
            return;
        }

        try {
            setupFiles();
            launchNativeProcess();
        } catch (Exception e) {
            Log.e(TAG, "Failed to start server", e);
            updateNotification("Error: " + e.getMessage());
        }
    }

    private void stopServer() {
        if (serverProcess != null) {
            Log.i(TAG, "Stopping native server process");
            serverProcess.destroy();
            serverProcess = null;
        }
        // Also kill by name in case process was orphaned
        killOrphanedProcesses();
        isRunning = false;
    }

    /**
     * Deploy the native binary and SSL certs from assets or lib directory.
     */
    private void setupFiles() throws IOException {
        File filesDir = getFilesDir();
        File modelsDir = new File(filesDir, "models");
        File sslDir = new File(filesDir, "ssl");
        File audioDir = new File(filesDir, "audio");
        File sshDir = new File(filesDir, "ssh/keys");

        modelsDir.mkdirs();
        sslDir.mkdirs();
        audioDir.mkdirs();
        sshDir.mkdirs();

        // Deploy SSL certs from assets if they exist and aren't already deployed
        deploySslFromAssets(sslDir);

        Log.i(TAG, "Files directory: " + filesDir.getAbsolutePath());
        Log.i(TAG, "Models directory: " + modelsDir.getAbsolutePath());
    }

    private void deploySslFromAssets(File sslDir) {
        String[] sslFiles = {"server.crt", "server.key", "ca.crt"};
        for (String filename : sslFiles) {
            File target = new File(sslDir, filename);
            if (target.exists()) continue;

            try (InputStream is = getAssets().open("ssl/" + filename)) {
                try (FileOutputStream fos = new FileOutputStream(target)) {
                    byte[] buf = new byte[4096];
                    int n;
                    while ((n = is.read(buf)) > 0) {
                        fos.write(buf, 0, n);
                    }
                }
                Log.i(TAG, "Deployed SSL: " + filename);
            } catch (IOException e) {
                Log.d(TAG, "SSL asset not found (will need manual deploy): " + filename);
            }
        }
    }

    /**
     * Launch the native kanaha-audio-httpd process.
     *
     * The binary is packaged as libkanaha_audio_httpd.so in the APK's
     * lib/arm64-v8a/ directory (Android requires .so extension for native libs).
     */
    private void launchNativeProcess() throws IOException, InterruptedException {
        String nativeLibDir = getApplicationInfo().nativeLibraryDir;
        String executablePath = nativeLibDir + "/libkanaha_audio_httpd.so";

        File executable = new File(executablePath);
        if (!executable.exists()) {
            Log.e(TAG, "Native binary not found: " + executablePath);

            // List what's in the native lib dir for debugging
            File dir = new File(nativeLibDir);
            String[] files = dir.list();
            if (files != null) {
                for (String f : files) {
                    Log.i(TAG, "  native lib: " + f);
                }
            }
            throw new IOException("Native binary not found: " + executablePath);
        }

        if (!executable.canExecute()) {
            executable.setExecutable(true);
        }

        File filesDir = getFilesDir();
        String modelsDir = new File(filesDir, "models").getAbsolutePath();
        String sslDir = new File(filesDir, "ssl").getAbsolutePath();

        String[] command = {
            executablePath,
            "-p", String.valueOf(SERVER_PORT),
            "-m", modelsDir,
            "-c", sslDir + "/server.crt",
            "-k", sslDir + "/server.key",
            "-a", sslDir + "/ca.crt"
        };

        Log.i(TAG, "Launching: " + String.join(" ", command));

        ProcessBuilder pb = new ProcessBuilder(command);
        pb.directory(filesDir);
        pb.redirectErrorStream(true);

        java.util.Map<String, String> env = pb.environment();
        env.put("LD_LIBRARY_PATH", nativeLibDir);
        env.put("HOME", filesDir.getAbsolutePath());

        serverProcess = pb.start();
        Log.i(TAG, "Process started with PID: " + getProcessId(serverProcess));

        // Read output in background thread
        startOutputReader(serverProcess);

        // Verify process is alive after 2 seconds
        Thread.sleep(2000);

        if (serverProcess.isAlive()) {
            isRunning = true;
            Log.i(TAG, "Kanaha Audio server running on port " + SERVER_PORT);
            updateNotification("Running on port " + SERVER_PORT);
        } else {
            int exitCode = serverProcess.exitValue();
            Log.e(TAG, "Server process exited with code: " + exitCode);
            updateNotification("Error: process exited (code " + exitCode + ")");
        }
    }

    private void startOutputReader(Process process) {
        new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    Log.i(TAG + "-native", line);
                }
            } catch (IOException e) {
                Log.d(TAG, "Output reader finished");
            }
        }, "kanaha-audio-output").start();
    }

    private void killOrphanedProcesses() {
        try {
            String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            String pattern = nativeLibDir + "/libkanaha_audio_httpd.so";
            ProcessBuilder pb = new ProcessBuilder("pkill", "-9", "-f", pattern);
            pb.redirectErrorStream(true);
            Process p = pb.start();
            p.waitFor(3, java.util.concurrent.TimeUnit.SECONDS);
            if (p.isAlive()) p.destroyForcibly();
        } catch (Exception e) {
            Log.d(TAG, "pkill cleanup: " + e.getMessage());
        }
    }

    private long getProcessId(Process process) {
        try {
            // Android API 24+ has Process.pid() but we use reflection for compatibility
            java.lang.reflect.Field f = process.getClass().getDeclaredField("pid");
            f.setAccessible(true);
            return f.getInt(process);
        } catch (Exception e) {
            return -1;
        }
    }

    private boolean isPortInUse(int port) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", port), 500);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // --- Notification management ---

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID, "Kanaha Audio Server",
            NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Kanaha Audio HTTP/2 server status");
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }

    private Notification createNotification(String text) {
        Intent notificationIntent = new Intent(this, MainActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0,
            notificationIntent, PendingIntent.FLAG_IMMUTABLE);

        return new Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Kanaha Audio")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build();
    }

    private void updateNotification(String text) {
        getSystemService(NotificationManager.class)
            .notify(NOTIFICATION_ID, createNotification(text));
    }

    private void acquireWakeLock() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kanaha:audio");
        wakeLock.acquire(60 * 60 * 1000L); // 1 hour max — auto-released if process dies
        Log.i(TAG, "Wake lock acquired");
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
            Log.i(TAG, "Wake lock released");
        }
    }
}
