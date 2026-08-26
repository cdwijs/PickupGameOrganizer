#!/usr/bin/env bash
#
# build.sh — compile prototype-qt for all three targets from the one source
# tree in src/.
#
#   ./build.sh                    # linux + tests + pwa + android
#   ./build.sh linux pwa          # only these
#   ./build.sh test               # build and run the widget test (host Qt)
#   ./build.sh --docker           # run the whole thing inside the Qt image
#   ./build.sh --docker android   # ... for one target
#
# Everything it produces lands outside the repository, under $BUILD_DIR
# (default /shared/tmp/prototype-qt, or $TMPDIR/prototype-qt off this setup):
#
#   $BUILD_DIR/dist/counter                  native Linux executable
#   $BUILD_DIR/dist/pwa/                     installable PWA — serve this folder
#   $BUILD_DIR/dist/counter-debug.apk        Android package, arm64-v8a
#
# It needs Qt 6 for each target it is asked to build; the claude-code-agent-qt
# image (Dockerfile.qt in claude-code-docker) has all three, which is what
# --docker uses. See README.md.

set -euo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)

if [ -d /shared/tmp ]; then
    BUILD_DIR=${BUILD_DIR:-/shared/tmp/prototype-qt}
else
    BUILD_DIR=${BUILD_DIR:-${TMPDIR:-/tmp}/prototype-qt}
fi
DIST="$BUILD_DIR/dist"

QT_IMAGE=${QT_IMAGE:-claude-code-agent-qt:latest}
ANDROID_ABI=${ANDROID_ABI:-arm64-v8a}

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# --- argument parsing --------------------------------------------------------

USE_DOCKER=0
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --docker)             USE_DOCKER=1 ;;
        linux|pwa|android|test) TARGETS+=("$arg") ;;
        all)                  TARGETS+=(linux test pwa android) ;;
        -h|--help)            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                    die "unknown argument '$arg' (try --help)" ;;
    esac
done
[ ${#TARGETS[@]} -gt 0 ] || TARGETS=(linux test pwa android)

# --- --docker: re-run this script inside the Qt image ------------------------
#
# The build container gets the same two bind mounts this workspace has, so it
# compiles the working tree in place. Host paths come from the running
# container's own mount table — inside a container, /workspace is not a path
# the daemon can resolve.

if [ "$USE_DOCKER" = 1 ]; then
    command -v docker >/dev/null || die "--docker needs the docker CLI"
    docker image inspect "$QT_IMAGE" >/dev/null 2>&1 \
        || die "image $QT_IMAGE not found — build it from Dockerfile.qt in claude-code-docker"

    mount_source() {
        docker inspect "$(hostname)" \
            --format "{{range .Mounts}}{{if eq .Destination \"$1\"}}{{.Source}}{{end}}{{end}}" 2>/dev/null
    }
    HOST_WORKSPACE=${HOST_WORKSPACE:-$(mount_source /workspace)}
    HOST_SHARED_TMP=${HOST_SHARED_TMP:-$(mount_source /shared/tmp)}
    [ -n "$HOST_WORKSPACE" ] || die "can't resolve the host path of /workspace — set HOST_WORKSPACE"
    case "$SRC" in /workspace/*) ;; *) die "--docker expects the tree under /workspace, not $SRC" ;; esac

    mounts=(-v "$HOST_WORKSPACE:/workspace")
    [ -n "$HOST_SHARED_TMP" ] && mounts+=(-v "$HOST_SHARED_TMP:/shared/tmp")

    step "building in $QT_IMAGE"
    # Runs as root (the image has no agent user), so hand the results back to
    # whoever started the build.
    docker run --rm -i --entrypoint bash "${mounts[@]}" \
        -e HOME=/root -e BUILD_DIR="$BUILD_DIR" "$QT_IMAGE" -lc \
        "set -e
         cd '$SRC'
         ./build.sh ${TARGETS[*]}
         chown -R $(id -u):$(id -g) '$BUILD_DIR'"
    exit 0
fi

# --- toolchain discovery -----------------------------------------------------
#
# QT_HOST_LINUX in the image points at a directory that does not exist in every
# generation of it, so the host Qt is looked for rather than trusted. The host
# Qt is what supplies moc, rcc and androiddeployqt when cross-compiling.

find_host_qt() {
    local candidate
    for candidate in "${QT_HOST_LINUX:-}" "${QT_ROOT:-}/gcc_64" "${QT_ROOT:-}/linux_gcc_64"; do
        [ -n "$candidate" ] && [ -f "$candidate/bin/qt-cmake" ] && { echo "$candidate"; return; }
    done
}
QT_HOST=$(find_host_qt)

cmake_build() { cmake --build "$1" --parallel "$(nproc)"; }

# aqt's wasm package ships bin/qt-cmake without the executable bit, so every
# qt-cmake is run through sh rather than executed directly.
qt_cmake() { sh "$1/bin/qt-cmake" "${@:2}"; }

# --- targets -----------------------------------------------------------------

build_linux() {
    step "Linux (x86_64)"
    local b="$BUILD_DIR/linux"
    if [ -n "$QT_HOST" ]; then
        qt_cmake "$QT_HOST" -S "$SRC" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release
    else
        # No aqt Qt: fall back to the distribution's Qt 6 (qt6-base-dev).
        command -v qmake6 >/dev/null || die "no Qt 6 for Linux found (looked at \$QT_HOST_LINUX, \$QT_ROOT and qmake6)"
        cmake -S "$SRC" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release
    fi
    cmake_build "$b"
    install -D -m755 "$b/counter" "$DIST/counter"
    echo "  $DIST/counter"
}

build_test() {
    step "tests"
    local b="$BUILD_DIR/linux"   # the tests are part of the host build
    [ -f "$b/CMakeCache.txt" ] || build_linux
    cmake_build "$b"
    ctest --test-dir "$b" --output-on-failure
}

build_pwa() {
    step "WebAssembly / PWA"
    local b="$BUILD_DIR/wasm" qt_wasm=${QT_WASM:-${QT_ROOT:-}/wasm_singlethread}
    [ -f "$qt_wasm/bin/qt-cmake" ] || die "no Qt for WebAssembly at $qt_wasm (set \$QT_WASM)"
    [ -n "$QT_HOST" ] || die "cross-compiling needs a host Qt for moc/rcc (set \$QT_HOST_LINUX)"

    # Qt hard-codes the Emscripten it was built against, and a different one is
    # not a portability problem to shrug at: 3.1.56 against Qt 6.7.3 links and
    # runs but never paints, so the page comes up blank with QPainter warnings
    # in the console and nothing to suggest the toolchain. Find the version Qt
    # wants, then an emsdk that provides it.
    local want emsdk
    want=$(sed -n 's/.*set(QT_EMCC_RECOMMENDED_VERSION "\([0-9.]*\)").*/\1/p' \
           "$qt_wasm/lib/cmake/Qt6/QtPublicWasmToolchainHelpers.cmake" 2>/dev/null | head -1)
    for emsdk in "${EMSDK_DIR:-}" "/shared/tmp/emsdk-$want" /opt/emsdk; do
        [ -n "$emsdk" ] && [ -f "$emsdk/emsdk_env.sh" ] || continue
        # emsdk_env.sh is chatty on stdout and unset-variable-hostile.
        set +u
        # shellcheck disable=SC1091
        . "$emsdk/emsdk_env.sh" >/dev/null 2>&1
        set -u
        command -v emcc >/dev/null || continue
        [ -z "$want" ] && break
        [ "$(emcc -dumpversion 2>/dev/null)" = "$want" ] && break
    done
    command -v emcc >/dev/null || die "emcc not on PATH (set \$EMSDK_DIR to an emsdk checkout)"

    local have; have=$(emcc -dumpversion 2>/dev/null)
    if [ -n "$want" ] && [ "$have" != "$want" ]; then
        printf '\033[33mwarning:\033[0m emcc %s, but this Qt was built against %s.\n' "$have" "$want" >&2
        printf '  Mismatched Emscripten produces a page that loads and stays blank.\n' >&2
        printf '  Fix the image (ARG EMSDK_VERSION=%s in Dockerfile.qt), point\n' "$want" >&2
        printf '  $EMSDK_DIR at an emsdk with %s, or set ALLOW_EMSDK_MISMATCH=1.\n' "$want" >&2
        [ "${ALLOW_EMSDK_MISMATCH:-0}" = 1 ] || die "refusing to build a blank PWA"
    fi
    echo "  emcc $have (Qt wants ${want:-unknown})"

    qt_cmake "$qt_wasm" -S "$SRC" -B "$b" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$QT_HOST"
    cmake_build "$b"

    # Qt emits counter.html + counter.js + counter.wasm + qtloader.js. Only the
    # HTML is touched: the PWA head block goes in, and the entry point becomes
    # index.html so the manifest's start_url and the service worker's navigation
    # fallback both point at a real file.
    rm -rf "$DIST/pwa"
    mkdir -p "$DIST/pwa"
    cp "$b"/counter.js "$b"/counter.wasm "$b"/qtloader.js "$DIST/pwa/"
    [ -f "$b/qtlogo.svg" ] && cp "$b/qtlogo.svg" "$DIST/pwa/"
    cp "$SRC"/web/manifest.json "$SRC"/web/sw.js "$SRC"/web/icon.svg "$DIST/pwa/"
    python3 - "$b/counter.html" "$SRC/web/pwa-head.html" "$DIST/pwa/index.html" <<'PY'
import sys
shell_path, head_path, out_path = sys.argv[1:4]
shell = open(shell_path, encoding='utf-8').read()
head = open(head_path, encoding='utf-8').read()
if '</head>' not in shell:
    raise SystemExit(f'{shell_path}: no </head> to inject into — Qt changed its shell template')
shell = shell.replace('<title>counter</title>', '<title>Qt counter</title>')
shell = shell.replace('</head>', head.rstrip('\n') + '\n  </head>', 1)
open(out_path, 'w', encoding='utf-8').write(shell)
PY
    echo "  $DIST/pwa/  ($(du -sh "$DIST/pwa" | cut -f1))"
}

build_android() {
    step "Android ($ANDROID_ABI)"
    local b="$BUILD_DIR/android" qt_android=${QT_ANDROID:-${QT_ROOT:-}/android_${ANDROID_ABI//-/_}}
    [ -f "$qt_android/bin/qt-cmake" ] || die "no Qt for Android at $qt_android (set \$QT_ANDROID)"
    [ -n "$QT_HOST" ] || die "cross-compiling needs a host Qt for androiddeployqt (set \$QT_HOST_LINUX)"
    [ -n "${ANDROID_SDK_ROOT:-}" ] || die "set \$ANDROID_SDK_ROOT"
    [ -n "${ANDROID_NDK_ROOT:-}" ] || die "set \$ANDROID_NDK_ROOT"

    # Gradle downloads its own distribution and the Android plugin; keeping its
    # home under $BUILD_DIR means the second build is offline and fast.
    export GRADLE_USER_HOME=${GRADLE_USER_HOME:-$BUILD_DIR/gradle-home}
    # And the debug signing key with it, so a rebuild in a fresh container
    # still installs over the copy already on the phone instead of being
    # refused for a signature mismatch.
    export ANDROID_USER_HOME=${ANDROID_USER_HOME:-$BUILD_DIR/android-home}

    # Debug, not Release: androiddeployqt only signs the release APK when it is
    # handed a keystore, while the debug one is signed with the standard debug
    # key and installs on any device. See README.md for a release build.
    qt_cmake "$qt_android" -S "$SRC" -B "$b" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DQT_HOST_PATH="$QT_HOST" \
        -DQT_ANDROID_ABIS="$ANDROID_ABI" \
        -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
        -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT"
    cmake_build "$b"
    cmake --build "$b" --target apk

    local apk
    apk=$(find "$b" -name '*.apk' -newer "$b/CMakeCache.txt" -print -quit)
    [ -n "$apk" ] || die "the apk target ran but produced no .apk under $b"
    install -D -m644 "$apk" "$DIST/counter-debug.apk"
    echo "  $DIST/counter-debug.apk  ($(du -h "$DIST/counter-debug.apk" | cut -f1))"
}

# --- run ---------------------------------------------------------------------

mkdir -p "$DIST"
for target in "${TARGETS[@]}"; do
    "build_$target"
done

step "done"
ls -l "$DIST"
