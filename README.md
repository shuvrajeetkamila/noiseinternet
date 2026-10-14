# NOISE INTERNET

Sharing an internet connection between a Windows PC and an Android phone **through sound**: the giver
encodes bytes into an audible waveform, plays it out of a loudspeaker, the other end hears it on its
microphone and decodes it - and vice versa, in both directions. No Wi-Fi Direct, no Bluetooth, no USB, no
shared router. It exists for the situation where the only path between two machines is air.

This repository contains the real modem, not a mock-up: OFDM/DBPSK/FSK profiles with convolutional FEC,
CRC-8/CRC-32 framing, a versioned frame format, ARQ with acks and selective retransmit, challenge-response
pairing with ChaCha20-Poly1305 and replay protection, a calibration sweep, a loopback-only HTTP/CONNECT
proxy, and an Android `VpnService` tunnel that carries UDP and DNS.

## Read these first, in this order

| | |
|---|---|
| [`docs/STATUS.md`](docs/STATUS.md) | gates A-H, each **PASS / PARTIAL / FAIL / NOT TESTED**, with the reason |
| [`reports/RESULTS.md`](reports/RESULTS.md) | every measurement, the benchmark tables, and 28 defects found while building this (21 fixed) |
| [`packaging/windows/README-RUN.md`](packaging/windows/README-RUN.md) | how to run the built Windows exe and the Android apk, step by step, including what to record |
| [`artifacts/README.md`](artifacts/README.md) | the published binaries: sizes, hashes, what they are and what they are not |

## Current state, in four sentences

The engine compiles and passes its unit tests on Linux, Windows (MSVC) and Android (NDK r26), and its wire
format, FEC and AEAD construction agree with independently generated reference vectors. A 2 048-byte
message crosses the **modelled** device chain in both directions at once, reassembled and SHA-256-verified
at each end, in 30.3 s - about 540 bit/s, roughly 67 bytes per second. **No message has ever crossed real
speakers and a real microphone**: no build host here has audio hardware, and neither binary has been
installed on a phone, so over-the-air performance and the two hardware-dependent gates are NOT TESTED, and
the proxy/VPN leg is FAIL. Nothing in this repository is reported as an internet speed; a symbol rate that
no transducer has produced is not a throughput claim.

## Prebuilt binaries

Built by CI from the tip of the working branch and published as the pre-release **`ci-build`**:
<https://github.com/shuvrajeetkamila/noiseinternet/releases/tag/ci-build>

- `noiseinternet-debug.apk` - Android 6.0+, arm64-v8a and x86_64, **debug-signed** with a throwaway key that
  no longer exists.
- `NoiseInternet-windows-x64.zip` - the standalone Windows exe (C++ runtime linked in, so no redistributable
  to install; `dumpbin /dependents` in CI verifies that rather than assuming it), `ni.exe`,
  `README-RUN.md`, `NOTICE-redist.txt`, `BUILD-INFO.txt` with a hash per file, and `redist/` holding
  `vcruntime140.dll`, `vcruntime140_1.dll` and `msvcp140.dll` for a dynamically-linked build.
- `noiseinternet-android-windows.zip` - both of the above plus the instructions at the top level.
- `NoiseInternet.exe` - the bare executable, for when you already have the rest.

Unsigned, debug-signed, and untested on hardware. Every green push republishes them with new hashes, so
verify a download against the manifest on that page rather than against a number quoted in a README.

## Layout

```
shared/acoustic_core/   the engine: modem, framing, FEC, ARQ, pairing, crypto, proxy, CLI (`ni`), 8 test suites
android/                Kotlin app (roles, devices, profile, calibration, link/internet tests) + VpnService tunnel + JNI
windows/                WASAPI backend and console front end, CMake, BUILD.md
packaging/windows/      README-RUN.md and the redist notice that go inside the Windows zip
protocol/               frame-format spec, modulation profile table, reference vectors
python/                 generator for those vectors (uses `cryptography` as the independent implementation)
tests/physical/         the over-the-air protocol and the pair script that refuses to run without devices
reports/                RESULTS.md, linksim.csv, bench_sweep.csv/.json, raw logs, reproduce.sh
docs/                   STATUS.md (gates), ADR-001 (design decisions and rejected alternatives)
scripts/                build_local.sh, syntax_check.sh, raw_to_csv.py
```

## Build and test it yourself

```
bash scripts/build_local.sh          # Linux: cmake + tests, needs only a C++17 compiler
cd build-linux && ctest              # 8 suites, ~3 600 assertions
./ni selftest --trials 5 --snr 25    # per-profile modem performance in the modelled room
./ni linksim --bytes 2048 --profile 11 --snr 25 --no-multipath   # the verified 2 kB transfer
```

Windows: `cmake -S windows -B build && cmake --build build --config Release` (see
[`windows/BUILD.md`](windows/BUILD.md)). Android: `bash android/build_apk.sh` (JDK 17, SDK 34, NDK
26.1.10909125 - see [`android/BUILD.md`](android/BUILD.md)). `.github/workflows/build.yml` is what CI runs,
and it posts the Windows build/test/package transcript onto the commit as a comment.
