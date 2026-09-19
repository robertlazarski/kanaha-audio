# C/C++ and Java in Kanaha Audio: where Java starts and where it ends

Kanaha Audio is a C program that happens to ship inside an APK. The Java in the
repository is about 1,000 lines across four files, and its job is to satisfy
Android so that a native process may exist and hold the microphone. Everything
the app is *for* — microphone capture, whisper.cpp, YAMNet, timecode, the
Axis2/C service, the MCP server — is C and C++ in one process with no JNI.

This document draws the line precisely: what Android forces through Java, what
is in Java today only for convenience, and how the C side reaches whisper,
TensorFlow Lite and any future C/C++ library with an ordinary function call.

## The seam is a process boundary, not JNI

The two halves never share an address space.

```
Java (ART process, org.kanaha.audio)          C (child process, same UID)
──────────────────────────────────────         ─────────────────────────────────
MainActivity                                   libkanaha_audio_httpd.so  (= Apache httpd -X)
  runtime permission prompts                     mod_ssl + mod_http2 + mod_axis2
  FLAG_KEEP_SCREEN_ON                            axis2_json_rpc_msg_recv
  start/stop the service                           └─ android_static_service_lookup("AudioSearchService")
AudioService (foreground, type=microphone)            └─ audio_search_service_invoke_json      (adapter, strong symbol)
  startForeground + notification                          └─ audio_search_service_invoke_json_impl
  wake lock                                                   ├─ whisper_bridge_*      → whisper.h   (C API over C++)
  deploy config, deploy MCP binary                            ├─ yamnet_bridge_*       → TfLite C API
  CertProvisioning (keypair + CSR)                            ├─ audio_recording_*     → AAudio
  ProcessBuilder ──exec──────────────────────▶                ├─ ltc_decode_*          → libltc
  read child stdout → logcat                                  └─ audio_sftp_*          → SFTP client
NetworkDiscoveryService (NsdManager mDNS)
                                               libkanaha_mcp.so → files/kanaha-audio-mcp
                                                 same service code, JSON-RPC over stdio
```

Java starts C. It passes an argument list and an environment, and reads the
child's standard output. There is no `System.loadLibrary`, no `native` method,
no JNI environment pointer anywhere in the app. The C side never calls back
into Java either: results go out as HTTP responses or MCP replies.

This is the opposite of the usual Android pattern, where Java is the program
and C is a library it calls into. Here C is the program and Java is the
supervisor Android requires.

## The Java layer, file by file

| File | Lines | What it does | Required by Android? |
|---|---|---|---|
| `AudioService.java` | ~525 | Foreground service. `startForeground` with a notification, wake lock, deploys the Apache config set and the MCP binary, runs `CertProvisioning`, launches the httpd child with `ProcessBuilder`, mirrors its stdout to logcat, registers mDNS. | The service, the notification and the wake lock: **yes**. The deploy steps and stdout mirror: **no**, convenience. |
| `MainActivity.java` | ~204 | Requests `RECORD_AUDIO` (and `POST_NOTIFICATIONS` on Android 13+), holds `FLAG_KEEP_SCREEN_ON`, toggles the service, warns when the port is taken, shows the device IP. | The permission prompts: **yes**. The rest: **no**. |
| `NetworkDiscoveryService.java` | ~188 | Advertises `_https._tcp` with `api=kanaha-audio-search` in the TXT record through `NsdManager`. | **No.** A small C mDNS responder would do the same. |
| `CertProvisioning.java` | ~136 | Mints the device RSA keypair and PKCS#10 CSR on first run (BouncyCastle), reports whether a CA-signed cert has been pushed back. The key never leaves the device. | **No.** The httpd already links OpenSSL; C could mint the same files. |

The launch itself, from `AudioService.launchNativeProcess()`:

```
<nativeLibraryDir>/libkanaha_audio_httpd.so -f <files>/apache/conf/httpd.conf -d <files>/apache -X
  env: LD_LIBRARY_PATH=<nativeLibraryDir>  HOME=<files>  KANAHA_AUDIO_MODELS=<files>/models
```

`-X` is single-process mode, which is what mod_axis2 expects on Android (no
shared memory, pool allocator). The models directory is the only piece of
application state Java hands to C, and it does so through the environment.

### Why the httpd binary is called `libkanaha_audio_httpd.so`

It is not a shared library. Android extracts files under `jniLibs/<abi>/` to a
directory the app can execute from, and nothing else in the APK gets that
treatment. Naming an executable `lib*.so` is the packaging trick that puts it on
disk at install time. `libkanaha_mcp.so` arrives the same way and is copied to
the stable path `files/kanaha-audio-mcp` by `deployMcpBinary()`, which writes a
temp file and renames it over the old one so a running MCP session keeps its
inode and a same-size update is never skipped.

## What Android forces through Java, and why it cannot be avoided

These are the items that keep the Java layer from being zero. Each is a
platform contract that only a Java component can sign.

**An entry point.** An APK needs a manifest and at least one component the
system can start. The process Android creates is an ART process. A
`NativeActivity` can avoid Java for an *activity*, but there is no native
equivalent of a `Service`, and this app needs a service, not an activity.

**A foreground service with the microphone type.** The manifest declares
`android:foregroundServiceType="microphone"` and `onStartCommand` calls
`startForeground` immediately, because the system kills a service that does not
post its notification within a few seconds. This single call does three things
the native child cannot do for itself: it keeps the app out of the cached-app
freezer, it entitles the app to the microphone while the service runs, and it
gives the user the persistent notification Android requires for an app that
records.

**Permission context for the child.** Android checks microphone access by UID
and app-op, not by process. The child launched by `ProcessBuilder` runs under
the app's UID, so it inherits both the granted `RECORD_AUDIO` permission and
the foreground process state that the app-op requires. That is the meaning of
"provides `RECORD_AUDIO` permission context" in the service's header comment.
The same fact is why `adb shell run-as org.kanaha.audio ./files/kanaha-audio-mcp`
can record: `run-as` spawns the binary under the app's UID too, and the
microphone is allowed as long as the app is in the foreground.

**The runtime permission prompts.** `RECORD_AUDIO` and, since Android 13,
`POST_NOTIFICATIONS` must be requested through an Activity. There is no native
API for this.

**The wake lock.** `PowerManager` is a Java API.

Everything else in the Java layer exists because it was convenient to write
there first. It could move to C without changing the architecture.

## How the C side calls whisper, YAMNet and the rest

The single most useful property of this design is that once the process is
native, every C or C++ library is a function call away. No marshalling, no
JNI signatures, no copying buffers across a VM boundary.

The chain for one request:

1. httpd accepts the HTTP/2 request; mod_axis2 hands the JSON body to
   `axis2_json_rpc_msg_recv`.
2. On Android there is no `dlopen`, so the receiver asks the static registry
   for `AudioSearchService` and gets `audio_search_service_invoke_json`, the
   strong symbol in `axis2_static_service_adapter.c` that overrides the weak
   stub in Axis2/C core. (See `HTTP2_ANDROID.md` in axis2-c-core for the weak
   symbol mechanism.)
3. The adapter converts the json-c object to a string and calls
   `audio_search_service_invoke_json_impl()`, which dispatches on the `action`
   field: `searchKeywords`, `transcribe`, `detectAudioEvents`, `startRecording`,
   `decodeLTC`, `sftpTransfer`, and so on.
4. Each action calls a bridge, and each bridge is a thin C file over one
   library's C API:

| Bridge | Library | API surface used | Language of the library |
|---|---|---|---|
| `whisper/whisper_android_bridge.c` | whisper.cpp | `whisper.h` | C++ behind a C header |
| `yamnet/yamnet_bridge.c` | TensorFlow Lite | `TfLiteModelCreateFromFile`, `TfLiteInterpreterInvoke` | C++ behind the TFLite C API |
| `recording/audio_recording.c` | AAudio | `AAudioStreamBuilder_*`, data callback | Android NDK, C |
| `ltc/ltc_decoder.c` | libltc | `ltc_decoder_*` | C |
| `sftp/audio_sftp.c` | SFTP client | its C API | C |

The bridges are also where the concurrency rule lives. httpd runs a threaded
MPM and mod_axis2 does not serialise invocations, so `whisper_android_bridge.c`
holds one mutex around every entry point that touches the model context. A new
bridge copies that pattern.

C++ never appears in the service or bridge sources. It enters the binary only
because whisper.cpp and TensorFlow Lite are written in it; the final link uses
`clang++` with `-static-libstdc++` so the C++ runtime is inside the executable
and the C code above it never sees it. That is what "no JNI" means in
practice: the boundary between the service and a C++ library is a C header,
which costs nothing, rather than a VM boundary, which costs a marshalling layer
and a second language.

The compute code has no Android in it beyond `__android_log_print` and AAudio.
`main.c` compiles with `fprintf` fallbacks off-Android, and the whisper, YAMNet
and LTC bridges build on a Linux host unchanged, which is where their logic is
tested.

## The MCP binary: zero Java at runtime

`kanaha_mcp_main.c` and `kanaha_mcp.c` link the same service and bridge objects
as the httpd binary and speak JSON-RPC 2.0 over stdin and stdout. A client
launches it directly:

```
adb -s <serial> shell -T run-as org.kanaha.audio ./files/kanaha-audio-mcp
```

No httpd, no TLS, no provisioning and no Java are involved in that path. Java's
only contribution was copying the binary into `files/` once. The same
`audio_search_service_invoke_json_impl()` serves both front doors, so a tool
added for HTTP is automatically an MCP tool once its catalog entry exists.

## Adding another C/C++ library

The recipe is the same every time, and it is the reason the design scales to
libraries that did not exist when the app was written:

1. Cross-compile the library as a static archive with the NDK toolchain into
   the deps prefix. If it vendors ggml (llama.cpp does, whisper.cpp does), build
   one ggml and point both at it, or the link will see two copies.
2. Write `<lib>/<lib>_bridge.c` over the library's C API: init from the models
   directory, lazy load, one mutex, a status function, and the operation.
3. Add an `action` branch in `audio_search_service.c` that parses the request,
   calls the bridge, and writes escaped JSON into the fixed response buffer.
4. Add a schema constant and a table row in `kanaha_mcp.c`.
5. Add the archive to both link lines in `build-android.sh`.

Nothing in the Java layer changes unless the library needs a new environment
variable. A language model through llama.cpp is the next planned instance of
this recipe: `llama.h` is a C API, the grammar sampler takes a GBNF string,
and the bridge is the whisper bridge with a different header.

## What JNI would have cost

For comparison, the conventional design would keep the app in Java and load the
DSP as a shared library. Every operation would then need a `native` method
declaration, a matching C function with the JNI signature, explicit conversion
of every argument and result across the VM boundary, care with local and global
references, and a threading model that respects the VM. Audio buffers would be
copied at least once per crossing. The C code would be shaped by Java's needs
and would not build on a server.

The process-boundary design pays instead with a foreground-service supervisor
of a thousand lines that never changes, and gets a C program that is the same
program on the phone and on a Linux host.

## Summary: the boundary in one table

| Concern | Lives in | Because |
|---|---|---|
| Existing as an app, manifest, entry point | Java | Only Java components can be started by the system |
| Foreground service, notification, microphone entitlement | Java | `startForeground` and `foregroundServiceType` have no native equivalent |
| Runtime permission prompts | Java | Activity API only |
| Wake lock | Java | `PowerManager` API only |
| Launching the native process | Java | Java is the entry point; `ProcessBuilder` is the natural tool |
| Config and binary deployment | Java today | Convenience; movable to C |
| Certificate keypair and CSR | Java today | Convenience; OpenSSL is already linked |
| mDNS advertisement | Java today | Convenience; movable to C |
| HTTP/2, TLS, mTLS, routing | C | Apache httpd and mod_axis2 |
| Microphone capture | C | AAudio is a native-first API |
| whisper.cpp, YAMNet, LTC, SFTP | C over C/C++ APIs | Direct function calls in one process |
| MCP server | C | Same service code, stdio transport, no Java at runtime |
| Anything added later that is C or C++ | C | Link it and call it |

Java starts at the manifest and ends at `ProcessBuilder.start()`. Everything
after that is a Unix process that happens to be Apache httpd.
