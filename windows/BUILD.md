# Building and running NoiseInternet.exe (Windows)

**Status: compiled, linked and unit-tested by CI (8/8 ctest suites on MSVC); never run on audio
hardware.** The machine this project was written on is a Linux sandbox with no MSVC, no MinGW-w64 and no
`zig`, so nothing below was executed locally - the compiler that checked it is GitHub Actions' Windows
runner, and the published binaries are described in
[`../artifacts/README.md`](../artifacts/README.md). Do not read any of that as "the Windows app works over
sound": no message has crossed a speaker and a microphone yet. The measured state of the whole project is
[`../reports/RESULTS.md`](../reports/RESULTS.md) and [`../docs/STATUS.md`](../docs/STATUS.md).

**The exe is standalone.** `NI_STATIC_RUNTIME` (ON by default) links the C++ runtime into the binary with
`/MT`, so `NoiseInternet.exe` needs no Visual C++ redistributable; the status screen prints which CRT it
was built against, and the CI zip ships the three runtime DLLs under `redist/` anyway, as a fallback for a
`-DNI_STATIC_RUNTIME=OFF` build. To run what you downloaded: `README-RUN.md` at the root of the zip, in
the repository as [`../packaging/windows/README-RUN.md`](../packaging/windows/README-RUN.md).

## Layout

```
windows/
  CMakeLists.txt                     builds ni_core + NoiseInternet.exe + ni.exe, and the unit tests
  include/ni/audio_wasapi.h          the backend contract the shared core calls under -DNI_WITH_WASAPI
  src/NoiseInternet/audio_wasapi.cpp WASAPI device enumeration + shared-mode render/capture
  src/NoiseInternet/main.cpp         console front end: role switch, calibration, link and internet tests
```

The protocol, framing, FEC, crypto, ARQ, proxy and calibration logic are **not** duplicated here: they
are the same `shared/acoustic_core` sources the Android app and the Linux CLI compile. `NI_WITH_WASAPI`
is the only thing that changes behaviour, and it only switches on device enumeration and the audio pump.

## Prerequisites

* Windows 10/11, Visual Studio 2019/2022 with the "Desktop development with C++" workload
  **or** MSYS2/MinGW-w64 (`pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake
  mingw-w64-ucrt-x86_64-ninja`), **or** `zig` 0.13+ as a drop-in `cc`/`cxx`.
* CMake >= 3.16.
* A machine with a working speaker *and* microphone. A loopback device ("Stereo Mix", "What U Hear")
  is enumerated and flagged `[LOOPBACK - unusable as a microphone]`; do not use one for a real test,
  because it would measure the sound card, not the room.

## Configure and build

```bat
:: Visual Studio
cd noiseinternet\windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build --output-on-failure

:: MinGW-w64 / MSYS2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For zig as the compiler (cross from Linux, produces an `.exe` but cannot run it):

```sh
zig cc -target x86_64-windows-gnu   # use as CMAKE_C_COMPILER, CMAKE_CXX_COMPILER = zig c++
cmake -S . -B build-zig -DCMAKE_C_COMPILER=zig -DCMAKE_CXX_COMPILER="zig;c++" ...
```
`link ole32 uuid ksuser` must resolve; with `-target x86_64-windows-gnu` zig ships its own import
libraries, so the cross-build can compile and link even though the resulting binary has never been
executed. That would be a *compiled* claim, never a *tested* claim.

## Run

```bat
build\Release\NoiseInternet.exe --list-devices
build\Release\NoiseInternet.exe --giver --spk {endpoint-id} --mic {endpoint-id} --secret "correct horse" --profile 3
build\Release\NoiseInternet.exe --receiver --secret "correct horse" --profile 3 --proxy-port 8471
```

Interactive keys (blank line reprints the status screen): `a` calibrate, `l` link test, `i` internet
test, `r` switch role, `p` profile table, `s` status JSON, `q` quit.

Point the receiving machine's browser at `http://127.0.0.1:8471` (the receiver's local proxy) to use
whatever the link currently carries. Nothing is bound to a routable address: the proxy listens on
127.0.0.1 only, and there is no inbound firewall rule to open.

## Codec-only check without a room (explicitly not an acoustic test)

```bat
NoiseInternet.exe --receiver --wav-in rec.wav --wav-out tx.wav
```
This reads captured samples from a file and writes the tones it wanted to play. It exercises the modem,
frame codec and link state machine on Windows, and the status screen prints
`[FILE MODE - not an acoustic link]` on every refresh so the number cannot be mistaken for over-the-air
performance.

## What still cannot be proven from here

1. That WASAPI `Initialize` accepts the float format chosen for every real device (the code refuses the
   stream and prints the device's preferred rate rather than resampling — a device that wants 44.1 kHz
   needs `--fs 44100`).
2. That `--duplex` (FDD + NLMS AEC) is anything other than an experiment on the modelled channel.
3. Any throughput, latency or loss figure for a real room. The only session-level measurement that
   exists today is `delivered 0` in the simulator (see `reports/linksim.csv`), which must be fixed
   before any Windows run reports success.
