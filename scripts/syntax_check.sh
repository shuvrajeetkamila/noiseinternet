#!/usr/bin/env bash
# Quick syntax gate used while developing: compiles every core source to an object file.
# Full build + tests: scripts/build_core.sh
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=shared/acoustic_core
OUT=${NI_OBJDIR:-/tmp/niobj}
mkdir -p "$OUT"
FLAGS="-std=c++17 -O1 -g0 -I$SRC/include -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare"
n=0
fail=0
for f in "$SRC"/src/*.cpp; do
  o="$OUT/$(basename "$f").o"
  if ! g++ $FLAGS -c "$f" -o "$o" 2> "$OUT/$(basename "$f").log"; then
    echo "=== $(basename "$f") ==="
    head -25 "$OUT/$(basename "$f").log"
    fail=1
  fi
  n=$((n+1))
done
echo "checked $n files (objects in $OUT)"
exit $fail
