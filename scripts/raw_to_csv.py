#!/usr/bin/env python3
"""Derive the CSV benchmark exports from the raw logs in reports/raw/.

Both inputs are produced by the same runs quoted in reports/RESULTS.md:

  reports/raw/selftest_*.txt   the fixed-point table printed by
                               `ni selftest --profile all --trials N --snr S`
                               (header-driven: profile, snr_dB, ok, rawBER, fecBER, burst_ms,
                               B/s, evec%, then a free-text note that may contain spaces)
  reports/raw/linksim.jsonl    one JSON object per trial of `ni linksim ... --json`

Every number here is a measurement written by the program under test in this workspace; nothing is
synthesised, and empty/failed fields stay empty rather than being filled in. The `delivered` column
of linksim.csv: rows whose `cfg` begins with `fix-` were taken after the burst-capacity fix (RESULTS.md
defect 18) and include hash-verified multi-kB transfers; the older rows, every one of them `delivered 0`,
are kept because they are the record of what the broken code did.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "reports", "raw")
OUT = os.path.join(ROOT, "reports")

# Fields exported from each linksim trial record, in this order. Absent keys become empty cells.
# The .jsonl holds one record per linksim batch: the batch settings at the top level (cfg, profile,
# snr, extra, bytes) and the run summary nested under "r". Per-side diagnostics (pairing state,
# rx_malformed, ack counters) are inside r.error's human-readable text and are exported with the raw
# file, not flattened here.
LINKSIM_FIELDS = [
    "cfg", "profile", "snr", "bytes", "extra", "trials", "ok", "delivered", "seconds",
    "rtt_ms", "up_bps", "down_bps", "retransmits", "lost_frames", "snr_db", "error",
]


def csv_cell(v):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return repr(round(v, 4))
    s = str(v)
    if any(c in s for c in ',"\n'):
        return '"' + s.replace('"', '""') + '"'
    return s


def selftest_to_csv(src, dst):
    with open(src, encoding="utf-8") as fh:
        lines = [ln.rstrip("\n") for ln in fh if ln.strip()]
    if not lines or not lines[0].lstrip().startswith("profile"):
        # Only the summary survived in this capture (it was produced with grep/tail), so there is no
        # table to convert. Skip it rather than invent columns.
        print("skipping %s: no table header" % os.path.relpath(src, ROOT))
        return 0
    header = lines[0].split()
    cols = header + ["note"]
    rows = []
    for ln in lines[1:]:
        if ln.startswith("="):
            continue
        toks = ln.split(None, len(header))
        if len(toks) < len(header):
            continue
        rec = dict(zip(header, toks[: len(header)]))
        note = toks[len(header)] if len(toks) > len(header) else ""
        rec["note"] = note
        rows.append(rec)
    with open(dst, "w", encoding="utf-8", newline="") as fh:
        fh.write(",".join(cols) + "\n")
        for r in rows:
            fh.write(",".join(csv_cell(r.get(c)) for c in cols) + "\n")
    return len(rows)


def linksim_to_csv(src, dst):
    recs = []
    with open(src, encoding="utf-8") as fh:
        for ln in fh:
            ln = ln.strip()
            if ln.startswith("{"):
                try:
                    recs.append(json.loads(ln))
                except ValueError:
                    pass
    with open(dst, "w", encoding="utf-8", newline="") as fh:
        fh.write(",".join(LINKSIM_FIELDS) + "\n")
        for raw in recs:
            r = dict(raw)
            sub = raw.get("r")
            if isinstance(sub, dict):
                r.update(sub)
            fh.write(",".join(csv_cell(r.get(c)) for c in LINKSIM_FIELDS) + "\n")
    return len(recs)


def main():
    n = 0
    for name in sorted(os.listdir(RAW)):
        if name.startswith("selftest") and name.endswith(".txt"):
            dst = os.path.join(OUT, name.replace(".txt", ".csv"))
            k = selftest_to_csv(os.path.join(RAW, name), dst)
            if k:
                print("wrote %s (%d rows)" % (os.path.relpath(dst, ROOT), k))
                n += 1
    ls = os.path.join(RAW, "linksim.jsonl")
    if os.path.exists(ls):
        dst = os.path.join(OUT, "linksim.csv")
        k = linksim_to_csv(ls, dst)
        print("wrote %s (%d rows)" % (os.path.relpath(dst, ROOT), k))
        n += 1
    if not n:
        raise SystemExit("no raw logs found under %s" % RAW)
    return 0


if __name__ == "__main__":
    sys.exit(main())
