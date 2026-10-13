# NOISE INTERNET — implementation status

Updated 2026-10-13. Branch `arena/15b1e419-noiseinternet`. Everything below describes this working
tree; the measured evidence lives in [`reports/RESULTS.md`](../reports/RESULTS.md) and its raw logs in
`reports/raw/`, with CSV exports in `reports/*.csv`.

This file says what exists, what was run, and what was *not* proved. It is deliberately blunt about the
last category: the headline finding is that the protocol stack is implemented and unit-tested, but an
end-to-end file transfer across the simulated acoustic link does **not** yet complete, and there is no
hardware or app artifact anywhere in this workspace.

## Vocabulary (do not skip)

| Word | Means here |
|---|---|
| **built** | source compiles into `libni_core.a` / the `ni` CLI with `-Wall -Wextra`, zero warnings suppressed by deleting the check |
| **unit-tested** | a test in `shared/acoustic_core/tests/` asserts it, run by `ctest`, currently 7/7 green, 3 557 assertions |
| **self-tested** | exercised by `ni selftest` / `ni phy test` / `ni linksim` on the modelled device chain: modem → room/impulse response → noise → AGC → quantiser → demodulator, all in software |
| **measured** | a number this host printed during a real run, saved under `reports/raw/` |
| **proved** | the acceptance criterion is met by evidence |
| **not proved** | code exists and may well work; nothing here demonstrates it |
| **NOT TESTED** | the tooling or hardware to test it is absent from this environment |

There is no word in this table that means "speaker to microphone". Nothing in this workspace has ever
been played out of a real loudspeaker or captured by a real microphone, so **no result here is an
over-the-air result**, and no number here is an internet speed.

## Milestones

### 1. Repository skeleton, shared core, build system — **proved**
`shared/acoustic_core/` (19 C++ sources, 18 headers) builds with CMake; `scripts/build_local.sh`
produces `build-linux/`; `scripts/syntax_check.sh` parses every source with the host compiler.
`ni audit` runs and reports the truth about this machine (`audio devices: 0`, `audio backend: not
compiled in`).
*Not proved:* the Android and Windows build paths (§ below), and `NI_BUILD_SHARED`/`NI_USE_LIBSODIUM`
configurations, which were configured but never exercised as test targets.

### 2. Versioned frame format, CRCs, reference vectors — **proved**
16-byte big-endian header, CRC-8 over the header, CRC-32 over the frame, `protocol/modulation_profiles.json`
plus 6 vector files; `t_crc_bits` (436 assertions) and `t_frame` (407) check them against independently
computed values, including a hex reference frame and rejection of a wrong `version`/`session_id`.
*Not proved:* nothing on this milestone — it is as done as it can be without hardware.

### 3. Acoustic modem: multi-carrier DPSK/QAM/OFDM + FSK — **partial**
16 profiles, N_FFT 256, band-limited PN sync pair, pilot-based channel estimation, coherent burst
descriptor carrying PDU length and modulation id. `ni render … | ni decode …` round-trips a payload
through a WAV file with matching SHA-256 (308 ms burst, measured SNR 64.9 dB, EVM 15.5 %).
`ni selftest --profile all --trials 8 --snr 25` gives per-profile frame success: 7/8 (FD-LOW-DBPSK),
5/8 (FD-HIGH-DBPSK), 4/8 (DBPSK-WIDE, DBPSK-MID-ECHO), 2/8 (DBPSK-MID), 1/8 (DQPSK-WIDE), 0/8 for the
FSK and QAM16/64 profiles (`reports/selftest_snr25_dfree10.csv`).
*Not proved:* a bit error rate in air; the FSK/FD-QAM16 zero-success cases are unexplained; the EVM
floor of 50–77 % at the receiver means the higher-order profiles are not usable yet.

### 4. Sync, channel estimation, FEC — **partial**
Rate-1/2 K=7 convolutional code with `d_free = 10` (asserted by the test's own state-diagram search),
soft LLRs, erasure marking for deep-fade bins, block interleaver. Measured coding gain on one channel:
raw 3.333 % → post-FEC 0.0734 %, and 271 corrupted bits become 0 information errors when marked
unknown (`reports/RESULTS.md` §2). The sync/estimator path is unit-tested (`t_fec` 311, `t_fft` 111).
*Not proved:* that a receiver in a room locks onto a transmitter. The FSK profiles' descriptor failure
(`synced but bit stream unusable`) is still unexplained.

### 5. Pairing, AEAD, replay protection — **proved in simulation**
HKDF-SHA256 session keys, ChaCha20-Poly1305 on DATA, truncated MAC on control frames, 4-word SAS,
attempt limits with backoff, replay window with a duplicate window. 2 223 + 57 assertions
(`t_crypto`, `t_pairing`) run against RFC 8439 / RFC 4231 / RFC 5869 A.1 vectors and Python's
`cryptography` package, plus deliberate-tamper and replay-rejection cases. Over the simulated link,
`ni linksim` reached `"pairing": "paired"` on both sides with `pairing_failures: 0`, so the handshake
survives the modem path, not just the unit test.
*Not proved:* that the SAS is compared between two humans on real devices; that pairing resists a
device in the same room with real acoustic eavesdropping.

### 6. Reliable link layer (ARQ, selective retransmit, duplexing) — **partial**
`t_arq` (52 assertions) proves fragmentation into bursts, burst parsing, cumulative + bitmap selective
acks, retransmit-until-give-up, best-effort frame release, AEAD-tamper refusal and unpaired-plaintext
refusal. The simulator shows acks, RTT and retransmits living on the wire (e.g. `acks_sent 39`,
`retransmits 37`, `lost_frames 5` in the DBPSK-WIDE run).
**The gap:** across 16 trials on 8 configurations (profiles 2/3/5/11, SNR 25 and 35 dB, paced and
`--unpaced`) every run ended `delivered 0` with `A got 0 B got 0`; `reports/linksim.csv` is all
`ok = 0`. The three open suspects, and why they are plausible, are recorded in `reports/RESULTS.md` §4.
So: an ARQ engine that is correct against its own tests, on a link that does not yet carry a message.
*Not proved:* payload transfer through the full `Node` + PCM path, in either direction.

### 7. Local HTTP/CONNECT proxy — **not proved**
`proxy.cpp` implements a loopback-only proxy (127.0.0.1, `Proxy-Authorization: Noise <token>`), request
size caps, per-stream reassembly, stream routing, and a link-test stream, and `ni link-test` exists to
drive it. None of that was ever carried across a working acoustic session, because milestone 6 does not
yet complete. Verdict: **built, not tested end-to-end**.

### 8. Echo cancellation and full duplex — **not proved**
`NlmsAec` (1024 taps, NLMS) plus two FDD profiles exist and `ni duplex` runs them against the modelled
channel; the FDD profiles are in fact the two best performers in `selftest` (7/8, 5/8) — but that
measures a frequency-split OFDM path with no echo path, so it says nothing about cancellation quality.
Half-duplex works in the sense that turns are taken; full duplex was never demonstrated where both ends
transmit at once into a shared microphone.

### 9. Calibration and adaptive modulation — **partial**
`ni calibrate --trials 3 --snr 25` probes 8 bands through the modelled chain, prints ok %, raw/post-FEC
BER, measured bps, clip, and recommends `DBPSK-MID` with a suggested MTU of 224
(`reports/raw/calibrate.txt`). Rate adaptation is wired (`ChannelEstimator`, `Adaptation`,
`LinkSimOptions::adapt`) but **off by default in the simulator**, because the receiver demodulates with
its own configured profile (`modem.cpp:426` discards the descriptor's `mod_id`), so a one-sided rate
change breaks interoperability. The 802.11-style common-preamble refactor that would fix it was not
attempted here.
*Not proved:* that a real calibration on a real phone+PC pair picks a usable profile, and that the
adaptation loop tracks a changing room.

### 10. Android and Windows apps, artifacts — **not delivered**
As of this commit the tree contains **no `android/` directory, no `windows/` directory, no `tests/`
directory and no `artifacts/` directory**. Only the portable C++ core, its unit tests, the protocol
spec files, the Python vector generator, `scripts/` and `reports/` exist. So the platform shells are not
"unverified source" — for this checkout they are *absent*, and the honest statement is that milestone 10
was not delivered here.

Two independent reasons, both true, and neither one is a tooling excuse that hides a missing file:

1. Nothing in this sandbox can build them: no JDK/Android SDK/NDK (so no APK, and nothing is
   "debug-signed" either — signing needs the tools), no MSVC or `zig` (so no EXE), and `cmake` only via
   the wheel in `/home/user/env`.
2. Even with a toolchain, the audio backends (`NI_WITH_WASAPI`, `NI_WITH_AAUDIO`) were never compiled
   here because the core is built with no audio backend at all - `ni audit` prints
   `audio backend: not compiled in` and 0 devices. The shared core's transport and PHY are exercised
   only through files and the simulator.

The interface the shells would sit on does exist and is tested: the flat C ABI in
`shared/acoustic_core/include/ni/abi.h` (`ni_abi_create/destroy/start/step/feed_pcm/pull_pcm/
send_bytes/render/decode/calibrate/test_link/version/profiles_json/audio_enumerate` + `ni_abi_free`),
which the Linux CLI uses itself, so a Kotlin or Win32 shell is a thin binding rather than a second
implementation. `docs/BUILD_ANDROID.md` / `docs/BUILD_WINDOWS.md` (if present) or `docs/ADR-001`
record what would have to be compiled in what order.

## Gates

Gate letters follow the acceptance list in the project brief; the criterion is restated in each row so a
verdict cannot be misread. Where `docs/ADR-001` used a letter for a slightly different cut of the same
question, this table is authoritative.

| Gate | Criterion being judged | Verdict | Evidence or blocker |
|---|---|---|---|
| A | Code builds; frame format, CRCs, vectors agree with an independent implementation | **PASS** | `build_local.sh` green; `ctest` 7/7; vectors from `python/gen_vectors.py` against Python `cryptography` + RFCs |
| B | PHY transmits arbitrary data over the modelled device chain and it decodes | **PARTIAL** | WAV round-trip sha256-equal; per-profile 7/8 … 0/8; FSK and QAM16/64 fail; no over-the-air data (no hardware) |
| C | Reliable session (FEC + ARQ + acks + pairing) carries a multi-kB message across the link | **FAIL** | pairing completes, acks/RTT/retransmits live, but `delivered 0` in 16/16 trials; `reports/linksim.csv` |
| D | Security: AEAD, replay protection, no plaintext on a required-crypto link, no public exposure | **PARTIAL** | unit-proven refusal/lockout/replay cases; loopback-only bind is source-verified, not penetration-tested |
| E | Usable internet: HTTP/CONNECT proxy tunnelled over the acoustic session | **FAIL** | never attempted end-to-end; blocked by gate C. No external host was contacted from this sandbox |
| F | Hardware in the loop: real speaker → air → real microphone on two devices | **NOT TESTED** | `ni audit` → 0 audio devices; no capture or render ever ran here |
| G | Platform artifacts: installable APK and Windows EXE, VPN tunnel on device | **FAIL** | no `android/` or `windows/` source in this tree, and no JDK/SDK/NDK/MSVC/zig to build one even if it existed |
| H | Documentation, reproducible benchmarks (CSV + JSON), automated tests separated from physical tests | **PARTIAL** | this file, `RESULTS.md`, `raw/` logs, `reports/*.csv`, `scripts/raw_to_csv.py`, 7 ctest targets; **`ctest` registration of the physical-test harness and a benchmark-runner script are missing** |

## What is explicitly *not* claimed

* No internet sharing. Two endpoints exchanging a message is not a tunnel, and this project has not
  completed a message either.
* No throughput figure. `ni profiles` prints single-burst payloads of 28 B/s to 1 714 B/s, and
  `ni phy test` measured 1 059 B/s for QAM16-WIDE on the model channel: those are link-layer rates under
  a simulated channel, not "internet speed", and the only session-level rate available is 0 B/s.
* No over-the-air performance of any kind, no latency figure for a real room, no claim that `aec` helps.
* No comparison claim against existing projects beyond the verified arithmetic in
  `docs/ADR-001` (audionet's README 3 200 bps is arithmetically 1 800 bps uncoded ≈ 225 B/s).
* No novelty claim about the PHY itself; OFDM-FSK/DPSK over audio is prior art. The work here is the
  frame/ARQ/security/calibration stack on top, which prior art does not have.

## Reproducing every number in this file

```sh
# build + unit tests (~1 min, needs cmake; /home/user/env has one)
PATH=/home/user/env/bin:$PATH bash scripts/build_local.sh          # cmake --build + ctest
cd build-linux && ctest --output-on-failure                        # expect: 100% tests passed, 7 tests out of 7

# physical-layer evidence (each takes tens of seconds)
./build-linux/ni selftest --profile all --trials 8 --snr 25 | tee reports/raw/selftest_snr25_dfree10.txt
./build-linux/ni calibrate --trials 3 --snr 25          | tee reports/raw/calibrate.txt
./build-linux/ni linksim --bytes 228 --trials 2 --max-ms 3000 --profile 3 --snr 35 \
    --no-multipath --json >> reports/raw/linksim.jsonl            # expect ok:false until gate C passes
python3 scripts/raw_to_csv.py                                      # regenerates reports/*.csv
```

## Known gaps and next actions, in the order that unblocks the most

1. **Gate C.** Get one 228-byte message across `ni linksim`. Work the three suspects in `RESULTS.md` §4:
   achievable demod SNR through the device model (measured −14 … −45 dB at nominal 35 dB), the
   turnaround/listen window, and `modem.cpp:426` ignoring the descriptor's modulation id.
2. Register the tests with the physical-test harness and add a benchmark runner
   (`scripts/run_benchmarks.sh`) so `RESULTS.md` can be regenerated in one command.
3. `protocol/modulation_profiles.json` has drifted from the C++ table: it is missing the `cp` field and
   ids 14/15, so the spec file and the firmware disagree. Generate it from the code or add a test that
   diffs them.
4. Only profiles 2/3/5/10/11 have been round-tripped through the full link; 11 of 16 have not.
5. No Windows/Android compile was attempted with a real toolchain; expect the platform files to need
   fixes before they build.
