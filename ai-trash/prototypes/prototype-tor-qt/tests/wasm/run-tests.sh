#!/usr/bin/env bash
#
# run-tests.sh — build and run the WebAssembly tests under Node:
#   netshim_test  unit test of src/wasm/netshim.c
#   tor_spike     the real libtor.a on a pthread on top of netshim (no network)
#
# Needs deps/build-deps.sh wasm first. Output goes to $OUT_DIR
# (default /shared/tmp/tor-qt-wasm-tests).

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEPS=${DEPS_PREFIX:-$ROOT/deps/install/wasm}
OUT_DIR=${OUT_DIR:-/shared/tmp/tor-qt-wasm-tests}
EMSDK=${EMSDK:-/opt/emsdk}
command -v emcc >/dev/null || source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
[ -f "$DEPS/lib/libtor.a" ] || { echo "missing $DEPS/lib/libtor.a - run deps/build-deps.sh wasm first" >&2; exit 1; }
mkdir -p "$OUT_DIR"

COMMON=(-O2 -Wall -pthread -sEXIT_RUNTIME=1 -I"$ROOT/src/wasm")

emcc "${COMMON[@]}" -sPTHREAD_POOL_SIZE=2 \
    "$ROOT/src/wasm/netshim.c" "$HERE/netshim_test.c" -o "$OUT_DIR/netshim_test.js"
node "$OUT_DIR/netshim_test.js"

# tor wants more than Emscripten's default 64 KB pthread stack.
emcc "${COMMON[@]}" -sPTHREAD_POOL_SIZE=8 -sDEFAULT_PTHREAD_STACK_SIZE=1048576 -sSTACK_SIZE=1048576 \
    -sINITIAL_MEMORY=134217728 -I"$DEPS/include" \
    "$ROOT/src/wasm/netshim.c" "$HERE/tor_spike.c" \
    "$DEPS/lib/libtor.a" "$DEPS/lib/libevent.a" "$DEPS/lib/libssl.a" "$DEPS/lib/libcrypto.a" "$DEPS/lib/libz.a" \
    -o "$OUT_DIR/tor_spike.js"
timeout 120 node "$OUT_DIR/tor_spike.js" | grep -vE '^\w{3} [0-9]+ [0-9:.]+ \[notice\]'
