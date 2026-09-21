# Android ARM64 Cross-Compilation Guide — Kanaha Audio

This document provides instructions for cross-compiling all native dependencies for Kanaha Audio on Android ARM64 (aarch64). It covers both the speech model (whisper.cpp) and the audio event detection model (TensorFlow Lite + YAMNet).

## Overview

Kanaha Audio is a dual-model audio analysis service running on Android. Both ML models and all supporting libraries must be cross-compiled for ARM64 since the Android NDK does not include pre-built versions.

### What Gets Built

| Component | Purpose | Output | Size |
|-----------|---------|--------|------|
| whisper.cpp + ggml | Speech-to-text inference | libwhisper.a, libggml*.a | ~15 MB |
| TensorFlow Lite C API | ML inference runtime (YAMNet) | libtensorflowlite_c.a, libtensorflow-lite.a | ~10 MB |
| TFLite deps (XNNPACK, ruy, abseil, etc.) | Optimized CPU kernels + utilities | ~30 .a files | ~25 MB |
| FlatBuffers (host) | Schema compiler for TFLite build | flatc (host binary) | ~4.5 MB |

The shared dependencies (Apache httpd, Axis2/C, OpenSSL, nghttp2, APR, json-c) are documented in the [Kanaha Camera cross-compilation guide](../../kanaha/docs/ANDROID_CROSS_COMPILATION.md) and are reused by Kanaha Audio from the same `~/android-cross-builds/deps/arm64-v8a/` directory.

### Build Directory Structure

```
~/android-cross-builds/
├── deps/
│   └── arm64-v8a/
│       ├── include/
│       │   ├── whisper.h              # whisper.cpp C API
│       │   ├── ggml*.h               # ggml tensor library headers
│       │   └── tensorflow/
│       │       └── lite/
│       │           └── c/
│       │               ├── c_api.h    # TFLite C API (used by yamnet_bridge.c)
│       │               ├── c_api_types.h
│       │               ├── c_api_experimental.h
│       │               └── common.h
│       └── lib/
│           ├── libwhisper.a           # whisper.cpp
│           ├── libggml.a              # ggml (tensor computation)
│           ├── libggml-base.a
│           ├── libggml-cpu.a
│           ├── libtensorflowlite_c.a  # TFLite C API wrapper
│           ├── libtensorflow-lite.a   # TFLite core runtime
│           ├── libXNNPACK.a           # Optimized ARM64 NEON kernels
│           ├── libruy_*.a             # Matrix multiplication (ARM64 optimized)
│           ├── libabsl_*.a            # Abseil C++ utilities
│           ├── libflatbuffers.a       # FlatBuffer serialization
│           ├── libcpuinfo.a           # CPU feature detection
│           ├── libpthreadpool.a       # Thread pool for XNNPACK
│           ├── libfarmhash.a          # Hash functions
│           └── libfft2d_fftsg*.a      # FFT (for audio feature extraction)
├── whisper.cpp/                       # whisper.cpp source
├── tensorflow/                        # TensorFlow source (v2.16.1)
├── flatbuffers/                       # FlatBuffers source (v23.5.26)
│   └── build-host/
│       └── flatc                      # Host-native schema compiler
├── build-whisper.sh                   # whisper.cpp build script
└── build-tflite.sh                    # TFLite build script
```

> **Note:** The `deps/arm64-v8a/` directory is shared with Kanaha Camera. Both projects link against the same cross-compiled Apache httpd, Axis2/C, OpenSSL, and APR libraries. The whisper.cpp and TFLite libraries are specific to Kanaha Audio.

---

## Prerequisites

### NDK Installation

Ensure Android NDK r28 is installed:

```bash
# Install via Android Studio SDK Manager, or:
$HOME/Android/Sdk/cmdline-tools/latest/bin/sdkmanager "ndk;28.0.12916984"

# Verify NDK
ls $HOME/Android/Sdk/ndk/28.0.12916984/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
```

### Environment Variables

Add to `~/.bashrc`:

```bash
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/28.0.12916984
export NDK_ROOT=$ANDROID_NDK_HOME
export PATH=$ANDROID_NDK_HOME:$PATH
```

### System Dependencies

```bash
# Required for building
cmake --version   # 3.16+ required
git --version
make --version

# cmake 3.16+ is required for TFLite's FetchContent support
```

### Create Build Directory

```bash
mkdir -p ~/android-cross-builds/deps/arm64-v8a/{include,lib}
cd ~/android-cross-builds
```

---

## Step 1: Cross-Compile whisper.cpp

whisper.cpp provides speech-to-text inference for the `searchKeywords` and `transcribe` operations.

### Clone Source

```bash
cd ~/android-cross-builds
git clone --depth 1 https://github.com/ggerganov/whisper.cpp.git
```

### Build

The build script handles CMake configuration and installation:

```bash
./build-whisper.sh
```

Or manually:

```bash
cd ~/android-cross-builds/whisper.cpp
mkdir build-android-arm64 && cd build-android-arm64

cmake .. \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
    -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
    -DCMAKE_ANDROID_API=21 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/android-cross-builds/deps/arm64-v8a" \
    -DBUILD_SHARED_LIBS=OFF \
    -DWHISPER_BUILD_EXAMPLES=OFF \
    -DWHISPER_BUILD_TESTS=OFF \
    -DWHISPER_BUILD_SERVER=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_METAL=OFF \
    -DGGML_CUDA=OFF \
    -DGGML_VULKAN=OFF \
    -DGGML_BLAS=OFF \
    -DCMAKE_C_FLAGS="-fPIC" \
    -DCMAKE_CXX_FLAGS="-fPIC"

make -j$(nproc)
```

### Key CMake Flags

| Flag | Value | Why |
|------|-------|-----|
| `BUILD_SHARED_LIBS` | OFF | Static linking into the PIE executable |
| `WHISPER_BUILD_EXAMPLES` | OFF | Not needed on device |
| `GGML_OPENMP` | OFF | Android NDK lacks OpenMP; we use n_threads=4 instead |
| `GGML_METAL/CUDA/VULKAN` | OFF | Not available on Android ARM64 |
| `CMAKE_C_FLAGS="-fPIC"` | — | Required for linking into position-independent executables |

### Install

```bash
DEPS=$HOME/android-cross-builds/deps/arm64-v8a

# Headers
cp whisper.cpp/include/whisper.h $DEPS/include/
find whisper.cpp/ggml/include -name "*.h" -exec cp {} $DEPS/include/ \;

# Libraries
find build-android-arm64 -name "*.a" -exec cp {} $DEPS/lib/ \;
```

### Verify

```bash
ls $DEPS/lib/libwhisper.a $DEPS/lib/libggml*.a
ls $DEPS/include/whisper.h $DEPS/include/ggml.h
```

---

## Step 2: Cross-Compile TensorFlow Lite C API

TensorFlow Lite provides the inference runtime for YAMNet audio event detection via the `detectAudioEvents` operation.

### Why TF v2.16.1

The latest TF HEAD often has build issues with cross-compilation (FlatBuffers version mismatches, API changes in XNNPACK delegates). TF v2.16.1 is the latest stable release that builds cleanly with NDK r28 and CMake 3.16+.

### Clone Source

```bash
cd ~/android-cross-builds
git clone --depth 1 --branch v2.16.1 https://github.com/tensorflow/tensorflow.git
```

This is a shallow clone (~500 MB). The full TF repo is much larger — shallow clone is sufficient for building TFLite.

### Build Host FlatBuffers Compiler

TFLite's cross-compilation requires a host-native `flatc` binary to generate FlatBuffer schema headers during the build. TF v2.16.1 requires FlatBuffers v23.5.26 specifically.

```bash
cd ~/android-cross-builds
git clone --depth 1 --branch v23.5.26 https://github.com/google/flatbuffers.git
mkdir -p flatbuffers/build-host && cd flatbuffers/build-host

cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DFLATBUFFERS_BUILD_TESTS=OFF \
    -DFLATBUFFERS_BUILD_FLATHASH=OFF

make -j$(nproc) flatc

# Verify
./flatc --version
# Expected: flatc version 23.5.26
```

> **Why a specific version?** TFLite generates FlatBuffer headers during the build and validates that the `flatc` binary version matches the FlatBuffer library version compiled into TFLite. A version mismatch causes a compile-time static assertion failure: `Non-compatible flatbuffers version included`.

### Build TFLite

The build script handles everything:

```bash
cd ~/android-cross-builds
./build-tflite.sh
```

Or manually:

```bash
cd ~/android-cross-builds/tensorflow
mkdir build-android-arm64 && cd build-android-arm64

FLATC_DIR=$HOME/android-cross-builds/flatbuffers/build-host

cmake ../tensorflow/lite/c \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
    -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
    -DCMAKE_ANDROID_API=21 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DTFLITE_C_BUILD_SHARED_LIBS=OFF \
    -DTFLITE_ENABLE_XNNPACK=ON \
    -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_NNAPI=OFF \
    -DTFLITE_ENABLE_EXTERNAL_DELEGATE=OFF \
    -DTFLITE_HOST_TOOLS_DIR="$FLATC_DIR" \
    -DCMAKE_C_FLAGS="-fPIC" \
    -DCMAKE_CXX_FLAGS="-fPIC"

make -j$(nproc)
```

Build time: ~5-15 minutes depending on CPU cores.

### Key CMake Flags

| Flag | Value | Why |
|------|-------|-----|
| `TFLITE_C_BUILD_SHARED_LIBS` | OFF | Static .a libraries for linking into PIE executable |
| `TFLITE_ENABLE_XNNPACK` | ON | Optimized ARM64 NEON kernels — ~2-3x faster inference |
| `TFLITE_ENABLE_GPU` | OFF | GPU delegate needs OpenCL; YAMNet is fast enough on CPU |
| `TFLITE_ENABLE_NNAPI` | OFF | NNAPI adds complexity; not needed for ~10ms YAMNet frames |
| `TFLITE_HOST_TOOLS_DIR` | path to flatc | Required for cross-compilation (flatc generates headers) |
| `CMAKE_POSITION_INDEPENDENT_CODE` | ON | Required for PIE executables on Android |

### Install

```bash
DEPS=$HOME/android-cross-builds/deps/arm64-v8a

# Headers
mkdir -p $DEPS/include/tensorflow/lite/c
for h in c_api.h c_api_types.h c_api_experimental.h common.h; do
    cp tensorflow/tensorflow/lite/c/$h $DEPS/include/tensorflow/lite/c/
done

# ALL static libraries (TFLite + XNNPACK + ruy + abseil + helpers)
find build-android-arm64 -name "*.a" -type f -exec cp {} $DEPS/lib/ \;
```

### Verify

```bash
# Headers
ls $DEPS/include/tensorflow/lite/c/c_api.h

# Key libraries
ls $DEPS/lib/libtensorflowlite_c.a    # C API wrapper (264K)
ls $DEPS/lib/libtensorflow-lite.a       # Core runtime (9.3M)
ls $DEPS/lib/libXNNPACK.a              # ARM64 NEON kernels
```

### What Gets Linked

The TFLite dependency chain for static linking:

```
yamnet_bridge.c
  → libtensorflowlite_c.a      (C API wrapper)
    → libtensorflow-lite.a      (core runtime: kernels, interpreter, graph)
      → libXNNPACK.a            (optimized ARM64 NEON compute kernels)
      → libpthreadpool.a        (thread pool for XNNPACK parallelism)
      → libcpuinfo.a            (ARM64 feature detection: NEON, FP16, etc.)
      → libruy_*.a              (matrix multiplication, ARM64 optimized)
      → libabsl_*.a             (abseil C++ utilities: strings, status, sync)
      → libflatbuffers.a        (FlatBuffer deserialization for .tflite models)
      → libfarmhash.a           (hash functions for op resolution)
      → libfft2d_fftsg*.a       (FFT for audio feature extraction kernels)
      → libeight_bit_int_gemm.a (quantized integer matrix multiply)
```

All of these must be on the link line. The `CMakeLists.txt` and `build-android.sh` in the Kanaha Audio source already include the complete list.

---

## Step 3: Download YAMNet Model

The YAMNet `.tflite` model file is not compiled — it's downloaded and pushed to the device.

```bash
# Download from TensorFlow Hub (Apache 2.0; the 521-class list is the AudioSet
# ontology, CC BY 4.0)
curl -L -o yamnet.tflite \
    "https://tfhub.dev/google/lite-model/yamnet/tflite/1?lite-format=tflite"

# Verify: 16 MB, and the file starts with the TFL3 magic
ls -lh yamnet.tflite                 # 16M, verified 2026-09-20
head -c 8 yamnet.tflite | xxd        # ....TFL3

# Push it where the app can read it. The app's own directory is drwx------,
# so go via /data/local/tmp and copy with run-as:
adb push yamnet.tflite /data/local/tmp/
adb shell run-as org.kanaha.audio cp /data/local/tmp/yamnet.tflite files/models/
adb shell rm /data/local/tmp/yamnet.tflite
```

The service auto-detects `yamnet.tflite` in the models directory at startup. If the file is not present, the service runs in whisper-only mode (the `detectAudioEvents` action returns an error, but all other operations work normally); `getStatus` reports which mode you are in as `yamnet_ready`.

**Measured cost of having it, on a Moto G Play 2024 (2026-09-20).** Detection
takes 37–68 ms for a 3.7 s clip, against 3.8 s for a whisper keyword search on
the same clip, because YAMNet is a small network over a 0.5 s hop. One MCP
process sits at 34 MB with YAMNet initialised, 137 MB once `tiny.en` loads, and
268 MB after both have run. Session start is unchanged. Adding the model does
not slow anything else down.

---

## Step 4: Download Whisper Model

```bash
# Download ggml-format model (English-only recommended for keyword search).
# tiny.en (78 MB) is the one to start with: on a Moto G Play 2024 it
# transcribes an 8 s clip in 3.8-4.5 s against base.en's (148 MB) 8.7-8.9 s.
curl -L -o ggml-tiny.en.bin \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"

# Push it, again via /data/local/tmp because the app directory is drwx------
adb push ggml-tiny.en.bin /data/local/tmp/
adb shell run-as org.kanaha.audio cp /data/local/tmp/ggml-tiny.en.bin files/models/
adb shell rm /data/local/tmp/ggml-tiny.en.bin
```

See [WHISPER_MODELS.md](WHISPER_MODELS.md) for model tier comparison and performance benchmarks on Pixel 9 Pro.

---

## Step 5: Build Kanaha Audio

With all dependencies cross-compiled and installed in `~/android-cross-builds/deps/arm64-v8a/`, you can build the Kanaha Audio native binary.

### Using build-android.sh (standalone httpd binary)

```bash
cd ~/repos/kanaha-audio
./build-android.sh
```

This produces `build-android/kanaha-audio-httpd` — a standalone PIE executable you can push to the phone and test with curl.

### Using Android Studio (APK with all three targets)

Open the project in Android Studio and build normally. The `CMakeLists.txt` conditionally links cross-compiled libraries for `arm64-v8a` builds.

Three targets are built:
1. `libkanaha-audio.so` — shared library (for potential JNI use)
2. `libkanaha_audio_httpd.so` — HTTP server executable (launched via ProcessBuilder)
3. `libkanaha_audio_mcp.so` — MCP stdio executable (launched by Claude Desktop)

### Verify on Device

This is the **standalone-binary** route: the server runs as the shell user out of
`/data/local/tmp`, with its own models and audio directories there. It is for
testing the C build without the APK. The **app** route is different — the app's
data directory is `drwx------`, so its models live in `files/models` and you put
them there with `run-as` (Steps 3 and 4). Do not mix the two paths up when a
model appears to be missing.

```bash
# Push the binary
adb push build-android/kanaha-audio-httpd /data/local/tmp/kanaha-audio/

# Push models
adb push ggml-base.en.bin /data/local/tmp/kanaha-audio/models/
adb push yamnet.tflite /data/local/tmp/kanaha-audio/models/

# Start the server
adb shell /data/local/tmp/kanaha-audio/kanaha-audio-httpd

# Test whisper (speech keyword search)
curl -s -H "Content-Type: application/json" \
    -d '{"action":"searchKeywords","audio_file":"/data/local/tmp/kanaha-audio/audio/test.wav","keywords":["next slide please"]}' \
    http://localhost:18080/services/AudioSearchService/searchKeywords

# Test YAMNet (audio event detection)
curl -s -H "Content-Type: application/json" \
    -d '{"action":"detectAudioEvents","audio_file":"/data/local/tmp/kanaha-audio/audio/love_supreme.wav","events":["Saxophone"],"threshold":0.5,"mode":"first_onset"}' \
    http://localhost:18080/services/AudioSearchService/detectAudioEvents
```

---

## Troubleshooting

### "required file not found" when running build-tflite.sh

CRLF line endings. Fix with:
```bash
sed -i 's/\r$//' build-tflite.sh
```

### FlatBuffers version mismatch

```
error: static assertion failed: Non-compatible flatbuffers version included
```

The host `flatc` version must match TF's expected FlatBuffers version. TF v2.16.1 requires FlatBuffers v23.5.26. Rebuild flatc from the correct tag:
```bash
cd ~/android-cross-builds
rm -rf flatbuffers
git clone --depth 1 --branch v23.5.26 https://github.com/google/flatbuffers.git
# Then rebuild as shown in Step 2
```

### "Please specify where those binaries can be found by using -DTFLITE_HOST_TOOLS_DIR"

The TFLite CMake build needs a host-native `flatc` binary. Ensure the `TFLITE_HOST_TOOLS_DIR` flag points to the directory containing `flatc`:
```bash
-DTFLITE_HOST_TOOLS_DIR=$HOME/android-cross-builds/flatbuffers/build-host
```

### Undefined symbols during final link

If you get undefined symbols from TFLite dependencies (ruy, abseil, XNNPACK), ensure all `.a` files from the TFLite build are installed. The build produces ~88 static libraries. Run:
```bash
find ~/android-cross-builds/tensorflow/build-android-arm64 -name "*.a" -type f \
    -exec cp {} ~/android-cross-builds/deps/arm64-v8a/lib/ \;
```

### XNNPACK compilation errors on TF HEAD

Use a stable TF release tag instead of HEAD. TF HEAD frequently has build issues with cross-compilation due to in-progress refactoring. TF v2.16.1 is known to work.

### Android 15 (API 35) ELF alignment

Android 15 requires 16KB page-aligned ELF segments. Both `CMakeLists.txt` and `build-android.sh` include the required linker flags:
```
-Wl,-z,max-page-size=16384
-Wl,-z,separate-loadable-segments
```

---

## Quick Reference

### Environment Variables

| Variable | Value | Used by |
|----------|-------|---------|
| `ANDROID_NDK_HOME` | `$HOME/Android/Sdk/ndk/28.0.12916984` | All cross-compilation |
| `DEPS` | `$HOME/android-cross-builds/deps/arm64-v8a` | Install target |
| `CC` | `$TOOLCHAIN/bin/aarch64-linux-android21-clang` | C compiler |
| `CXX` | `$TOOLCHAIN/bin/aarch64-linux-android21-clang++` | C++ compiler (linking) |

### Build Scripts

| Script | What it builds | Time |
|--------|---------------|------|
| `build-whisper.sh` | whisper.cpp + ggml for ARM64 | ~2-3 min |
| `build-tflite.sh` | TFLite C API + all deps for ARM64 | ~5-15 min |
| `build-android.sh` | Kanaha Audio httpd binary | ~10 sec |

### Library Inventory

After all builds complete:
```bash
# Count Kanaha Audio-specific libraries
ls ~/android-cross-builds/deps/arm64-v8a/lib/libwhisper.a       # whisper.cpp
ls ~/android-cross-builds/deps/arm64-v8a/lib/libggml*.a         # ggml (4 files)
ls ~/android-cross-builds/deps/arm64-v8a/lib/libtensorflow*.a   # TFLite (2 files)
ls ~/android-cross-builds/deps/arm64-v8a/lib/libXNNPACK.a       # XNNPACK
ls ~/android-cross-builds/deps/arm64-v8a/lib/libruy_*.a         # ruy (~20 files)
ls ~/android-cross-builds/deps/arm64-v8a/lib/libabsl_*.a        # abseil (~40 files)
```

### Licenses

All cross-compiled dependencies are permissive:

| Dependency | License |
|-----------|---------|
| whisper.cpp + ggml | MIT |
| TensorFlow Lite | Apache 2.0 |
| XNNPACK | BSD |
| FlatBuffers | Apache 2.0 |
| Abseil | Apache 2.0 |
| ruy | Apache 2.0 |
| YAMNet model | Apache 2.0 |

No GPL dependencies. The entire Kanaha Audio stack (including all cross-compiled libraries) is Apache 2.0 compatible.
