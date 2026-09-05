/*
 * Kanaha Audio
 * Audio Server Foreground Service
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Launches the native Apache httpd as a foreground service (Path B). The httpd
 * binary (packaged as libkanaha_audio_httpd.so) is a real Apache build with
 * mod_http2 + mod_ssl + mod_axis2 and AudioSearchService statically linked, so
 * requests are served over genuine HTTP/2 (ALPN h2) + mTLS — replacing the prior
 * hand-rolled HTTP/1.1 server. Audio DSP (whisper.cpp, YAMNet, AAudio, LTC, SFTP)
 * runs inside the Axis2/C service module. See docs/PATH_B_HTTP2_MIGRATION.md.
 *
 * This service:
 *   1. Provides RECORD_AUDIO permission context for the native httpd child process
 *   2. Keeps it alive via foreground service + wake lock
 *   3. Deploys the Apache config set + certs and detects port conflicts with Camera
 *
 * Architecture:
 *   MainActivity → startForegroundService → AudioService
 *     → deploy apache/ config + ssl/ certs to ServerRoot
 *     → ProcessBuilder launches httpd -f conf/httpd.conf -d apache -X
 *     → Apache (mod_http2 + mod_axis2) serves AudioSearchService over HTTP/2+mTLS
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
     * Set up the Apache ServerRoot tree and deploy config + certs from assets.
     *
     * Path B layout (ServerRoot = filesDir/apache), mirroring the Kanaha Camera app:
     *   apache/conf/{httpd.conf,ssl.conf,http2-performance.conf,axis2.conf,mime.types}
     *   apache/ssl/{server.crt,server.key,ca.crt}
     *   apache/htdocs/index.html
     *   apache/axis2c/{axis2.xml,services/AudioSearchService/services.xml}
     *   apache/logs/   (created so httpd can write)
     */
    private void setupFiles() throws IOException {
        File filesDir = getFilesDir();
        File apacheDir = new File(filesDir, "apache");

        // Apache ServerRoot tree
        new File(apacheDir, "conf").mkdirs();
        new File(apacheDir, "logs").mkdirs();
        new File(apacheDir, "htdocs").mkdirs();
        File sslDir = new File(apacheDir, "ssl");
        sslDir.mkdirs();
        new File(apacheDir, "axis2c/services/AudioSearchService").mkdirs();
        new File(apacheDir, "axis2c/modules").mkdirs();

        // Audio working dirs (unchanged)
        new File(filesDir, "models").mkdirs();
        new File(filesDir, "audio").mkdirs();
        new File(filesDir, "ssh/keys").mkdirs();

        deployMcpBinary(filesDir);

        // Certs go under the ServerRoot so ssl.conf's "ssl/server.crt" resolves.
        deploySslFromAssets(sslDir);

        // Apache configuration + Axis2/C repository
        deployApacheConfig(apacheDir);

        Log.i(TAG, "Apache ServerRoot: " + apacheDir.getAbsolutePath());
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
                if (filename.endsWith(".key")) {
                    // Private key: restrict to owner-only (defense in depth beyond
                    // the app sandbox — matters on rooted devices). Clear all, then
                    // grant owner read/write.
                    target.setReadable(false, false);
                    target.setWritable(false, false);
                    target.setReadable(true, true);
                    target.setWritable(true, true);
                }
            } catch (IOException e) {
                Log.d(TAG, "SSL asset not found (will need manual deploy): " + filename);
            }
        }
    }

    /**
     * Deploy the Apache config set + Axis2/C repository from assets to the
     * ServerRoot. httpd.conf and ssl.conf carry a {{KANAHA_HOSTNAME}} placeholder
     * substituted with the device hostname (cosmetic ServerName).
     */
    private void deployApacheConfig(File apacheDir) {
        File conf = new File(apacheDir, "conf");
        String hostname = getHostname();
        // Each deploy is independent so one missing/failed asset doesn't block the rest.
        deployAssetWithHostname("apache/httpd.conf", new File(conf, "httpd.conf"), hostname);
        deployAssetWithHostname("apache/ssl.conf", new File(conf, "ssl.conf"), hostname);
        deployAsset("apache/http2-performance.conf", new File(conf, "http2-performance.conf"));
        deployAsset("apache/axis2.conf", new File(conf, "axis2.conf"));
        deployAsset("apache/mime.types", new File(conf, "mime.types"));
        deployAsset("apache/htdocs/index.html", new File(apacheDir, "htdocs/index.html"));
        deployAsset("axis2c/axis2.xml", new File(apacheDir, "axis2c/axis2.xml"));
        deployAsset("axis2c/services/AudioSearchService/services.xml",
            new File(apacheDir, "axis2c/services/AudioSearchService/services.xml"));
        Log.i(TAG, "Apache configuration deployed (ServerName host: " + hostname + ")");
    }

    /** Copy an asset verbatim to a file; logs and continues if the asset is missing. */
    private void deployAsset(String assetPath, File target) {
        try {
            target.getParentFile().mkdirs();
            try (InputStream is = getAssets().open(assetPath);
                 FileOutputStream fos = new FileOutputStream(target)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = is.read(buf)) > 0) fos.write(buf, 0, n);
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to deploy asset: " + assetPath, e);
        }
    }

    /** Copy a text asset to a file, substituting {{KANAHA_HOSTNAME}}; logs and continues on error. */
    private void deployAssetWithHostname(String assetPath, File target, String hostname) {
        try {
            target.getParentFile().mkdirs();
            StringBuilder sb = new StringBuilder();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(
                    getAssets().open(assetPath), java.nio.charset.StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) sb.append(line).append('\n');
            }
            String out = sb.toString().replace("{{KANAHA_HOSTNAME}}", hostname);
            // Explicit UTF-8 to match the input charset (FileWriter would use the
            // platform default).
            try (java.io.Writer w = new java.io.OutputStreamWriter(
                    new FileOutputStream(target), java.nio.charset.StandardCharsets.UTF_8)) {
                w.write(out);
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to deploy asset: " + assetPath, e);
        }
    }

    /** Cosmetic ServerName host; clients connect by IP, so the value need not resolve. */
    private String getHostname() {
        String model = android.os.Build.MODEL;
        if (model == null || model.isEmpty()) return "kanaha-audio.local";
        return model.replaceAll("[^A-Za-z0-9_-]", "-").toLowerCase() + ".local";
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
        File apacheDir = new File(filesDir, "apache");
        String modelsDir = new File(filesDir, "models").getAbsolutePath();
        String configPath = new File(apacheDir, "conf/httpd.conf").getAbsolutePath();

        // Path B: launch the REAL Apache httpd (mod_http2 + mod_ssl + mod_axis2)
        // so AudioSearchService is served over genuine HTTP/2 (ALPN h2). The
        // packaged binary libkanaha_audio_httpd.so must now BE a real Apache build
        // with AudioSearchService statically linked — see
        // docs/PATH_B_HTTP2_MIGRATION.md. Apache args:
        //   -f <httpd.conf>   -d <ServerRoot>   -X single-process mode for Android
        String[] command = {
            executablePath,
            "-f", configPath,
            "-d", apacheDir.getAbsolutePath(),
            "-X"
        };

        Log.i(TAG, "Launching: " + String.join(" ", command));

        ProcessBuilder pb = new ProcessBuilder(command);
        pb.directory(apacheDir);
        pb.redirectErrorStream(true);

        java.util.Map<String, String> env = pb.environment();
        env.put("LD_LIBRARY_PATH", nativeLibDir);
        env.put("HOME", filesDir.getAbsolutePath());
        // The Axis2/C AudioSearchService reads the whisper models directory from
        // this env var (real Apache doesn't take the old -m flag).
        env.put("KANAHA_AUDIO_MODELS", modelsDir);

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
            Log.e(TAG, "Server process exited with code: " + exitCode
                + " — check native logs (Apache config error or port " + SERVER_PORT + " conflict)");
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

    /**
     * Copy the MCP binary out of the APK's native lib directory into filesDir,
     * so Claude can be pointed at a path that survives reinstalls.
     *
     * The packaged location under /data/app carries a random hash that changes
     * every time the app is installed, which makes it useless in a static
     * Claude Desktop config. This copy is stable.
     *
     * Note the caller still needs "run-as": the app data directory is
     * drwx------, so the adb shell user (uid 2000) has no traversal into it at
     * all, wherever the binary happens to sit.
     */
    private void deployMcpBinary(File filesDir) {
        File src = new File(getApplicationInfo().nativeLibraryDir, "libkanaha_mcp.so");
        File dst = new File(filesDir, "kanaha-audio-mcp");
        if (!src.exists()) {
            Log.w(TAG, "MCP binary not packaged: " + src);
            return;
        }
        if (dst.exists() && dst.length() == src.length()) {
            return;  // already current
        }
        try (java.io.InputStream in = new java.io.FileInputStream(src);
             java.io.OutputStream out = new java.io.FileOutputStream(dst)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        catch (java.io.IOException e) {
            Log.e(TAG, "Failed to deploy MCP binary", e);
            return;
        }
        if (!dst.setExecutable(true, true)) {
            Log.w(TAG, "Could not mark MCP binary executable: " + dst);
        }
        Log.i(TAG, "MCP binary deployed: " + dst.getAbsolutePath());
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
