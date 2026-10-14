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
installed on a phone, so over-the-air performance and the two hardware-dependent gates are NOT TESTED. The
proxied-HTTP leg now does run end to end in the model - `curl` -> receiver's proxy -> acoustic link -> the
giver's fetch helper -> an origin, and the origin's own log shows the request arriving in a measured run -
but the **response** does not get back yet (the receiver reports the reply's frames as malformed and the
proxy answers 504), so no byte of an origin response has ever crossed this link and nothing here is
described as internet-sharing. Nothing in this repository is reported as an internet speed; a symbol rate
that no transducer has produced is not a throughput claim.

## Before the first pairing attempt: three things that must match

A pairing handshake is a three-way exchange between two devices, and if any of these three disagree the
two ends are silent to each other - which is what the first hardware report ("it never paired, from both
the exe and the apk") looked like. All three are now enforced or unified, and both apps display what they
are running so a mismatch is visible instead of mysterious:

1. **Passphrase.** 8+ characters, typed the same on both devices. The engine refuses to start without one
   (before this, it simply sent no pairing challenge and reported nothing), and both apps refuse before
   opening the microphones. The match code appears only after a successful pairing, so an empty match code
   with an empty passphrase field is the first thing to check.
2. **Profile and session id.** Both apps now open on **FD-LOW-DBPSK (profile 10)** and **session 7**; they
   used to default to different values (profile 11/session 7 on the PC, profile 2/session 0 on the phone),
   and two endpoints on different profiles or sessions cannot decode each other at all. If you change one,
   change it on both - the two panels show both numbers.
3. **Adaptive modulation, off by default.** It used to be on in every node, and a profile change moved the
   requester's own modem while only asking the peer to follow, so one lost control frame left the two ends
   on different profiles permanently. It is now off unless a caller opts in, it cannot fire before the
   link is up, and a change is committed only after the peer acknowledges it.

What the model says is *not* an explanation for a link that will not pair, so it is not worth guessing
about: lost bursts up to 10%, a 100-300 ms sound-card queue, and a 300 ppm clock offset between the two
devices all still pair and deliver (`reports/RESULTS.md` §4.8). What the model cannot know is the room -
so both apps show the measured input level, whether bursts are arriving at all, and whether they decode,
which is what separates "nothing audible" from "audible but unintelligible" in one line.

## Prebuilt binaries

Built by CI from the tip of the working branch and published as the pre-release **`ci-build`**:
<https://github.com/shuvrajeetkamila/noiseinternet/releases/tag/ci-build>

- `noiseinternet-debug.apk` - Android 6.0+, arm64-v8a and x86_64, **debug-signed** with a throwaway key that
  no longer exists.
- `NoiseInternet-windows-x64.zip` - the standalone Windows app (C++ runtime linked in, so no redistributable
  to install; `dumpbin /dependents` in CI verifies that rather than assuming it). `NoiseInternet.exe` in it is
  the **windowed** app: role radios, the machine's real microphones and speakers in drop-downs, profile,
  session id, passphrase, a read-only **match code** field, Start/Stop/Switch role/Calibrate/Link test/
  Internet test, a measured status panel and an engine log. `NoiseInternet-console.exe` is the same app from
  a terminal (`--list-devices`, `--wav-*`, single-key tests), plus `ni.exe`, `README-RUN.md`,
  `NOTICE-redist.txt`, `ui-check.txt` (the control inventory and endpoint count the build agent measured
  when it ran the window: 26 controls, 16 profiles, 0 endpoints because a build agent has none, `ui-check PASSED`), `BUILD-INFO.txt` with a hash per file, and `redist/` holding `vcruntime140.dll`,
  `vcruntime140_1.dll` and `msvcp140.dll` for a dynamically-linked build.
- `noiseinternet-android-windows.zip` - both of the above plus the instructions at the top level.
- `NoiseInternet.exe` - the bare windowed app, for when you already have the rest.

Both roles exist on both platforms and either machine can be the one that gives the internet: the two ends
exchange bursts in both directions the whole time, and `Start` / `Switch role` change only *who fetches*.
What is not finished is the last mile of the tunnel - see `packaging/windows/README-RUN.md`, "Who gives, who
receives, and which way the bytes go" - so this is not yet a working "share my internet" product.

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
