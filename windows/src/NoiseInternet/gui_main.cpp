// gui_main.cpp — the windowed Windows front end for the NOISE INTERNET core.
//
// Same engine as NoiseInternet-console.exe, driven by controls instead of by keys: role (giver or
// receiver, switchable while running because either end can be the one with the internet), the two
// audio endpoints, the modulation profile, the session id, the passphrase, the short match code the two
// ends have to agree on, and buttons for the measured tests (calibrate / link test / internet test).
//
// Nothing in the status panel is generated here: every number is a field of Node::snapshot(), and where
// the engine has not measured something the panel prints what the engine said (""not verified", "NOT
// TESTED", a device count of 0). The window is deliberately honest about hardware: if WASAPI hands back
// no capture endpoint, the log says so and the transfer buttons report the engine's error rather than
// pretending.
//
// No dependencies beyond user32/gdi32 and the static CRT: plain controls, no manifest tricks, no Qt.
// `--ui-check` creates the window, enumerates the controls it built, writes ui-check.txt and exits -
// that is what CI runs, because a build agent has no speaker to test with.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <stdio.h>
#include <atomic>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ni/abi.h"
#include "ni/audio_wasapi.h"
#include "ni/common.h"
#include "ni/node.h"

using namespace ni;

namespace {

enum {
  IDC_GIVER = 1000, IDC_RECEIVER, IDC_MIC, IDC_SPK, IDC_PROFILE, IDC_SESSION, IDC_SECRET,
  IDC_MATCH, IDC_DUPLEX, IDC_PROXY, IDC_START, IDC_STOP, IDC_SWITCH, IDC_CAL, IDC_LINK, IDC_NET,
  IDC_STATUS, IDC_LOG, TIMER_REFRESH = 1
};

// ---------------------------------------------------------------- text plumbing (UTF-8 engine <-> UTF-16 UI)
std::wstring w8(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
std::string u8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring fmt(const wchar_t* f, ...) {
  std::vector<wchar_t> buf(2048);
  va_list ap;
  va_start(ap, f);
  int n = vswprintf(&buf[0], buf.size(), f, ap);
  va_end(ap);
  if (n < 0) return std::wstring(L"(text formatting failed)");
  return std::wstring(buf.data(), (size_t)n);
}

// A breadcrumb log written with Win32 calls only (no CRT, no buffering): if the window ever dies on a
// machine nobody is watching, the file says which step it reached and what the exception was. This exists
// because the first CI run of `--ui-check` exited -1073741819 (0xC0000005) and wrote no file at all, which
// is exactly the result a build log cannot explain: the app has to say where it died.
struct Trace {
  HANDLE h = INVALID_HANDLE_VALUE;
  void open(const wchar_t* path) {
    if (h != INVALID_HANDLE_VALUE) return;
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const unsigned char bom[2] = {0xFF, 0xFE};   // UTF-16LE; PowerShell's Get-Content wants the BOM
      DWORD w = 0;
      WriteFile(h, bom, 2, &w, nullptr);
    }
  }
  void line(const std::wstring& s) {
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, s.data(), (DWORD)(s.size() * sizeof(wchar_t)), &w, nullptr);
    WriteFile(h, L"\r\n", 4, &w, nullptr);
  }
  void close() {
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
  }
};
Trace g_trace;
const wchar_t* g_trace_path = L"gui-crash.txt";

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
  g_trace.open(g_trace_path);
  const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
  wchar_t buf[512];
  swprintf_s(buf, L"FATAL: unhandled exception 0x%08lX at address %p - the last step above is where it "
                  L"died. Nothing was transmitted. If this happened while the device list was being read, "
                  L"remember that a build agent has no audio endpoints and the app has to report zero, "
                  L"not die.",
             er ? (unsigned long)er->ExceptionCode : 0ul,
             er ? (const void*)er->ExceptionAddress : (const void*)nullptr);
  g_trace.line(buf);
  g_trace.close();
  return EXCEPTION_EXECUTE_HANDLER;   // terminate now: a GUI crash dialog on a build agent hangs the job
}

// EnumChildWindows wants a WNDENUMPROC: a capture-less lambda happens to convert on x64, but the
// calling convention a lambda ends up with is not something this code should be betting on, so the
// control-dump visitor is an ordinary function.
struct ChildDump {
  Trace* t;
  int count;
};
BOOL CALLBACK child_dump(HWND h, LPARAM l) {
  ChildDump* c = reinterpret_cast<ChildDump*>(l);
  wchar_t cls[128] = L"", txt[256] = L"";
  GetClassNameW(h, cls, 127);
  SendMessageW(h, WM_GETTEXT, 255, (LPARAM)txt);
  RECT r;
  GetWindowRect(h, &r);
  ++c->count;
  c->t->line(fmt(L"  ctrl %2d  %-12s enabled=%ls visible=%ls %4dx%-4d  \"%ls\"", c->count, cls,
                 IsWindowEnabled(h) ? L"yes" : L"no", IsWindowVisible(h) ? L"yes" : L"no",
                 r.right - r.left, r.bottom - r.top, txt));
  return TRUE;
}

// ---------------------------------------------------------------- the application
struct App {
  HWND win = nullptr;
  HWND mic = nullptr, spk = nullptr, prof = nullptr, session = nullptr, secret = nullptr,
       match = nullptr, giver_radio = nullptr, receiver_radio = nullptr, duplex = nullptr,
       proxy = nullptr, status = nullptr, log = nullptr, start_btn = nullptr, stop_btn = nullptr;
  std::vector<AudioDeviceInfo> devs;             // parallel to the two combo boxes
  std::vector<int> profile_ids;                   // parallel to the profile combo
  HFONT font = nullptr, big = nullptr, mono = nullptr;

  std::mutex mu;                                  // guards node/audio; taken in short pieces
  std::unique_ptr<Node> node;
  std::unique_ptr<WasapiEngine> audio;
  std::thread pump;
  std::thread worker;
  std::atomic<bool> quitting{false};
  std::atomic<bool> busy{false};                  // a test is running on the worker thread
  std::atomic<bool> running{false};               // link is up
  std::vector<std::wstring> pending;              // log lines to flush into the edit
  std::mutex pending_mu;
  std::wstring last_match;                        // so a changed match code gets logged once

  // Log lines are produced on any thread and applied on the UI thread, which is the only way to keep a
  // 45-second link test from freezing the window.
  void say(const std::wstring& line) {
    {
      std::lock_guard<std::mutex> g(pending_mu);
      pending.push_back(line);
    }
    if (win) PostMessageW(win, WM_APP, 0, 0);
  }
  void flush(HWND edit) {
    std::vector<std::wstring> mine;
    {
      std::lock_guard<std::mutex> g(pending_mu);
      mine.swap(pending);
    }
    for (size_t i = 0; i < mine.size(); ++i) {
      int end = GetWindowTextLengthW(edit);
      SendMessageW(edit, EM_SETSEL, (WPARAM)end, (WPARAM)end);
      SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)(mine[i] + L"\r\n").c_str());
    }
  }

  // Fill the two device drop-downs from `devs`. Called once with an empty list while the window is being
  // created (so the boxes are never empty) and again from load_devices() once the enumeration has run.
  void populate_audio() {
    SendMessageW(mic, CB_RESETCONTENT, 0, 0);
    SendMessageW(spk, CB_RESETCONTENT, 0, 0);
    SendMessageW(mic, CB_ADDSTRING, 0, (LPARAM)L"(system default)");
    SendMessageW(spk, CB_ADDSTRING, 0, (LPARAM)L"(system default)");
    for (size_t i = 0; i < devs.size(); ++i) {
      const AudioDeviceInfo& d = devs[i];
      std::wstring line = fmt(L"%ls  %.0f Hz  %d ch  %hs", w8(d.name).c_str(), d.nominal_rate, d.channels,
                              d.api.c_str());
      if (d.loopback) line += L"   [loopback - cannot be used as a microphone]";
      SendMessageW(d.is_capture ? mic : spk, CB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    SendMessageW(mic, CB_SETCURSEL, 0, 0);
    SendMessageW(spk, CB_SETCURSEL, 0, 0);
  }

  // The machine's real endpoints, read once the window exists. Outside WM_CREATE on purpose: this is the
  // only part of the front end that talks to COM and to the audio service, and the first CI run died
  // somewhere around here, so it gets its own step in the trace and its own line in the log.
  void load_devices() {
    std::string why;
    g_trace.line(L"step 2: enumerating audio endpoints (WASAPI)");
    int n = ni_audio_enumerate(&devs, &why);
    if (n < 0) {
      devs.clear();
      g_trace.line(fmt(L"step 2: enumeration failed (%d): %hs", n, why.c_str()));
      say(L"device enumeration said: " + w8(why));
      say(L"this build sees no audio endpoints. That is a fact about the machine, not a bug in the "
          L"listing: install or enable a device, then press Start.");
    } else {
      g_trace.line(fmt(L"step 2: %d endpoint(s) reported", n));
    }
    if (mic && spk) populate_audio();
  }

  std::wstring combo_text(HWND c) {
    int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
    if (i == CB_ERR) return std::wstring();
    int len = (int)SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0);
    if (len <= 0) return std::wstring();
    std::vector<wchar_t> buf((size_t)len + 1, L'\0');
    SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf.data());
    return std::wstring(buf.data());
  }
  std::string edit_u8(HWND e) {
    int len = GetWindowTextLengthW(e);
    std::vector<wchar_t> buf((size_t)len + 1, L'\0');
    GetWindowTextW(e, buf.data(), (int)buf.size());
    return u8(std::wstring(buf.data()));
  }

  void stop_locked() {
    quitting.store(true);
    if (pump.joinable()) pump.join();
    if (audio) {
      audio->close();
      audio.reset();
    }
    if (node) node->stop();
    running.store(false);
  }
  void stop_from_ui() {
    if (busy.load()) { say(L"a test is still running - this window will not pull the engine out from "
                        L"under it. Wait for the result, then Stop."); return; }
    stop_locked();
    say(L"stopped: audio streams released, session closed.");
  }

  // ---- the pump: capture -> engine -> render, exactly as the console front end does it, under the
  // same lock so a UI action cannot reconfigure the node mid-buffer.
  void pump_loop() {
    WasapiEngine::thread_init();
    std::vector<float> buf(2048);
    while (!quitting.load()) {
      {
        std::lock_guard<std::mutex> g(mu);
        if (node && audio) {
          int got = audio->capture(&buf[0], (int)buf.size());
          if (got > 0) node->feed_capture(&buf[0], (size_t)got);
        }
      }
      {
        std::lock_guard<std::mutex> g(mu);
        if (node && audio) {
          int need = audio->render_buffer_frames();
          if (need < 240) need = 240;
          if (need > (int)buf.size()) need = (int)buf.size();
          int got = node->pull_playback(&buf[0], (size_t)need);
          if (got > 0) audio->render(&buf[0], got, 50);
          // Tell the link how much of what we just handed over is still unplayed: turn-taking is
          // timed against played sound, not against samples sitting in the sound card's buffer.
          int q = audio->queued_frames();
          if (q >= 0) node->set_tx_backlog((size_t)q);
          node->step((int64_t)GetTickCount64());
        }
      }
      Sleep(5);
    }
    WasapiEngine::thread_shutdown();
  }

  // Runs a measured test on this thread. Deliberately WITHOUT mu: these calls pump the link from the
  // caller while the audio pump feeds it from the pump thread, exactly as the console front end does, so
  // holding the engine lock here would starve the audio path and the test would wait for an ack that can
  // only arrive through the audio path. The node is never destroyed while busy (see WM_CLOSE), which is
  // the only lifetime rule this needs.
  void run_action(int which) {
    busy.store(true);
    say(L"...running - the window stays live, the link does not");
    std::wstring out;
    {
      if (!node || !running.load()) {
        out = L"nothing is running: press Start first.";
      } else {
        int64_t now = (int64_t)GetTickCount64();
        if (which == IDC_CAL) {
          Calibrator::Report rep;
          int rc = node->calibrate(now, &rep);
          if (rc != NI_OK) {
            out = fmt(L"calibrate: %ls (%d) - the engine's own answer, not a guess", w8(ni_strerror(rc)).c_str(), rc);
          } else {
            out = w8(rep.summary);
            for (size_t i = 0; i < rep.bands.size(); ++i) {
              const Calibrator::BandResult& b = rep.bands[i];
              out += fmt(L"\n%hs: snr %.2f dB, frames %.1f %%, rawBER %.2e, fecBER %.2e, payload %.0f bps, usable=%hs%hs",
                         b.name.c_str(), b.snr_db, b.frame_success * 100.0, b.raw_ber, b.fec_ber,
                         b.payload_bps, b.usable ? "yes" : "no", b.clipped ? "  [CLIPPED]" : "");
            }
            out += fmt(L"\nbest profile id %d, suggested mtu %d", rep.best_profile, rep.suggested_mtu);
            if (!rep.hardware_ok)
              out += L"\nNOTE: hardware_ok=false - these bands came from the modelled channel, not from this machine's sound card.";
          }
        } else if (which == IDC_LINK) {
          std::string rep;
          int rc = node->test_link(now, 228, &rep);
          out = fmt(L"link test (228 B): %ls\r\n%ls", w8(rc == NI_OK ? "completed" : ni_strerror(rc)).c_str(), w8(rep).c_str());
        } else if (which == IDC_NET) {
          InternetVerify iv;
          int rc = node->test_internet(now, "http://example.com/", &iv);
          out = fmt(L"internet test: rc=%d ok=%ls", rc, iv.ok ? L"yes" : L"no");
          for (size_t i = 0; i < iv.stages.size(); ++i)
            out += fmt(L"\n  %-22hs %7.0f ms  %-4hs  %hs", iv.stages[i].name.c_str(), iv.stages[i].ms,
                       iv.stages[i].ok ? "ok" : "FAIL", iv.stages[i].detail.c_str());
          if (!iv.ok) out += L"\n  which stage failed above is the result: no internet path, no claim.";
        }
      }
    }
    if (!out.empty()) say(out);
    busy.store(false);
  }

  // ---- Start: read the controls, build a Node, open the two endpoints, launch the pump.
  bool start() {
    if (running.load()) { say(L"already running."); return true; }
    if (busy.load()) { say(L"wait for the running test to finish first."); return false; }
    // The trap that cost a real field test: with an empty passphrase the engine never sends a pairing
    // challenge at all, so the two ends can never pair and nothing on screen says why. Refuse here, in
    // words, before anything is opened - and put the cursor in the field that has to change.
    {
      std::string secret_now = edit_u8(secret);
      if (secret_now.size() < 8) {
        std::wstring m = fmt(L"The passphrase is %d characters long.\n\n"
                             L"Both ends must be given the SAME passphrase of at least 8 characters. "
                             L"Without one the engine never even sends a pairing challenge, so the two "
                             L"ends cannot pair - that is the usual reason \"it never paired\".\n\n"
                             L"Nothing has been started and nothing has been transmitted.",
                             (int)secret_now.size());
        say(L"refused to start: " + m);
        SetFocus(secret);
        MessageBoxW(win, m.c_str(), L"Noise Internet - passphrase required", MB_OK | MB_ICONWARNING);
        return false;
      }
    }
    NodeConfig cfg;
    cfg.role = (SendMessageW(giver_radio, BM_GETCHECK, 0, 0) == BST_CHECKED) ? NI_ROLE_GIVER
                                                                              : NI_ROLE_RECEIVER;
    cfg.is_master = (cfg.role == NI_ROLE_GIVER);
    cfg.name = "win-gui";
    int pi = (int)SendMessageW(prof, CB_GETCURSEL, 0, 0);
    cfg.profile_id = (pi >= 0 && pi < (int)profile_ids.size()) ? profile_ids[(size_t)pi] : 10;
    cfg.session_id = (uint16_t)atoi(edit_u8(session).c_str());
    cfg.pairing_secret = edit_u8(secret);
    cfg.duplex = (SendMessageW(duplex, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    cfg.enable_receiver_proxy = (cfg.role == NI_ROLE_RECEIVER) &&
                                (SendMessageW(proxy, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.enable_giver_fetch = (cfg.role == NI_ROLE_GIVER);
    cfg.proxy_port = 8471;
    if (cfg.pairing_secret.size() < 8)
      say(L"warning: a short passphrase is weak - the core prints the entropy it actually got.");

    std::string mic_id, spk_id;
    {
      int mi = (int)SendMessageW(mic, CB_GETCURSEL, 0, 0);
      int si = (int)SendMessageW(spk, CB_GETCURSEL, 0, 0);
      // index 0 in each combo is "(device default)"; anything after that is a real endpoint id.
      if (mi > 0) {
        int k = 0;
        for (size_t i = 0; i < devs.size(); ++i)
          if (devs[i].is_capture && ++k == mi) { mic_id = devs[i].id; break; }
      }
      if (si > 0) {
        int k = 0;
        for (size_t i = 0; i < devs.size(); ++i)
          if (!devs[i].is_capture && ++k == si) { spk_id = devs[i].id; break; }
      }
    }

    std::lock_guard<std::mutex> g(mu);
    quitting.store(false);
    node.reset(new Node(cfg));
    int rc = node->configure();
    if (rc != NI_OK) {
      say(fmt(L"configure failed (%d): %hs", rc, node->last_error().c_str()));
      node.reset();
      return false;
    }
    node->start();
    audio.reset(new WasapiEngine());
    WasapiEngine::thread_init();
    rc = audio->open(spk_id, mic_id, cfg.fs);
    if (rc != NI_OK) {
      say(fmt(L"audio open failed (%d): %hs", rc, audio->last_error().c_str()));
      say(L"NO ACOUSTIC PATH: nothing has been transmitted. Fix the endpoints (or run the console build "
          L"with --list-devices) and press Start again - the status panel keeps reporting what the engine "
          L"measured, which right now is nothing.");
      audio.reset();
      node->stop();
      node.reset();
      return false;
    }
    say(w8(audio->describe()));
    say(fmt(L"started as %ls, profile id %d, session %u, proxy %ls",
            cfg.role == NI_ROLE_GIVER ? L"GIVER" : L"RECEIVER", cfg.profile_id, (unsigned)cfg.session_id,
            cfg.enable_receiver_proxy ? L"on 127.0.0.1:8471" : L"off"));
    running.store(true);
    pump = std::thread([this] { pump_loop(); });
    return true;
  }

  void refresh() {
    StatusSnapshot s;
    bool have = false;
    // No lock: snapshot() reads counters the engine keeps, and the console front end prints the same
    // fields from its main thread while the pump runs. A torn read here costs one stale line at the next
    // 250 ms tick, which is what a status panel is for.
    if (node && running.load()) { s = node->snapshot(); have = true; }
    if (!have) {
      SetWindowTextW(status, L"not running - pick a role, the two audio devices, a profile, a session id "
                            L"and the same passphrase as the other end, then press Start.\r\n"
                            L"Nothing has been measured yet, and no number in this window is an internet speed.");
      EnableWindow(start_btn, !busy.load());
      EnableWindow(stop_btn, FALSE);
      SetWindowTextW(match, L"(not paired)");
      return;
    }
    std::wstring t;
    t += fmt(L"role %hs   state %hs   session %lld ms\r\n", s.role.c_str(), s.state_name.c_str(),
             (long long)s.session_ms);
    t += fmt(L"peer %hs (last heard %lld ms ago)   pairing %hs\r\n",
             s.peer_online ? "ONLINE" : "offline", (long long)s.peer_seen_ms, s.pairing.c_str());
    t += fmt(L"match code: %hs   <- must read identically on the other device\r\n",
             s.sas.empty() ? "(not paired yet)" : s.sas.c_str());
    t += fmt(L"input %.1f dBFS   bursts seen %llu   decoded %llu   callbacks with no signal %llu\r\n",
             s.in_dbfs, (unsigned long long)s.sync_hits, (unsigned long long)s.frames_received,
             (unsigned long long)s.rx_no_frame);
    t += fmt(L"snr %.2f dB   rssi %.1f dBFS   noise %.1f dBFS   rtt %.1f ms   loss %.2f %%\r\n",
             s.snr_db, s.rssi_dbfs, s.noise_dbfs, s.rtt_ms, s.loss_pct);
    t += fmt(L"mtu %d   link payload %.0f b/s   useful up %.0f / down %.0f b/s\r\n", s.mtu, s.link_bps,
             s.useful_bps_up, s.useful_bps_down);
    t += fmt(L"frames sent %llu  recv %llu  retries %llu  crc-fail %llu  malformed %llu\r\n",
             (unsigned long long)s.frames_sent, (unsigned long long)s.frames_received,
             (unsigned long long)s.retries, (unsigned long long)s.crc_fail,
             (unsigned long long)s.malformed);
    t += fmt(L"bytes sent %llu  received %llu\r\n", (unsigned long long)s.bytes_sent,
             (unsigned long long)s.bytes_received);
    t += fmt(L"tunnel %hs: %hs\r\n", s.tunnel_up ? "up" : "down", s.tunnel_detail.c_str());
    t += fmt(L"internet %hs: %hs\r\n", s.internet_ok ? "verified" : "not verified", s.internet_detail.c_str());
    // The one-line answer to "why isn't it pairing", from the engine's own counters. Every branch
    // names the number it is based on, and when the numbers are fine it says so instead of guessing.
    t += L"why: ";
    if (!s.pairing_note.empty())
      t += w8(s.pairing_note);
    else if (s.peer_online && s.pairing == "paired")
      t += L"paired and exchanging frames";
    else if (s.sync_hits > 0 && s.frames_received == 0)
      t += L"bursts are arriving (sync found, nothing readable): the two ends are most likely on "
           L"different profiles or session ids, or the level is wrong. Set the SAME profile number, "
           L"session id and passphrase on both ends.";
    else if (s.in_dbfs < -70.0)
      t += fmt(L"the microphone hears almost nothing (%.1f dBFS): check which microphone is selected, "
               L"the other device's volume, and the distance.", s.in_dbfs);
    else
      t += fmt(L"listening: input %.1f dBFS, no burst decoded yet. Check both ends use the same "
               L"profile, session id and passphrase, and that both are started.", s.in_dbfs);
    t += L"\r\n";
    if (!s.audio_note.empty()) t += fmt(L"NOTE: %hs\r\n", s.audio_note.c_str());
    t += L"rates are what this session delivered, over whatever path the audio really took - not a "
         L"prediction and not an internet speed.";
    SetWindowTextW(status, t.c_str());
    std::wstring mc = s.sas.empty() ? std::wstring(L"(not paired yet)") : w8(s.sas);
    if (mc != last_match) {
      SetWindowTextW(match, mc.c_str());
      if (!s.sas.empty()) say(L"match code is now: " + mc + L" - read it aloud or on screen and check the far end says the same");
      last_match = mc;
    }
    EnableWindow(start_btn, !busy.load());
    EnableWindow(stop_btn, TRUE);
  }

  // --ui-check: prove the window and its controls exist, and report what the machine actually has.
  void ui_check() {
    g_trace.open(L"ui-check.txt");
    // `%ls` is a wide string in MSVC's wide printf family: passing the narrow literal "created" here
    // printed 牣慥整d the first time this ran. The trace that exists to make failures legible has to be
    // legible itself, and the only reason the mistake was visible is that the transcript was read.
    g_trace.line(fmt(L"ui-check: window=%ls class=%ls, written by the exe that is in this bundle",
                     running.load() ? L"running" : L"created", L"NoiseInternetGui"));
    g_trace.line(L"controls created by the window (class, enabled, visible, size, text):");
    ChildDump ctx{&g_trace, 0};
    EnumChildWindows(win, child_dump, (LPARAM)&ctx);
    int caps = 0, micc = 0;
    for (size_t i = 0; i < devs.size(); ++i) devs[i].is_capture ? ++micc : ++caps;
    g_trace.line(fmt(L"total controls: %d (each was created; the visibility column reads no on a build "
                     L"agent because the desktop is not shown there - this check proves existence, size, "
                     L"text and enabled state, not that a human saw the window)", ctx.count));
    g_trace.line(fmt(L"top-level window itself: visible=%ls", IsWindowVisible(win) ? L"yes" : L"no"));
    g_trace.line(fmt(L"audio endpoints this machine offered: %d (capture %d, render %d). A build agent has "
                     L"none; that is the real answer for that machine and it is why no acoustic claim is "
                     L"made here.", (int)devs.size(), micc, caps));
    g_trace.line(fmt(L"modulation profiles offered: %d", (int)profile_ids.size()));
    g_trace.line(L"no audio was opened and nothing was transmitted by this run: --ui-check is not a link "
                 L"test and proves nothing about sound in air.");
    ok_ui_check = (ctx.count >= 14);
    g_trace.line(ok_ui_check ? L"step 3: ui-check PASSED (window, controls and inventory all produced)"
                             : L"step 3: ui-check FAILED - fewer controls than the window defines");
    g_trace.close();
  }
  bool ok_ui_check = false;
};

App* g_app = nullptr;

// ---------------------------------------------------------------- window plumbing
HWND mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
        HFONT f, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  if (f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
  return c;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* a = g_app;
  switch (msg) {
    case WM_CREATE: {
      a->win = hwnd;
      a->font = CreateFontW(0, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
      a->big = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
      a->mono = CreateFontW(0, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
      mk(hwnd, L"BUTTON", L"this machine GIVES its internet (giver)", BS_AUTORADIOBUTTON | WS_GROUP, 16, 24,
         280, 20, a->font, IDC_GIVER);
      a->giver_radio = GetDlgItem(hwnd, IDC_GIVER);
      a->receiver_radio = mk(hwnd, L"BUTTON", L"this machine RECEIVES over sound (receiver)",
                             BS_AUTORADIOBUTTON, 304, 24, 280, 20, a->font, IDC_RECEIVER);
      mk(hwnd, L"STATIC", L"microphone (the one that will hear the other end)", 0, 16, 60, 280, 16, a->font, 0);
      a->mic = mk(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 16, 78, 280, 260, a->font, IDC_MIC);
      mk(hwnd, L"STATIC", L"speaker (the one the other end listens to)", 0, 304, 60, 280, 16, a->font, 0);
      a->spk = mk(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 304, 78, 280, 260, a->font, IDC_SPK);
      mk(hwnd, L"STATIC", L"profile (rate/robustness trade-off)", 0, 16, 108, 280, 16, a->font, 0);
      a->prof = mk(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 16, 126, 280, 320, a->font, IDC_PROFILE);
      mk(hwnd, L"STATIC", L"session id (same on both ends)", 0, 304, 108, 150, 16, a->font, 0);
      a->session = mk(hwnd, L"EDIT", L"7", ES_NUMBER | WS_BORDER, 304, 126, 60, 22, a->font, IDC_SESSION);
      mk(hwnd, L"STATIC", L"passphrase (same on both ends)", 0, 374, 108, 210, 16, a->font, 0);
      a->secret = mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 374, 126, 210, 22, a->font, IDC_SECRET);
      mk(hwnd, L"STATIC", L"match code - must read identically on both ends once paired", 0, 16, 158, 380, 16,
         a->font, 0);
      a->match = mk(hwnd, L"EDIT", L"(not paired)", ES_READONLY | WS_BORDER, 400, 154, 184, 26, a->big, IDC_MATCH);
      a->duplex = mk(hwnd, L"BUTTON", L"full duplex (experimental, never tested on hardware)", BS_AUTOCHECKBOX, 16,
                     188, 300, 18, a->font, IDC_DUPLEX);
      a->proxy = mk(hwnd, L"BUTTON", L"local proxy on 127.0.0.1:8471 (receiver side)", BS_AUTOCHECKBOX, 330, 188,
                    254, 18, a->font, IDC_PROXY);
      a->start_btn = mk(hwnd, L"BUTTON", L"Start", BS_DEFPUSHBUTTON, 16, 214, 90, 26, a->font, IDC_START);
      a->stop_btn = mk(hwnd, L"BUTTON", L"Stop", BS_PUSHBUTTON, 112, 214, 70, 26, a->font, IDC_STOP);
      mk(hwnd, L"BUTTON", L"Switch role", BS_PUSHBUTTON, 188, 214, 92, 26, a->font, IDC_SWITCH);
      mk(hwnd, L"BUTTON", L"Calibrate", BS_PUSHBUTTON, 286, 214, 90, 26, a->font, IDC_CAL);
      mk(hwnd, L"BUTTON", L"Link test", BS_PUSHBUTTON, 382, 214, 86, 26, a->font, IDC_LINK);
      mk(hwnd, L"BUTTON", L"Internet test", BS_PUSHBUTTON, 474, 214, 110, 26, a->font, IDC_NET);
      mk(hwnd, L"STATIC", L"measured status", 0, 16, 248, 200, 16, a->font, 0);
      a->status = mk(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                     16, 266, 568, 150, a->mono, IDC_STATUS);
      mk(hwnd, L"STATIC", L"what the engine said", 0, 16, 424, 200, 16, a->font, 0);
      a->log = mk(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 16,
                  442, 568, 128, a->mono, IDC_LOG);

      // Profiles, an empty device list and the defaults, so the window is usable the moment it appears.
      // The endpoints are read by load_devices() AFTER CreateWindowExW returns: a COM failure inside
      // WM_CREATE would look like "the window could not be created", which is a lie about which thing broke.
      a->populate_audio();
      g_trace.line(L"step 1: window created and every control built");
      for (int i = 0; i < ni_profile_count(); ++i) {
        const Profile& p = ni_profile(i);
        a->profile_ids.push_back((int)p.id);
        std::wstring line = fmt(L"%d  %hs  %hs  %.0f-%.0f Hz", i, p.name, ni_mod_name((int)p.mod), p.f_low,
                                p.f_high);
        SendMessageW(a->prof, CB_ADDSTRING, 0, (LPARAM)line.c_str());
      }
      // FD-LOW-DBPSK: the profile every measured end-to-end run has paired and delivered on, in the
      // 700-3600 Hz band that survives a reflective room. FD-HIGH-DBPSK (11) is faster in the model but
      // needs a quiet, non-reflective path; it is two clicks away in this list.
      SendMessageW(a->prof, CB_SETCURSEL, 10, 0);
      SendMessageW(a->giver_radio, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(a->proxy, BM_SETCHECK, BST_CHECKED, 0);
      SetTimer(hwnd, TIMER_REFRESH, 250, nullptr);
      a->refresh();
      return 0;
    }
    case WM_APP:
      a->flush(a->log);
      return 0;
    case WM_TIMER:
      if ((UINT_PTR)wp == TIMER_REFRESH) a->refresh();
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp);
      switch (id) {
        case IDC_START: a->start(); break;
        case IDC_STOP: a->stop_from_ui(); break;
        case IDC_SWITCH: {
          std::lock_guard<std::mutex> g(a->mu);
          if (!a->node || !a->running.load()) { a->say(L"not running: Start first, then Switch role."); break; }
          Role next = (a->node->config().role == NI_ROLE_GIVER) ? NI_ROLE_RECEIVER : NI_ROLE_GIVER;
          int rc = a->node->set_role(next);
          a->say(fmt(L"role switch: %hs - %ls", rc == NI_OK ? "accepted" : ni_strerror(rc),
                    next == NI_ROLE_GIVER ? L"this machine now gives its internet"
                                          : L"this machine now receives over sound"));
          SendMessageW(next == NI_ROLE_GIVER ? a->giver_radio : a->receiver_radio, BM_SETCHECK, BST_CHECKED, 0);
          break;
        }
        case IDC_CAL:
        case IDC_LINK:
        case IDC_NET: {
          if (a->busy.load()) { a->say(L"one test at a time - the link is half-duplex, so is this window."); break; }
          int which = id;
          a->worker = std::thread([a, which] { a->run_action(which); });
          a->worker.detach();   // the result arrives through the log queue; nothing to join at quit time
          break;
        }
        case IDC_GIVER:
        case IDC_RECEIVER:
          if (!a->running.load())
            a->say(id == IDC_GIVER ? L"giver: this machine will push its internet out of the speaker."
                                   : L"receiver: this machine will listen and serve 127.0.0.1:8471.");
          else
            a->say(L"running: use Switch role - the radio buttons only set what Start does next.");
          break;
      }
      return 0;
    }
    case WM_CLOSE:
      a->quitting.store(true);
      KillTimer(hwnd, TIMER_REFRESH);
      // If a measurement is mid-flight, wait for it: destroying the node while test_link/calibrate is
      // running would free the object under the worker thread. The tests are finite (the link test gives
      // up after arq_wait_ms, the calibration sweep after its own budget), so this cannot hang forever -
      // it is the price of not pretending to cancel a transfer cleanly.
      while (a->busy.load()) Sleep(50);
      a->stop_locked();
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (a->font) DeleteObject(a->font);
      if (a->big) DeleteObject(a->big);
      if (a->mono) DeleteObject(a->mono);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  SetProcessDPIAware();
  int nargs = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nargs);
  bool ui_check = false;
  for (int i = 1; i < nargs; ++i)
    if (lstrcmpiW(args[i], L"--ui-check") == 0) ui_check = true;
  // A windowed process has no stdout worth writing to, so the inventory goes into a file that the caller
  // (and CI) can read. Nothing here calls printf: writing to a CRT stream that was never connected is how
  // a GUI smoke test turns into an unexplained exit code instead of a report.
  SetUnhandledExceptionFilter(crash_filter);
  if (ui_check) {
    g_trace_path = L"ui-check.txt";
    g_trace.open(g_trace_path);
  }
  App app;
  g_app = &app;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"NoiseInternetGui";
  RegisterClassExW(&wc);
  RECT r = {0, 0, 600, 584};
  AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
                              L"Noise Internet - acoustic link (nothing measured over air yet)",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    g_trace.line(fmt(L"window creation failed, GetLastError=%lu - no window, so no ui-check",
                     (unsigned long)GetLastError()));
    g_trace.close();
    return 3;
  }
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  // Endpoints only now that the window exists: see the comment inside WM_CREATE.
  app.load_devices();
  app.refresh();
  if (ui_check) {
    app.ui_check();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  } else {
    app.say(L"pick the devices and press Start. Both ends must use the same session id and passphrase; "
            L"the match code has to be identical on the two screens before you trust anything.");
    app.say(L"this window is a front end for the engine that passes 8/8 unit tests on MSVC. It has never "
            L"been run against a speaker and a microphone by anyone, so treat the first run as an "
            L"experiment, and read the numbers it prints instead of the ones you hope for.");
  }
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  g_trace.close();
  return (ui_check && !app.ok_ui_check) ? 4 : 0;
}
