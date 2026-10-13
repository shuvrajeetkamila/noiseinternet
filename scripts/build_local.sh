#!/usr/bin/env bash
# build_local.sh — configure + build the shared core, the `ni` CLI and the test suite with the
# host compiler. This is the fastest loop for development and the build that produced the
# evidence in docs/RESULTS.md on Linux/gcc. Windows is built natively with MSVC (see
# windows/BUILD.md) and Android with android/build_apk.sh; both also run in CI
# (.github/workflows/build.yml), which is where the published APK and EXE come from.
#
#   scripts/build_local.sh [build-dir] [--debug] [--tests-off] [--clean]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$here/shared/acoustic_core"
# first positional argument may be the build dir; if it starts with '-' it is a flag instead
case "${1:-}" in
  ''|-*) BUILD="$here/build-linux" ;;
  *) BUILD="$1"; shift ;;
esac
BUILD_TYPE=Release
TESTS=ON
CLEAN=0
for a in "$@"; do
  case "$a" in
    --debug) BUILD_TYPE=Debug ;;
    --tests-off) TESTS=OFF ;;
    --clean) CLEAN=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

CMAKE="$(command -v cmake || true)"
if [ -z "$CMAKE" ] && [ -x /home/user/.venv/bin/cmake ]; then CMAKE=/home/user/.venv/bin/cmake; fi
if [ -z "$CMAKE" ]; then echo "cmake not found (install cmake or use a python venv with it)" >&2; exit 1; fi

[ "$CLEAN" = 1 ] && rm -rf "$BUILD"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
"$CMAKE" -S "$CORE" -B "$BUILD" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DNI_BUILD_TESTS="$TESTS"
make -C "$BUILD" -j"$NPROC"
echo
echo "built: $BUILD/ni   (core: $BUILD/libni_core.a)"
"$BUILD/ni" audit
