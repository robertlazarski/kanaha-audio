# Path B: kanaha-audio → real HTTP/2 (Apache + mod_http2 + mod_axis2)

## Why
The prior kanaha-audio server (`cpp/apache-httpd/apache_httpd_android.c`, shipped
as `libkanaha_audio_httpd.so`) was a hand-rolled OpenSSL server: one `SSL_read`
into an 8 KB buffer, HTTP/1 text parsing, no ALPN callback, and nghttp2 linked but
only used for `nghttp2_version()`. Verified on a Pixel 10 Pro XL (2026-06-10):
`curl --http2` showed `ALPN: server did not agree on a protocol` → HTTP/1.1.

Path B replaces it with the **same real-Apache stack the Kanaha Camera app uses**,
so `AudioSearchService` is served over genuine HTTP/2 (ALPN `h2`) + mTLS, and the
CVE-2026-49975 hardening applies.

## What this change already did (committed source)
- **Apache config set** under `app/src/main/assets/apache/` (+ `assets/axis2c/axis2.xml`):
  `httpd.conf`, `ssl.conf` (mTLS, h2 vhost), `http2-performance.conf`
  (CVE-2026-49975 H2 limits), `axis2.conf`, `mime.types`, `htdocs/index.html`.
  Modeled on the camera app; port **8443**, `SSLVerifyClient require`, `Protocols h2`.
- **`AudioService.java`** now sets up the Apache ServerRoot (`filesDir/apache`),
  deploys the config + certs from assets (with `{{KANAHA_HOSTNAME}}` substitution),
  and launches the binary as real Apache:
  `libkanaha_audio_httpd.so -f conf/httpd.conf -d apache -X`, passing the whisper
  models dir via the `KANAHA_AUDIO_MODELS` env var.

## Native binary — BUILT (recipe)
The packaged `jniLibs/arm64-v8a/libkanaha_audio_httpd.so` is now a **real Apache
2.4.66** (mod_ssl + mod_http2 + mod_axis2) with `AudioSearchService` +
whisper/yamnet/ltc/sftp statically linked — built and verified on a Pixel 10 Pro XL
(2026-06-10): `ALPN: server accepted h2` → `HTTP/2 200`,
`server: Apache/2.4.66 (Unix) OpenSSL/3.2.0 Axis2C/2.0.0`.

Build/rebuild recipe: **`~/android-cross-builds/link-httpd-audio.sh`** (clones the
camera's `link-httpd-axis2.sh`, swapping in the audio service objects + DSP libs).
It:
1. Compiles `axis2c/axis2_static_service_adapter.c` (strong override of mod_axis2's
   weak `audio_search_service_invoke_json`) + `audio_search_service.c` + the DSP
   (`whisper_android_bridge.c`, `yamnet_bridge.c`, `audio_recording.c`, …) into
   `libkanaha_audio_services.a`.
2. Links real httpd (`httpd-2.4.66` static module set) + `libmod_axis2.a` +
   `libaxis2_engine.a` (whole-archive) + `libkanaha_audio_services.a` (whole-archive)
   + whisper/tflite/ruy/ltc/ssh2/aaudio. **Link with `clang++`** (not `clang`) —
   tflite/ruy need libc++/libc++abi. **android28** target (AAudio is API 26+).
3. Strip with `llvm-strip --strip-unneeded` → copy to the jniLibs path (filename
   unchanged; `AudioService.java` and the `pkill` pattern still reference it).

Then `JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 ./gradlew :app:assembleDebug`
(Gradle 8.12 can't run under JDK 25), `adb install -r`, and verify with the curl
below.

`apache_httpd_android.c` is superseded by Path B (still built by `build-android.sh`
as the standalone desktop test CLI; not used by the app).

### The intended use case
Keyword-spotting the phrase **"next slide please"** in the audio of a recorded
demo, so slide transitions can be located after the fact. That is the reason the
phrase appears in the verify command below — it is the real query, not a
placeholder.

The service already exposes `startRecording` / `stopRecording` alongside
`searchKeywords`, so the whole flow is covered by the existing API:

1. `startRecording` when the demo starts
2. `stopRecording` when it ends
3. `searchKeywords` over the resulting file for `"next slide please"`

### Architectural decision to confirm
The custom server did "all audio operations" in one process. Under Path B, HTTP is
Apache and `searchKeywords` is request/response over an existing `audio_file` — which
fits the Axis2 service model. **If any audio work must run always-on (continuous
AAudio recording independent of HTTP requests), decide where it lives**: either a
separate native thread the Axis2 service starts, or a second component.

**For the use case above this question does not arise** — recording is bounded by
explicit start/stop calls and the search runs afterwards over a finished file, so
nothing needs to run independently of HTTP requests. It *would* arise if the
phrase had to be detected **live**, to advance slides as they are spoken; that is
a different design and is not what the current API supports.

## Verify on device (the test that settles it)
```
adb forward tcp:8443 tcp:8443
SSL=~/kanaha-certs   # your CA dir: client.crt, client.key, ca.crt
curl -v -sk --http2 --cert $SSL/client.crt --key $SSL/client.key --cacert $SSL/ca.crt \
  -H "Content-Type: application/json" \
  -d '{"action":"searchKeywords","audio_file":"presentation.wav","keywords":["next slide please"]}' \
  https://localhost:8443/services/AudioSearchService/searchKeywords 2>&1 | grep -i ALPN
```
Success = `ALPN: server accepted h2` + `using HTTP/2` (kanaha-calcs already shows this).
Failure = `server did not agree on a protocol` → still the old server / mod_http2 not linked.

## Certs note
Run a small private CA off-device (its key never ships). Each device mints its own
key + CSR on first run and is provisioned with a CA-signed cert; issue a client
cert from the same CA for the `tools/*.sh` and the curl examples above. Point the
`SSL=` line at wherever that client cert + `ca.crt` live.

## Rollback
Revert `AudioService.java` and remove `assets/apache/` + `assets/axis2c/axis2.xml`;
the prior custom-server binary + args return the app to HTTP/1.1.
