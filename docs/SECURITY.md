# Security Model

Kanaha Audio shares the same security model as Kanaha Camera Control System.

## Transport Security — TLS is Mandatory

The app serves requests via **Apache httpd 2.4.66 (mod_http2 + mod_ssl + mod_axis2)**
over HTTP/2 (ALPN `h2`) with mutual TLS. See
[PATH_B_HTTP2_MIGRATION.md](PATH_B_HTTP2_MIGRATION.md) for how the server moved
from the original hand-rolled HTTP/1.1 server to real Apache (verified on device:
`ALPN: server accepted h2` → `HTTP/2 200`).

- **HTTPS/HTTP2 + mTLS**: all API traffic is TLS with mutual certificate auth; `Protocols h2 http/1.1` (TLS-only — there is no cleartext listener)
- **Certificate chain**: self-signed Kanaha CA (off-device) → per-device server cert (minted on-device, provisioned on first run) + CA-issued client certs
- **No anonymous access**: `SSLVerifyClient require` (in `ssl.conf`) rejects connections without a valid client cert at the TLS handshake
- **Modern TLS only**: `SSLProtocol all -SSLv3 -TLSv1 -TLSv1.1` (TLS 1.2 / 1.3)

### Certificate Configuration

`AudioService` deploys `ssl.conf` + the certs to the Apache ServerRoot and launches
`httpd -f conf/httpd.conf -d apache -X`; Apache enforces mTLS via `ssl.conf`
(`SSLVerifyClient require`, `SSLCACertificateFile ssl/ca.crt`).

The standalone desktop test CLI (`apache_httpd_android.c`, built by `build-android.sh`)
still takes explicit flags:

```bash
kanaha-audio-httpd -p 8443 \
  -c /path/to/server.crt \    # Server certificate (PEM)
  -k /path/to/server.key \    # Server private key (PEM)
  -a /path/to/ca.crt           # CA cert for client verification (mTLS)
```

Uses the same Kanaha CA as the other apps (one trust anchor). The device mints
its own key + CSR on first run and is provisioned with a CA-signed cert; the CA
key lives off-device (never in the repo or APK).

### Bringing up a new phone

**The server will not start until the device is provisioned, and it is quiet
about it.** On first run the app writes `files/csr/audio.csr` and stops there;
the notification reads "Awaiting provisioning" and the only other trace is one
line in logcat:

```
W KanahaAudioService: Not provisioned; server not started. Run: kanaha-provision.sh <serial> audio
```

Nothing is wrong with the phone — the port is simply closed. Sign the CSR with
the CA off-device and copy the signed cert plus the CA cert into
`files/apache/ssl/` as `server.crt` and `ca.crt` (the app directory is
`drwx------`, so stage through `/data/local/tmp` and copy with `run-as`), then
restart the service. The private key never leaves the phone: only the CSR comes
off it, and only a certificate goes back.

Two things worth knowing before the first curl:

- **Name the cert the way you will call it.** A cert carrying only
  `DNS:audio.local` forces `--resolve` on every request; one carrying the
  phone's address does not, but then a DHCP change means re-provisioning.
- **MCP needs none of this.** The stdio binary runs under `run-as` over adb and
  never touches httpd or TLS, so an unprovisioned phone can still record,
  transcribe and speak. That is a quick way to tell a certificate problem from
  an app problem.

## HTTP/2 DoS Hardening (CVE-2026-49975)

The "HTTP/2 Bomb" chains an HPACK indexed-reference bomb with a zero-window
flow-control stall to exhaust server memory. `conf/http2-performance.conf` bounds
the blast radius for a phone's limited RAM — `H2MaxSessionStreams 1`,
`H2StreamTimeout 30`, `H2StreamMaxMemSize 65536` — alongside
`LimitRequestFieldSize 4096` / `LimitRequestFields 50` in `httpd.conf`. The
complete fix for the Cookie-crumb bypass is `mod_http2` ≥ 2.0.41.

## Input Validation

- Audio file paths are validated against path traversal attacks (`..` sequences rejected)
- Model names are restricted to plain names (no `/` or `..` — e.g., "base", "tiny")
- Keyword strings are sanitized before use in whisper.cpp
- JSON string parsing handles escaped quotes to prevent injection
- HTTP request parsing/dispatch is handled by Apache mod_http2 + mod_axis2 (the standalone test CLI anchors its `strncmp` method check at position 0 to prevent request smuggling)
- The `listAudioFiles` directory parameter falls back to the default app directory if `..` is detected

## Architecture Security Advantage

Unlike Kanaha Camera (which requires Intent IPC across a GPL process boundary), Kanaha Audio calls whisper.cpp directly as a C function call within the same process. This eliminates:

- IPC serialization/deserialization attack surface
- File-based response passing (no TOCTOU race conditions)
- Shell invocation risks (no `system()`, `popen()`, or `am broadcast` subprocess)

## Reporting Vulnerabilities

Report security issues via GitHub Issues with the "security" label.
