# Running Noise Internet — Windows exe and Android apk

Read the first three sections before you plug anything in. They are short and they are the difference
between a useful evening and a confusing one.

## What is in this zip

| file | what it is |
|---|---|
| `NoiseInternet.exe` | the Windows app: **a window with controls**, not a terminal. Same engine as everything else here, real WASAPI audio. Pick a role, pick your microphone and speaker, type a session id and a passphrase, press **Start**. Buttons run the measured tests; the status panel shows what the engine actually measured. **Standalone** — the C++ runtime is linked in (`/MT`), so you do not need to install the Visual C++ redistributable. |
| `NoiseInternet-console.exe` | the same app driven from a terminal, for the things a window is awkward for: `--list-devices` prints every endpoint id, `--wav-in/--wav-out` runs the codec through files, single keys drive the tests. See "The same thing from a terminal" below. |
| `ni.exe` | the engine as a command-line tool: `ni profiles`, `ni selftest`, `ni linksim`, `ni audit`, `ni demo`. `ni audit` on a machine with no sound card honestly reports 0 devices. |
| `ui-check.txt` | what the window looked like on the machine that built this zip: the top-level window, every control it created, and how many audio endpoints that machine offered (a build agent has none — that is a real answer, not a defect). Written by `NoiseInternet.exe --ui-check`. |
| `redist/` | `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, copied unmodified from Microsoft's redistributable on the build machine. **The exe in this zip does not need them.** They are here so a build made with `-DNI_STATIC_RUNTIME=OFF` (the smaller, dynamically-linked variant) can run by dropping them next to it. See `NOTICE-redist.txt`. |
| `BUILD-INFO.txt` | commit the binaries were built from, sizes, sha256 of every file in the zip, and the result of checking the exes' actual DLL imports. |

## Which build am I running? (read this first when something "does not work")

Both apps print the build they are, and they are supposed to match the release page you downloaded from:

- **Windows:** the title bar of the window is `Noise Internet [r6-123] - ...`, and the first line of the
  status panel is `build r6-123 (engine)`. Pressing Start also logs it.
- **Android:** the first line of the report (the big pane, and the text the **Copy report** button
  puts on the clipboard) is
  `build: engine r6-123   app 0.6-ci.123   ...`.

If those two strings are not the same build number, one of your two devices is running an older app and
none of the fixes described below are in it. This exists because a round of fixes was reported as "did not
work" when the copy on the phone was in fact the previous build — nothing on the screen said so.

**Installing the APK over an older copy:** builds from before round 6 were signed with a fresh random key
each time, so Android refuses to install the new one over the old one ("App not installed" / "package
conflicts"). If you see that, uninstall **Noise Internet** once and install the new APK. From round 6 on
every build uses the same (committed, debug-only) key, so later updates install over earlier ones.

## Who gives, who receives, and which way the bytes go

This is the part people get wrong, so it is worth stating plainly.

- **Traffic is two-way in both roles.** Whatever else is happening, the two ends are constantly
  exchanging bursts: frames, acknowledgements, retransmission requests, pairing challenges, keepalives,
  measured link state. A giver transmits *and* receives. A receiver transmits *and* receives.
- **`GIVER` / `RECEIVER` describe where the internet is, not the direction of the data.** The giver is
  the machine that has an internet connection (a phone on mobile data, or a laptop on Wi-Fi) and is
  willing to fetch things on behalf of the other end. The receiver is the machine that wants those bytes.
- **Either machine can be the giver.** Both the Windows app and the Android app implement both roles, and
  both can change role without restarting: tick the other radio button before Start, or press
  **Switch role** while running. So "phone gives, laptop receives" and "laptop gives, phone receives" are
  both supported — they are the same code with the roles swapped.
- **What is still missing is the last mile of the tunnel.** Data can now be carried both ways; the path
  that turns that into "a browser on the receiver loads a web page" is not finished: the receiver's local
  proxy carries UDP and DNS (and, on Windows, a CONNECT proxy for TCP that the desktop harness does not
  currently pair — defect 21). Treat "share my internet" as **not yet demonstrated end to end**, and read
  the app's Internet test output to see exactly which stage got through.

## What has and has not been proven

**Proven:** the code compiles for x64 Windows (MSVC) and for arm64/x86_64 Android (NDK r26); the engine
passes all 8 unit-test suites on both Linux and Windows; the framing, CRCs, FEC and encryption agree with
independently generated reference vectors; a 2 048-byte message crosses the **modelled** device chain in
each direction at once and is verified by SHA-256 at both ends in about 30 s; the Windows app's window and
every control in its inventory are created on every build (`ui-check.txt` in this zip is that run), on a
machine with no audio hardware at all.

**Not proven — this is the whole point of a physical test:** nothing in this zip has ever played a sound
through a loudspeaker or heard one through a microphone. No build host used here has audio hardware, and
no phone has ever run this app. So:

- The transfer rate you will get over real air is unknown. The modelled figure, 540 bit/s (**about 67
  bytes per second**), is a ceiling-like reference from a quiet simulated room, not a prediction, and not
  an internet speed.
- "Internet sharing" is **not** demonstrated; see the section above for exactly what is and is not wired.
  If you press **Internet test** and it fails, that is the recorded state, not your fault.
- The APK is signed with a throwaway debug key and the EXE is unsigned. Android will warn you about
  "unknown sources"; Windows SmartScreen will warn about an unsigned file. Nothing vouches for these
  binaries except this repository and its build log.

## Two ways to use it

### A. No hardware, 60 seconds, useful: check the engine on your PC

```
ni.exe selftest --trials 3 --snr 25
ni.exe linksim --bytes 228 --profile 11 --snr 25 --no-multipath --max-ms 120000
ni.exe audit
```

That runs modem + error correction + retransmission through the modelled room on one machine and prints
what it measured. `ni audit` tells you how many audio devices Windows offered (0 is a real answer on a
machine with no sound card). This exercises everything **except** speakers and a microphone — it is not
evidence about acoustic performance. You can use the windowed app the same way: `NoiseInternet.exe
--ui-check` prints the same device count and exits.

### B. The real thing: Windows PC + Android phone, speaker to microphone

You need: a Windows 10/11 PC with a working loudspeaker and microphone, an Android 6.0+ phone, the two
binaries, and about 30 minutes. Put them **10–40 cm apart**, on a quiet table, and turn the phone's media
volume to roughly 70–90 %. Use each device's built-in speaker and microphone; headsets and Bluetooth move
the signal away from the air.

**1. Install the APK.** With `adb` (Android platform-tools) and USB debugging enabled:

```
adb install noiseinternet-debug.apk
```

Without adb: copy `noiseinternet-debug.apk` to the phone, tap it, allow installing from that source. The
app is named **Noise Internet**. It asks for the microphone permission once — it needs it to listen, and it
runs a foreground service so the link survives the screen going off.

**Before anything else: the three mistakes that stop pairing dead.** Every one of them was made in a real
field test of this app, and none of them produced an error message at the time — that is fixed now, but the
fix is only useful if you know what to look for.

1. **An empty passphrase.** With no passphrase the engine does not send a pairing challenge at all, so the
   two ends cannot pair and no amount of waiting will change that. Both apps now refuse to start and tell
   you. Pick one passphrase, 8+ characters, and type the *same* one on both devices. The **match code**
   cannot appear without it.
2. **Different profile numbers or session ids on the two devices.** The profile is which set of tones is
   used, and the session id is who is being talked to. If they differ, each end transmits in a language
   the other is not listening to. Both apps now start on **profile 10, session id 7**; if you change one,
   change it on both.
3. **One end changing profile by itself.** Older builds had automatic profile adaptation enabled, and an
   endpoint that decided to adapt switched *its own* tones while the other end kept its own — after which
   the pair could never understand each other again. Adaptation is now off unless you ask for it, and a
   change is only applied once the other end confirms it.

The status panel now answers "why isn't it pairing" in one line, from the engine's own counters — see the
table at the end of this file for what each answer means.

**2. Start the Windows app.** Double-click `NoiseInternet.exe`. If SmartScreen objects, see the table
below. The window has, from the top:

- **role**, two radio buttons: *this machine GIVES its internet (giver)* / *this machine RECEIVES over
  sound (receiver)*. Choose one now; you can change it later without restarting.
- **microphone**, a drop-down of your real capture endpoints (the list comes from WASAPI, and anything the
  system offers that is a loopback — your own output stream — is labelled as unusable here).
- **speaker**, a drop-down of your real render endpoints.
- **profile**, the modulation profile: rate against robustness. It starts on **10 FD-LOW-DBPSK**, the
  profile every measured end-to-end run has completed on, and the low band (700–3600 Hz) is the one that
  copes with a room that reflects sound. `11 FD-HIGH-DBPSK` is faster in the model but needs a quiet,
  non-reflective path; `0 FSK-VOICE` is the most robust and the slowest; `7`/`8 ULTRA` are the
  near-ultrasonic ones most speakers cannot reproduce.
- **session id** and **passphrase**: both ends must use the *same* values or they will not pair.
- **match code**: read-only, empty until a pairing succeeds. When it fills in it shows four words; the
  other end must show the *identical* four words (the phone prints it as `match code:` in its status box).
  If they differ, something is in the middle — do not continue.
- **full duplex (experimental)**: leave unticked. FDD plus echo cancellation has never been tested on
  hardware.
- **local proxy on 127.0.0.1:8471 (receiver side)**: tick it if you want the receiver to serve the local
  proxy port.

Then press **Start**. The first lines in the log say which role started, and the status panel behind it
begins reporting. If the log says `audio open failed`, nothing is being transmitted and the panel will keep
saying so — fix the endpoints and press Start again.

**3. Start the phone app** with the same session id, the same passphrase, the same profile number, and
the opposite role (if the PC is the giver, the phone is the receiver). Choose the built-in microphone and
built-in speaker, not a headset. Press **Start** there. Leave the VPN-tunnel switch off for a first run
unless you specifically want to see the tunnel permission dialog.

**4. Get the link up, then test it.**

- Wait until both ends say the peer is **ONLINE** and a **match code** appears. Compare the four words on
  both screens. That is your only defence against a third device joining in, so actually compare them.
- Press **Calibrate** on one end. It sweeps the bands, prints measured SNR / frame success / BER /
  payload rate per profile, and suggests a profile and MTU. This is the honest way to pick a profile for
  your room instead of trusting the model's choice.
- Press **Link test**: 228 bytes through the air, hash-compared at the far end. It is slow by design — in
  the model that test took a median of 44 s. **Internet test** asks the giver to fetch
  `http://example.com/` and prints each stage's result; which stage fails *is* the answer.
- **Switch role** hands the "who has the internet" duty to the other end while the session is running.
- **Stop** releases the audio devices and closes the session.

**5. What to write down.** Distance, volume %, measured `snr`, `frames sent` / `received`, `retries`,
`crc-fail`, `malformed`, whether the link test reported completed or an error, and the elapsed time —
they are all on the status panel, and the log keeps the engine's own wording. `tests/physical/OTA_PROTOCOL.md`
in the repository is the full sheet. That is what would move gates C, D, F from PARTIAL / NOT TESTED to
something better — and if it fails, that report is just as valuable, because nothing here will quietly
substitute a simulation for your result.

## The same thing from a terminal

`NoiseInternet-console.exe` is the same application with a text interface. Useful when you want to paste a
result into a bug report, or drive it from a script:

```
NoiseInternet-console.exe --list-devices
NoiseInternet-console.exe --giver --spk "<render id>" --mic "<capture id>" --profile 11 --session 7 --secret "a-long-passphrase-you-make-up"
NoiseInternet-console.exe --receiver --proxy-token hunter2     # then: curl -x http://127.0.0.1:8471 -H "Proxy-Authorization: Noise hunter2" http://example.com/
```

Single keys at the prompt: `a` calibrate · `l` link test (228 B) · `i` internet test · `r` switch role ·
`p` profile table · `s` status as JSON · `q` quit (prints a final status screen). Enter on an empty line
reprints the status screen. Every number on it comes from the engine's own counters; unmeasured fields are
visible as such. The line `tunnel : down  no local proxy on this role` is expected on the giver.

The console build accepts `--proxy-token <token>`, which is how you give the receiver's proxy a token you
know. The window itself still has no field for it (defect 27).

## If it does not work

| you see | what it means |
|---|---|
| `audio open failed` / `NOT TESTED: no usable capture/render pair` in the window's log | WASAPI refused the endpoint pair — most often no microphone present or exclusive-mode use by another app. The app reports this instead of pretending to transmit. Check with `NoiseInternet-console.exe --list-devices`. |
| `device enumeration unavailable` or `NOT TESTED: this build has no audio backend` | the exe was built without `-DNI_WITH_WASAPI=ON`, or the machine truly has no capture endpoint. Rebuild; the flag is on by default in `windows/CMakeLists.txt`. |
| SmartScreen "Windows protected your PC" | expected: unsigned. "More info → Run anyway", or verify the exe against the sha256 in `BUILD-INFO.txt` / the release page first. |
| The window opens but the mic/speaker drop-downs only say `(system default)` | that machine exposed no capturable endpoints to WASAPI. The app will not invent one; it will fail at Start and say so. |
| Play Protect / "unknown source" on Android | expected: debug-signed, not from the Play Store. |
| The two ends never show the peer as online | work through the one-line **why** on each status panel, in this order: (1) *"no passphrase"* — type the same 8+ character passphrase on both ends; (2) *"bursts are arriving (sync found) but none of them decodes"* — the two ends are on different profile numbers or session ids, or the level is wrong: compare profile number, session id and passphrase character by character; (3) *"the microphone hears almost nothing (… dBFS)"* — the wrong microphone is selected, the other device is too quiet, or the devices are too far apart; (4) *"no burst decoded yet"* with a sane input level (−40 to −10 dBFS) — the other end may not be transmitting at all: check that it says it is running, and that it is on the same session id. |
| Match code stays empty, no audio at all | the giver's speaker is muted or the phone's media volume is 0; or the phone is applying "voice isolation"/noise suppression — turn audio effects off, and use the built-in mic and speaker, not a headset or Bluetooth (that routes the signal away from the air). |
| The window says *refused to start: the passphrase is N characters long* | that is the check working. Both ends need the same passphrase of 8+ characters; without one the engine never sends a pairing challenge. |
| CRC failures and lots of retries | too loud or too far: the model has a ~26 dB receiver distortion floor (defect 20), and real AGC in phones makes that worse. Lower the volume a little and try again. |
| Internet test fails at an early stage | the known state: the desktop proxy path does not pair (defect 21) and the Android tunnel carries no TCP. Which stage failed is the result — that is what the printout is for. |
| A browser pointed at `127.0.0.1:8471` says it needs a password | correct: the local proxy uses `Proxy-Authorization: Noise <token>`, which browsers cannot send. The window has a **proxy token** field and a **show secrets** checkbox; tick it to read the token, or use the console build with `--proxy-token` and curl, as shown above. |
| The volume keeps changing while the app runs | that is the app, once, on purpose: if the output volume is below half it is raised to 85 % for the session and put back when you press Stop. The status panel says `volume: speaker 85%, microphone 100%`, and the copy in the log records the value it was raised from. |
| The status panel says the sound is going to headphones/Bluetooth | that is the app catching the most expensive mistake in this project's history: with a headset connected, the tones go into your ear and the room hears nothing. Unplug it, or pick the built-in speaker. |
| The window closes while a test is running | it will not: pressing Close during a link test waits for the result, because killing the engine mid-measurement would produce a number that means nothing. The tests are bounded (the link test gives up after ~12 s of silence). |

## Building it yourself instead of trusting the zip

Windows: `cmake -S windows -B build` then `cmake --build build --config Release` (needs Visual Studio
2022 "Desktop development with C++" + CMake; see `windows/BUILD.md`). That produces both
`NoiseInternet.exe` (the window) and `NoiseInternet-console.exe`. Android: `android/build_apk.sh` (needs
JDK 17, Android SDK platform 34, build-tools and NDK 26.1.10909125; see `android/BUILD.md`). The CI
definitions in `.github/workflows/build.yml` are exactly what produced these files, including the size and
hash manifest and the `ui-check.txt` transcript.
