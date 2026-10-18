#!/usr/bin/env bash
#
# build-deps.sh — build the C/C++ libraries that prototype-tor-qt links into
# its own binary: tor (as libtor.a), OpenSSL, libevent, zlib and libdatachannel
# (with libjuice + usrsctp) for the WebRTC side of the Snowflake client.
#
#   deps/build-deps.sh linux      # host x86_64, everything static
#   deps/build-deps.sh android    # arm64-v8a, OpenSSL shared (Qt needs it too)
#   deps/build-deps.sh wasm       # Emscripten + pthreads; datachannel-wasm
#                                 # (browser WebRTC) instead of libdatachannel
#
# Output: deps/install/<target>/{include,lib}, which prototype-tor-qt.pro reads.
# Sources and build trees go to $WORK_DIR (default /shared/tmp/prototype-tor-qt-deps).
#
# Environment:
#   ANDROID_NDK_ROOT   NDK for the android target (default: newest in $ANDROID_SDK_ROOT/ndk)
#   ANDROID_API        minimum API level (default 28)
#   EMSDK              emsdk root for the wasm target (default /opt/emsdk)
#   JOBS               parallel jobs (default nproc)

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:-}
case "$TARGET" in
    linux|android|wasm) ;;
    *) echo "usage: $0 linux|android|wasm" >&2; exit 2 ;;
esac

if [ -d /shared/tmp ]; then WORK_DIR=${WORK_DIR:-/shared/tmp/prototype-tor-qt-deps}
else WORK_DIR=${WORK_DIR:-${TMPDIR:-/tmp}/prototype-tor-qt-deps}; fi
PREFIX=${PREFIX:-$HERE/install/$TARGET}
JOBS=${JOBS:-$(nproc)}
ANDROID_API=${ANDROID_API:-28}

TOR_VERSION=0.4.9.14
TOR_SHA256=182449ac1c8ff43278da27b69b02040e11d03d0327db6d9126e4190ec6235e4a
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
LIBEVENT_VERSION=2.1.12-stable
LIBEVENT_SHA256=92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb
ZLIB_VERSION=1.3.2
ZLIB_SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
LIBDATACHANNEL_TAG=v0.24.6
DATACHANNEL_WASM_TAG=v0.4.0

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

DL="$WORK_DIR/download"
BUILD="$WORK_DIR/build-$TARGET"
mkdir -p "$DL" "$BUILD" "$PREFIX/include" "$PREFIX/lib"

fetch() { # url sha256
    local file="$DL/$(basename "$1")"
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file.part" "$1" || die "download failed: $1"
        mv "$file.part" "$file"
    fi
    echo "$2  $file" | sha256sum -c --quiet - || die "checksum mismatch for $file"
}
unpack() { # tarball -> fresh dir under $BUILD
    local dir="$BUILD/$2"
    rm -rf "$dir"
    tar xzf "$DL/$1" -C "$BUILD"
}

# --- toolchain ---------------------------------------------------------------

if [ "$TARGET" = android ]; then
    if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
        ANDROID_NDK_ROOT=$(ls -d "${ANDROID_SDK_ROOT:-/opt/android-sdk}"/ndk/* 2>/dev/null | sort -V | tail -1)
    fi
    [ -d "${ANDROID_NDK_ROOT:-}" ] || die "Android NDK not found; set ANDROID_NDK_ROOT"
    export ANDROID_NDK_ROOT
    TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
    HOST=aarch64-linux-android
    export PATH="$TC/bin:$PATH"
    export CC="$TC/bin/${HOST}${ANDROID_API}-clang"
    export CXX="$TC/bin/${HOST}${ANDROID_API}-clang++"
    export AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" STRIP="$TC/bin/llvm-strip"
    CONFIGURE_HOST=(--host=$HOST)
    CMAKE_TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
                     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$ANDROID_API)
elif [ "$TARGET" = wasm ]; then
    EMSDK=${EMSDK:-/opt/emsdk}
    command -v emcc >/dev/null || source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1 \
        || die "emsdk not found; set EMSDK"
    export CC=emcc CXX=em++ AR=emar RANLIB=emranlib
    # config.sub in tor does not know "emscripten"; any wasm32 triple makes
    # configure cross-compile, and emcc ignores it.
    CONFIGURE_HOST=(--host=wasm32-unknown-linux-musl)
    CMAKE_TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
else
    export CC=${CC:-gcc} CXX=${CXX:-g++} AR=${AR:-ar} RANLIB=${RANLIB:-ranlib}
    CONFIGURE_HOST=()
    CMAKE_TOOLCHAIN=()
fi
if [ "$TARGET" = wasm ]; then
    # tor runs on its own pthread; everything shares one wasm memory.
    export CFLAGS="-O2 -pthread" CXXFLAGS="-O2 -pthread"
else
    export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
fi

# --- zlib --------------------------------------------------------------------

step "zlib $ZLIB_VERSION"
fetch https://zlib.net/zlib-$ZLIB_VERSION.tar.gz $ZLIB_SHA256
unpack zlib-$ZLIB_VERSION.tar.gz zlib-$ZLIB_VERSION
( cd "$BUILD/zlib-$ZLIB_VERSION"
  CHOST=${HOST:-} ./configure --static --prefix="$PREFIX" >/dev/null
  make -j"$JOBS" >/dev/null && make install >/dev/null )

# --- OpenSSL -----------------------------------------------------------------
# Android: shared, named libcrypto_3.so / libssl_3.so, because Qt's TLS backend
# on Android dlopens exactly those names. Tor and libdatachannel link the same
# copies. Linux: static (Qt there uses the system OpenSSL for its own TLS).

step "OpenSSL $OPENSSL_VERSION"
fetch https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz $OPENSSL_SHA256
unpack openssl-$OPENSSL_VERSION.tar.gz openssl-$OPENSSL_VERSION
( cd "$BUILD/openssl-$OPENSSL_VERSION"
  common=(--prefix="$PREFIX" --libdir=lib no-tests no-docs no-apps no-legacy no-engine)
  if [ "$TARGET" = android ]; then
      CC=clang ./Configure --config="$HERE/openssl-android-qt.conf" android-qt-arm64 shared -D__ANDROID_API__=$ANDROID_API \
          "${common[@]}" >/dev/null
      make -j"$JOBS" build_libs >/dev/null
      cp -a include/openssl "$PREFIX/include/"
      cp libcrypto_3.so libssl_3.so "$PREFIX/lib/"
      "$STRIP" "$PREFIX/lib/libcrypto_3.so" "$PREFIX/lib/libssl_3.so"
      ln -sf libcrypto_3.so "$PREFIX/lib/libcrypto.so"
      ln -sf libssl_3.so "$PREFIX/lib/libssl.so"
  elif [ "$TARGET" = wasm ]; then
      # No assembler, no dlopen, no mlock/mmap secure heap in a browser.
      ./Configure linux-generic32 no-asm no-shared no-dso no-afalgeng no-secure-memory no-ui-console \
          "${common[@]}" >/dev/null
      make -j"$JOBS" build_libs >/dev/null
      make install_dev >/dev/null
  else
      ./Configure linux-x86_64 no-shared "${common[@]}" >/dev/null
      make -j"$JOBS" build_libs >/dev/null
      make install_dev >/dev/null
  fi )

# --- libevent ----------------------------------------------------------------

step "libevent $LIBEVENT_VERSION"
fetch https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz $LIBEVENT_SHA256
unpack libevent-$LIBEVENT_VERSION.tar.gz libevent-$LIBEVENT_VERSION
( cd "$BUILD/libevent-$LIBEVENT_VERSION"
  ./configure "${CONFIGURE_HOST[@]}" --prefix="$PREFIX" --disable-shared --enable-static --with-pic \
      --disable-openssl --disable-samples --disable-libevent-regress --disable-debug-mode >/dev/null
  make -j"$JOBS" >/dev/null && make install >/dev/null )

# --- tor ---------------------------------------------------------------------
# Built as libtor.a (no main()); the app starts it with tor_run_main().
# Client-only: the relay and dirauth modules are compiled out.

step "tor $TOR_VERSION"
fetch https://dist.torproject.org/tor-$TOR_VERSION.tar.gz $TOR_SHA256
unpack tor-$TOR_VERSION.tar.gz tor-$TOR_VERSION
( cd "$BUILD/tor-$TOR_VERSION"
  static_ssl=(--enable-static-openssl)
  [ "$TARGET" = android ] && static_ssl=()
  ./configure "${CONFIGURE_HOST[@]}" \
      --enable-static-libevent --with-libevent-dir="$PREFIX" \
      --enable-static-zlib --with-zlib-dir="$PREFIX" \
      "${static_ssl[@]}" --with-openssl-dir="$PREFIX" \
      --disable-module-relay --disable-module-dirauth \
      --disable-asciidoc --disable-manpage --disable-html-manual \
      --disable-unittests --disable-lzma --disable-zstd --disable-seccomp \
      --disable-libscrypt --disable-systemd --disable-tool-name-check \
      --disable-gcc-hardening --disable-linker-hardening >/dev/null
  make -j"$JOBS" libtor.a >/dev/null
  cp libtor.a "$PREFIX/lib/"
  cp src/feature/api/tor_api.h "$PREFIX/include/" )

# --- datachannel-wasm (wasm) / libdatachannel (native) -----------------------
# In the browser, WebRTC is the browser's own RTCPeerConnection: datachannel-wasm
# implements the same rtc:: API on top of it, plus rtc::WebSocket for the
# direct-to-bridge fallback. Its two JS libraries go to lib/js/ for the link.

if [ "$TARGET" = wasm ]; then
    step "datachannel-wasm $DATACHANNEL_WASM_TAG"
    DCW="$DL/datachannel-wasm-$DATACHANNEL_WASM_TAG"
    [ -d "$DCW" ] || git clone -q --depth 1 --branch "$DATACHANNEL_WASM_TAG" \
        https://github.com/paullouisageneau/datachannel-wasm.git "$DCW"
    rm -rf "$BUILD/datachannel-wasm"
    cmake -S "$DCW" -B "$BUILD/datachannel-wasm" "${CMAKE_TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD/datachannel-wasm" -j"$JOBS" >/dev/null
    cp "$BUILD/datachannel-wasm/libdatachannel-wasm.a" "$PREFIX/lib/"
    rm -rf "$PREFIX/include/rtc" && cp -r "$DCW/wasm/include/rtc" "$PREFIX/include/"
    mkdir -p "$PREFIX/lib/js" && cp "$DCW"/wasm/js/*.js "$PREFIX/lib/js/"
    step "done: $PREFIX"
    ls -la "$PREFIX/lib"
    exit 0
fi

step "libdatachannel $LIBDATACHANNEL_TAG"
LDC="$DL/libdatachannel-$LIBDATACHANNEL_TAG"
[ -d "$LDC" ] || git clone -q --depth 1 --branch "$LIBDATACHANNEL_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/paullouisageneau/libdatachannel.git "$LDC"
rm -rf "$BUILD/libdatachannel"
cmake -S "$LDC" -B "$BUILD/libdatachannel" "${CMAKE_TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON \
    -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF \
    -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=$([ "$TARGET" = linux ] && echo ON || echo OFF) \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" >/dev/null
cmake --build "$BUILD/libdatachannel" --target datachannel-static -j"$JOBS" >/dev/null
cp "$BUILD/libdatachannel/libdatachannel-static.a" "$PREFIX/lib/libdatachannel.a"
cp "$BUILD/libdatachannel/deps/libjuice/libjuice-static.a" "$PREFIX/lib/libjuice.a"
cp "$BUILD/libdatachannel/deps/usrsctp/usrsctplib/libusrsctp.a" "$PREFIX/lib/libusrsctp.a"
rm -rf "$PREFIX/include/rtc" && cp -r "$LDC/include/rtc" "$PREFIX/include/"

step "done: $PREFIX"
ls -la "$PREFIX/lib"
