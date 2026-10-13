#!/usr/bin/env bash
# run_ota_pair.sh - drive one over-the-air payload trial and append a CSV row.
#
# The script never invents a result. It renders a burst, asks you to play it, records on the peer, and
# writes whatever `ni decode` actually reported. If the toolchain has no audio path, or the recording is
# empty, the row says NOT TESTED with the reason, because a missing measurement is information and a
# fabricated one is contamination.
#
# Usage (two machines):
#   TX:  ./run_ota_pair.sh render  [profile] [bytes]      -> prints the command to run on RX
#   RX:  ./run_ota_pair.sh capture [profile] [sha256]     -> records, decodes, appends results.csv
set -u

NI=${NI:-./ni}
DIR=${NI_OTA_DIR:-.}
PROFILE=${2:-DBPSK-WIDE}
BYTES=${3:-512}          # render mode: payload size. In capture mode the size is read back below,
                        # because the only honest number for that column is the one that was decoded.
CSV="$DIR/ota_results.csv"
DIST=${OTA_DISTANCE_M:-1.0}
VOL=${OTA_VOLUME:-unset}

need_ni() {
  if [ ! -x "$NI" ]; then
    echo "no $NI - build it first (scripts/build_local.sh) or set NI=/path/to/ni" >&2
    exit 2
  fi
}

# True only if the engine itself reports at least one usable endpoint pair.
audio_ok() {
  "$NI" audit 2>/dev/null | grep -qE "audio devices:[[:space:]]*[1-9]"
}

row() { # row <status> <snr> <evm> <retries> <elapsed_ms> <note>
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$DIST" "$VOL" "$PROFILE" "$BYTES" \
    "$1" "$2" "$3" "$4" "$5" "$6" >>"$CSV"
}

ensure_header() {
  [ -f "$CSV" ] || echo 'timestamp_utc,distance_m,volume,profile,payload_bytes,decoded,snr_db,evm_pct,retries,elapsed_ms,note' >"$CSV"
}

case "${1:-}" in
  render)
    need_ni
    audio_ok || echo "WARNING: no audio backend in this build; this run can only produce a file, not an acoustic test." >&2
    ensure_header
    out="$DIR/ota_burst.wav"
    echo "== rendering $BYTES bytes on $PROFILE -> $out"
    "$NI" render --out "$out" --profile "$PROFILE" --random "$BYTES" --pad 2048 | tee "$DIR/ota_render.log"
    sha=$(sed -n 's/.*payload sha256 = \([0-9a-f]\{64\}\).*/\1/p' "$DIR/ota_render.log" | head -1)
    echo
    echo "now play $out from the loudspeaker while the peer runs:"
    echo "  ./run_ota_pair.sh capture $PROFILE $sha"
    ;;
  capture)
    need_ni
    ensure_header
    sha=${3:-}
    rec="$DIR/ota_rec.wav"
    echo "== recording 4 s from the microphone -> $rec"
    if command -v arecord >/dev/null 2>&1; then
      arecord -q -f F32LE -r 48000 -c 1 -d 4 "$rec" 2>"$DIR/ota_arecord.err"
    elif command -v rec >/dev/null 2>&1; then
      rec -q -r 48000 -c 1 "$rec" trim 0 4 2>"$DIR/ota_arecord.err"
    else
      echo "no recorder found (arecord/rec). Record 48 kHz mono float to $rec yourself, then re-run." >&2
      row NOT_TESTED - - - - "no recorder on this machine"
      exit 3
    fi
    if [ ! -s "$rec" ]; then
      echo "recording is empty - the microphone did not capture anything" >&2
      row FAIL - - - - "empty recording"
      exit 4
    fi
    args=(decode --in "$rec" --profile "$PROFILE")
    [ -n "$sha" ] && args+=(--expect-sha256 "$sha")
    args+=(--save "$DIR/ota_got.bin")
    echo "== decoding"
    "$NI" "${args[@]}" 2>&1 | tee "$DIR/ota_decode.log"
    snr=$(sed -n 's/.*snr[=: ]\(-\?[0-9.]*\).*/\1/p' "$DIR/ota_decode.log" | head -1)
    evm=$(sed -n 's/.*e[vc][ve]*m[=: ]\([0-9.]*\).*/\1/p' "$DIR/ota_decode.log" | head -1)
    if [ -f "$DIR/ota_got.bin" ]; then
      BYTES=$(wc -c <"$DIR/ota_got.bin" | tr -d " ")
    else
      BYTES=-
    fi
    if grep -qi "match\|MATCHED\|sha256 ok" "$DIR/ota_decode.log"; then
      row PASS "${snr:--}" "${evm:--}" 0 0 "see ota_decode.log"
      echo "PASS"
    else
      row FAIL "${snr:--}" "${evm:--}" 0 0 "no payload match reported; see ota_decode.log"
      echo "FAIL (that is a result, not an error to hide)"
    fi
    ;;
  *)
    sed -n '1,20p' "$0" | grep '^#' | cut -c3-
    exit 1
    ;;
esac
