#!/bin/sh
# Every layer of the C, checked against an independent implementation:
# node:crypto and WebCrypto for the primitives, and prototype-minimal's own
# JavaScript for the roster.
#
#   ./run-tests.sh
#
# Builds its harnesses into a scratch directory ($WASM_TEST_BUILD, default
# /shared/tmp/prototype-webassembly) so nothing generated lands in the tree.
set -e
cd "$(dirname "$0")"

BUILD=${WASM_TEST_BUILD:-/shared/tmp/prototype-webassembly}
mkdir -p "$BUILD"
export WASM_TEST_BUILD="$BUILD"
export PATH="/usr/lib/llvm-20/bin:$PATH"
CLANG=${CLANG:-clang}
SRC=../src

build() {
  $CLANG --target=wasm32 -nostdlib -O2 -I$SRC \
    -Wl,--no-entry -Wl,--export-dynamic -o "$BUILD/t_$1.wasm" $2 "harness_$1.c"
}

echo "building the app module"
(cd .. && ./build.sh >/dev/null)

echo "building test harnesses"
build sha  "$SRC/base.c $SRC/sha256.c"
build aes  "$SRC/base.c $SRC/aesgcm.c"
build p256 "$SRC/base.c $SRC/p256.c"

# The roster test compares against prototype-minimal's own functions, lifted
# out of its app.js so the comparison is with the real thing rather than a
# restatement of it.
awk '/^\/\/ ---- roster parser/,/^\/\/ ---- state \+ render/' \
  ../../prototype-minimal/app.js | head -n -1 > "$BUILD/js_roster.mjs"
cat >> "$BUILD/js_roster.mjs" <<'JS'
export { parseRoster, renderRoster, capitalizeName, displayName, isUserIn,
         normalizeUserSlots, addUser, removeUser, countFilled, shortWeekday, extractTime };
JS

fail=0
for t in sha.test.mjs aes.test.mjs p256.test.mjs app.test.mjs roster.test.mjs; do
  echo
  echo "=== $t ==="
  node "$t" || fail=1
done
echo
[ $fail -eq 0 ] && echo "ALL SUITES PASSED" || echo "SUITE FAILURES"
exit $fail
