#!/bin/bash
# Build kanaha-audio as REAL Apache httpd (mod_http2 + mod_ssl + mod_axis2)
# with AudioSearchService + whisper/yamnet/ltc/sftp statically linked.
# Path B (see kanaha-audio/docs/PATH_B_HTTP2_MIGRATION.md). Clones
# link-httpd-axis2.sh (camera) + adds the audio DSP libs from build-android.sh.
set -euo pipefail

HTTPD=$HOME/android-cross-builds/httpd-2.4.66
DEPS=$HOME/android-cross-builds/deps/arm64-v8a
NDK=$HOME/Android/Sdk/ndk/28.0.12916984
# android28: required because AAudio (-laaudio) is API 26+. The prebuilt httpd
# .a files were built at android21 and link forward-compatibly under android28.
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang
# C++ driver for the final link — tflite/ruy/whisper pull in libc++/libc++abi
# (__cxxabiv1 vtables, __cxa_guard_*); the C driver wouldn't resolve them.
CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++
AR=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar

SRC=$HOME/repos/kanaha-audio/kanaha-audio-app/app/src/main/cpp

INCLUDES="-I$SRC -I$DEPS/include -I$DEPS/include/apr-1 \
 -I$DEPS/include/axis2-2.0.0 -I$DEPS/include/axis2-2.0.0/platforms/unix \
 -I$DEPS/include/json-c -I$DEPS/include/nghttp2 -I$DEPS/include/openssl"
CFLAGS="-O2 -fPIC -D__ANDROID__ -DANDROID -DUSE_CROSS_COMPILED_LIBS=1 \
 -DAXIS2_JSON_ENABLED=1 -DWITH_NGHTTP2=1 -DWITH_OPENSSL=1 \
 -Wall -Wno-unused-parameter -Wno-unused-variable \
 -fsigned-char"

cd "$HTTPD"

echo "=== Compiling AudioSearchService + DSP objects ==="
# Service adapter (strong override) + impl + the DSP it calls into.
# NOTE: excludes main.c and apache_httpd_android.c — those are the old custom
# server, now replaced by real Apache + mod_axis2.
SERVICE_SRCS=(
  "$SRC/axis2c/axis2_static_service_adapter.c"
  "$SRC/axis2c/audio_search_service.c"
  "$SRC/audio_util.c"
  "$SRC/whisper/whisper_android_bridge.c"
  "$SRC/yamnet/yamnet_bridge.c"
  "$SRC/recording/audio_recording.c"
  "$SRC/recording/audio_tone.c"
  "$SRC/speech/audio_speak.c"
  "$SRC/recording/audio_sidecar.c"
  "$SRC/recording/gps_reader.c"
  "$SRC/sftp/audio_sftp.c"
  "$SRC/ltc/ltc_decoder.c"
)
OBJS=()
for s in "${SERVICE_SRCS[@]}"; do
  o="audio_$(basename "${s%.c}").o"
  echo "  CC $(basename "$s")"
  $CC $CFLAGS $INCLUDES -c "$s" -o "$o"
  OBJS+=("$o")
done
$AR rcs libkanaha_audio_services.a "${OBJS[@]}"
echo "=== libkanaha_audio_services.a built ==="

echo "=== Linking real Apache httpd (audio) ==="
$CXX -fPIC -o httpd-audio modules.o buildmark.o \
  -Wl,--export-dynamic \
  -Wl,-z,max-page-size=16384 \
  -Wl,-z,separate-loadable-segments \
  -L$DEPS/lib \
  server/.libs/libmain.a \
  modules/aaa/.libs/libmod_authz_core.a \
  modules/core/.libs/libmod_so.a \
  modules/http/.libs/libmod_http.a \
  modules/http/.libs/libmod_mime.a \
  modules/loggers/.libs/libmod_log_config.a \
  modules/metadata/.libs/libmod_headers.a \
  modules/ssl/.libs/libmod_ssl.a \
  modules/http2/.libs/libmod_http2.a \
  -lssl -ldl -lcrypto \
  modules/arch/unix/.libs/libmod_unixd.a \
  modules/mappers/.libs/libmod_dir.a \
  modules/mappers/.libs/libmod_rewrite.a \
  server/mpm/worker/.libs/libworker.a \
  os/unix/.libs/libos.a \
  $DEPS/lib/libmod_axis2.a \
  -Wl,--whole-archive $DEPS/lib/libaxis2_engine.a -Wl,--no-whole-archive \
  -Wl,--whole-archive libkanaha_audio_services.a -Wl,--no-whole-archive \
  $DEPS/lib/libaxis2_deployment.a \
  $DEPS/lib/libaxis2_description.a \
  $DEPS/lib/libaxis2_context.a \
  $DEPS/lib/libaxis2_phaseresolver.a \
  $DEPS/lib/libaxis2_core_utils.a \
  $DEPS/lib/libaxis2_http_common.a \
  $DEPS/lib/libaxis2_http_util.a \
  $DEPS/lib/libaxis2_h2_transport.a \
  $DEPS/lib/libaxis2_h2_sender.a \
  $DEPS/lib/libaxis2_axiom.a \
  $DEPS/lib/libaxis2_axiom_util.a \
  $DEPS/lib/libaxis2_parser.a \
  $DEPS/lib/libaxis2_soap.a \
  $DEPS/lib/libaxis2_addr.a \
  $DEPS/lib/libaxis2_xpath.a \
  $DEPS/lib/libaxis2_unix.a \
  $DEPS/lib/libaxis2_attachments.a \
  $DEPS/lib/libaxis2_clientapi.a \
  $DEPS/lib/libaxutil.a \
  $DEPS/lib/libneethi.a \
  $DEPS/lib/libguththila.a \
  $DEPS/lib/libpcre2-8.a \
  -ljson-c \
  $DEPS/lib/libnghttp2.a \
  -lltc -lssh2 \
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
  $DEPS/lib/libaprutil-1.a \
  $DEPS/lib/libexpat.a \
  $DEPS/lib/libapr-1.a \
  -lm -llog -lz -pthread -static-libstdc++

echo "=== Link complete ==="
file "$HTTPD/httpd-audio"
ls -lh "$HTTPD/httpd-audio"
