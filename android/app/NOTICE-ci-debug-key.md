# The committed debug signing key (`android/ci-debug.p12`)

**This file is a debug key. It is public on purpose. It must never sign a release build.**

## Why it exists

Every CI run used to let the Android Gradle plugin generate `~/.android/debug.keystore` on the runner —
a brand-new random keypair per build. Android refuses to install an APK whose signature differs from the
installed copy of the same application id ("App not installed", "package conflicts"). So each published
APK could only be installed after uninstalling the previous one, and a user who installed the new file
without uninstalling the old one saw an install failure, not an app. In round 5 that read as "did not
work", and there was no way to tell the two apart — the app did not even say which build it was.

A committed key fixes both problems: build N+1 installs over build N, and the build stamp shown in the
app (see `NI_BUILD_STAMP`) tells the user which build they are holding.

## What it is

| | |
|---|---|
| file | `android/ci-debug.p12` (PKCS#12) |
| alias | `noiseinternetci` |
| store password | `noiseinternet` |
| key password | `noiseinternet` |
| key | RSA 2048, generated 2026-10-15, valid 30 years |
| subject | `CN=Noise Internet CI debug key, O=Noise Internet (debug builds only)` |
| sha256 of the file | `6e9c381c9f8f8f37279ccf0416b08e8bbb2a35be3094ea5232fe55f3e00b8e28` |

Because the password and the key live in the same repository, **anybody** can produce an APK that
Android will accept as an update to this app. That is exactly the same exposure as the stock debug
keystore (`android`/`androiddebugkey`) that countless CI systems commit, and it is acceptable **only**
for an app that is installed by hand from this repository and is on no store. It is not acceptable for a
release, and `android/app/build.gradle.kts` deliberately leaves the `release` build type unsigned so that
this key cannot be used by accident.

## If this app is ever released for real

1. Generate a release key (`keytool -genkeypair ...`), keep it out of the repository, and add a
   `release` signing config reading from an uncommitted `keystore.properties` or from CI secrets.
2. Note that switching to a different key means users must uninstall the debug build once — Android
   will not update across signatures.
3. Remove this file and this notice, or keep them marked clearly as debug-only.
