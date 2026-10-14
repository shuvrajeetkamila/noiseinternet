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
| **unit-tested** | a test in `shared/acoustic_core/tests/` asserts it, run by `ctest`, currently 8/8 green, 3 590 assertions |
| **self-tested** | exercised by `ni selftest` / `ni bench` / `ni linksim` on the modelled device chain: modem → room/impulse response → noise → AGC → quantiser → demodulator, all in software |
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
**The gap was real and it was ours, not the radio's:** every earlier run ended `delivered 0` because
`ArqConfig::max_burst_bytes` let the ARQ fill a burst with more bytes than one FD-LOW burst can carry,
`Modem::tx_render` refused it, and `render_burst` returned without saying so (defect 18 in
`reports/RESULTS.md` §4.6, plus four smaller ones in the same path and a receive-side rescan that made the
simulation too slow to run long enough to see the fix work).

**Measured now** (rows `fix25-*` in `reports/linksim.csv`, from `reports/raw/linksim.jsonl`; simulated
channel, both directions at once, `ok` requires the far end to have reassembled the whole message with a
matching SHA-256; median of 3 trials per row, `--max-ms 900000`, so a run that cannot finish grinds for
15 simulated minutes instead of timing out early - compare rows only against rows measured the same way):
38 B each way on FD-LOW-DBPSK in 14.8 s (range 12.0-19.4, 3/3 ok); 228 B in 44.2 s (3/3 ok);
**2048 B on FD-HIGH-DBPSK in 30.34 s = 540.0 bps (67.5 B/s, 3/3 ok)** and on DBPSK-MID in
29.96 s = 546.9 bps (68.4 B/s, 3/3 ok). These are the post-defect-25 numbers: a use-after-free in the
out-of-order reassembly path (defects 24-25) was hiding behind exactly this table, and the two headline
figures did not move when it was fixed. Still failing: 2 kB on the narrow 82-byte-burst profile finished in
1 trial of 3 (377 s) and not in the other two (1 325 retransmits, 181 frames lost); 2 kB under multipath on
FD-HIGH finished in 1 trial of 3 (42.3 s); and pairing itself does not complete on FD-LOW with 5 % burst
loss or with multipath, where the giver sends 1 150 bursts and receives none (defect 26). `ni demo` (the
proxy leg) does not pair either (defect 21).
*Not proved:* anything over real speakers and microphones; a multi-kB transfer on every profile; a transfer
that survives multipath.

### 6a. Pairing preconditions — **enforced, not assumed**

Three things have to agree across the two devices before pairing can happen at all, and until this
session none of them was checked anywhere. A **passphrase**: the engine sent no pairing challenge when
it was empty and reported nothing, so the peer simply stayed offline for ever - the reported symptom -
and it is now refused at `Node::start()` with the reason in `pairing_note`, which both panels render. A
**profile and session id**: the phone's service fell back to profile 2 / session 0 while the PC opened
on profile 10 / session 7 and the library defaulted to 2 / 0 again - three answers to one question, and
any mismatch is permanent silence - so all of them now agree on FD-LOW-DBPSK (10) and session 7, and
both apps display what they are running. And **adaptive modulation**: it was enabled by default in every
node, and a profile change used to move the requester's modem immediately while the peer only adjusted
its bookkeeping, so one lost control frame left the two ends on different profiles with no way back -
deafness by design, triggered after six bursts with a poor success ratio, which is precisely the state
of a real link in its first seconds. It is now off by default, cannot fire before the link is up, and a
change is committed only after the peer acknowledges it (defect 33). What a *pairing attempt* does with
all of that in order is still a physical question: see gap 0.

### 7. Local HTTP/CONNECT proxy — **the HTTP leg is proved in the model; CONNECT is not**
`proxy.cpp` implements a loopback-only proxy (127.0.0.1, `Proxy-Authorization: Noise <token>`), request
size caps, per-stream reassembly, stream routing, and a link-test stream. The request leg now runs end to
end and is measured - request out, origin reached - and the response leg does not complete (defect 41: the
reply is pushed onto the link, the receiver reports the frames as malformed, the proxy answers 504). The
CONNECT path opens a tunnel (`connect_ok 1`) and a TLS handshake over it failed on its first attempt
(`curl` exit 56, HTTP 000). Verdict: **built; request leg measured; response leg and CONNECT open**. Fixing
even this much took five defects (36-40) found only after the giver's helper was given a trace - and one of
them had made every earlier claim about this row false.

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

### 10. Android and Windows apps, artifacts — **built by CI; never run on a device**
`android/` (Gradle project, Kotlin `NoiseCore`/`PhyTools` bindings, `AudioEngine`, a foreground
`NoiseService` that also owns a UDP/DNS `VpnService` tunnel, the operator UI) and `windows/` (CMake
project, WASAPI device enumeration + shared-mode render/capture, a console front end with the same field
list) now exist as written code. `tests/physical/` holds the over-the-air protocol and a runner that
refuses to emit a CSV row when no recorder exists; `artifacts/` is empty by design and says why.

Proved, in a narrow but real sense: the **interface** both shells sit on works. `t_abi` (33 assertions, in
`ctest`) drives the flat C ABI the way a shell does - create from JSON, refuse a config that is not an
object, `start`/`step`/`stop`, status and report documents, the refusal code for application data on an
unpaired node, FFI input validation, the 16-entry profile table the spinners render, and a stateless
`ni_abi_render` -> `ni_abi_decode` round trip of a 40-byte payload. That is the layer the JNI bridge and
the Win32 front end call, and it is green.

Not proved: that any of it installs, hears, plays, opens a tunnel or moves a byte over air. The compile
half of that sentence is no longer open - CI builds both platforms (see gate G and gap 6) and the reason it
matters is visible in the history: the WASAPI backend, written in a sandbox with no toolchain, called
`ni_wasapi_enumerate(out, ...)` with `out` as an **unnamed parameter**, so it could not have compiled at
all; the first MSVC job then produced 31 more errors of the same kind, and the first Android job 5 Kotlin
ones. What a compiler cannot test is what the apps do with a sound card, so the Windows front end gained an
honest middle step: `NoiseInternet.exe --ui-check` creates the real window, creates every control,
enumerates the machine's real endpoints and writes the inventory to `ui-check.txt`, which ships in the
bundle. The build agents report 0 audio endpoints, which is the correct answer for a machine with no sound
card and is printed as such rather than papered over. The shipped `NoiseInternet.exe` is now that windowed
front end (role, devices, profile, session, passphrase, match code, test buttons); the terminal front end
ships beside it as `NoiseInternet-console.exe`.

How to build each one, and what to expect on the first attempt:
[`../android/BUILD.md`](../android/BUILD.md), [`../windows/BUILD.md`](../windows/BUILD.md).

## Gates

Gate letters follow the acceptance list in the project brief; the criterion is restated in each row so a
verdict cannot be misread. Where `docs/ADR-001` used a letter for a slightly different cut of the same
question, this table is authoritative.

| Gate | Criterion being judged | Verdict | Evidence or blocker |
|---|---|---|---|
| A | Code builds; frame format, CRCs, vectors agree with an independent implementation | **PASS** | `build_local.sh` green; `ctest` 8/8 (3 590 assertions) on Linux, and CI also configures the same core with MSVC 19.51 - **8/8 there as well** - and compiles it for arm64-v8a/x86_64 with NDK r26 inside the APK build; vectors from `python/gen_vectors.py` against Python `cryptography` + RFCs |
| B | PHY transmits arbitrary data over the modelled device chain and it decodes | **PARTIAL** | WAV round-trip sha256-equal; per-profile 7/8 … 0/8; FSK and QAM16/64 fail; no over-the-air data (no hardware) |
| C | Reliable session (FEC + ARQ + acks + pairing) carries a multi-kB message across the link | **PARTIAL** | **2 048 bytes each way, both directions, SHA-256 verified, in 30.3 s = 540 bps (67.5 B/s) on FD-HIGH-DBPSK and 546.9 bps on DBPSK-MID, 3 trials of 3 in both cases** over the modelled room at 20-26 dB measured SNR (`reports/linksim.csv`, rows `fix25-*`; `RESULTS.md` §4.6 has the root cause of the earlier `delivered 0`, defects 24-25 the use-after-free that was hiding in this table's reassembly path). Still failing: 2 kB on the 82-byte-burst profile and under multipath complete in only 1 trial of 3, pairing does not complete on FD-LOW at 5 % burst loss (defect 26), ~10 % of frames go to a receiver-side distortion floor (defect 20), and a lost frame wedges the message silently (defect 22). Never attempted over real transducers. | **Loss model corrected (defect 42):** `--burst-loss` now drops whole bursts rather than 5 ms blocks, and the re-measured result is that 38 B on FD-LOW completes at 5% (12.02 s) and 10% (13.93 s) loss - the earlier `pairing did not complete` for the *same command* was an artefact of the model (defect 26, now closed). 30% loss pairs but stalls the payload in one direction. A modelled device queue (100/300 ms) and a 300 ppm clock offset do not stop pairing either; see `reports/RESULTS.md` §4.8.
| D | Security: AEAD, replay protection, no plaintext on a required-crypto link, no public exposure | **PARTIAL** | unit-proven refusal/lockout/replay cases; loopback-only bind is source-verified, not penetration-tested |
| E | Usable internet: HTTP/CONNECT proxy tunnelled over the acoustic session | **PARTIAL (thin)** | The **request** leg now runs end to end and is measured: `curl` -> receiver's proxy (token accepted) -> acoustic link in 14-byte fragments -> giver's helper parses the head, resolves the host, connects, and the origin logs the request; the helper's own log shows the reply pushed onto the link (`push n=185`, then the end-of-response marker). **The response never arrives**: the receiving side reports `rx_malformed` 23-98 with `rx_payload_bytes 0`, and the proxy answers 504 (defect 41, open). No byte of an origin response has ever crossed this link, and the claim this row made earlier - "a real http://api.github.com/zen response, 108 bytes, sha256 recorded" - was our own proxy's 407 error page, captured because the demo never sent the proxy token (defect 36, corrected in `reports/RESULTS.md` §4.7). CONNECT opens a tunnel (`connect_ok 1`) and then a TLS handshake over it fails (`curl` exit 56, HTTP 000). Only a plain-HTTP origin with no explicit port has ever been shown to start. Not PASS: "a browser on the receiver loads a page" is untested everywhere and no usable-throughput claim is made. |
| F | Hardware in the loop: real speaker → air → real microphone on two devices | **NOT TESTED** | `ni audit` → 0 audio devices; no capture or render ever ran here |
| G | Platform artifacts: installable APK and Windows EXE, VPN tunnel on device | **PARTIAL** | CI builds both from this branch and publishes four assets on the `ci-build` pre-release: <https://github.com/shuvrajeetkamila/noiseinternet/releases/tag/ci-build> - the debug APK (NDK-r26 core for arm64-v8a + x86_64 inside, keystore created and destroyed on the runner), a **standalone** `NoiseInternet.exe` that is now the **windowed** app (role radios, WASAPI-fed device lists, profile, session, passphrase, a read-only match-code field, Start/Stop/Switch role/Calibrate/Link test/Internet test, measured status panel; `/MT` confirmed from the exe's own import table rather than from the build option), `NoiseInternet-console.exe` for the terminal, a Windows bundle zip (`ni.exe`, `README-RUN.md`, `BUILD-INFO.txt`, `ui-check.txt`, unmodified CRT DLLs under `redist/`), and a combined android+windows zip. The packaging step **runs** the window (`--ui-check`, 60 s bound) and refuses to publish if it does not come back with 0, then ships the inventory it wrote: the current bundle records `step 3: ui-check PASSED`, **26 controls** with classes, sizes, texts and enabled states, 16 modulation profiles, and 0 audio endpoints (the truth about a build agent, and the reason no acoustic claim comes with it). `file` on the published exe says `PE32+ executable (GUI) x86-64`, and its import list is `GDI32.dll KERNEL32.dll ole32.dll SHELL32.dll USER32.dll WS2_32.dll` - a real window, still standalone. All four assets are rebuilt and republished on every green push, with sha256 generated on the runner, so the release body is the authority and `artifacts/README.md` explains the design. **Still not done: installing or running either on a device, the VpnService tunnel, and release signing.** Tests are 8/8 on both platforms - the Windows job is what caught a use-after-free that Linux masked (defects 24-25) |
| H | Documentation, reproducible benchmarks (CSV + JSON), automated tests separated from physical tests | **PARTIAL** | `protocol/modulation_profiles.json` now exists and is generated from the C++ table by `ni profiles --json`, so the spec file and the code agree (16 profiles); `reports/bench_sweep.csv`/`.json` and `reports/raw/selftest_snr25_streaming.txt` were re-run after the channel-model rework (defect 19) so the published PHY numbers match the code in this tree. Otherwise this file, `RESULTS.md`, `raw/` logs, `reports/bench_sweep.csv` + `.json` (315 rows from `ni bench --csv/--json`: medians over trials, a `failed_trials` count, and an `evidence_level` field stating in the file that this is a simulated channel), `scripts/raw_to_csv.py`, 8 ctest targets with 3 590 assertions all green, and `tests/physical/` (OTA protocol + runner, whose only exercised path here is its refusal to write a row without a recorder). Still missing: one-command benchmark regeneration and CI wiring. |

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
./build-linux/ni bench --profile 2,3,5,10,11 --trials 3 \
    --csv reports/bench_sweep.csv --json reports/bench_sweep.json   # the CSV + JSON benchmark
./build-linux/ni calibrate --trials 3 --snr 25          | tee reports/raw/calibrate.txt
./build-linux/ni linksim --bytes 228 --trials 2 --max-ms 3000 --profile 3 --snr 35 \
    --no-multipath --json >> reports/raw/linksim.jsonl            # expect ok:false until gate C passes
python3 scripts/raw_to_csv.py                                      # regenerates reports/*.csv
```

## Known gaps and next actions, in the order that unblocks the most

0. **Gate G follow-ups, updated by the first field report - and by what the follow-up measurement ruled
   out.** The three preconditions that make pairing *possible at all* are now enforced rather than
   assumed: a passphrase of 8+ characters, the same one on both ends (the engine refuses to start without
   it and says so in the status both panels render); and the same profile and session id, which all three
   configurations now default to (FD-LOW-DBPSK, session 7 - they used to disagree, defect 43). What the
   model says is *not* the explanation is also worth knowing before another attempt: lost bursts up to
   10%, a 100-300 ms device queue, and a 300 ppm clock offset all still pair (§4.8). So a failed attempt
   should be diagnosed from the numbers the panels now show, in this order: the input level in dBFS
   (nothing audible?), bursts arriving but not decoding (profile/session mismatch, or too loud/too quiet),
   and only then the room itself. Install the CI APK on a phone and run the CI
   EXE on a Windows box with a speaker and a microphone: that is still the only thing standing between the
   current artifacts and gates E/F, and the report from the first attempt ("it never paired, from both the
   exe and the apk") produced four fixes that all sit in the path a first run takes - defects 32-35. The
   second report (the phone's level sat at -120 dBFS and never moved) produced three more, and reading the
   Windows audio code afterwards a fourth. **Defect 45**: the profile drop-down was empty because the
   Android parser handed a JSON array to `JSONObject()`; it accepts both shapes now, and the Start button
   refuses to run with a guessed profile - the first report could not tell a guessed profile from a
   mismatched one, which is why that had to stop being possible. **Defect 46**: six status fields on both
   Android screens read JSON keys the core does not write (`profile_name` for `profile`,
   `useful_bps_up`/`down` for `useful_up_bps`/`down`, `retries` for `retransmits`), which is why the
   profile line read `-`; all now use the core's spelling, checked field by field against
   `StatusSnapshot::to_json()`. **Defect 47**: `WasapiEngine::capture` released a whole device packet
   after copying only the part that fitted in the caller's buffer, dropping the rest silently - it keeps a
   hold-over buffer now and drops nothing, which matters because the lost samples fall inside the burst
   the far end is decoding. **Defects 49-52**, from the user saying the numbers were too hard to read:
   with `assist` on (window + APK) a receiver beacons every 4 s even when it has heard nothing - the
   silence is what made a dead microphone and a dead app indistinguishable - each end sweeps its own
   output level down 0.85/0.5/0.3/1.0 until the link pairs (hardware speakers distort at full scale and
   nothing on the transmitting end can see it), the engine writes one plain sentence (the new `advice`
   field) that both front ends print above the numbers, the Android app asks the platform for its real
   sample rate instead of demanding 48 kHz and now shows `DEVICE RESAMPLED` when the device resamples
   (audible-but-undecodable), and the ABI's own JSON scanner stopped matching key names that appear as
   values. t_abi asserts the sweep moves: 48 checks, 0 failures; `ctest` 8/8. The phone's capture path was rewritten in the same pass: one recorder instead
   of two (`start()` opened one and `captureLoop()` opened a second, unreleased, both holding the
   microphone), float capture with automatic fallback to 16-bit PCM, a failed read retried instead of
   ending the session, and the panel's `audio:` line now names the source, the format, the failed-read
   count and whether capture is open at all. None of this has been near a phone or a sound card.
   things to check first on a re-run, in order: (a) does the passphrase field have the *same* 8+ characters
   on both ends (the apps now refuse to start otherwise, with the reason on screen); (b) the same profile
   number and session id on both - both apps now start on profile 10 / session id 7; (c) the one-line
   **why** on each status panel, which now distinguishes "the microphone hears nothing", "bursts arrive but
   do not decode" and "no passphrase" from each other. The old advice about device ids in
   `packaging/windows/README-RUN.md` still applies. `packaging/windows/README-RUN.md` is the procedure, and the first thing to try in the window
   is **Start** on both ends followed by comparing the four-word **match code**, which now appears on both
   screens (the snapshot carries the SAS; before this only the diagnostics JSON did, so no front end could
   display it). The cheap code task here is defect 27 - a proxy-token field in the Android config, without
   which nothing outside the app can authenticate to its proxy (the Windows console build already has
   `--proxy-token`, and the window still lacks it).
1. **Gate C, remaining half.** Pairing under loss (defect 26) is the cheapest hard thing left: the counters already say the giver's bursts never arrive, so start from the sim's loss injection, not from the handshake.
   Next, the 82-byte-profile 2 kB run and multipath, which now each succeed 1 trial in 3 - a retry budget that gives up (defect 22) is plausibly all that separates them. A 2 kB transfer already completes on two profiles; what is left is the
   ~10 % frame loss in the streaming model (defect 20): a per-symbol timing/phase tracker in the receiver
   (CP-based fine alignment + common-phase-error correction), because `measured_snr_db` saturates near
   26 dB whatever the acoustic SNR is. Then the multipath pairing failure, and per-message abort instead of
   a silent wedge (defect 22).
2. One command to regenerate the evidence: `ni bench` already writes CSV + JSON by itself, and
   `scripts/raw_to_csv.py` converts the self-test/linksim logs, but `ni selftest` / `ni calibrate` still
   need `tee`, and there is no wrapper that runs all of them and no CI job. Add `scripts/run_benchmarks.sh`.
3. ~~Fix the simulator's channel application before anything else on gate C~~ **done**: the model is now
   streaming (persistent filter/AGC/noise state, noise in front of the AGC, continuous SRO resampling) and
   `feed_pcm` no longer rescans its window per callback (`RESULTS.md` items 19 and 19b). The
   "guard interval per burst" idea is *not* what was done and the earlier measurement showed it breaks
   pairing; what mattered was removing the per-block re-initialisation.
4. `protocol/modulation_profiles.json` has drifted from the C++ table: it is missing the `cp` field and
   ids 14/15, so the spec file and the firmware disagree. Generate it from the code or add a test that
   diffs them.
5. Only profiles 2/3/5/10/11 have been round-tripped through the full link; 11 of 16 have not.
6. ~~No Windows/Android compile was attempted with a real toolchain~~ **done by CI**, which found and
   fixed 31 C++ errors and 5 Kotlin errors on the way (WASAPI API misuse, `M_PI`, `strtok_r`,
   `VpnService.Builder.addAllowedApplication`, `protect(fd)`, `PROPVAR`, `GetPeriodicity`, the `Initialize`
   session-GUID argument, `SPEAKER_*` masks, private `Impl`, JNI namespace qualification). What remains on the
   platform side is runtime behaviour, which needs devices, not compilers.
