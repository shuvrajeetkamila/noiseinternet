# Over-the-air (physical) test protocol

The automated suite in `shared/acoustic_core/tests/` and every number in `reports/RESULTS.md` were
produced **without any sound hardware**. This directory is the plan for the tests that cannot be run in
this environment; nothing here has been executed, and no row of the result table below is filled in.

## Ground rules

1. A row stays empty until a human reads the number off a screen or off `ni`'s output. If a test could
   not be run, write `NOT TESTED` in the cell and the reason in the notes column. An empty CSV is a
   correct result; a filled-in guess is not.
2. No substitutes. Speaker to microphone means a loudspeaker and a microphone in the same room, with the
   microphone actually picking up the loudspeaker. Wi-Fi Direct, USB, a Bluetooth profile, a virtual
   audio cable, "Stereo Mix", `loopback` devices, or copying a WAV from one machine to the other are
   *not* acoustic tests and may not be reported as ones. (`ni decode` of a file someone else sent you is
   a codec test, which is a different, weaker claim.)
3. Local mock servers do not prove internet access. A fetch from `api.github.com` through the receiver's
   proxy does - and it must be verified by the payload's SHA-256, not by a page appearing.
4. Report link rate and useful throughput separately. The burst carries headers, FEC and framing; a
   "seconds to load a page" number is a different measurement and must be timed with a stopwatch or the
   engine's own `elapsed_ms`, not inferred.

## Equipment

Two machines, each with a working speaker and microphone (a phone + a laptop is the target case), a
measuring tape, and a quiet room. Optional: a sound level meter or a phone SPL app to record the loudness
at 1 m, so the volume setting can be reproduced between runs.

## Test 1 - one-way payload (the minimum claim worth making)

On the transmitting machine:

```sh
ni render --out burst.wav --profile DBPSK-WIDE --random 512 --pad 2048
# it prints: payload sha256 = <H>
aplay burst.wav            # or: paplay / sdplay, from the machine's speaker
```

While that plays, on the receiving machine:

```sh
arecord -f F32LE -r 48000 -c 1 -d 4 rec.wav
ni decode --in rec.wav --profile DBPSK-WIDE --expect-sha256 <H> --save got.bin
```

Pass only if `ni decode` reports the payload matched. Record distance, speaker volume, the measured SNR
and EVM it prints, and the wall-clock time. Repeat 5 times per configuration.

`ni decode` also has an auto-sweep mode (no `--profile`): it tries every profile and reports which ones
locked, which is the fastest way to find a usable band on unknown hardware.

## Test 2 - two-way session (this is where the project currently stands, and it does not pass)

Run the CLI or the app on both machines, giver and receiver, same passphrase and session id, then
calibrate, then the link test. The gate is not "pairing succeeded" but **the link test's SHA-256 matching**
- in simulation pairing succeeds and the transfer still does not complete (`reports/linksim.csv` is all
`delivered 0`), so hardware results must be read with that in mind.

## Test 3 - proxy fetch (the first "internet" claim)

Only after test 2 passes: on the receiver machine point a browser or `curl -x http://127.0.0.1:8471` at a
known small URL whose content hash you can compute independently, and compare. Record the elapsed time,
the bytes moved, and the engine's counters for retries and lost frames from the same screen. Anything less
than a verified hash of content that came from outside the receiver's machine is not internet access.

## Test 4 - full duplex and echo cancellation

Set the duplex option on both ends, run test 2, and record whether the ERLE figure moves at all. If the
link only works in half duplex, that is the finding - the FDD path is an experiment in this codebase, not
a supported mode.

## Results table (fill it in by hand; it is intentionally empty)

| # | date | devices (spk / mic) | distance m | volume | profile | payload B | runs | decoded | mean SNR dB | EVM % | retries | elapsed ms | notes |
|---|------|---------------------|-----------|--------|---------|-----------|------|---------|-------------|-------|---------|-----------|-------|
| 1 | | | | | | | | | | | | | NOT TESTED - no hardware in the authoring environment |
| 2 | | | | | | | | | | | | | NOT TESTED |
| 3 | | | | | | | | | | | | | NOT TESTED |
| 4 | | | | | | | | | | | | | NOT TESTED |

The same columns are what `scripts/`-driven runs emit, so a manual table and a scripted log can be
compared row by row.
