#!/usr/bin/env python3
"""Front-end parity check: the Windows window and the Android screen must offer the same things.

Why this exists
---------------
The task says the profile, the session id and the passphrase have to be in both apps, and that every
window which shows written results needs a working copy button. Both of those were true on one side and
missing on the other at different times (the proxy token was in neither, the session id defaulted to 0
on the phone and 7 everywhere else, the record-and-analyse button existed only on Windows). A human
reading two source files will not catch the next one either, so this script reads both front ends and
refuses to pass while a control is missing from one of them.

It checks *presence and wiring*, not pixels: that the control is created with the right style (a
password field for a secret), that its value is read, and that it reaches the engine. It cannot prove
that a human sees the window - that is what the GUI's own --ui-check layout audit and a real run on
hardware are for.

Exit code 0 = both front ends offer everything; 1 = something is missing (printed).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

WINDOWS_GUI = ROOT / "windows/src/NoiseInternet/gui_main.cpp"
ANDROID_LAYOUT = ROOT / "android/app/src/main/res/layout/activity_main.xml"
ANDROID_MAIN = ROOT / "android/app/src/main/java/com/noiseinternet/app/MainActivity.kt"
ANDROID_SERVICE = ROOT / "android/app/src/main/java/com/noiseinternet/core/NoiseService.kt"

# name -> (what it is for, windows regex, android regexes over layout + kotlin)
CHECKS = [
    ("profile selector", "the modulation profile both ends must match",
     r"IDC_PROFILE", [r'@\+id/profile"']),
    ("session id field", "the session number both ends must match",
     r"IDC_SESSION", [r'@\+id/session_id"', r"EXTRA_SESSION"]),
    ("passphrase field", "the pairing secret; must be masked in both",
     r"IDC_SECRET", [r'@\+id/secret"', r"textPassword", r"EXTRA_SECRET"]),
    ("role choice", "give internet vs receive internet",
     r"IDC_GIVER", [r'@\+id/role_giver"', r"@\+id/role_receiver", r"EXTRA_ROLE"]),
    ("proxy token field", "the receiver proxy's bearer token (defect 27); masked",
     r"IDC_TOKEN", [r'@\+id/proxy_token"', r"EXTRA_PROXY_TOKEN"]),
    ("give/receive start", "starting the session",
     r"IDC_START", [r'@\+id/start"']),
    ("stop", "stopping the session",
     r"IDC_STOP", [r'@\+id/stop"']),
    ("record and analyse", "the microphone self-test the field reports needed",
     r"IDC_REC", [r'@\+id/record_test"', r"diagnosePcm"]),
    ("link test", "the two-way link test",
     r"IDC_LINK", [r'@\+id/link_test"']),
    ("internet test", "the giver's own internet check",
     r"IDC_NET", [r'@\+id/net_test"']),
    ("copy results", "every window that shows written results needs a working copy button",
     r"IDC_COPY_STATUS", [r'@\+id/copy_report"', r"ClipboardManager"]),
    ("match code shown", "the pairing match code has to be readable and comparable on both ends",
     r"sas|match_code", [r"st\.sas|match code"]),
    # The second copy button: the Windows window has two panes of written results (status and log), and a
    # copy button that exists for one of them is a copy button that fails the moment someone wants the
    # other. The Android app has one results pane and one button.
    ("copy the log pane", "the Windows log is the pane users paste; it needs its own copy button",
     r"IDC_COPY_LOG", [r'@\+id/copy_report"']),
    # Defect 71, from the field: the phone's copy button copied an empty pane. Presence of the control is
    # not enough - the copy has to take the report the reader is looking at. Checked below in detail; this
    # entry keeps the two platforms honest about *having* a copy report handler.
    ("copy report handler", "the copy button must copy the report that is on screen",
     r"IDC_COPY_STATUS", [r"private fun copyReport\(\)"]),
    # Defect 69: the phone started on session 0 while the PC started on 7, so two apps with the same
    # passphrase could never pair and nothing on either screen said so.
    ("session id default", "both front ends must start on the same session id (7)",
     r'IDC_SESSION.\s*;|mk\(hwnd, L"EDIT", L"7"', [r'sessionEdit\.setText\("7"\)']),
    # Defect F: a number the engine never measured must not be printed as a reading on either platform.
    ("not-measured wording", "both front ends must obey the measured flags instead of printing defaults",
     r"phy_measured", [r"phyMeasured", r"arqMeasured"]),
]

def read(path: Path) -> str:
    if not path.exists():
        print(f"  MISSING FILE: {path.relative_to(ROOT)}")
        return ""
    return path.read_text(encoding="utf-8", errors="replace")

def main() -> int:
    win = read(WINDOWS_GUI)
    lay = read(ANDROID_LAYOUT)
    act = read(ANDROID_MAIN)
    svc = read(ANDROID_SERVICE)
    android = lay + "\n" + act + "\n" + svc

    print(f"windows: {WINDOWS_GUI.relative_to(ROOT)} ({len(win)} bytes)")
    print(f"android: {ANDROID_LAYOUT.relative_to(ROOT)} + MainActivity.kt + NoiseService.kt "
          f"({len(android)} bytes)")
    print()
    print(f"{'control':<22} {'windows':<9} {'android':<9} why")
    print("-" * 96)
    bad = []
    for name, why, wrx, arxs in CHECKS:
        w = re.search(wrx, win) is not None
        a = all(re.search(rx, android) for rx in arxs)
        print(f"{name:<22} {'yes' if w else 'NO':<9} {'yes' if a else 'NO':<9} {why}")
        if not (w and a):
            bad.append(name)

    # The masked-secret requirement, checked precisely: the mk(...) statement that creates the control
    # must carry ES_PASSWORD. (The first version of this check searched the whole file for ES_PASSWORD
    # after the string "IDC_SECRET" and was satisfied by the *proxy token* field further down - which is
    # exactly how a real unmasked passphrase field stayed in the GUI.)
    for control, label in (("IDC_SECRET", "passphrase"), ("IDC_TOKEN", "proxy token")):
        m = re.search(r"mk\(.*?" + control + r"\s*\)?\s*;", win, re.S)
        if m is None:
            # the id is the last argument; match everything from mk( up to the first ';' that follows it
            m = re.search(r"mk\(" + r"[^;]*" + control, win, re.S)
        if m is None:
            bad.append(f"windows {label} control not found")
            print(f"\nWINDOWS: no mk(...) statement creates the {control} control")
        elif "ES_PASSWORD" not in m.group(0):
            bad.append(f"windows {label} not masked")
            print(f"\nWINDOWS: the {label} field is created without ES_PASSWORD:\n  "
                  + " ".join(m.group(0).split()))

    # Defect 71 in full: the Android copy handler must take the *report* panes. The old one copied only
    # `results` (the pane that is empty at rest) and its acknowledgement was overwritten by the 500 ms
    # refresh loop, so the button answered "nothing to copy yet" for ever.
    if "private fun copyReport()" not in act:
        bad.append("android copy handler missing")
        print("\nANDROID: MainActivity has no copyReport() - the copy button has nowhere to copy from")
    else:
        if "reportBase" not in act:
            bad.append("android copy does not read the report pane")
            print("\nANDROID: copyReport() does not read reportBase, i.e. the report renderStatus() builds")
        if re.search(r"val text = results\.text\?\.toString\(\)\.orEmpty\(\)", act):
            bad.append("android copy still reads only the results pane")
            print("\nANDROID: the old 'copy results.text only' handler is still present")
        if "setPrimaryClip" not in act:
            bad.append("android copy never reaches the clipboard")
            print("\nANDROID: copyReport() never calls setPrimaryClip")

    # The proxy token must not be written to the log pane: that is the pane people paste.
    if re.search(r'proxy_token\(\)\.c_str\(\)[^\n]*say|say[^\n]*proxy_token\(\)\.c_str\(\)', win):
        bad.append("windows token in the log pane")
        print("\nWINDOWS: the proxy token is written to the log pane, which is the pane users paste")

    print()
    if bad:
        print(f"FAIL: {len(bad)} item(s) missing or wrong: {', '.join(bad)}")
        return 1
    print("PASS: both front ends offer the same controls, and both have a copy button")
    return 0

if __name__ == "__main__":
    sys.exit(main())
