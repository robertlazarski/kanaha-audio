# Kanaha Audio

Keyword audio search, instrument detection, SMPTE timecode decoding, microphone recording, SFTP file transfer, and 15 MCP tools for Claude — served from Apache httpd on a phone via [mod_axis2](https://axis.apache.org/axis2/c/core/), a C module that adds JSON-RPC web services to httpd (like mod_ssl adds TLS). No JNI. No Java audio stack. No Intent IPC. The native httpd process opens the microphone, searches audio for spoken phrases, detects instruments, decodes LTC timecode, and SFTPs the results to your workstation. Apache 2.0, so every line can flow upstream.

**What C can do on Android that most people don't realize:** Record audio from a phone microphone (AAudio NDK), run speech-to-text (whisper.cpp) and audio classification (YAMNet/TFLite) in-process, decode professional SMPTE timecode (libltc), transfer files over SSH (libssh2), schedule kernel-level multi-device sync (`clock_nanosleep`), speak back through the same speaker (flite), and serve it all over HTTP/2 with mTLS — from a single 22 MB binary, no Java layer involved. Six C libraries statically linked, dispatched through one [weak symbol registry](https://github.com/apache/axis-axis2-c-core/blob/master/docs/HTTP2_ANDROID.md#solution-weak-symbol-architecture). Tested on devices spanning 2017–2026: a 2017 Moto X4, a **$99 Moto G Play 2024** (the current room-mic phone), a Pixel 9 Pro and a Pixel 10 Pro XL.

**Why this exists:** [Apache Axis2/C 2.0.0](https://github.com/apache/axis-axis2-c-core) was recently released with HTTP/2, JSON-RPC, and Android support. Kanaha Audio is a showcase application designed to demonstrate that the framework is worth contributing to. See [Axis2/C HTTP/2 on Android](https://github.com/apache/axis-axis2-c-core/blob/master/docs/HTTP2_ANDROID.md) for the technical foundation.

| Feature | Details |
|---------|---------|
| Audio inference | Two ML models: whisper.cpp (speech-to-text) + YAMNet/TFLite (audio event detection, 521 classes) |
| Recording | AAudio NDK microphone capture, WAV output (16kHz or 44.1kHz) |
| SMPTE/LTC | Decode timecode from Tentacle Sync recordings (libltc) |
| SFTP | Transfer recordings to storage server (libssh2, ed25519 PKI) |
| EDL generation | Automatic edit lists from audio cues — replaces manual timecode entry |
| Tone sync | AAudio speaker output with `clock_nanosleep` scheduling |
| GPS sidecar | Recording metadata with coordinates for trim arithmetic |
| Web services | Apache Axis2/C HTTP/2 JSON-RPC |
| Service dispatch | Weak symbol registry |
| Security | HTTPS + mTLS (mutual TLS) |
| AI integration | MCP (Model Context Protocol) — 15 tools for Claude Desktop |
| License | **Apache 2.0** (all deps permissive) |

## Primary Use Case

Record room audio during a multi-camera shoot. Kanaha Audio detects when the performance starts (saxophone solo), finds spoken cues ("next slide please") for slide inserts, and detects when it ends (drum solo). The [EDL generator](https://github.com/robertlazarski/kanaha-audio/blob/main/docs/EDL.md) turns these timestamps into an ffmpeg edit list — replacing the manual timecode entry in [parseLTC.sh](https://github.com/robertlazarski/kanaha).

Also works standalone: find "next slide please" timestamps in any WAV recording to automate third-camera priority cuts in video editing.

```bash
curl -sk --http2 \
  --cert client.crt --key client.key --cacert ca.crt \
  -H "Content-Type: application/json" \
  -d '{"action":"searchKeywords","audio_file":"presentation.wav","keywords":["next slide please"]}' \
  "https://$PHONE:8443/services/AudioSearchService/searchKeywords"
```

Response:
```json
{
  "success": true,
  "matches": [
    {"keyword": "next slide please", "start_ms": 45230, "end_ms": 46100, "confidence": 0.92},
    {"keyword": "next slide please", "start_ms": 128450, "end_ms": 129100, "confidence": 0.88}
  ],
  "total_matches": 2,
  "audio_duration_ms": 3600000,
  "processing_time_ms": 45200
}
```

## Multi-Device Workflow

The phones are servers. Your laptop is the client. Bash scripts on a Linux desktop orchestrate multiple phones over WiFi — the opposite of the usual "phone app with a cloud backend" pattern. The app itself is minimal (one button); all the real work happens from curl and bash.

```bash
# Laptop discovers phones via mDNS automatically:
./tools/test-audio-workflow.sh workflow

# Or specify IPs directly:
# A budget Android phone (Moto G Play 2024) records room audio via built-in mic
# Pixel records 4K video + LTC/SMPTE timecode on ch1 via iRig Pro I/O mono
# All traffic is mTLS (mutual TLS) — both client and server certificates
# required. The script passes certs to curl automatically.
./tools/test-audio-workflow.sh workflow \
  --audio-device 192.168.8.126 --video-device 192.168.8.159

# What happens:
#   1. Both devices start recording simultaneously
#      (GPS sidecar timestamps sync the two devices — no LTC needed
#      on the Moto since it records room audio for edit cues, not timecode)
#   2. You play audio cues (music, speech, music)
#   3. Script analyzes the Moto room recording on-device:
#      - whisper.cpp finds "next slide please" at 60.0s and 62.0s
#      - YAMNet finds Music starting at 30.2s, ending at 99.4s
#   4. Script generates an ffmpeg EDL and assembles the final video
```

[Kanaha Audio](https://github.com/robertlazarski/kanaha-audio) and [Kanaha Camera Control](https://github.com/robertlazarski/kanaha) are two applications built on the same [mod_axis2](https://axis.apache.org/axis2/c/core/) foundation — same HTTP/2 JSON-RPC dispatch, same mTLS security, same curl-based scripting. Kanaha Camera Control adds 9 video operations. Kanaha Audio adds 14 audio operations. Both expose MCP tools for Claude. The framework scales to new services without changing the architecture.

See [test-audio-workflow.sh](https://github.com/robertlazarski/kanaha-audio/blob/main/tools/test-audio-workflow.sh), [generate-edl.sh](https://github.com/robertlazarski/kanaha-audio/blob/main/tools/generate-edl.sh), and [EDL.md](https://github.com/robertlazarski/kanaha-audio/blob/main/docs/EDL.md).

## Kanaha Audio Architecture

```
curl → HTTPS/HTTP2+mTLS → Apache httpd → Apache Axis2/C → audio_search_service.c
  → "searchKeywords"    → whisper.cpp     → keyword timestamps
  → "detectAudioEvents" → TFLite/YAMNet   → instrument/event detection
  → "startRecording"    → AAudio input    → WAV file + GPS sidecar
  → "stopRecording"     → finalize WAV    → duration + file size
  → "playTone"          → AAudio output   → speaker (clock_nanosleep sync)
  → "speak"             → flite           → speaker (spoken read-back)
  → "decodeLTC"         → libltc          → SMPTE timecode frames as JSON
  → "sftpTransfer"      → libssh2         → file transfer to storage server
  → "getStatus"         → recording state + GPS + model info
```

No Intent IPC needed (unlike [Kanaha Camera](https://github.com/robertlazarski/kanaha)). All dependencies are permissive C/C++ libraries that link directly into the same native process — two ML models, microphone recording, tone playback, and GPS metadata through one Apache Axis2/C service. No JNI, no Java audio stack, no serialization overhead.

## Kanaha Audio API Operations

| Operation | Description |
|-----------|-------------|
| `searchKeywords` | Find keyword timestamps in audio file |
| `transcribe` | Full transcription with word-level timestamps |
| `detectAudioEvents` | YAMNet audio event detection (521 AudioSet classes) |
| `startRecording` | Record from microphone to WAV (supports `start_at` scheduling) |
| `stopRecording` | Stop recording, finalize WAV, return duration |
| `playTone` | Sine wave through speaker (supports `start_at` for multi-device sync) |
| `speak` | Say a line of text on this device's speaker (flite, in-process, no network) |
| `listRecordings` | List WAV recordings with sizes and durations |
| `getStatus` | Model state, recording state, GPS location |
| `listModels` | Available whisper models on device |
| `loadModel` | Load/switch whisper model |
| `listAudioFiles` | List processable audio files |
| `playAudio` | Play a WAV file through the device speaker |
| `decodeLTC` | Decode SMPTE/LTC timecode from WAV (libltc) |
| `sftpTransfer` | Transfer audio files to storage server via SFTP (libssh2) |

## Kanaha Audio MCP Support

Kanaha Audio exposes all 15 operations as MCP tools, so Claude can record audio, search for keywords, detect instruments, and decode timecode directly. See [MCP.md](https://github.com/robertlazarski/kanaha-audio/blob/main/docs/MCP.md) for Claude Desktop configuration.

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | \
  adb -s 192.168.8.126:5555 shell run-as org.kanaha.audio ./files/kanaha-audio-mcp
```

`run-as` is required, not optional: the app data directory is `drwx------`, so
the adb shell user has no traversal into it wherever the binary sits. Works the
same over WiFi (`adb connect <ip>:5555`) as over USB.

Also useful for finding misplaced phones. During development, a Pixel went missing. "Claude, find my phone" → Claude called `playTone` in a loop until the 1kHz beep was traced to a couch cushion. Not its intended purpose, but `playTone` doesn't judge.

## Device Discovery

```bash
# mDNS (instant, requires avahi-utils)
./tools/kanaha-audio-discover.sh

# Direct IP
./tools/kanaha-audio-discover.sh --ip 192.168.1.100

# JSON output
./tools/kanaha-audio-discover.sh --json
```

## License

Apache License 2.0. See [LICENSE](https://github.com/robertlazarski/kanaha-audio/blob/main/LICENSE).

All dependencies use permissive or weak-copyleft licenses. Code can flow upstream to Apache Axis2/C directly. Note: libltc is LGPL-3.0 (statically linked — `build-android.sh` produces object files for relinking per LGPL-3.0 §4).

| Dependency | License |
|-----------|---------|
| whisper.cpp | MIT |
| ggml | MIT |
| TensorFlow Lite | Apache 2.0 |
| YAMNet model | Apache 2.0 |
| AAudio | Android system lib |
| Apache Axis2/C | Apache 2.0 |
| Apache httpd | Apache 2.0 |
| OpenSSL | Apache 2.0 |
| nghttp2 | MIT |
| json-c | MIT |
| libssh2 | BSD |
| flite | BSD |
| libltc | LGPL-3.0 |

## Documentation

| Document | Description |
|----------|-------------|
| [LEGAL.md](docs/LEGAL.md) | License analysis and compatibility |
| [SECURITY.md](docs/SECURITY.md) | Security model (mTLS) + HTTP/2 DoS hardening (CVE-2026-49975) |
| [PATH_B_HTTP2_MIGRATION.md](docs/PATH_B_HTTP2_MIGRATION.md) | How the server moved from a hand-rolled HTTP/1.1 server to real Apache + mod_http2 (genuine HTTP/2) |
| [WHISPER_MODELS.md](docs/WHISPER_MODELS.md) | Model selection guide (sizes, RAM, English-only vs multilingual) |
| [WHISPER_PERFORMANCE.md](docs/WHISPER_PERFORMANCE.md) | Performance expectations, workload sizing, implementation differences |
| [EDL.md](docs/EDL.md) | EDL generation — automatic edit lists from audio cues (replaces parseLTC.sh magic values) |
| [MCP.md](docs/MCP.md) | MCP (Model Context Protocol) — 14 audio tools for Claude Desktop |
| [ANDROID_CROSS_COMPILATION.md](docs/ANDROID_CROSS_COMPILATION.md) | Cross-compiling every dependency for arm64-v8a, and getting the two models onto the phone |
| [CPP_AND_JAVA_DESIGN.md](docs/CPP_AND_JAVA_DESIGN.md) | Why the audio path is C and the supervisor is Java, and where the line between them sits |
| [NOTICE](NOTICE) | Third-party attribution |
| [TRADEMARKS.md](TRADEMARKS.md) | Trademark acknowledgments |
