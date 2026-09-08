/*
 * Kanaha Audio — Network Discovery Service (mDNS/DNS-SD)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Core Kanaha feature: every Kanaha server app (camera, audio, calcs)
 * advertises itself as a _https._tcp service so clients can discover it without
 * a static IP. The TXT record carries api=kanaha-audio-search plus device metadata and the
 * current IP. Android does not let a regular app set a custom mDNS hostname, so
 * clients read the device IP from the "ip" TXT record.
 */

package org.kanaha.audio;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.util.Log;

import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * mDNS/DNS-SD registration for the Kanaha Audio server.
 * Registered as: {deviceName}._https._tcp.local
 */
public class NetworkDiscoveryService {
    private static final String TAG = "KanahaAudioDiscovery";
    private static final String SERVICE_TYPE = "_https._tcp.";
    private static final String API_ID = "kanaha-audio-search";
    private static final String NAME_PREFIX = "kanaha-audio";

    private final Context context;
    private final NsdManager nsdManager;
    private final AtomicBoolean isRegistered = new AtomicBoolean(false);

    // The listener for the current registration. unregisterService() clears this
    // BEFORE requesting the async unregister, so a late callback from a superseded
    // listener (identity check below) cannot mutate state for a new registration.
    private volatile NsdManager.RegistrationListener registrationListener;

    public NetworkDiscoveryService(Context context) {
        this.context = context.getApplicationContext();
        this.nsdManager = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
    }

    /** Advertise the server on the given port. Safe to call again (re-registers). */
    public boolean registerService(int port) {
        if (nsdManager == null) {
            Log.e(TAG, "NsdManager not available on this device");
            return false;
        }
        // Tear down any existing registration first and detach its listener.
        unregisterService();

        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(generateServiceName());
        info.setServiceType(SERVICE_TYPE);
        info.setPort(port);
        for (Map.Entry<String, String> e : buildAttributes().entrySet()) {
            info.setAttribute(e.getKey(), e.getValue());
        }

        NsdManager.RegistrationListener listener = createListener();
        registrationListener = listener;
        try {
            Log.i(TAG, "Registering mDNS " + SERVICE_TYPE + " on port " + port);
            nsdManager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to register mDNS service", e);
            registrationListener = null;
            return false;
        }
    }

    /** Withdraw the advertisement. Safe to call when not registered. */
    public void unregisterService() {
        NsdManager.RegistrationListener listener = registrationListener;
        if (listener == null || nsdManager == null) {
            return;
        }
        // Detach synchronously: any late callback from this listener is now a no-op.
        registrationListener = null;
        isRegistered.set(false);
        try {
            nsdManager.unregisterService(listener);
            Log.i(TAG, "mDNS unregistration initiated");
        } catch (Exception e) {
            Log.e(TAG, "Failed to unregister mDNS service", e);
        }
    }

    public boolean isRegistered() {
        return isRegistered.get();
    }

    /** DNS-safe, device-derived service name. Android de-dupes collisions. */
    private String generateServiceName() {
        String base = Build.MODEL != null ? Build.MODEL : "device";
        String safe = base.replaceAll("[^A-Za-z0-9-]", "-").replaceAll("-+", "-");
        while (safe.startsWith("-")) safe = safe.substring(1);
        while (safe.endsWith("-")) safe = safe.substring(0, safe.length() - 1);
        if (safe.isEmpty()) safe = "device";
        return NAME_PREFIX + "-" + safe;
    }

    private Map<String, String> buildAttributes() {
        Map<String, String> a = new HashMap<>();
        a.put("txtvers", "1");
        a.put("api", API_ID);
        a.put("model", String.valueOf(Build.MODEL));
        a.put("manufacturer", String.valueOf(Build.MANUFACTURER));
        a.put("android", String.valueOf(Build.VERSION.SDK_INT));
        a.put("version", getAppVersion());
        String ip = getCurrentIpAddress();
        if (ip != null) {
            a.put("ip", ip);
        }
        return a;
    }

    private String getCurrentIpAddress() {
        // Prefer a private LAN address (10/172.16-31/192.168) that mDNS clients can
        // actually reach. Skip link-local and the 192.0.0.0/24 464xlat CLAT range,
        // which is not a LAN IP. Fall back to any other IPv4 only if no LAN one exists.
        String fallback = null;
        try {
            for (NetworkInterface intf : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (intf.isLoopback() || !intf.isUp()) continue;
                for (InetAddress addr : Collections.list(intf.getInetAddresses())) {
                    if (addr.isLoopbackAddress() || addr.isLinkLocalAddress()) continue;
                    String host = addr.getHostAddress();
                    if (host == null || host.indexOf(':') >= 0) continue;  // IPv4 only
                    if (host.startsWith("192.0.0.")) continue;             // 464xlat CLAT, not a LAN IP
                    if (addr.isSiteLocalAddress()) return host;
                    if (fallback == null) fallback = host;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to get IP address", e);
        }
        return fallback;
    }

    private String getAppVersion() {
        try {
            return context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "1.0";
        }
    }

    private NsdManager.RegistrationListener createListener() {
        return new NsdManager.RegistrationListener() {
            @Override
            public void onServiceRegistered(NsdServiceInfo info) {
                if (registrationListener != this) return;  // superseded by a newer registration
                isRegistered.set(true);
                Log.i(TAG, "mDNS registered: " + info.getServiceName()
                        + " port " + info.getPort()
                        + " (hostname is the Android system default; use the ip TXT record)");
            }
            @Override
            public void onRegistrationFailed(NsdServiceInfo info, int errorCode) {
                if (registrationListener != this) return;
                isRegistered.set(false);
                Log.e(TAG, "mDNS registration failed (code " + errorCode + ")");
            }
            @Override
            public void onServiceUnregistered(NsdServiceInfo info) {
                // State is cleared synchronously in unregisterService(); just log.
                Log.i(TAG, "mDNS unregistered");
            }
            @Override
            public void onUnregistrationFailed(NsdServiceInfo info, int errorCode) {
                Log.e(TAG, "mDNS unregistration failed (code " + errorCode + ")");
            }
        };
    }
}
