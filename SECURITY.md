# Security Threat Model — Kanaha Audio

## Project Description

Kanaha Audio is an Android application that runs a **real Apache httpd** in a
forked child process, serving an Axis2/C web service over HTTP/2 with mutual TLS.
It records audio from the device microphone and analyses it on-device: speech
transcription and keyword timestamping via whisper.cpp, non-speech event
classification via TFLite/YAMNet, SMPTE/LTC timecode decoding via libltc, and
file transfer via libssh2.

It is intended for **small trusted LANs** — a laptop controlling one or more
phones during a recording session. It is not intended to be exposed to the
internet, and every endpoint requires a client certificate.

Statically linked into the server binary:

| Component | Version | Role |
|---|---|---|
| Apache httpd | 2.4.66 | HTTP server, mod_ssl + mod_http2 |
| OpenSSL | 3.2.0 | TLS 1.3, mutual authentication |
| Axis2/C | 2.0.0 | Service container, JSON-RPC dispatch |
| nghttp2 | 1.64.0 | HTTP/2 |
| whisper.cpp / ggml | — | Speech recognition, DTW token alignment |
| TensorFlow Lite | — | YAMNet audio event classification |
| libssh2 | — | SFTP with ed25519 keys |
| libltc | — | SMPTE/LTC timecode |
| json-c | 0.18 | JSON parsing |

## Roles and Trust Levels

| Role | Trust Level | Description |
|---|---|---|
| **Device owner** | Fully trusted | Physical control of the phone. Installs the app, holds the screen lock, can enable adb. |
| **adb user** | Fully trusted | Anyone with adb access can `run-as` the app on a debuggable build, reading all app storage including SSH private keys. Treated as equivalent to device ownership. |
| **mTLS client** | Trusted | Holds `client.crt` / `client.key`. Can invoke every operation: record, transcribe, transfer files, read any path the app uid can read. |
| **LAN peer** | Untrusted | Can reach port 8443 but is rejected during the TLS handshake without a client certificate. |
| **Audio file content** | Untrusted | WAV data may originate from another device, an SFTP transfer, or a recording of an uncontrolled environment. **Always hostile input.** |

Note that there is no privilege separation *within* the mTLS client role. A
client certificate is a full-access credential.

## Security Boundaries

### What IS a security issue

- **Memory corruption reachable from a WAV file or a JSON request** — heap or
  stack overflow, use-after-free, double-free, or null dereference in the WAV
  parser, the JSON dispatch layer, or the DSP bridges. This is the highest-value
  class: it is C code parsing untrusted binary input.
- **Crashes from malformed media** — division by zero, unbounded loops, or
  allocation driven by unvalidated header fields. The service is meant to survive
  a corrupt or hostile file with an error, not a signal.
- **Path traversal** — a caller-supplied `audio_file`, `model`, or SFTP path
  escaping its intended directory, or reading app-private material such as
  `files/ssh/keys/`.
- **Authentication bypass** — reaching any operation without a valid client
  certificate, or a configuration in which `SSLVerifyClient require` is not
  enforced on some path.
- **Format string vulnerabilities** — caller-controlled data reaching
  `__android_log_print` or the `LOGI`/`LOGE` macros as the format argument.
- **Key disclosure** — SSH private keys or TLS key material returned by an API,
  written to logs, or exposed through a traversal.
- **SSH host key verification** — now fails closed (a server absent from
  `known_hosts` is refused). A regression back to soft-fail would make SFTP
  transfers interceptable on the LAN.
- **HTTP/2 protocol abuse** — stream or header handling that crashes or exhausts
  the process. The CVE-2026-49975 limits in `http2-performance.conf` exist for
  this reason.
- **Unbounded memory growth per request.** More serious here than in a
  conventional server: `mod_axis2` allocates from a pool whose `free` is a no-op,
  and `MaxConnectionsPerChild 0` means the httpd child never recycles. Any
  per-request allocation is effectively permanent for the life of the app.

### What is NOT a security issue

- **Physical access to an unlocked device.** Everything the app holds is
  reachable by its owner. This project does not defend against a stolen,
  unlocked phone.
- **adb access.** By design, `run-as` on a debuggable build grants app-level
  access. Enabling adb is a deliberate act by the device owner and is required
  for the MCP integration to work at all.
- **The server key ships inside the APK** (`assets/ssl/server.key`). Anyone who
  obtains an APK can impersonate the audio server to a client. This is accepted
  for a private LAN with a distribution the owner controls; do not attach a
  release APK containing a real key to a public release. Generating or
  provisioning the key out of band is the fix if that changes.
- **The client tooling in `tools/` disables server verification** (`curl -sk`),
  so a LAN peer can impersonate the phone *to the laptop*. The server still
  rejects an anonymous client, but the mutual half is not enforced client-side.
  Closing it needs server certs with an IP `subjectAltName`; tracked as a known
  gap, not yet done.
- **The shared client certificate model.** Every device in a deployment shares
  one client certificate. Losing one device means regenerating and redeploying
  certificates everywhere rather than revoking a single credential. This is an
  accepted trade-off for a small trusted LAN, documented in
  [docs/SECURITY.md](docs/SECURITY.md); it is not a vulnerability report.
- **Exposure to a hostile network.** The threat model assumes a private LAN.
  Publishing port 8443 to the internet is a deployment decision outside the
  model.
- **Transcription accuracy.** Whisper mis-hearing a phrase, or YAMNet
  misclassifying a sound, is a quality issue.
- **Vulnerabilities in Android itself,** or in an unmodified upstream dependency
  — report those upstream. Report here if this project *uses* a dependency
  unsafely.
- **Denial of service at the network layer.** SYN floods and connection
  exhaustion are the network's problem.

## Architecture and Attack Surface

### Request path

```
mTLS client (laptop)
   │  HTTP/2 + client certificate, port 8443
   ▼
Apache httpd child process  (mod_ssl → mod_http2 → mod_axis2)
   │
   ▼
Axis2/C  axis2_json_rpc_msg_recv
   │  weak symbol resolved at link time to:
   ▼
audio_search_service_invoke_json()      [static service adapter]
   │  json_object* → char*, 64 KB response buffer
   ▼
audio_search_service_invoke_json_impl() [action dispatch]
   │
   ├─► read_wav_pcm16()   ← untrusted binary input
   ├─► whisper.cpp / ggml
   ├─► TFLite / YAMNet
   ├─► libltc
   └─► libssh2
```

The MCP binary is a **second, parallel entry point** to the same service code,
reached over adb stdio rather than the network. It runs in its own process with
its own model context; it does not share state with the httpd child.

### Attack surface by component

| Component | Exposure | Notes |
|---|---|---|
| `audio_util.c` WAV reader | **Untrusted binary** | Every audio operation reaches it. Header fields drive allocation. |
| `audio_search_service.c` | **Untrusted JSON** | Action dispatch, path construction, fixed-size buffers. |
| `axis2_static_service_adapter.c` | Framework boundary | 64 KB response buffer; `pthread_once` init in a forked child. |
| whisper / YAMNet bridges | Untrusted audio samples | Large models, large allocations, third-party inference code. |
| `audio_sftp.c` | Outbound network + keys | Host key verification, `servers.json` parsing. |
| `audio_recording.c` | Device microphone | Writer thread lifecycle, WAV header finalisation. |
| `kanaha_mcp.c` | adb stdio | Same operations, no TLS; access control is adb itself. |
| Apache + Axis2/C + OpenSSL | Network | Upstream code; statically linked, so "keep current" means rebuilding the binary. The shipped OpenSSL is 3.2.0 (the first release of that series); rebuild against a current 3.2.x/3.3.x. |

### Memory safety profile

The service is C. There is no bounds checking beyond what the code does
explicitly. Three structural facts shape the risk:

1. **Allocation sizes come from file headers.** The WAV reader takes `data_size`
   and `sample_rate` from the file and uses them to allocate and to divide.
2. **Fixed-size buffers are common.** Path construction uses `char buf[1024]`
   with `snprintf`; the service response buffer is a fixed 65536 bytes.
3. **Freed memory is not reclaimed under mod_axis2.** The allocator's `free` has
   no body, so a leak is permanent for the child's lifetime, and the child is
   never recycled. Measured growth is ~7 kB per request.

### Build-time notes

- Built against the NDK targeting **android28** (AAudio requires API 26+).
- `-fsigned-char` is mandatory across every translation unit. `axis2_char_t` is
  plain `char`, which is *unsigned* on ARM and *signed* on x86; mixing
  signedness between the service objects and the Axis2/C libraries produces
  silent misbehaviour rather than a link error.
- `-Wl,--export-dynamic` keeps the dynamic symbol table through stripping, so
  the weak/strong symbol resolution can be verified on a built binary with
  `nm -D`.
- The manifest sets `android:extractNativeLibs="true"`. Both native artefacts
  are **executed**, not dynamically loaded, and need to exist as real files.

## Deployment Hardening

See [docs/SECURITY.md](docs/SECURITY.md) for TLS configuration, certificate
generation, the HTTP/2 DoS limits, and device-loss procedure.

Two settings worth restating:

- **`SSLVerifyClient require`** — the whole access control model. Without it
  every operation is anonymous.
- **`H2MaxSessionStreams 1`** and **`LimitRequestFieldSize 4096`** — the
  CVE-2026-49975 mitigation, and also what keeps HTTP/2 stable on older
  hardware.

## Dependency Security

Keep the statically linked components current; the app ships them, so an
upstream fix does not reach users until the binary is rebuilt and released:

| Component | Minimum | Reason |
|---|---|---|
| Apache httpd | 2.4.66 | HTTP/2 and mod_ssl fixes |
| OpenSSL | 3.2.0 | TLS 1.3 |
| nghttp2 | 1.64.0 | HTTP/2 protocol handling |
| Axis2/C | 2.0.0 | Current release |

Because everything is statically linked, `libkanaha_audio_httpd.so` must be
rebuilt with `build-httpd-audio.sh` whenever any of these is updated.

## Reporting Security Issues

**Do not open a public issue for a security vulnerability.**

Report privately through GitHub's security advisory form on this repository
(*Security* → *Report a vulnerability*). That creates a private thread visible
only to the maintainer.

Please include:

- The affected component and file
- A description of the issue and its impact
- Steps to reproduce, ideally with a minimal proof of concept — a crafted WAV
  file or JSON request is more useful than a description of one
- The device and Android version, if relevant

Expect an initial response within a few days. This is a personal project rather
than a funded one, so please allow reasonable time for a fix before public
disclosure. Credit will be given in the release notes unless you prefer
otherwise.
