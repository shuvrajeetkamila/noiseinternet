#!/usr/bin/env bash
# build_apk.sh - local wrapper around the Gradle debug build of the Android app.
#
# This is the same task CI runs (.github/workflows/build.yml -> job "android"), packaged as one command
# for a machine that has the SDK. It does not download an SDK, invent a keystore, or pretend a build
# succeeded: if the toolchain is missing it says which piece is missing and stops.
#
# Requires, in this order:
#   1. JDK 17                              (java -version)
#   2. Android cmdline-tools + SDK         (ANDROID_HOME or ANDROID_SDK_ROOT set)
#   3. These SDK packages, exact versions:  platforms;android-34  build-tools;34.0.0
#                                           ndk;26.1.10909125     cmake;3.22.1
#   4. Gradle 8.7+ (this script uses `gradle`; add a wrapper with `gradle wrapper` if you prefer)
set -u
here=$(cd "$(dirname "$0")" && pwd)
need() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING: $2"; exit 1; }; }
need java "JDK 17 (java not on PATH)"
need gradle "Gradle 8.7+ (https://gradle.org/install, or use Android Studio's bundled Gradle)"
: "${ANDROID_HOME:=${ANDROID_SDK_ROOT:-}}"
[ -n "$ANDROID_HOME" ] || { echo "MISSING: ANDROID_HOME (or ANDROID_SDK_ROOT) is not set"; exit 1; }
for p in "platforms/android-34" "build-tools/34.0.0" "ndk/26.1.10909125" "cmake/3.22.1"; do
  [ -d "$ANDROID_HOME/$p" ] || { echo "MISSING SDK package: install it with  sdkmanager \"${p/\//;}\""; exit 1; }
done
cd "$here"
gradle :app:assembleDebug --no-daemon --stacktrace "$@" || exit 1
apk=$(find app/build/outputs/apk/debug -name '*.apk' 2>/dev/null | head -1)
[ -n "$apk" ] || { echo "gradle reported success but no APK was found under app/build/outputs/apk"; exit 1; }
echo
echo "built: $apk"
echo "signed with: the debug keystore Gradle created (\$HOME/.android/debug.keystore) - NOT a release key"
echo "install:     adb install -r \"$apk\""
