#!/bin/sh
# Build app.wasm from the C sources. Freestanding wasm32: no libc, no
# emscripten, nothing but clang and wasm-ld.
#
#   ./build.sh          build
#   ./build.sh -g       build without optimisation, keeping names for debugging
set -e
cd "$(dirname "$0")"

CLANG=${CLANG:-clang}
export PATH="/usr/lib/llvm-20/bin:$PATH"

OPT=-O2
[ "$1" = "-g" ] && OPT="-O0 -g"

$CLANG --target=wasm32 -nostdlib $OPT \
  -Wall -Wextra -Wno-unused-parameter \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--stack-first \
  -o app.wasm src/*.c

ls -l app.wasm
