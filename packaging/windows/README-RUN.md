# Running Noise Internet — Windows exe and Android apk

Read the first two sections before you plug anything in. They are short and they are the difference
between a useful evening and a confusing one.

## What is in this zip

| file | what it is |
|---|---|
| `NoiseInternet.exe` | the Windows app: same engine, real WASAPI audio, console UI. **Standalone** — built with the C++ runtime linked in (`/MT`), so you do not need to install the Visual C++ redistributable. |
| `ni.exe` | the same engine as a command-line tool: `ni profiles`, `ni selftest`, `ni linksim`, `ni audit`, `ni demo`. Nothing here generates fake results; `ni audit` on a machine with no sound hardware honestly reports 0 devices. |
| `redist/` | `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, copied unmodified from Microsoft's redistributable on the build machine. **The exe in this zip does not need them.** They are here so a build made with `-DNI_STATIC_RUNTIME=OFF` (the smaller, dynamically-linked variant) can run by dropping them next to it. See `NOTICE-redist.txt`. |
| `BUILD-INFO.txt` | commit the binaries were built from, sizes, sha256 of every file in the zip, and the result of checking the exe's actual DLL imports. |

## What has and has not been proven

**Proven:** the code compiles for x64 Windows (MSVC) and for arm64/x86_64 Android (NDK r26); the engine
passes all 8 unit-test suites on both Linux and Windows; the framing, CRCs, FEC and encryption agree with
independently generated reference vectors; a 2 048-byte message crosses the **modelled** device chain in
each direction at once and is verified by SHA-256 at both ends in about 30 s.

**Not proven — this is the whole point of a physical test:** nothing in this zip has ever played a sound
through a loudspeaker or heard one through a microphone. No build host used here has audio hardware, and
no phone has ever run this app. So:

- The transfer rate you will get over real air is unknown. The modelled figure, 540 bit/s (**about 67
  bytes per second**), is a ceiling-like reference from a quiet simulated room, not a prediction, and not
  an internet speed.
- "Internet sharing" is **not** demonstrated. The proxy leg currently does not even pair on the desktop
  harness, and the Android tunnel carries only UDP and DNS — a browser needs TCP, so do not expect web
  pages to load. If you try `i` (internet test) and it fails, that is the recorded state, not your fault.
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
evidence about acoustic performance.

### B. The real thing: Windows PC + Android phone, speaker to microphone

You need: a Windows 10/11 PC with a working loudspeaker and microphone, an Android 6.0+ phone, the two
binaries, and about 30 minutes. Put them **10–40 cm apart**, on a quiet table, and turn the phone's media
volume to roughly 70–90 %.

**1. Install the APK.** With `adb` (Android platform-tools) and USB debugging enabled:

```
adb install noiseinternet-debug.apk
```

Without adb: copy `noiseinternet-debug.apk` to the phone, tap it, allow installing from that source. The
app is named **Noise Internet**. It asks for the microphone permission once — it needs it to listen, and it
runs a foreground service so the link survives the screen going off.

**2. Find out what audio endpoints your PC has.**

```
NoiseInternet.exe --list-devices
```

It prints every capture ("mic") and render ("spk") endpoint with its id. Ignore anything marked
`[LOOPBACK - unusable as a microphone]`; that is the PC's own output stream, not a microphone. Copy the
two ids you want.

**3. Decide who is the giver.** The **giver** is the machine with internet that pushes bytes over its
loudspeaker. The **receiver** is the machine that listens. Both sides must use the *same* session id,
*same* passphrase and *same* profile, or they will simply not pair.

On the PC, as giver, with the profile that performs best in the model (11 = FD-HIGH-DBPSK):

```
NoiseInternet.exe --giver --spk "<render id>" --mic "<capture id>" --profile 11 --session 7 --secret "a-long-passphrase-you-make-up"
```

On the phone: role **Receiver (uses the link)**, Microphone = the phone's built-in mic, Speaker = the
phone's built-in loudspeaker, profile 11, session 7, the same passphrase, then press **Start**.

Leave the full-duplex box unticked (FDD + echo cancellation is experimental and has never been tested on
hardware). Tick "Route UDP/DNS through a VPN tunnel" only if you specifically want to see the tunnel
permission dialog and the DNS path — the app will say plainly in its status text what it managed to set up.

**4. Test the link.** On the PC press `l` then Enter — that sends 228 bytes through the air and waits for
the acknowledgement path. It is slow by design: in the model that test took a median of 44 s. On the phone
tap **Link test** for the same thing in the other direction. Single-key commands on the PC:

```
a = calibrate (sweeps the bands, prints per-profile SNR/BER/rate and a suggested profile + MTU)
l = link test, 228 B
i = internet test (giver fetches http://example.com/ and prints each stage's result)
r = switch role at runtime
p = the profile table the core actually uses
s = status as JSON
q = quit (prints a final status screen)
```

Press Enter on an empty line to reprint the status screen. Every number on it comes from the engine's own
counters; unmeasured fields print `-`. The line `tunnel : down  no local proxy on this role` is expected on
the giver.

**5. What to write down.** Distance, volume %, `measured SNR`, `frames sent/recv`, `retries`, `crc-fail`,
`malformed`, whether the link test reported `completed` or an error string, and the elapsed time.
`tests/physical/OTA_PROTOCOL.md` in the repository is the full sheet; the app's **Measured reports** panel
shows the same data on screen. That is what would move gates C, D, F from PARTIAL / NOT TESTED to
something better — and if it fails, that report is just as valuable, because nothing here will quietly
substitute a simulation for your result.

## If it does not work

| you see | what it means |
|---|---|
| `device enumeration unavailable` or `NOT TESTED: this build has no audio backend` | the exe was built without `-DNI_WITH_WASAPI=ON`, or the machine truly has no capture endpoint. Rebuild; the flag is on by default in `windows/CMakeLists.txt`. |
| SmartScreen "Windows protected your PC" | expected: unsigned. "More info → Run anyway", or verify `NoiseInternet.exe` against the sha256 in `BUILD-INFO.txt` / the release page first. |
| Play Protect / "unknown source" on Android | expected: debug-signed, not from the Play Store. |
| The two ends never show `peer ONLINE` | different session id, different passphrase, or different profile. Check `s` on the PC and Refresh on the phone. |
| `pairing did not complete`, no audio at all | the giver's speaker is muted or the phone's media volume is 0; or the phone is applying "voice isolation"/noise suppression — turn audio effects off and use the built-in mic/speaker, not a headset or Bluetooth (that routes the signal away from the air). |
| CRC failures and lots of retries | too loud or too far: the model has a ~26 dB receiver distortion floor (defect 20), and real AGC in phones makes that worse. Lower the volume a little and try again. |
| `i` (internet test) fails at an early stage | the known state: `ni demo`'s proxy path does not pair (defect 21) and the Android tunnel carries no TCP. Which stage failed is the result — that's what the printout is for. |
| A browser pointed at `127.0.0.1:8471` says it needs a password | correct: the local proxy uses `Proxy-Authorization: Noise <token>`, which browsers cannot send. Use curl, and give the receiver a token you know: `NoiseInternet.exe --receiver ... --proxy-token hunter2` then `curl -x http://127.0.0.1:8471 -H "Proxy-Authorization: Noise hunter2" http://example.com/`. The app has no equivalent field yet (defect 27), so its proxy can only be driven from inside the app. |

## Building it yourself instead of trusting the zip

Windows: `cmake -S windows -B build` then `cmake --build build --config Release` (needs Visual Studio
2022 "Desktop development with C++" + CMake; see `windows/BUILD.md`). Android: `android/build_apk.sh`
(needs JDK 17, Android SDK platform 34, build-tools and NDK 26.1.10909125; see `android/BUILD.md`). The CI
definitions in `.github/workflows/build.yml` are exactly what produced these files, including the size and
hash manifest.
