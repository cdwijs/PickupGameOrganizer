#!/usr/bin/env bash
#
# build.sh — build prototype-tor-qt for all three targets with qmake.
#
#   ./build.sh                    # linux + android + wasm
#   ./build.sh linux wasm         # only these
#   ./build.sh --clean android    # wipe that target's build tree first
#
# The native dependencies (tor, OpenSSL, libevent, zlib, libdatachannel /
# datachannel-wasm) are built with deps/build-deps.sh the first time a target
# needs them; delete deps/install/<target> to rebuild them.
#
# The deliverables land in dist/ next to this script:
#
#   dist/prototype-tor-qt            native Linux executable (x86_64)
#   dist/prototype-tor-qt-debug.apk  Android package, arm64-v8a, debug-signed
#   dist/web/                        browser build: serve this folder over HTTPS
#
# Intermediate build trees and Gradle's cache stay out of the repository, under
# $BUILD_DIR (default /shared/tmp/prototype-tor-qt-build).
#
# Toolchains (defaults match the Qt image):
#   QT_HOST_LINUX   Qt for Linux, also the host Qt for androiddeployqt  (/opt/Qt/6.8.3/gcc_64)
#   QT_ANDROID      Qt for Android arm64-v8a                            (/opt/Qt/6.8.3/android_arm64_v8a)
#   QT_WASM_MT_DIR  Qt for WebAssembly *with threads*                   (/opt/Qt/6.8.3/wasm_multithread)
#   ANDROID_SDK_ROOT, ANDROID_NDK_ROOT, EMSDK (default /opt/emsdk)
#   JOBS            parallel jobs (default nproc)

set -euo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
PRO="$SRC/prototype-tor-qt.pro"
DIST="$SRC/dist"

if [ -d /shared/tmp ]; then BUILD_DIR=${BUILD_DIR:-/shared/tmp/prototype-tor-qt-build}
else BUILD_DIR=${BUILD_DIR:-${TMPDIR:-/tmp}/prototype-tor-qt-build}; fi
JOBS=${JOBS:-$(nproc)}

QT_HOST_LINUX=${QT_HOST_LINUX:-/opt/Qt/6.8.3/gcc_64}
QT_ANDROID=${QT_ANDROID:-/opt/Qt/6.8.3/android_arm64_v8a}
QT_WASM_MT_DIR=${QT_WASM_MT_DIR:-/opt/Qt/6.8.3/wasm_multithread}
EMSDK=${EMSDK:-/opt/emsdk}

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# --- arguments -----------------------------------------------------------------

CLEAN=0
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        linux|android|wasm) TARGETS+=("$arg") ;;
        all)                TARGETS+=(linux android wasm) ;;
        --clean)            CLEAN=1 ;;
        -h|--help)          sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                  die "unknown argument '$arg' (try --help)" ;;
    esac
done
[ ${#TARGETS[@]} -gt 0 ] || TARGETS=(linux android wasm)

# --- helpers -------------------------------------------------------------------

need_qmake() { # qt-dir what
    [ -x "$1/bin/qmake" ] || die "no $2 at $1 (set the matching variable, see --help)"
}

# deps/build-deps.sh <target>, unless its output is already there.
ensure_deps() {
    if [ -f "$SRC/deps/install/$1/lib/libtor.a" ]; then
        echo "  deps: deps/install/$1 (delete it to rebuild)"
    else
        step "native dependencies for $1"
        "$SRC/deps/build-deps.sh" "$1"
    fi
}

# qmake + make in $BUILD_DIR/<target>; extra args go to qmake.
qmake_build() { # target qt-dir [qmake args...]
    local b="$BUILD_DIR/$1"
    [ "$CLEAN" = 1 ] && rm -rf "$b"
    mkdir -p "$b"
    # qmake complains about the container's C locale on every run.
    (cd "$b" && LC_ALL=C.UTF-8 "$2/bin/qmake" "$PRO" "${@:3}" && make -j"$JOBS")
}

# emsdk_env.sh is chatty and unset-variable-hostile.
load_emsdk() {
    command -v emcc >/dev/null && return
    [ -f "$EMSDK/emsdk_env.sh" ] || die "emsdk not found at $EMSDK (set \$EMSDK)"
    set +u
    # shellcheck disable=SC1091
    . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    set -u
    command -v emcc >/dev/null || die "emcc not on PATH after sourcing $EMSDK/emsdk_env.sh"
}

# --- targets -------------------------------------------------------------------

build_linux() {
    step "Linux (x86_64)"
    need_qmake "$QT_HOST_LINUX" "Qt for Linux"
    ensure_deps linux
    qmake_build linux "$QT_HOST_LINUX"
    install -D -m755 "$BUILD_DIR/linux/prototype-tor-qt" "$DIST/prototype-tor-qt"
    echo "  $DIST/prototype-tor-qt  ($(du -h "$DIST/prototype-tor-qt" | cut -f1))"
}

build_android() {
    step "Android (arm64-v8a)"
    need_qmake "$QT_ANDROID" "Qt for Android"
    [ -x "$QT_HOST_LINUX/bin/androiddeployqt" ] || die "androiddeployqt not found in $QT_HOST_LINUX (set \$QT_HOST_LINUX)"
    export ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-/opt/android-sdk}
    if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
        ANDROID_NDK_ROOT=$(ls -d "$ANDROID_SDK_ROOT"/ndk/* 2>/dev/null | sort -V | tail -1)
    fi
    [ -d "${ANDROID_NDK_ROOT:-}" ] || die "Android NDK not found; set \$ANDROID_NDK_ROOT"
    export ANDROID_NDK_ROOT
    ensure_deps android

    # Gradle's cache and the debug signing key live under $BUILD_DIR: rebuilds
    # are offline, and a fresh container still signs with the same key, so the
    # APK installs over the copy already on the phone.
    export GRADLE_USER_HOME=${GRADLE_USER_HOME:-$BUILD_DIR/gradle-home}
    export ANDROID_USER_HOME=${ANDROID_USER_HOME:-$BUILD_DIR/android-home}
    # Without a passwd entry Java's user.home is "?", and Gradle then looks for
    # ?/.android/debug.keystore.
    export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:+$JAVA_TOOL_OPTIONS }-Duser.home=$BUILD_DIR/android-home"

    # Debug: androiddeployqt signs that one with the standard debug key, so it
    # installs on any device without a keystore.
    qmake_build android "$QT_ANDROID" CONFIG+=debug
    (cd "$BUILD_DIR/android" && make apk)
    local apk="$BUILD_DIR/android/android-build/prototype-tor-qt.apk"
    [ -f "$apk" ] || die "make apk ran but produced no $apk"
    install -D -m644 "$apk" "$DIST/prototype-tor-qt-debug.apk"
    echo "  $DIST/prototype-tor-qt-debug.apk  ($(du -h "$DIST/prototype-tor-qt-debug.apk" | cut -f1))"
}

build_wasm() {
    step "WebAssembly (threads)"
    need_qmake "$QT_WASM_MT_DIR" "Qt for WebAssembly with threads"
    load_emsdk

    # Qt is tied to the Emscripten it was built with; a mismatch links but can
    # misbehave at runtime.
    local want have
    want=$(sed -n 's/.*set(QT_EMCC_RECOMMENDED_VERSION "\([0-9.]*\)").*/\1/p' \
           "$QT_WASM_MT_DIR/lib/cmake/Qt6/QtPublicWasmToolchainHelpers.cmake" 2>/dev/null | head -1)
    have=$(emcc -dumpversion 2>/dev/null)
    if [ -n "$want" ] && [ "$have" != "$want" ]; then
        printf '\033[33mwarning:\033[0m emcc %s, but this Qt was built against %s\n' "$have" "$want" >&2
        [ "${ALLOW_EMSDK_MISMATCH:-0}" = 1 ] || die "set ALLOW_EMSDK_MISMATCH=1 to build anyway"
    fi
    echo "  emcc $have (Qt wants ${want:-unknown})"
    ensure_deps wasm

    qmake_build wasm "$QT_WASM_MT_DIR"

    # Only what a static host needs: our page, service worker and manifest
    # (copied in by the .pro), Qt's loader and the app itself. The generated
    # prototype-tor-qt.html lacks the IndexedDB mount, so it is left out.
    local b="$BUILD_DIR/wasm" f
    rm -rf "$DIST/web"
    mkdir -p "$DIST/web"
    for f in index.html coi-serviceworker.js manifest.webmanifest icon.svg qtloader.js \
             prototype-tor-qt.js prototype-tor-qt.wasm prototype-tor-qt.worker.js; do
        [ -f "$b/$f" ] || die "the wasm build produced no $f"
        cp "$b/$f" "$DIST/web/"
    done
    echo "  $DIST/web/  ($(du -sh "$DIST/web" | cut -f1))"
}

# --- run -----------------------------------------------------------------------

mkdir -p "$DIST"
for target in "${TARGETS[@]}"; do
    "build_$target"
done

step "done"
ls -l "$DIST"
