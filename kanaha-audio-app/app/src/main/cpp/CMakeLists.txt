# CMakeLists.txt for Kanaha Audio
# Apache httpd + Axis2/C + whisper.cpp Integration
#
# ARCHITECTURE: No Intent IPC — whisper.cpp links directly
#
# This build system links pre-built cross-compiled Apache httpd, Axis2/C,
# and whisper.cpp libraries. Unlike Kanaha Camera (which needs Intent IPC
# across a GPL boundary), all dependencies here are permissive and link
# into the same process.

cmake_minimum_required(VERSION 3.18.1)

project("kanaha-audio")

# Configure C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============================================================================
# Cross-compiled Dependencies Path
# ============================================================================
# These libraries were cross-compiled for Android ARM64 using the NDK.
# Shared with Kanaha Camera for Axis2/C deps, plus whisper.cpp additions.

set(CROSS_COMPILED_DEPS "$ENV{HOME}/android-cross-builds/deps/arm64-v8a")

# Only link cross-compiled libs for arm64-v8a (Pixel 9 Pro target)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(USE_CROSS_COMPILED_LIBS ON)
    message(STATUS "Building for arm64-v8a - linking cross-compiled libraries")
else()
    set(USE_CROSS_COMPILED_LIBS OFF)
    message(STATUS "Building for ${ANDROID_ABI} - using stub implementation only")
endif()

# ============================================================================
# Find Android System Libraries
# ============================================================================
find_library(log-lib log)
find_library(android-lib android)
find_library(z-lib z)
find_library(aaudio-lib aaudio)

# ============================================================================
# Include Directories
# ============================================================================
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(USE_CROSS_COMPILED_LIBS)
    include_directories(
        ${CROSS_COMPILED_DEPS}/include
        ${CROSS_COMPILED_DEPS}/include/apr-1
        ${CROSS_COMPILED_DEPS}/include/axis2-2.0.0
        ${CROSS_COMPILED_DEPS}/include/axis2-2.0.0/platforms/unix
        ${CROSS_COMPILED_DEPS}/include/json-c
        ${CROSS_COMPILED_DEPS}/include/nghttp2
        ${CROSS_COMPILED_DEPS}/include/openssl
    )

    # whisper.cpp headers are installed directly to $DEPS/include/
    # (whisper.h, ggml.h, etc. — no subdirectory)

    # TFLite C API headers (for YAMNet bridge)
    # Installed to $DEPS/include/tensorflow/lite/c/ during cross-compile
endif()

# ============================================================================
# Preprocessor Definitions
# ============================================================================
add_definitions(
    -DANDROID
    -DAPACHE_HTTPD_ANDROID
)

if(USE_CROSS_COMPILED_LIBS)
    add_definitions(
        -DAXIS2_JSON_ENABLED=1
        -DAXIS2_H2_JSON_ONLY_BUILD=1
        -DWITH_NGHTTP2=1
        -DWITH_OPENSSL=1
        -DUSE_CROSS_COMPILED_LIBS=1
    )
endif()

# ============================================================================
# Import Pre-built Cross-compiled Libraries (arm64-v8a only)
# ============================================================================
if(USE_CROSS_COMPILED_LIBS)
    set(DEPS_LIB_DIR ${CROSS_COMPILED_DEPS}/lib)

    # APR - Apache Portable Runtime
    add_library(apr-1 STATIC IMPORTED)
    set_target_properties(apr-1 PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libapr-1.a)

    add_library(aprutil-1 STATIC IMPORTED)
    set_target_properties(aprutil-1 PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaprutil-1.a)

    add_library(expat STATIC IMPORTED)
    set_target_properties(expat PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libexpat.a)

    # OpenSSL - TLS/mTLS support
    add_library(ssl STATIC IMPORTED)
    set_target_properties(ssl PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libssl.a)

    add_library(crypto STATIC IMPORTED)
    set_target_properties(crypto PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libcrypto.a)

    # json-c - JSON parsing for JSON-RPC
    add_library(json-c STATIC IMPORTED)
    set_target_properties(json-c PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libjson-c.a)

    # nghttp2 - HTTP/2 protocol support
    add_library(nghttp2 STATIC IMPORTED)
    set_target_properties(nghttp2 PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libnghttp2.a)

    # Axis2/C libraries
    add_library(axutil STATIC IMPORTED)
    set_target_properties(axutil PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxutil.a)

    add_library(guththila STATIC IMPORTED)
    set_target_properties(guththila PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libguththila.a)

    add_library(axis2_axiom STATIC IMPORTED)
    set_target_properties(axis2_axiom PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxis2_axiom.a)

    add_library(axis2_parser STATIC IMPORTED)
    set_target_properties(axis2_parser PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxis2_parser.a)

    add_library(axis2_engine STATIC IMPORTED)
    set_target_properties(axis2_engine PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxis2_engine.a)

    add_library(axis2_http_common STATIC IMPORTED)
    set_target_properties(axis2_http_common PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxis2_http_common.a)

    add_library(axis2_http_util STATIC IMPORTED)
    set_target_properties(axis2_http_util PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libaxis2_http_util.a)

    add_library(neethi STATIC IMPORTED)
    set_target_properties(neethi PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libneethi.a)

    # whisper.cpp + ggml (cross-compiled for ARM64)
    add_library(whisper STATIC IMPORTED)
    set_target_properties(whisper PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libwhisper.a)

    add_library(ggml STATIC IMPORTED)
    set_target_properties(ggml PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libggml.a)

    add_library(ggml-base STATIC IMPORTED)
    set_target_properties(ggml-base PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libggml-base.a)

    add_library(ggml-cpu STATIC IMPORTED)
    set_target_properties(ggml-cpu PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libggml-cpu.a)

    # TensorFlow Lite C API + runtime (for YAMNet audio event detection)
    # The C API wrapper (tensorflowlite_c) calls into the core runtime
    # (tensorflow-lite), which depends on XNNPACK, abseil, ruy, etc.
    # All cross-compiled from TF v2.16.1 via build-tflite.sh.
    add_library(tensorflowlite_c STATIC IMPORTED)
    set_target_properties(tensorflowlite_c PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libtensorflowlite_c.a)

    add_library(tensorflow-lite STATIC IMPORTED)
    set_target_properties(tensorflow-lite PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libtensorflow-lite.a)

    # XNNPACK optimized CPU kernels (ARM64 NEON)
    add_library(XNNPACK STATIC IMPORTED)
    set_target_properties(XNNPACK PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libXNNPACK.a)

    add_library(pthreadpool STATIC IMPORTED)
    set_target_properties(pthreadpool PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libpthreadpool.a)

    add_library(cpuinfo STATIC IMPORTED)
    set_target_properties(cpuinfo PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libcpuinfo.a)

    # FlatBuffers, ruy matrix multiply, farmhash, FFT
    add_library(flatbuffers STATIC IMPORTED)
    set_target_properties(flatbuffers PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libflatbuffers.a)

    add_library(farmhash STATIC IMPORTED)
    set_target_properties(farmhash PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libfarmhash.a)

    add_library(fft2d_fftsg STATIC IMPORTED)
    set_target_properties(fft2d_fftsg PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libfft2d_fftsg.a)

    add_library(fft2d_fftsg2d STATIC IMPORTED)
    set_target_properties(fft2d_fftsg2d PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libfft2d_fftsg2d.a)

    add_library(eight_bit_int_gemm STATIC IMPORTED)
    set_target_properties(eight_bit_int_gemm PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libeight_bit_int_gemm.a)

    # Ruy matrix multiplication library (ARM64 optimized kernels)
    add_library(ruy_frontend STATIC IMPORTED)
    set_target_properties(ruy_frontend PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_frontend.a)

    add_library(ruy_context STATIC IMPORTED)
    set_target_properties(ruy_context PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_context.a)

    add_library(ruy_context_get_ctx STATIC IMPORTED)
    set_target_properties(ruy_context_get_ctx PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_context_get_ctx.a)

    add_library(ruy_ctx STATIC IMPORTED)
    set_target_properties(ruy_ctx PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_ctx.a)

    add_library(ruy_trmul STATIC IMPORTED)
    set_target_properties(ruy_trmul PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_trmul.a)

    add_library(ruy_block_map STATIC IMPORTED)
    set_target_properties(ruy_block_map PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_block_map.a)

    add_library(ruy_kernel_arm STATIC IMPORTED)
    set_target_properties(ruy_kernel_arm PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_kernel_arm.a)

    add_library(ruy_pack_arm STATIC IMPORTED)
    set_target_properties(ruy_pack_arm PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_pack_arm.a)

    add_library(ruy_apply_multiplier STATIC IMPORTED)
    set_target_properties(ruy_apply_multiplier PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_apply_multiplier.a)

    add_library(ruy_prepare_packed_matrices STATIC IMPORTED)
    set_target_properties(ruy_prepare_packed_matrices PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_prepare_packed_matrices.a)

    add_library(ruy_allocator STATIC IMPORTED)
    set_target_properties(ruy_allocator PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_allocator.a)

    add_library(ruy_prepacked_cache STATIC IMPORTED)
    set_target_properties(ruy_prepacked_cache PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_prepacked_cache.a)

    add_library(ruy_system_aligned_alloc STATIC IMPORTED)
    set_target_properties(ruy_system_aligned_alloc PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_system_aligned_alloc.a)

    add_library(ruy_tune STATIC IMPORTED)
    set_target_properties(ruy_tune PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_tune.a)

    add_library(ruy_cpuinfo STATIC IMPORTED)
    set_target_properties(ruy_cpuinfo PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_cpuinfo.a)

    add_library(ruy_thread_pool STATIC IMPORTED)
    set_target_properties(ruy_thread_pool PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_thread_pool.a)

    add_library(ruy_blocking_counter STATIC IMPORTED)
    set_target_properties(ruy_blocking_counter PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_blocking_counter.a)

    add_library(ruy_wait STATIC IMPORTED)
    set_target_properties(ruy_wait PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_wait.a)

    add_library(ruy_denormal STATIC IMPORTED)
    set_target_properties(ruy_denormal PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_denormal.a)

    add_library(ruy_profiler_instrumentation STATIC IMPORTED)
    set_target_properties(ruy_profiler_instrumentation PROPERTIES IMPORTED_LOCATION ${DEPS_LIB_DIR}/libruy_profiler_instrumentation.a)

    # Convenience variable: all TFLite libraries in link order.
    # tensorflowlite_c → tensorflow-lite → XNNPACK/ruy/abseil/etc.
    # Used by all three targets (shared lib, httpd, MCP).
    set(TFLITE_LIBS
        tensorflowlite_c
        tensorflow-lite
        XNNPACK
        pthreadpool
        cpuinfo
        flatbuffers
        farmhash
        fft2d_fftsg
        fft2d_fftsg2d
        eight_bit_int_gemm
        ruy_frontend
        ruy_context
        ruy_context_get_ctx
        ruy_ctx
        ruy_trmul
        ruy_block_map
        ruy_kernel_arm
        ruy_pack_arm
        ruy_apply_multiplier
        ruy_prepare_packed_matrices
        ruy_allocator
        ruy_prepacked_cache
        ruy_system_aligned_alloc
        ruy_tune
        ruy_cpuinfo
        ruy_thread_pool
        ruy_blocking_counter
        ruy_wait
        ruy_denormal
        ruy_profiler_instrumentation
    )
endif()

# ============================================================================
# Source Files
# ============================================================================
set(KANAHA_SOURCES
    audio_util.c
    apache-httpd/apache_httpd_android.c
    axis2c/audio_search_service.c
    whisper/whisper_android_bridge.c
    yamnet/yamnet_bridge.c
    recording/audio_recording.c
    recording/audio_tone.c
    recording/audio_sidecar.c
    recording/gps_reader.c
)

# Axis2/C adapter + MCP only compiled for the full cross-compiled build (arm64)
# The adapter provides the strong symbol that overrides the weak stub in
# axis2_json_rpc_msg_recv.c — this is what connects us to the Axis2/C framework.
if(USE_CROSS_COMPILED_LIBS)
    list(APPEND KANAHA_SOURCES
        axis2c/axis2_static_service_adapter.c
        axis2c/kanaha_mcp.c
    )
endif()

# ============================================================================
# Create Shared Library
# ============================================================================
add_library(
    kanaha-audio
    SHARED
    ${KANAHA_SOURCES}
)

# ============================================================================
# Compiler Flags
# ============================================================================
target_compile_options(kanaha-audio PRIVATE
    -Wall
    -Wextra
    -O2
    -fPIC
    -ffunction-sections
    -fdata-sections
    -Wno-unused-parameter
    -Wno-unused-variable
)

# ============================================================================
# Link Libraries
# ============================================================================
if(USE_CROSS_COMPILED_LIBS)
    target_link_libraries(
        kanaha-audio

        # Android system libraries
        ${log-lib}
        ${android-lib}
        ${z-lib}
        ${aaudio-lib}

        # Axis2/C libraries (order matters for static linking)
        # --whole-archive required for axis2_engine so weak symbols in
        # axis2_json_rpc_msg_recv.c are included and can be overridden
        # by strong symbols from axis2_static_service_adapter.c
        -Wl,--whole-archive
        axis2_engine
        -Wl,--no-whole-archive
        axis2_http_common
        axis2_http_util
        axis2_axiom
        axis2_parser
        guththila
        neethi
        axutil

        # APR - Apache Portable Runtime
        aprutil-1
        apr-1
        expat

        # JSON-RPC parsing
        json-c

        # HTTP/2 protocol
        nghttp2

        # TLS/mTLS support
        ssl
        crypto

        # whisper.cpp + ggml
        whisper
        ggml
        ggml-cpu
        ggml-base

        # TensorFlow Lite + deps (YAMNet audio event detection)
        ${TFLITE_LIBS}
    )

    # Android 15 (API 35) requires 16KB page aligned ELF segments
    target_link_options(kanaha-audio PRIVATE
        -Wl,--gc-sections
        -Wl,-z,max-page-size=16384
        -Wl,-z,separate-loadable-segments
    )
else()
    target_link_libraries(
        kanaha-audio
        ${log-lib}
        ${android-lib}
        ${z-lib}
    )

    target_link_options(kanaha-audio PRIVATE
        -Wl,--gc-sections
        -Wl,--strip-all
        -Wl,-z,max-page-size=16384
        -Wl,-z,separate-loadable-segments
    )
endif()

# ============================================================================
# Create Native Executable (for ProcessBuilder launch)
# ============================================================================
if(USE_CROSS_COMPILED_LIBS)
    # HTTP server executable
    add_executable(
        kanaha_audio_httpd.so
        main.c
        ${KANAHA_SOURCES}
    )

    set_target_properties(kanaha_audio_httpd.so PROPERTIES
        OUTPUT_NAME "kanaha_audio_httpd"
        PREFIX "lib"
        SUFFIX ".so"
        POSITION_INDEPENDENT_CODE ON
    )

    target_compile_options(kanaha_audio_httpd.so PRIVATE
        -Wall -Wextra -O2 -fPIE
        -ffunction-sections -fdata-sections
        -Wno-unused-parameter -Wno-unused-variable
    )

    target_link_libraries(
        kanaha_audio_httpd.so
        ${log-lib}
        ${android-lib}
        ${z-lib}
        ${aaudio-lib}
        -Wl,--whole-archive
        axis2_engine
        -Wl,--no-whole-archive
        axis2_http_common
        axis2_http_util
        axis2_axiom
        axis2_parser
        guththila
        neethi
        axutil
        aprutil-1
        apr-1
        expat
        json-c
        nghttp2
        ssl
        crypto
        # whisper.cpp + ggml
        whisper
        ggml
        ggml-cpu
        ggml-base
        # TensorFlow Lite + deps (YAMNet)
        ${TFLITE_LIBS}
    )

    target_link_options(kanaha_audio_httpd.so PRIVATE
        -fPIE -pie
        -Wl,--gc-sections
        -Wl,-z,max-page-size=16384
        -Wl,-z,separate-loadable-segments
    )

    install(TARGETS kanaha_audio_httpd.so DESTINATION lib/${ANDROID_ABI})

    # ========================================================================
    # MCP stdio binary — launched by Claude Desktop as a subprocess
    # ========================================================================
    add_executable(
        kanaha_audio_mcp.so
        axis2c/kanaha_mcp_main.c
        axis2c/kanaha_mcp.c
        axis2c/audio_search_service.c
        audio_util.c
        whisper/whisper_android_bridge.c
        yamnet/yamnet_bridge.c
        recording/audio_recording.c
        recording/audio_tone.c
        recording/audio_sidecar.c
        recording/gps_reader.c
    )

    set_target_properties(kanaha_audio_mcp.so PROPERTIES
        OUTPUT_NAME "kanaha_audio_mcp"
        PREFIX "lib"
        SUFFIX ".so"
        POSITION_INDEPENDENT_CODE ON
    )

    target_compile_options(kanaha_audio_mcp.so PRIVATE
        -Wall -Wextra -O2 -fPIE
        -ffunction-sections -fdata-sections
        -Wno-unused-parameter -Wno-unused-variable
    )

    target_link_libraries(
        kanaha_audio_mcp.so
        ${log-lib}
        ${z-lib}
        ${aaudio-lib}
        json-c
        whisper
        ggml
        ggml-cpu
        ggml-base
        # TensorFlow Lite + deps (YAMNet)
        ${TFLITE_LIBS}
    )

    target_link_options(kanaha_audio_mcp.so PRIVATE
        -fPIE -pie
        -Wl,--gc-sections
        -Wl,-z,max-page-size=16384
        -Wl,-z,separate-loadable-segments
    )

    install(TARGETS kanaha_audio_mcp.so DESTINATION lib/${ANDROID_ABI})
endif()

# ============================================================================
# ARCHITECTURE NOTES
# ============================================================================
#
# This build system creates three targets:
# 1. libkanaha-audio.so - Shared library
# 2. libkanaha_audio_httpd.so - Native executable (launched via ProcessBuilder)
# 3. libkanaha_audio_mcp.so - MCP stdio executable (launched by Claude Desktop)
#
# Key difference from Kanaha Camera:
# - whisper.cpp (MIT) links directly into the native process
# - No Intent IPC needed — all inference happens in-process
# - No GPL boundary to manage — all deps are Apache 2.0 or MIT
#
# Cross-compiled libraries (shared with Kanaha Camera):
# - Apache APR/APR-util, OpenSSL, nghttp2, Axis2/C, json-c
#
# New dependencies (cross-compile for ARM64):
# - whisper.cpp + ggml (MIT license)
#
# Communication Flow:
# HTTP/2 JSON → Axis2/C → audio_search_service.c → whisper_bridge → whisper.cpp (speech)
#                                                  → yamnet_bridge  → TFLite/YAMNet (events)
# (All in-process — no IPC, no fork/exec, no shell invocation)
#
# New dependencies for YAMNet:
# - TensorFlow Lite C API (Apache 2.0)
# - YAMNet .tflite model (Apache 2.0, ~3.7 MB)
# ============================================================================
