# AGENTS.md — Kanaha Audio

## Security Threat Model

See [SECURITY.md](SECURITY.md) for the full threat model: roles and trust
levels, what is and is not a security issue, attack surface by component, and
how to report a vulnerability. For deployment hardening see
[docs/SECURITY.md](docs/SECURITY.md).

## What this is

Kanaha Audio is an **Android app that runs a real Apache httpd in a forked child
process**. It is not a library and not a typical Android app:

- `libkanaha_audio_httpd.so` is Apache 2.4.66 (mod_ssl + mod_http2 + mod_axis2)
  with OpenSSL 3.2.0 and Axis2/C 2.0.0 statically linked. It is **executed as a
  binary**, not loaded as a library — which is why the manifest sets
  `android:extractNativeLibs="true"`.
- `AudioSearchService` is an Axis2/C service reached over **HTTP/2 with mutual
  TLS** on port 8443. Client certificates are required; there is no unauthenticated
  path.
- `libkanaha_mcp.so` is a second binary speaking **MCP JSON-RPC 2.0 over stdio**,
  launched through `adb` so an AI assistant can call the same operations.
- The DSP layer links **whisper.cpp/ggml** (speech), **TFLite/YAMNet** (audio
  event classification), **libltc** (SMPTE timecode) and **libssh2** (SFTP).

The security-relevant consequence: this process parses **untrusted binary media**
(WAV files) and **untrusted JSON**, in C, on a device with a hard per-app memory
budget. Weight scans accordingly.

## Operation modes

Two ways to run, consistent with the other Kanaha server apps:

- **Server mode (headless).** Start the foreground service directly, no UI:

      adb shell am start-foreground-service \
        -a org.kanaha.audio.START_SERVER -n org.kanaha.audio/.AudioService

  `AudioService` is `exported="true"` so adb/other apps can start it — a
  non-exported service cannot be started this way on modern Android (API 31+).
  It launches the mTLS httpd on 8443 and advertises mDNS, working with the phone
  locked and the screen off (the room-mic demo relies on this).
- **Standalone mode.** Launch the activity; it starts the same service. The
  recorder UI is a convenience, not required for the server.

Like every Kanaha server app, audio advertises **mDNS/DNS-SD** (`_https._tcp`,
TXT `api=kanaha-audio-search`) so clients discover it without a static IP.

## Highest-value scan areas

Ranked by exploitability, not by code size. Areas 1 and 2 are where a remote
caller's input reaches C parsing code.

### 1. WAV parsing — the primary untrusted-input path

There are **two** WAV parsers, and any new one is the first thing to scrutinise.
`read_wav_pcm16()` in `audio_util.c` is reached by `searchKeywords`,
`transcribe` and `detectAudioEvents`. `decodeLTC` does **not** use it — the LTC
decoder has its own header parser in `ltc/ltc_decoder.c`. Both parse
attacker-influenced header fields and drive allocation from them.

These patterns were found and are now guarded; re-verify any change near them,
and apply the same checks to any parser added later:

- **`sample_rate <= 0`** — used as a divisor and, in YAMNet, to derive a sample
  offset. A negative rate produced an out-of-bounds read before the buffer;
  `sample_rate = 0` is undefined behaviour (silent 0 on arm64, SIGFPE on an x86
  test build), not the clean crash you might expect. Now rejected in both parsers.
- **Negative `chunk_size`** — `fseek(f, chunk_size, SEEK_CUR)` with a negative
  signed value seeks *backwards*; a chunk that seeks onto itself pins the worker
  at 100% forever. Now rejected.
- **Unbounded `data_size`** — `malloc(data_size)` from the file, now bounded by
  the remaining file size and a 256 MB ceiling.
- **LTC channel count** — `read_buf` is a fixed stack buffer sized for two
  channels; a WAV declaring more overflowed it with file contents (the most
  severe defect found). Now rejected unless 1 or 2 channels.

Key files: `cpp/audio_util.c`, `cpp/ltc/ltc_decoder.c`, `cpp/yamnet/yamnet_bridge.c`

### 2. Caller-controlled file paths

`audio_file`, `model`, and the SFTP parameters all arrive as JSON strings from a
caller and end up in filesystem paths.

- `whisper_android_bridge.c` rejects any `audio_file` containing `..`, and its
  comment states the **service layer is the trust boundary** — verify that claim
  holds in `audio_search_service.c` rather than assuming it.
- Absolute paths *are* accepted by design. Consider what an authenticated client
  can read: the process runs as the app uid, so anything under the app's data
  directory is in scope, including `files/ssh/` keys.
- Model names reach a path via `snprintf` into fixed buffers. Check the
  traversal rejection in `whisper_bridge_load_model()` and the several
  `char buf[1024]` path constructions in `audio_search_service_init()`.

### 3. JSON dispatch and fixed-size response buffers

`audio_search_service_invoke_json_impl()` dispatches on an `action` string. The
adapter allocates a **fixed 65536-byte response buffer** and hands it down;
verify every writer respects the size, particularly `listRecordings`,
`listAudioFiles` and `transcribe`, whose output grows with the number of files
or the length of the audio.

Key files:
- `cpp/axis2c/axis2_static_service_adapter.c` (framework boundary, response buffer)
- `cpp/axis2c/audio_search_service.c` (action dispatch, ~51 KB)

### 4. The static service adapter and the weak-symbol contract

`audio_search_service_invoke_json()` is a **strong symbol overriding a weak stub**
inside Axis2/C's `axis2_json_rpc_msg_recv.c`. If it fails to link, the framework
silently answers "service not available" instead of failing loudly. Scan for
lifetime errors around the `json_object *` returned to the framework, and for the
`pthread_once` initialisation added for Apache's forked child.

### 5. SFTP and key handling

`cpp/sftp/audio_sftp.c` uses libssh2 with ed25519 keys and reads server
definitions from `files/ssh/servers.json`.

- **Host key checking now fails closed** (it did not before: a missing or
  not-found `known_hosts` was logged at DEBUG and allowed through, making every
  transfer MITM-able on the LAN). The server must be listed in
  `files/ssh/known_hosts` or the connection is refused. Re-verify if this changes.
- `servers.json` is parsed from app storage; check the JSON handling for the
  same fixed-buffer patterns as above.
- Private keys live in `files/ssh/keys/`. Confirm nothing logs them and that no
  API returns their contents.

### 6. Recording lifecycle and the AAudio writer thread

`cpp/recording/audio_recording.c` runs a writer thread and finalises the WAV
header on stop. Scan for races between `startRecording`/`stopRecording` and the
writer, double-stop handling, and what happens when the process is killed
mid-recording (the header is then never finalised).

### 7. Memory growth under mod_axis2

`mod_axis2` allocates from an Apache pool whose **`free` is a no-op**, and the
pool's lifetime is the httpd child, not the request. `httpd.conf` ships
`MaxConnectionsPerChild 0`, so the child never recycles. Measured on a Moto X4:
**~7 kB per request, ~37,600 requests to grow 256 MB.** That is not a practical
DoS for the intended workload, but any newly added per-request allocation
compounds permanently. Treat "leaks a little per request" as more serious here
than in a normal server.

### 8. Format strings in logging

Confirm no caller-controlled string reaches `__android_log_print` or the
`LOGI`/`LOGE` macros as the *format* argument. `LOGE(user_input)` is exploitable;
it must be `LOGE("%s", user_input)`. The action-dispatch and path-handling code
logs client-supplied filenames and model names.

### 9. TLS configuration

`assets/apache/ssl.conf` sets `SSLVerifyClient require` and
`assets/apache/http2-performance.conf` carries the CVE-2026-49975 HTTP/2 limits
(`H2MaxSessionStreams 1`, `LimitRequestFieldSize 4096`). Verify these are not
weakened, and that no vhost falls back to accepting anonymous clients.

## Project structure

```
kanaha-audio-app/app/src/main/
  cpp/
    audio_util.c              Shared WAV reader -- highest-value scan target
    axis2c/
      audio_search_service.c  Action dispatch, all 14 operations
      axis2_static_service_adapter.c  Strong symbol overriding Axis2/C's weak stub
      kanaha_mcp.c            MCP JSON-RPC 2.0 protocol
      kanaha_mcp_main.c       MCP stdio entry point
    whisper/                  whisper.cpp bridge (speech, DTW timestamps)
    yamnet/                   TFLite/YAMNet bridge (audio events)
    recording/                AAudio capture, tone generation, sidecar JSON
    sftp/                     libssh2 transfer
    ltc/                      SMPTE/LTC decode
  assets/apache/              httpd.conf, ssl.conf, http2-performance.conf
  assets/axis2c/              axis2.xml + AudioSearchService services.xml
  java/org/kanaha/audio/
    AudioService.java         Deploys config/certs, launches httpd, deploys MCP binary
    MainActivity.java         Start/stop UI, port-conflict check
build-httpd-audio.sh          Builds the real-Apache binary (the app server)
build-android.sh              Builds the MCP binary + a desktop test CLI
```

**Do not confuse the two build scripts.** `build-httpd-audio.sh` produces the
Apache binary the app actually runs. `build-android.sh` produces the MCP binary
and a standalone desktop CLI from the superseded `apache_httpd_android.c`. The
latter script previously installed its CLI over the Apache binary, silently
reverting the app to a hand-rolled HTTP/1.1 server; it no longer does.

## Testing

- **On-device is the only meaningful integration test.** The service needs
  AAudio, an app-private filesystem layout, and mTLS.
- Verify HTTP/2 is genuinely in use — the failure mode is a silent fallback:
  ```sh
  curl -v -sk --http2 --cert client.crt --key client.key --cacert ca.crt \
    https://<phone>:8443/services/AudioSearchService 2>&1 | grep -i ALPN
  ```
  Success is `ALPN: server accepted h2`. Anything else means the old server or a
  mod_http2 link problem.
- MCP requires `run-as`, always: the app data directory is `drwx------`, so the
  adb shell user has no traversal into it.
  ```sh
  echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | \
    adb -s <dev> shell run-as org.kanaha.audio ./files/kanaha-audio-mcp
  ```
- Whisper and ggml log through their own callback, which on Android goes to
  stderr and therefore nowhere. `whisper_bridge_init()` routes it into logcat —
  keep that, since library-internal failures are otherwise invisible. DTW
  alignment silently disabling itself is the known example.

## Reporting

Report vulnerabilities privately through GitHub's security advisory form on this
repository. Do not open a public issue. See [SECURITY.md](SECURITY.md).
