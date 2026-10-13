# ADR-001 — NOISE INTERNET initial architecture decision

Status: accepted (2026-10-13)
Deciders: Arena agent (implementing), user (product owner)

## 1. Environment audit (measured, not assumed)

All of the following was executed in the build sandbox on 2026-10-13 and is reproducible with
`scripts/env_probe.sh`.

| Probe | Command | Result | Consequence |
|---|---|---|---|
| OS | `cat /etc/os-release` | Debian 12 (bookworm), x86_64, kernel 6.1.158+ | Linux-only sandbox |
| CPU / RAM | `nproc`, `free -h` | 2 vCPU, 3.8 GiB | Small FFTs, avoid heavy parallel builds |
| Audio hardware | `ls /dev/snd`, `cat /proc/asound/cards` | **ABSENT** | No speaker, no microphone. Real over-the-air tests are physically impossible here → Gate B/C/F hardware portions = NOT TESTED |
| `aplay/arecord/pulse/pipewire` | `command -v` | ABSENT | No even virtual ALSA device; `snd-aloop` module cannot be loaded (no CAP_SYS_MODULE) |
| gcc / g++ | `g++ --version` | 12.2.0 | Native C++ core builds and runs |
| make | `make --version` | 4.3 | OK |
| cmake | absent from OS; installed from PyPI into `.venv` | 4.x OK | CMake usable via venv |
| clang/ninja | ABSENT | use gcc + make generator |
| JDK / `javac` / `kotlinc` / Gradle | ABSENT, and `apt.debian.org` + `deb.debian.org` are network-blocked | No JVM at all | **An APK cannot be produced in this sandbox.** Android SDK parts are served only from `dl.google.com` (blocked: verified `curl` → HTTP 000), `services.gradle.org` (blocked), `maven.google.com` (blocked). Release-binary hosts `objects.githubusercontent.com` are also blocked, so no toolchain tarball can be fetched |
| .NET SDK / MSBuild | ABSENT; `dotnet.microsoft.com` blocked (verified HTTP 000) | no C#/XAML toolchain | A .NET/WinUI Windows build cannot be produced here |
| Zig (installed from PyPI, allowed host) | `python -m ziglang version` | **0.17.0 present** | Cross-compiles real Windows binaries: verified `zig c++ -target x86_64-windows-gnu` produced a 220 KB PE32+ `t.exe` |
| Zig → Android | `zig c++ -target aarch64-linux-android` | links fail: `unable to provide libc ... android` (needs the NDK sysroot) | Android **link** impossible; Android **compile/syntax** validation of the native core is still possible (`-fsyntax-only`) |
| Zig → WebAssembly | `zig c++ -target wasm32-wasi` | works (verified 960 KB module) | The same C++ core can be shipped as WASM for a browser-based real-hardware test rig |
| Outbound network | `curl` probes | `github.com`, `api.github.com`, `codeload.github.com`, `pypi.org`, `files.pythonhosted.org` = 200. Everything else blocked | A *real external Internet* test is still possible through the proxy (e.g. `api.github.com`), which is what Gate F needs |
| Python | 3.11.2 + numpy/scipy installed into venv | OK | Independent reference implementation for cross-validation of the protocol |

## 2. Decisions taken

### D1 — One portable C++17 core, thin platform shells
The entire technical risk (DSP, modulation, FEC, sync, framing, ARQ, crypto, proxy
framing) lives in `shared/acoustic_core` as **one** C++17 library with a flat C ABI
(`include/ni/abi.h`). Android binds it through JNI, Windows links it directly, and the
browser harness (if built) binds it through WASM. This satisfies "one shared protocol
specification for both platforms" structurally rather than by promise: there is only one
implementation of the wire format, so a cross-platform mismatch is impossible by
construction. It is also the arrangement the master prompt prefers (§3 "Prefer a shared,
portable C++ DSP core").

Rejection of the alternative: two independent implementations (Kotlin DSP + C# DSP) would
triple the DSP surface we must validate and could never be *proven* equal without the very
hardware we lack.

### D2 — Windows stack: C++/Win32 + WASAPI instead of C#/.NET
The master prompt allows "or an equivalent well-supported Windows-native stack". C#/.NET was
chosen by preference in the prompt, but there is no .NET SDK in this environment and no way
to obtain one (blocked hosts). C++ with the raw WASAPI COM APIs:
* is dependency-free (no NAudio, no NuGet, no .exe bundler),
* is exactly the API layer a .NET `NAudio`/`CSCore` implementation would call,
* **can actually be compiled to a real, launchable Windows PE32+ binary in this sandbox** via Zig.
Delivering C# source that nobody can compile here would violate rule 15 ("implement the
highest-value achievable milestone, build it, run it"). The C# variant is documented in
`docs/BUILD_INSTRUCTIONS.md` as a supported alternative path with the project files that would be
needed, but it is NOT the shipped implementation, and this document states why.

### D3 — Android stack: Kotlin + AudioRecord/AudioTrack + JNI to the core; no Gradle build here
Full Kotlin sources, Gradle KTS scripts, manifest, JNI bridge, and `CMakeLists.txt` are written
as if building normally (they are what the user's Android Studio will consume). They are
labelled NOT BUILT with the exact missing-inputs list (§1). No fake APK is produced. The
Android app deliberately keeps *no* DSP in Kotlin: it is a capture/playback/service/UI/proxy
shell around the shared core, which is what makes an unbuilt Android app low-risk.

### D4 — Modem PHY: real-valued OFDM, burst-mode, per-burst sync
Chosen: passband (real) OFDM, `N_FFT=256`, cyclic prefix 64 samples, subcarrier spacing
`fs/256` (187.5 Hz at 48 kHz), bands defined in Hz so numerics adapt automatically to
44.1 kHz and 48 kHz. Per-burst CAZAC (Zadoff–Chu) preamble for timing + coarse CFO, then
differential modulation (DBPSK/DQPSK) or scattered-pilot coherent QAM (16/64-QAM),
rate-1/2 K=7 (133,171 octal) convolutional FEC with 3-bit soft-decision Viterbi, block
interleaver, CRC16 (header) + CRC32 (frame).
Justification: this is the architecture used by proven open-source acoustic modems
(`audionet`, and the design points in the SmartTones / MF-Track / Audiostore literature) —
see `docs/RELATED_WORK.md`. Reusing that *design* (not their code, to keep licensing simple)
is faster and less risky than inventing a PHY, and the master prompt explicitly asks for it
(rules 9, 17). Burst mode (sync per ≤250 ms burst) is chosen deliberately: it makes sample-clock
drift between two unsynchronised devices a non-problem (0.006 samples of drift at 500 ppm over
a whole burst) and avoids a continuous-tracking loop.
Modulation set required by the prompt §5.3 is implemented as five selectable modes over the
same engine: wideband 2-FSK (robust baseline), DBPSK, DQPSK, 16-QAM, 64-QAM + adaptive
selection from measured per-subcarrier SNR.

### D5 — Reliability first, half-duplex first, then measured full-duplex
Half-duplex turn-taking ARQ (stop-and-wait with selective repeat, cumulative ACK + NAK
bitmap) is the default. Full-duplex is implemented as *frequency-division* (each direction
own band) + NLMS acoustic echo cancellation, and is only ever labelled after ERLE and
simultaneous-both-directions numbers exist (§5.6 rule).

### D6 — Network layer: user-space HTTP/CONNECT proxy first, `VpnService` second
Milestone order from the prompt (§7.1) is followed literally: an authenticated, loopback-only,
memory-capped HTTP proxy carried over the acoustic byte pipe is the shipped, testable gateway.
`VpnService` packet tunnel on Android is implemented as real code (routes, TUN read/write,
no loop of the acoustic control path) but is documented as the *second* tier, and Windows gets
no virtual adapter at all in this milestone (would need an installer + driver, i.e. exactly the
thing the prompt says not to fake). Statuses are reported separately: HARDWARE / SIGNAL /
PEER / SESSION / FRAMES / TUNNEL / INTERNET (§6 rule) — never conflated.

### D7 — Crypto: vendored, standard primitives, validated against RFC vectors
No third-party crypto is downloadable here (no vcpkg/conan/apt). So `ni_crypto` contains
SHA-256, HMAC-SHA-256, HKDF-SHA-256, ChaCha20, Poly1305 and ChaCha20-Poly1305 AEAD written to
their RFCs, and is *validated against the RFC 6234 / RFC 4231 / RFC 8439 / RFC 5869 test-vector
sets* in `tests/crypto_tests` — so correctness is evidenced, not assumed. Pairing uses an
out-of-band 6-digit code + HKDF + challenge/response + AEAD + replay window (no custom cipher,
no embedded secret). `CMake` option `NI_USE_LIBSODIUM` lets a downstream build swap in libsodium.

### D8 — What counts as evidence in this environment
Because there is no transducer, the pipeline is validated by a hierarchy that the final report
keeps strictly separate:
1. **pure algorithmic** (frame/FEC/FFT vectors),
2. **sample-domain loopback** — the modem renders real 16-bit PCM, the receiver DSP recovers
   bytes from those samples (WAV files on disk),
3. **two-process audio-pipe test** — giver and receiver are separate OS processes and the *only*
   bytes crossing the socket are PCM audio samples after a physical channel model (band-limited
   loudspeaker+mic FIRs, multipath, AGC, clipping, noise, sample-rate offset). Payload bytes
   never traverse the pipe. A `PAYLOAD-LEAK` self-check asserts this.
4. **real Internet through the link** — HTTP/CONNECT requests executed by the giver against
   `api.github.com`/pypi, verified hash-equal to a direct fetch.
Levels 1–4 are what this sandbox can prove. Level 5 (speaker→air→microphone) requires the user's
hardware; the tooling to run it (WAV rendering, recording decode, calibration report, per-device
sweep) is shipped for exactly that reason.

## 3. Highest-risk assumptions (and their mitigation)

| # | Assumption | Risk | Mitigation shipped |
|---|---|---|---|
| A1 | Phone/laptop speakers reproduce 15–20 kHz | HIGH — OS EQ/AGC/high-pass and small drivers usually kill it; would make "ultrasonic" claims false | Bands are configuration, not assumption: `VOICE/MID/WIDE/ULTRA` profiles all measured; ULTRA is expected to fail and is reported as such; calibration writes a per-device band table |
| A2 | OS audio stacks deliver unmodified phase-linear PCM | HIGH — AGC/noise suppression/beam-forming destroy constellations; on Android `VOICE_COMMUNICATION` input preset is the usual killer | Capture path uses `MEDIA`/`UNPROCESSED` source with documented fallback; Android enables `FLAG_AUDIO_FOCUS_FLAG... ` no — it disables the session's effects via `AudioRecord` `getMinBufferSize` selection and warns if `AAudio` low-latency unavailable; Windows WASAPI in **shared** mode is known to resample → app prefers exclusive mode and reports the *actual* device mix format |
| A3 | Half-duplex turnaround timing survives OS audio jitter | MED | Turnaround windows are measured, not guessed: driver latency is calibrated and stored; timeouts are ≥ 4× measured jitter |
| A4 | 240-byte frames + FEC give usable throughput | MED — ~0.2–2 kB/s realistic | Every benchmark reports *useful payload* throughput and retransmission overhead separately from raw symbol rate (prompt §5.5, §12); the report refuses broadband wording |
| A5 | `VpnService` tunnel is accepted by carriers/Android | MED | Tunnel is optional tier; proxy tier already provides real HTTP; limitations documented (`docs/KNOWN_LIMITATIONS.md`) |
| A6 | Windows user-space proxy can be called "internet sharing" | LOW but reputational | UI never says "tethering" unless TUNNEL+INTERNET both verified; §7.3 rule encoded as a status gate in the UI code |
| A7 | Zig `x86_64-windows-gnu` output launches on real Windows (no MSVC CRT, static link) | MED | EXE is built with `-static` + no manifest dependency on VCRedist; documented as "built here, launch NOT TESTED"; a CMake/MSVC path is given for the user |

## 4. Explicit non-goals for this milestone
* No kernel driver, no wintun/Npcap installation, no root, no unsigned driver (prompt rule 11/13).
* No claim of TCP/UDP/QUIC "general tethering" until measured (Gate G will be FAIL/PARTIAL, stated as such).
* No Android APK artefact fabrication.
