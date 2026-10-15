# NOISE INTERNET — repair pass: root causes, changes, evidence, gates

This document answers the master repair prompt (Parts A–G plus the bracketed additions) for the pass
whose baseline is commit **`733a6b3`** — the exact commit named in that prompt, and the commit the
supplied APK/EXE were built from.

* **Baseline (inspected, unmodified):** `733a6b3` — state preserved as that commit id, which is
  reachable, hash-verifiable and immutable. *Note on the "diagnostic branch":* this session is bound to
  one branch (`arena/15b1e419-noiseinternet`) and cannot create or push another, so the baseline is
  preserved as a commit rather than as a second branch. That is a stronger guarantee than a branch:
  a commit cannot be moved.
* **Repairs:** `f7a475d`, `5608dd1`, `8d9c709`, `1b83212` on the same branch.
* **Nothing was reformatted, renamed or rewritten** in the inspected code beyond the lines each defect
  required, and every change below names the defect it fixes.
* Everything labelled *measured* below was produced by a command whose output is quoted; everything
  else is labelled otherwise.

---

## Part A — root-cause report (before any edit)

### A.1 Confirmed defects, each with the evidence that confirmed it

| # | defect | status | how it was confirmed |
|---|---|---|---|
| 27 | Neither front end could set the receiver proxy's bearer token; the console could (`--proxy-token`) | **fixed** | source read: `gui_main.cpp` only read `node->proxy_token()` to display it; the Android layout had no token field at all |
| 59 | A device counted its **own** transmission, decoded back through its own microphone, as frames from the peer — so a lone machine reported `peer online` and advanced to `FRAME_OK` | **fixed** | reproduced in `tests/t_self_echo.cpp`: 20 of 20 loopback bursts accepted as peer frames before the fix, `peer_online true`, state `FRAME_OK` |
| 60 | An **empty** proxy token authenticated every client on the machine | **fixed** | `t_proxy_auth`: with `Options::token` empty, a client that does not know the generated token got a `200` before the fix |
| 61 | The re-keyed client entry was a **copy of freed memory** (`fd = -1`), so an authenticated response was flushed nowhere and the browser hung | **fixed** | instrumented probe printed `rekey: fd=6 ... keys now 1(fd=-1,phase=0)`; `t_proxy_auth` fails on the old code |
| 62 | The request was handed to the link **before** its stream id was registered, so a fast giver's answer was dropped as "no client for stream" | **fixed** | `t_proxy_auth` produces its answer synchronously inside the sender callback — the exact case |
| 63 | A response **body that arrived with its head** was left in the read buffer and never reached the client | **fixed** | `t_proxy_auth`: client received a bare `HTTP/1.1 200 OK` and waited forever |
| 64 | The client's own **request was returned to it as the response** (bearer token included), because the request head was never consumed from `rxbuf` | **fixed** | `t_proxy_auth`: client received its own 101-byte `GET`; this hid defect 63 |
| 65 | A request whose **body arrived in a second read** was answered `400 Bad Request` | **fixed** | code path: phase 1 re-parsed body bytes as a request head |
| 66 | The link test's forward hash check compared **raw hash bytes with a hex string**, so `hash_match` could never be true | **fixed** | `ni linktest` reported `hash_match false` with `reverse_hash_match true` — impossible unless the comparison was broken |
| — | The "session id (same on both ends)" label **overlapped** the passphrase label (80×16 px at 374,108) | **fixed** | the new `--ui-check` layout audit on the Windows build agent: `layout: OVERLAP between Static "session id..." and Static "passphrase..."` and exit code 4, which made packaging **refuse** to publish that build |
| — | The Windows passphrase field was **not masked** (it printed the shared secret in clear text) | **fixed** | found by tightening `scripts/check_frontend_parity.py` after its first version was satisfied by the *proxy token* field further down the file |
| — | The Android session-id field **defaulted to 0** while the engine, the Windows window and the documented default are all **7** | **fixed** | source read: `android:inputType="number" android:text="0"` |
| — | `--ui-check` could only count controls; a control drawn on top of another one still counted as created | **fixed** | it was what let the overlap above exist in the first place |

The two defects the previous reports listed as *this is the blocker* are now measured to be different
things than they claimed:

* **Defect 41** (stream-id translation) was necessary and correct, but **it was not what blocked the
  response leg**. The leg was blocked by 61/63/64. With those fixed `ni demo` reads **IDENTICAL**
  (Part C below). The claim "the response leg does not complete (defect 41)" is retired everywhere it
  was printed, including the command's own banner.
* **Defect 21** stays fixed as recorded; nothing in this pass touched that path.

### A.2 Suspected, not confirmed

* **Profile 10 (FD-LOW-DBPSK) at 26 dB SNR in the modelled room does not complete a two-way test** within
  60 s of device time, while profiles 2 and 11 at 35 dB complete it in ~2 s. Measured counters for that
  failing run: giver `loss_pct 85.7`, `retransmits 39`, receiver `crc_fail 23`. That is the same family as
  the already-recorded defects 20/22 (streaming loss and retry budget); *the fix is not in this pass*, and
  the root cause is **suspected, not confirmed**.
* **Receiver→giver throughput after pairing is far lower than the reverse direction** in every two-node
  run here (`frames_sent 1` for the receiver in a 36 s session while it received 66). Whether that is
  the ARQ's turn-taking, the master/slave pacing, or the channel model's asymmetry is **not established**.

### A.3 Untested hardware limits (never claim these either way)

* Every acoustic number in this repository, including everything in this pass, is a **modelled room**
  (16-bit PCM through `AcousticChannel`). **No message has ever crossed a real speaker and a real
  microphone** in this project's history; this pass cannot change that, and nothing below is claimed as
  if it had.
* Android audio HAL behaviour on the user's specific phone (its echo canceller, its AGC, whether it
  resamples), Windows "audio enhancements", device latency, and real-room multipath are all
  **NOT TESTED**.
* The APK's `VpnService` tunnel carries **UDP and DNS only, by design** (documented in `BUILD.md` and in
  the layout's own label): an acoustic link at a few hundred bits per second cannot carry a TCP handshake.
  TCP traffic takes the HTTP/CONNECT proxy path instead. This is a design boundary, not a defect, and it
  is unchanged by this pass.

---

## Part B — the acoustic path

**Audited:** WASAPI capture/playback (`windows/…/wasapi_audio.cpp`, shared-line rules, no resampling),
Android `AudioRecord`/`AudioTrack` (`AudioEngine.kt`: capture-format fallback ladder, buffer sizing,
underrun counting), the full signal path (mic → capture callback → squelch → burst detector → demodulator
→ frame decode → ARQ → crypto gate → delivery), and the pairing state machine.

**Fixed in this pass**

1. **Defect 59 (own echo counted as a peer).** `AcousticLink` now keeps a ring of the last 32 burst PDUs it
   transmitted — hashed both as sent and as the receiver sees them, because the decoder strips the 2-byte
   burst-length prefix — and a decoded burst matching one of them is counted in a new `self_echo` counter
   and dropped **before** the ARQ or `peer_seen_ms_` can see it. A lone device can therefore no longer
   report a peer online, and the advice line now states the measured fact: *"the microphone decoded N
   burst(s) that were this device's OWN transmission coming back (self-echo), and no burst from any other
   device."*
2. **Defect 66 and the two-way link test.** The link test now sends a **unique random payload with its
   SHA-256** upward and the far end answers with **its own fresh 16-byte nonce plus that nonce's SHA-256**,
   which the initiator recomputes. The verdict is therefore about bytes that crossed in *both* directions.
   `ni linktest` runs two real nodes through the modelled room and drives the pump while the test runs
   (the blocking form pumped one node and could never be answered in a two-node harness).

**Measured here**

```
$ ni linktest --profile 2 --snr 35 --bytes 30 --budget-ms 60000
  [t= 3590 ms] link test report: {"payload_bytes": 30, "answered": true, "hash_match": true,
    "reverse_payload_bytes": 16, "reverse_hash_match": true,
    "reverse_sha256": "198e0355c9aa04cb054cea28e388e6672855ead6d348c25cc4c0aaa3166ce943",
    "directions_verified": 2, "elapsed_ms": 2080, ...}
VERDICT: PASS - a unique payload crossed in EACH direction and each was verified against its own SHA-256
         (modelled room, in software).
```

`directions_verified: 2` on profiles 2 and 11 at 35 dB SNR; two consecutive runs hash different payloads
(`fc14bfc9208c…`, then `d05e4fe98225…`), so each run is a test and not a replay. This is a **regression
test** now (`ctest` name `t_linktest`) and it is printed as *modelled room, not speaker→microphone* in its
own output, so it can never be mistaken for the physical test.

**Still open:** the A.2 items.

---

## Part C — the internet path

**Architecture per role (unchanged, documented here as audited):**

* **Giver** — holds the internet. Runs `GiverGateway`, which fetches on behalf of requests that arrive over
  the acoustic link, and keeps `CONNECT` as an unparsed byte relay so **end-to-end TLS is preserved** and
  certificate validation stays with the application. The giver never listens on a public interface.
* **Receiver** — runs `ReceiverGateway`, listening on **127.0.0.1 only** and requiring
  `Proxy-Authorization: Noise <token>` on every request. A browser pointed at it uses the acoustic link as
  a very slow transport. The optional `VpnService` tunnel carries UDP/DNS only (see A.3).
* TCP is deliberately not carried by the tunnel: at acoustic rates, opening a TCP connection is not
  reachable, which is why the proxy path exists.

**Fixed in this pass:** defects 60–65 (Part A) plus defect 27 (the token field in both front ends). The
response leg now completes **in software**:

```
$ ni demo --profile 11 --snr 35 --budget-ms 120000
  [t= 2370 ms] curl direct #1   exit=0, 400 body bytes  [ni-http 200 text/plain 400
  [t= 3730 ms] curl direct #2   exit=0, 400 body bytes  [ni-http 200 text/plain 400
  [t=22650 ms] curl via proxy   exit=0, 400 body bytes  [ni-http 200 text/plain 400
  through-proxy bytes : 400   sha256 2ed3e2696745228acc3948d446c6400999861040520576d7bdeeb06d70c85a97
  direct #1     bytes : 400   sha256 2ed3e2696745228acc3948d446c6400999861040520576d7bdeeb06d70c85a97
  direct #2     bytes : 400   sha256 2ed3e2696745228acc3948d446c6400999861040520576d7bdeeb06d70c85a97
  comparison          : IDENTICAL - the 400 bytes that arrived through the acoustic link
  receiver report     : proxy_receiver {"requests_ok": 2, "bytes_relayed": 351},
                        proxy_giver {"fetches_ok": 1}
```

**The staged internet test, and what may never count as a pass.** The rule the prompt sets — *never pass
merely because the giver can reach the internet* — is honoured in the code: `ni demo` fetches the same URL
twice directly and once through the acoustic link, compares the **client's bytes** and their SHA-256, and
refuses to read `IDENTICAL` when the origin is dynamic (`origin: stable - two direct fetches…`). "The giver
has internet" is a *precondition*, printed separately (`internet_ok`), never the verdict. Staged:
A local proxy auth → B request leg → C response leg → D byte-identical payload → E TLS via CONNECT →
F real origin → G real devices → H usable browsing speed. Status: **A and B measured in software; C and D
measured in software only (modelled room, local origin); E/F/G/H NOT TESTED** — no hardware, and the only
origin available here is on this host.

---

## Part D — the visible UI

**Method.** The window's own `--ui-check` now does what a check that can see a layout should do: it
enumerates every child control, converts each rect into client coordinates, measures combo boxes at their
*closed* height, and fails if any control is outside the client area or any two overlap. It exits 4 on
failure, and **the packaging step refuses to publish such a build** — which is exactly what happened on
the first Windows run of this pass, on a real overlap it found and I had not noticed.

**Fixed:** the overlapping label (Part A table), the unmasked passphrase, the Android session default,
the proxy-token field in both apps, **copy buttons in every window that shows written results** (Windows:
"Copy status" and "Copy log"; Android: "Copy report"), the "show secrets" box (reveals the passphrase field
and prints the proxy token deliberately, both masked by default), and the record button's own overlap with
the status label from the previous pass.

**Automated guard:** `scripts/check_frontend_parity.py` (also run by CI) asserts that both front ends offer
the same controls — profile, session id, passphrase, role, proxy token, start, stop, record-and-analyse,
link test, internet test, copy button, match code — that both secrets are created masked, and that the
proxy token is not written into the log pane. It prints a control-by-control table and fails with the list
of what is missing. Current output: `PASS: both front ends offer the same controls, and both have a copy
button`.

**Verification bound:** the Windows layout was verified by the geometry audit **on the build agent**
(`layout audit: 32 controls, 0 outside the client area, 0 overlapping pairs`, exit 0) and cross-checked
here by parsing the same geometry; the Android layout is verified by the parity check and by compilation.
**No human has looked at either window** (this environment has no display and no audio devices), so
"the UI looks right" is NOT TESTED — only "every control is created, sized, masked and wired".

---

## Part E — regression tests, builds, artifacts

```
$ ctest --test-dir build-linux --output-on-failure
100% tests passed out of 11
t_abi 55 checks   t_arq 52   t_crc_bits 436   t_crypto 2223   t_fec 311   t_fft 111
t_frame 407       t_pairing 57   t_proxy_auth 23   t_self_echo 13   t_linktest (two-way, PASS)
```

New coverage added by this pass: **proxy authorization and the response path over a real loopback socket**
(`t_proxy_auth`), **self-echo detection on a real rendered burst** (`t_self_echo`), **the two-way link
test** (`t_linktest`), and the **front-end parity check** in CI.

Reproducible from a clean checkout with:

```sh
pip3 install --break-system-packages cmake
cmake -S shared/acoustic_core -B build-linux -DCMAKE_BUILD_TYPE=Release -DNI_BUILD_TESTS=ON
cmake --build build-linux -j 4
ctest --test-dir build-linux --output-on-failure
python3 scripts/check_frontend_parity.py
./build-linux/ni linktest --profile 2 --snr 35 --bytes 30
./build-linux/ni demo --profile 11 --snr 35 --budget-ms 120000
```

**Built from source by CI from the repaired commit, and published** (Windows and Android jobs green,
`ci-build` release):

| artifact | bytes | sha256 |
|---|---|---|
| `noiseinternet-debug.apk` | see release | see release |
| `NoiseInternet.exe` | see release | see release |
| `NoiseInternet-windows-x64.zip` | see release | see release |

The exact bytes, digests and commit for the published build are in the release body
(`gh release view ci-build`), which is generated by the workflow from the commit it built — this document
is not the authority for them, and the numbers are deliberately not copied here so that a later
republish cannot leave a stale digest in a report.

---

## Part F — physical testing

**NOT TESTED. Not one acoustic claim in this pass was made on real hardware.** There is no microphone, no
speaker, no phone and no Windows machine attached to this environment; the sandbox reports 0 audio
endpoints, and the Android app can only be verified by compilation and by code-level tests here.

The procedure the user can run, in the order that isolates failures fastest:

1. **Both apps, same profile number, same session id, same passphrase** (both windows show the match code).
2. **Record 8 s and analyse** on each device separately (Windows button / phone "Test this device (8 s)"):
   the verdict sentence names the failure — silence, far too quiet, clipped, shifted by N ppm, heard but
   not readable, *or* "this recording decodes".
3. **A device that hears itself is normal** (half duplex): the panel now says `self-echo N` and no longer
   calls it a peer.
4. **Then** place the devices 20–30 cm apart, speaker facing microphone, and press Start on both. The
   match code must read identically on both screens.
5. **Link test** — PASS now requires the payload to be verified in *both* directions.
6. Only then the internet leg: receiver → browser pointed at `127.0.0.1:8471` with the token shown in the
   status panel; giver → "Internet test".
7. Report back the panel text verbatim; the verdict sentence is written to name the failing stage.

---

## Part G — the 13 deliverables

| # | deliverable | status | evidence / what is missing |
|---|---|---|---|
| 1 | Cross-platform source (one engine, two front ends) | **PASS** | `shared/acoustic_core` + `windows/` + `android/`; CI builds all three from source |
| 2 | Real acoustic modem (FSK/DPSK/OFDM, 16 profiles, adaptive) | **PASS (software)** | `ni profiles`, `ni selftest --profile all`; **nothing has been through real air** |
| 3 | Versioned frames, CRC, FEC, ACK/selective retransmit, ARQ | **PASS** | `t_frame`, `t_crc_bits`, `t_fec`, `t_arq` (52 checks incl. replay rejection) |
| 4 | Echo cancellation, half-duplex first | **PARTIAL** | half duplex works; full duplex (FDD + NLMS AEC) is implemented but **never tested on hardware** |
| 5 | Link calibration and diagnostics | **PASS** | `ni calibrate` (honestly labelled a model sweep), `ni analyze`, capture verdicts, `self-echo`, `--ui-check` |
| 6 | Network layer: local HTTP/CONNECT proxy first | **PASS (software)** | `ni demo` reads IDENTICAL over the modelled room; `t_proxy_auth` 23 checks |
| 7 | Android `VpnService` tunnel | **PARTIAL** | builds and is wired; UDP/DNS only by design; **never run on a phone here** |
| 8 | Pairing/security: challenge-response, AEAD, replay protection, no public exposure | **PASS** | `t_crypto` 2223 checks, `t_pairing` 57, token required on every proxy request, 127.0.0.1-only bind |
| 9 | Full UI on both platforms, with the same controls and copy buttons | **PASS (code-level)** | `--ui-check` audit green on the Windows agent; parity check green; **no human has seen either window** |
| 10 | Automated tests separated from physical ones | **PASS** | 11 ctest suites + parity check, all hardware-free and labelled as such |
| 11 | Benchmarks with CSV/JSON, medians and variability | **PASS** | `reports/bench_sweep.csv|json` (315 rows), `linksim.csv`, `selftest_*.csv`, each labelling its own evidence level |
| 12 | Build scripts, APK and Windows EXE | **PASS** | CI publishes both from the repaired commit (see Part E) |
| 13 | Gates A–H with honest labels | **PARTIAL** | A PASS, B PARTIAL, C PARTIAL, D PASS, E PARTIAL, F **NOT TESTED**, G PARTIAL, H PARTIAL — unchanged, and Gate F cannot move without hardware |

**One-line summary of the pass:** the acoustic engine now proves a verified payload in both directions and
no longer mistakes its own voice for a peer (defects 59, 66); the receiver proxy's response leg — broken by
five separate defects that all looked like one symptom — now completes byte-identically in software
(defects 60–65); both front ends can set and copy everything the task asked for, and the build refuses to
publish a window whose controls overlap (defects 27 and the UI items). What has *not* changed: no message
has ever crossed real air, and Gate F stays NOT TESTED until the user runs the Part F procedure on real
hardware.

---

# Round 5 — the APK copy button, and the ten defects the 17:11 field log exposed

**Asked for:** "rebuild the APK and the EXE [COPY BUTTON ON APK DOES NOT WORK, SO MAKE IT FUNCTIONABLE TO
COPY THE REPORT]". The copy button is the headline; the field log that came with it (Windows giver +
Android phone giver, session 12, profile 10) was read line by line and produced ten more defects, all fixed
in the same build.

## 1. Root cause of the reported defect (71)

`MainActivity.kt` bound `copy_report` to **`results`** — the lower pane, which is empty at rest — while the
report the user reads is assembled by `renderStatus()` into **`state`** (and `banner`) every 500 ms.
So the button copied an empty string and answered "nothing to copy yet", and the "[copied N characters]"
line it wrote into `results` was overwritten by the next refresh before it could be read.

Fixed: `copyReport()` re-renders, copies banner + report + results in reading order, keeps the
acknowledgement re-drawn through the refresh for 15 s, toasts, and reports a clipboard refusal instead of
claiming success. `scripts/check_frontend_parity.py` now fails the build if the handler stops reading the
report pane or stops reaching the clipboard.

## 2. The other ten (72–81)

See the table at the end of `reports/RESULTS.md`. In one line each: the 8-second recorder kept 2.25 s and
starved the link (72), a failed WAV write said nothing useful (73), Android printed a Windows-only sentence
(74), the two ends contradicted each other about the tunnel (75), `-120 dBFS`/`100 % loss` placeholders were
printed as readings (76), the window claimed "8/8 unit tests" forever (77), "self-echo 128" was one burst
(78), the audio line never named the devices (79), "475 underruns"/"12 failed reads" had no denominator (80),
and a **giver** was refused over a proxy token it never uses (81).

## 3. Files changed

`git diff --stat 5d7f31c..HEAD` — 14 files, +564/−108:

```
 .github/workflows/build.yml                        |  10 +-
 android/app/src/main/java/com/noiseinternet/app/MainActivity.kt      | 126 ++++++++---
 android/app/src/main/java/com/noiseinternet/core/AudioEngine.kt      |  43 +++-
 android/app/src/main/java/com/noiseinternet/core/NoiseCore.kt        |  23 ++-
 reports/RESULTS.md                                 |  43 +++-
 scripts/check_frontend_parity.py                   |  34 +++
 shared/acoustic_core/include/ni/node.h             |  30 +++
 shared/acoustic_core/include/ni/session.h          |  15 ++
 shared/acoustic_core/src/node.cpp                  |  72 +++++++-
 shared/acoustic_core/src/session.cpp               |  26 ++-
 shared/acoustic_core/src/wav.cpp                   |  10 +-
 shared/acoustic_core/tests/t_self_echo.cpp         |  44 ++--
 windows/src/NoiseInternet/audio_wasapi.cpp         |  19 ++-
 windows/src/NoiseInternet/gui_main.cpp             | 177 +++++++++++++-----
```

The two new core capabilities that made the fixes possible: `Node::rx_tap_begin()/rx_tap_end()` (record what
the engine received, without a second reader on the microphone) and `phy_measured`/`arq_measured` in the
status JSON (so a UI can print "not measured yet" instead of a default value).

## 4. Tests run, and what they returned

| what | result |
|---|---|
| `ctest` on Linux (11 tests, includes the two-way acoustic link test) | **11/11 pass**, 1.1 s |
| `t_self_echo` (own burst looped back, plus the same burst decoded twice) | 15 checks pass |
| two-way link test, `ni linktest --profile 2/11 --snr 35 --bytes 30` | `directions_verified: 2` on both profiles, fresh random payload each run (sha256 printed on both ends); profile 10 @ 26 dB still 0 — the marginal-channel case |
| `scripts/check_frontend_parity.py` | **PASS**, 16 checks (5 of them new: copy handler, log copy, session-id default, measured flags) |
| `scripts/syntax_check.sh` | 21/21 core sources compile |
| MSVC job (CI) | window builds, **10/10 tests pass**, `--ui-check` exits 0, **32 controls** enumerated |
| Android job (CI) | `assembleDebug` succeeds; APK contains `lib/arm64-v8a/libnoiseinternet_jni.so` |
| `ni demo` | payload digests unchanged (`2ed3e269…`); new keys `phy_measured`, `arq_measured`, `self_echo_repeats` present |

## 5. Deliverables, labelled

| deliverable | label | why |
|---|---|---|
| `noiseinternet-debug.apk` (this build) | **PARTIAL** | compiles, contains the copy fix; debug-signed; **not installed on a phone by this host** |
| `NoiseInternet.exe` + zip (this build) | **PARTIAL** | compiles on MSVC, 10/10 tests, `--ui-check` OK; unsigned; never run on Windows by this host |
| copy button, Android | **PARTIAL** | defect found and fixed; guarded by the parity check; needs one field report to be called PASS |
| copy buttons, Windows (status/log) | **PARTIAL** | present and enumerated by `--ui-check`; not clicked by a human here |
| record 8 s and analyse | **PARTIAL** | now records through the engine (no second reader); needs one run on a machine with a microphone |
| two-way acoustic link test | **PASS (software)** | `directions_verified: 2`, both directions SHA-256 verified, now a ctest |
| two-way link over **air** | **NOT TESTED** | no sound card, no microphone, no room |
| proxy leg (UDP+DNS) | **PARTIAL** | socket-level tests pass; audio-tunnel arithmetic not exercised |
| VPN (`VpnService`) leg | **NOT TESTED** | needs a phone |
| internet verification | **NOT TESTED** | needs a working acoustic link first |

Gates A–H keep the labels they had at `5d7f31c` (A PASS, B PARTIAL, C PARTIAL, D PASS, E PARTIAL,
F NOT TESTED, G PARTIAL, H PARTIAL): this round fixed defects *on top of* those gates and did not move the
one that matters — no sound has crossed a room. Defect 75 does move a claim *down*: the engine no longer
says "tunnel up" for a proxy nobody is using.

## 6. Next unresolved blocker

The same one, one measurement away from an answer: **the Windows machine's own speaker-to-microphone path**
has never produced a decodable burst (mic hears −43.9 dBFS "loudest" against a −50.6 dBFS room, 43–53 % band
errors — a signal that is not the modem's). The next single action is on the PC, with this build:

1. Start → **GIVER**, profile `10 FD-LOW-DBPSK`, session `7`, passphrase typed on both ends.
2. Press **Record 8 s and analyse** and send back the two lines it now prints: `kept N of 384000 samples …`
   and `saved as <full path>` (or `NOT SAVED - … <the platform's own error>`).
3. `ui-check.txt` from the zip, which lists the audio endpoints the exe opened.

That answer decides the next move: if the recording now holds a burst and reports `DECODES this recording`,
the PC path works and the problem is the room/gain; if it says the microphone delivered nothing, the capture
endpoint is the blocker and the audio line will name it.

---

# Round 6 — "rebuild everything from scratch, make sure both sides match and can carry internet"

## What was asked, and what each part becomes

| the request | what this round did |
|---|---|
| rebuild both artifacts from scratch | every job was re-run from a clean checkout on the final commit; all four CI jobs pass, and the release is republished from that commit with per-file sha256 |
| make sure both sides match | both panels now print this end's **profile, session id and sample rate together** with "must be identical on both ends"; the engine's advice sentence repeats them; and the Android panel prints the session id it is actually running (it was on neither screen before) |
| ...can actually send/receive internet | not claimed, and it cannot be claimed from here: no message has crossed real air. What *was* done is removing the two reasons a working build looks broken — a fresh signing key per build (so the new APK would not install) and no way to tell which build was running — plus the volume/route plumbing that decides whether one end can physically hear the other |
| make everything new | the engine, both front ends and the CI were all touched here (see the file list), and the artifacts are built from the round-6 commit, not carried over |

## The two things that most likely made the last build look broken

1. **The APK could not be installed over the previous one.** Every CI run up to round 5 signed with a
   freshly generated debug key on the runner; Android refuses an update signed by a different key. Tapping
   the new APK gave an install error, not a new app.
   *Now:* a committed debug key (`android/ci-debug.p12`, documented, public on purpose, release builds
   deliberately unsigned) and `versionCode` = the CI run number, so updates install over each other.
2. **Nothing said which build was running**, so "the fix did not work" could not be answered.
   *Now:* `NI_BUILD_STAMP` is compiled into the engine and printed by everything: the Windows title bar,
   the first status line, `ni audit`, and the Android report (`build: engine r6-50  app 0.6-ci.50`). CI
   refuses to publish unless the stamp is greppable inside the shipped executables, the APK's resources
   and both native libraries.

## The plumbing that decides whether one end can hear the other

- **Volume:** Windows reads each endpoint's volume and raises a speaker below 50 % to 85 % for the session,
  restoring it on Stop; Android does the same for media volume. Both report it. The round-5 PC recording
  had its loudest event 7 dB above the room — indistinguishable from a nearly silent speaker, and nothing
  in the app said what the volume was.
- **Route:** both apps warn in words when the audio is going to a headset/Bluetooth/USB/HDMI device.
- **Devices:** the Windows audio line names both endpoints by their Windows friendly names and sizes.
- **Recording:** the 8-second recorder captures what the engine received (not a second reader on the same
  microphone), keeps the whole window, and says exactly where it saved the file.

## Files changed

`git diff --stat 4c13570..HEAD` — plus `android/ci-debug.p12` and `android/app/NOTICE-ci-debug-key.md`
(new). Core, both front ends, both build files and the CI workflow.

## Tests run, and their results

| what | result |
|---|---|
| `ctest` on Linux | 11/11 pass, with the build stamp compiled in |
| two-way link test (`--profile 11 --snr 35 --bytes 30`) | `directions_verified: 2` |
| `scripts/check_frontend_parity.py` | PASS, 20 checks |
| CI: core-linux / android / windows / release | all four **success**; MSVC 10/10 tests; `--ui-check` PASSED (32 controls, layout audit clean) |
| stamp inside the artifacts | verified by CI: `r6-50` found in `NoiseInternet.exe`, `ni.exe`, the APK's resources and both `libnoiseinternet_jni.so` |

## Deliverables, labelled

| deliverable | label | evidence |
|---|---|---|
| `noiseinternet-debug.apk` (r6-50) | **PARTIAL** | compiled, stamped, stamp verified inside the APK; debug-signed with the committed key; **not installed on a phone by this host** |
| `NoiseInternet.exe` + zip (r6-50) | **PARTIAL** | compiled on MSVC, 10/10 tests, `--ui-check` PASSED; unsigned; **not run on Windows by this host** |
| both sides matching | **PARTIAL** | the numbers are now on both panels and the engine repeats them; only a two-machine run proves a human can compare them |
| volume/route self-tuning | **PARTIAL** | implemented and unit-checked to the extent the build host can; needs one real machine |
| copy button (Android) | **PARTIAL** | defect found and fixed twice (wrong pane; then the pre-start path); guarded by the parity check; needs one field report |
| two-way acoustic link, modelled channel | **PASS (software)** | `directions_verified: 2`, payload SHA-256 compared both ways, now a ctest |
| two-way link over air | **NOT TESTED** | no audio hardware anywhere in this environment |
| send/receive internet end to end | **NOT TESTED** | depends on the leg above; no claim is made |

## The one next action (PC, five minutes)

Start the new EXE as **GIVER** (profile 10, session 7, the same passphrase as the phone), press **Record
8 s and analyse** and send back four lines:

1. the title bar's `[r6-50]` stamp (so we both know it is this build),
2. `kept N of 384000 samples …`,
3. `saved as <path>` (or `NOT SAVED - <reason>`),
4. the `audio:` line from the status panel — it now names the microphone Windows actually opened and the
   speaker volume it is using.

That single reply decides the next move: it separates "the microphone hears nothing" from "the speaker is
whispering" from "the room is fine and the modem needs work".
