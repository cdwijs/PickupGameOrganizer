#!/usr/bin/env bash
#
# build-deps.sh — build the WebRTC stack that prototype-webrtc-stun links into
# its own binary: libdatachannel (with libjuice + usrsctp) on OpenSSL natively,
# datachannel-wasm (the browser's RTCPeerConnection and WebSocket) in the browser.
#
#   deps/build-deps.sh linux      # host x86_64, everything static
#   deps/build-deps.sh android    # arm64-v8a, OpenSSL shared (Qt needs it too)
#   deps/build-deps.sh wasm       # Emscripten, no threads (Qt wasm_singlethread)
#
# Output: deps/install/<target>/{include,lib}, which prototype-webrtc-stun.pro reads.
# Sources and build trees go to $WORK_DIR (default /shared/tmp/prototype-webrtc-stun-deps).
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

if [ -d /shared/tmp ]; then WORK_DIR=${WORK_DIR:-/shared/tmp/prototype-webrtc-stun-deps}
else WORK_DIR=${WORK_DIR:-${TMPDIR:-/tmp}/prototype-webrtc-stun-deps}; fi
PREFIX=${PREFIX:-$HERE/install/$TARGET}
JOBS=${JOBS:-$(nproc)}
ANDROID_API=${ANDROID_API:-28}

OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
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
    CMAKE_TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
                     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$ANDROID_API)
elif [ "$TARGET" = wasm ]; then
    EMSDK=${EMSDK:-/opt/emsdk}
    command -v emcc >/dev/null || source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1 \
        || die "emsdk not found; set EMSDK"
    CMAKE_TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
else
    export CC=${CC:-gcc} CXX=${CXX:-g++} AR=${AR:-ar} RANLIB=${RANLIB:-ranlib}
    CMAKE_TOOLCHAIN=()
fi

# --- datachannel-wasm (wasm) -------------------------------------------------
# In the browser, WebRTC is the browser's own RTCPeerConnection: datachannel-wasm
# implements libdatachannel's rtc:: API on top of it, plus rtc::WebSocket for
# the ntfy subscriptions. Built without -pthread to match Qt wasm_singlethread.
# Its two JS libraries go to lib/js/ for the link.

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

export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"

# --- OpenSSL -----------------------------------------------------------------
# Android: static for libdatachannel, plus shared copies named libcrypto_3.so /
# libssl_3.so for Qt's TLS backend, which dlopens exactly those names. The app
# does not link the shared ones: a Qt kit may deploy its own OpenSSL under the
# same file names (Arch's AUR cross Qt does), and whichever lands in the APK
# last would then have to match what the app was linked against.
# Linux: static (Qt there uses the system OpenSSL for its own TLS).

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
      cp libcrypto_3.so libssl_3.so libcrypto.a libssl.a "$PREFIX/lib/"
      "$STRIP" "$PREFIX/lib/libcrypto_3.so" "$PREFIX/lib/libssl_3.so"
      ln -sf libcrypto_3.so "$PREFIX/lib/libcrypto.so"
      ln -sf libssl_3.so "$PREFIX/lib/libssl.so"
  else
      ./Configure linux-x86_64 no-shared "${common[@]}" >/dev/null
      make -j"$JOBS" build_libs >/dev/null
      make install_dev >/dev/null
  fi )

# --- libdatachannel (native) -------------------------------------------------
# Data channels only: no media, and no WebSocket (Qt does the ntfy HTTPS).

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
# The .pro raises a Qt kit's compile/link API level to this one if it is lower:
# the libraries above reference libc symbols that only exist from this level on.
[ "$TARGET" = android ] && echo "$ANDROID_API" > "$PREFIX/android-api"

step "done: $PREFIX"
ls -la "$PREFIX/lib"
