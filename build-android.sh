#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
#
# Build Kanaha Audio native httpd binary for Android ARM64
# Produces a standalone executable that can be pushed to the phone via adb
# and tested with curl from the laptop.
#
# Usage: ./build-android.sh
# Output: build-android/kanaha-audio-httpd

set -euo pipefail

# NDK and toolchain (same as Kanaha Camera)
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/28.0.12916984
TOOLCHAIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
CC=$TOOLCHAIN/bin/aarch64-linux-android28-clang
CXX=$TOOLCHAIN/bin/aarch64-linux-android28-clang++
AR=$TOOLCHAIN/bin/llvm-ar

# Dependencies (all cross-compiled static libs)
DEPS=$HOME/android-cross-builds/deps/arm64-v8a

# Source directories
SRC=$HOME/repos/kanaha-audio/kanaha-audio-app/app/src/main/cpp

# Output
BUILD=$HOME/repos/kanaha-audio/build-android
mkdir -p "$BUILD"

echo "=== Building Kanaha Audio httpd for Android ARM64 ==="
echo "NDK: $ANDROID_NDK_HOME"
echo "Deps: $DEPS"
echo "Sources: $SRC"

# Common flags
CFLAGS="-O2 -fPIE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"
CFLAGS="$CFLAGS -DANDROID -DAPACHE_HTTPD_ANDROID"
CFLAGS="$CFLAGS -DAXIS2_JSON_ENABLED=1 -DWITH_NGHTTP2=1 -DWITH_OPENSSL=1"
CFLAGS="$CFLAGS -DUSE_CROSS_COMPILED_LIBS=1"
# -fsigned-char: axis2_char_t is plain char, signed on x86 and unsigned on
# arm64. The Axis2/C libraries are built with -fsigned-char (see its
# configure.ac), so these sources must match or the same char behaves
# differently either side of the call.
CFLAGS="$CFLAGS -fsigned-char"

# Include paths
INCLUDES="-I$SRC"
INCLUDES="$INCLUDES -I$DEPS/include"
INCLUDES="$INCLUDES -I$DEPS/include/apr-1"
INCLUDES="$INCLUDES -I$DEPS/include/axis2-2.0.0"
INCLUDES="$INCLUDES -I$DEPS/include/axis2-2.0.0/platforms/unix"
INCLUDES="$INCLUDES -I$DEPS/include/json-c"
INCLUDES="$INCLUDES -I$DEPS/include/nghttp2"
INCLUDES="$INCLUDES -I$DEPS/include/openssl"
INCLUDES="$INCLUDES -I$DEPS/include"

# Compile each C source file
echo "--- Compiling C sources ---"
OBJECTS=()
for src in \
    "$SRC/main.c" \
    "$SRC/audio_util.c" \
    "$SRC/apache-httpd/apache_httpd_android.c" \
    "$SRC/axis2c/audio_search_service.c" \
    "$SRC/axis2c/axis2_static_service_adapter.c" \
    "$SRC/whisper/whisper_android_bridge.c" \
    "$SRC/yamnet/yamnet_bridge.c" \
    "$SRC/recording/audio_recording.c" \
    "$SRC/recording/audio_tone.c" \
    "$SRC/speech/audio_speak.c" \
    "$SRC/recording/audio_sidecar.c" \
    "$SRC/recording/gps_reader.c" \
    "$SRC/sftp/audio_sftp.c" \
    "$SRC/ltc/ltc_decoder.c"; do
    obj="$BUILD/$(basename "${src%.c}.o")"
    echo "  CC $src"
    "$CC" $CFLAGS $INCLUDES -c "$src" -o "$obj"  # shellcheck: flags are intentionally word-split
    OBJECTS+=("$obj")
done

# Link everything into a single PIE executable
echo "--- Linking ---"
"$CXX" -fPIE -pie \
    "${OBJECTS[@]}" \
    -L"$DEPS/lib" \
    -Wl,--whole-archive -laxis2_engine -Wl,--no-whole-archive \
    -Wl,--export-dynamic \
    -Wl,-z,max-page-size=16384 \
    -Wl,-z,separate-loadable-segments \
    -laxis2_http_common -laxis2_http_util \
    -laxis2_axiom -laxis2_parser -lguththila -lneethi -laxutil \
    -laprutil-1 -lapr-1 -lexpat \
    -ljson-c \
    -lnghttp2 \
    -lltc \
    -lssh2 \
    -lssl -lcrypto \
    -lwhisper -lggml -lggml-cpu -lggml-base \
    -ltensorflowlite_c -ltensorflow-lite \
    -lXNNPACK -lpthreadpool -lcpuinfo \
    -lflatbuffers -lfarmhash -lfft2d_fftsg -lfft2d_fftsg2d -leight_bit_int_gemm \
    -lruy_frontend -lruy_context -lruy_context_get_ctx -lruy_ctx \
    -lruy_trmul -lruy_block_map -lruy_kernel_arm -lruy_pack_arm \
    -lruy_apply_multiplier -lruy_prepare_packed_matrices \
    -lruy_allocator -lruy_prepacked_cache -lruy_system_aligned_alloc \
    -lruy_tune -lruy_cpuinfo -lruy_thread_pool \
    -lruy_blocking_counter -lruy_wait -lruy_denormal \
    -lruy_profiler_instrumentation \
    -lflite_cmu_us_kal16 -lflite_usenglish -lflite_cmulex -lflite \
    -laaudio \
    -llog -lz -lm -ldl \
    -static-libstdc++ \
    -o "$BUILD/kanaha-audio-httpd"

echo "=== httpd build complete ==="
ls -lh "$BUILD/kanaha-audio-httpd"

STRIP=$TOOLCHAIN/bin/llvm-strip
OUTPUT_DIR=$HOME/repos/kanaha-audio/kanaha-audio-app/app/src/main/jniLibs/arm64-v8a
mkdir -p "$OUTPUT_DIR"

# NOTE: kanaha-audio-httpd is NOT installed into jniLibs.
#
# It is the old hand-rolled OpenSSL server (apache_httpd_android.c), kept only
# as a desktop test CLI. The app is served by real Apache, built separately by
# build-httpd-audio.sh, which writes the same libkanaha_audio_httpd.so path.
#
# This script used to copy it over that file. The result was an app that had
# silently reverted to HTTP/1.1 with a server rejecting the Apache arguments
# AudioService passes it -- and because the .so is gitignored, nothing recorded
# the swap. If you want to refresh the app server, run build-httpd-audio.sh.
echo "=== httpd built at $BUILD/kanaha-audio-httpd (desktop CLI; not installed) ==="

# Build MCP binary (Claude Desktop stdio transport)
echo ""
echo "=== Building Kanaha Audio MCP binary ==="

MCP_OBJECTS=()
for src in \
    "$SRC/axis2c/kanaha_mcp_main.c" \
    "$SRC/axis2c/kanaha_mcp.c" \
    "$SRC/audio_util.c" \
    "$SRC/axis2c/audio_search_service.c" \
    "$SRC/whisper/whisper_android_bridge.c" \
    "$SRC/yamnet/yamnet_bridge.c" \
    "$SRC/recording/audio_recording.c" \
    "$SRC/recording/audio_tone.c" \
    "$SRC/speech/audio_speak.c" \
    "$SRC/recording/audio_sidecar.c" \
    "$SRC/recording/gps_reader.c" \
    "$SRC/sftp/audio_sftp.c" \
    "$SRC/ltc/ltc_decoder.c"; do
    obj="$BUILD/mcp_$(basename "${src%.c}.o")"
    echo "  CC $src"
    "$CC" $CFLAGS $INCLUDES -c "$src" -o "$obj"  # shellcheck: flags are intentionally word-split
    MCP_OBJECTS+=("$obj")
done

echo "--- Linking MCP binary ---"
"$CXX" -fPIE -pie \
    "${MCP_OBJECTS[@]}" \
    -L"$DEPS/lib" \
    -ljson-c \
    -lltc \
    -lssh2 \
    -lssl -lcrypto \
    -lwhisper -lggml -lggml-cpu -lggml-base \
    -ltensorflowlite_c -ltensorflow-lite \
    -lXNNPACK -lpthreadpool -lcpuinfo \
    -lflatbuffers -lfarmhash -lfft2d_fftsg -lfft2d_fftsg2d -leight_bit_int_gemm \
    -lruy_frontend -lruy_context -lruy_context_get_ctx -lruy_ctx \
    -lruy_trmul -lruy_block_map -lruy_kernel_arm -lruy_pack_arm \
    -lruy_apply_multiplier -lruy_prepare_packed_matrices \
    -lruy_allocator -lruy_prepacked_cache -lruy_system_aligned_alloc \
    -lruy_tune -lruy_cpuinfo -lruy_thread_pool \
    -lruy_blocking_counter -lruy_wait -lruy_denormal \
    -lruy_profiler_instrumentation \
    -lflite_cmu_us_kal16 -lflite_usenglish -lflite_cmulex -lflite \
    -laaudio \
    -llog -lz -lm -ldl \
    -static-libstdc++ \
    -o "$BUILD/kanaha-audio-mcp"

# Install the MCP binary. Claude reaches it over adb stdio, so it has to ship
# inside the APK as a lib/ entry -- the manifest sets extractNativeLibs="true",
# which is what makes it a real executable file on disk rather than an mmap.
"$STRIP" "$BUILD/kanaha-audio-mcp"
cp "$BUILD/kanaha-audio-mcp" "$OUTPUT_DIR/libkanaha_mcp.so"
echo "=== Installed: $OUTPUT_DIR/libkanaha_mcp.so ==="

echo "=== Build complete ==="
ls -lh "$BUILD/kanaha-audio-httpd" "$BUILD/kanaha-audio-mcp"
