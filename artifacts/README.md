# artifacts/

Built, installable things are supposed to land here. **There are none, and that is the honest state of
this project.**

| Expected artifact | State here | Why |
|---|---|---|
| `NoiseInternet.apk` | **absent** | no JDK, no Android SDK, no NDK in the authoring sandbox; `android/` has never been through a build. Not "debug-signed" either - signing needs the tools that do not exist here. |
| `NoiseInternet.exe` | **absent** | no MSVC, no MinGW-w64, no `zig`; `windows/` has never been compiled. |
| `libni_core.a` / `ni` | present but **git-ignored** (`build-*/`) | these are build outputs of `scripts/build_local.sh`; rebuild them, do not commit them. |
| measurement data | lives in `../reports/` | `RESULTS.md`, `raw/`, and the CSV/JSON exports. A binary without its log is not evidence. |

The rule for this directory: **a file may be placed here only together with the command that produced it
and the log it wrote.** A binary with no build log, and an APK with no `apkanalyzer`/`aapt` output next to
it, are exactly the kind of artifact that makes a report look finished while being unverifiable.

Build instructions, including the exact commands to sign a debug APK locally, are in
[`../android/BUILD.md`](../android/BUILD.md) and [`../windows/BUILD.md`](../windows/BUILD.md).
