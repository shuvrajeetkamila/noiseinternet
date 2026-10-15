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
#include "ni/audio_io.h"   // ni_wav_write_f32: the record button keeps what the microphone gave
#include "ni/audio_wasapi.h"
#include "ni/common.h"
#include "ni/node.h"
#include "ni/capture_diagnosis.h"

using namespace ni;

namespace {

enum {
  IDC_GIVER = 1000, IDC_RECEIVER, IDC_MIC, IDC_SPK, IDC_PROFILE, IDC_SESSION, IDC_SECRET,
  IDC_MATCH, IDC_DUPLEX, IDC_PROXY, IDC_START, IDC_STOP, IDC_SWITCH, IDC_CAL, IDC_LINK, IDC_NET,
  IDC_REC, IDC_COPY_STATUS, IDC_COPY_LOG, IDC_TOKEN, IDC_SHOW_TOKEN,
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
// Copy what a result pane says, because the first thing anyone does with a measurement is paste it
// somewhere - a bug report, an email, this conversation. Returns the number of characters copied, or -1.
int copy_text_to_clipboard(HWND owner, const std::wstring& text) {
  if (text.empty()) return -1;
  if (!OpenClipboard(owner)) return -1;
  EmptyClipboard();
  int copied = -1;
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (h) {
    void* dst = GlobalLock(h);
    if (dst) {
      memcpy(dst, text.c_str(), bytes);
      GlobalUnlock(h);
      if (SetClipboardData(CF_UNICODETEXT, h)) copied = (int)text.size();
      else GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
  return copied;
}

std::wstring edit_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring out((size_t)(n > 0 ? n : 0) + 1, L'\0');
  int got = GetWindowTextW(h, &out[0], (int)out.size());
  out.resize((size_t)(got > 0 ? got : 0));
  return out;
}

// The layout audit's visitor. Like child_dump below, it is an ordinary function because the calling
// convention a lambda gets in an EnumChildWindows callback is not something this code bets on.
struct LayoutItem {
  std::wstring what;
  RECT rc;
};
struct LayoutDump {
  HWND win;
  std::vector<LayoutItem>* out;
};
BOOL CALLBACK layout_dump(HWND h, LPARAM l) {
  LayoutDump* c = reinterpret_cast<LayoutDump*>(l);
  RECT r;
  GetWindowRect(h, &r);
  // window rects are in screen coordinates; the client-area test only makes sense in the window's own
  MapWindowPoints(HWND_DESKTOP, c->win, (LPPOINT)&r, 2);
  wchar_t cls[64] = L"", txt[128] = L"";
  GetClassNameW(h, cls, 63);
  SendMessageW(h, WM_GETTEXT, 127, (LPARAM)txt);
  if (wcscmp(cls, L"ComboBox") == 0) {
    // a combo box's window rect includes its drop-down list, which by design covers half the form
    int item = (int)SendMessageW(h, CB_GETITEMHEIGHT, 0, 0);
    if (item > 0) r.bottom = r.top + item + 8;   // closed height
  }
  LayoutItem it;
  it.what = std::wstring(cls) + L" \"" + txt + L"\"";
  it.rc = r;
  c->out->push_back(it);
  return TRUE;
}

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
  HWND token = nullptr, token_show = nullptr;     // receiver-side proxy token (defect 27)
  unsigned cfg_proxy_port_ = 8471;                // the port this session's proxy was started on

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
  // Write the captured audio somewhere the user can actually find it, and report the platform's own
  // reason when that fails. The old code called CreateFileW("ni-last-capture.wav") in the current working
  // directory and printed "(could not be written)" with no cause and no path - which is why the field
  // report could not say whether a recording existed (defect B). Three folders are tried in order of
  // usefulness: next to the program, then Documents\NoiseInternet (created if needed), then %TEMP%.
  // Returns the full path on success, "" on failure with the reason in *err.
  std::wstring save_capture_wav(const std::vector<float>& cap, int fs, std::wstring* err) {
    if (cap.empty() || fs <= 0) {
      if (err) *err = L"there were no samples to write";
      return std::wstring();
    }
    std::vector<std::wstring> dirs;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      std::wstring p(exe);
      size_t slash = p.find_last_of(L"\\/");
      if (slash != std::wstring::npos) dirs.push_back(p.substr(0, slash));
    }
    std::wstring docs;
    const wchar_t* up = _wgetenv(L"USERPROFILE");
    if (up && *up) docs = std::wstring(up) + L"\\Documents\\NoiseInternet";
    if (!docs.empty()) dirs.push_back(docs);
    const wchar_t* tp = _wgetenv(L"TEMP");
    if (tp && *tp) dirs.push_back(tp);
    std::wstring tried;
    for (size_t i = 0; i < dirs.size(); ++i) {
      if (i == 1) CreateDirectoryW(dirs[i].c_str(), nullptr);   // Documents\NoiseInternet may not exist
      if (GetFileAttributesW(dirs[i].c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      std::wstring full = dirs[i] + L"\\ni-last-capture.wav";
      int rc = ni_wav_write_f32(u8(full), &cap[0], cap.size(), (double)fs, 16);
      if (rc == NI_OK) return full;
      if (!tried.empty()) tried += L"; ";
      tried += fmt(L"%ls (%d: %ls)", full.c_str(), rc, w8(ni_strerror(rc)).c_str());
    }
    if (err)
      *err = tried.empty()
                 ? L"no folder was available to write into (no program folder, no Documents, no TEMP)"
                 : tried;
    return std::wstring();
  }

  void run_action(int which) {
    busy.store(true);
    say(L"...running - the window stays live, the link does not");
    std::wstring out;
    {
      if (!node || !running.load()) {
        out = L"nothing is running: press Start first.";
      } else {
        int64_t now = (int64_t)GetTickCount64();
        if (which == IDC_REC) {
          // Eight seconds of exactly what the engine received from the microphone, analysed with the same
          // code the phone runs, and written to a WAV the user can keep - because "what did it hear" is
          // not answerable from a level meter.
          //
          // This used to call audio->capture() from this thread while the pump thread was already reading
          // the same endpoint, so the two readers split the stream between them: eight seconds of wall
          // clock produced 107904 samples (2.25 s) and 163968 (3.4 s) in the two 2026-10-15 field runs,
          // and the link was starved for as long as the button was held (defect C). The engine's own
          // receive tap records every sample the engine was given, at full length, without touching the
          // audio threads at all.
          int fs_now = 0;
          {
            std::lock_guard<std::mutex> g(mu);
            if (!audio || !running.load()) {
              say(L"press Start first: the recorder is the same engine the link uses, and it has to be "
                  L"running for the tap to see anything.");
              busy.store(false);
              return;
            }
            fs_now = (int)audio->sample_rate();
          }
          const size_t want = (size_t)((fs_now > 0 ? fs_now : 48000) * 8);
          node->rx_tap_begin();
          const int64_t t0 = (int64_t)GetTickCount64();
          const int64_t until = t0 + 8000;
          while ((int64_t)GetTickCount64() < until) {
            if (quitting.load()) break;
            Sleep(20);   // the pump thread does the reading; this thread only waits
          }
          std::vector<float> cap = node->rx_tap_end();
          const double seconds = (double)cap.size() / (double)(fs_now > 0 ? fs_now : 48000);
          int pid = 10;
          {
            std::lock_guard<std::mutex> g(mu);
            int pi = (int)SendMessageW(prof, CB_GETCURSEL, 0, 0);
            if (pi >= 0 && pi < (int)profile_ids.size()) pid = profile_ids[(size_t)pi];
          }
          CaptureDiagnosis d;
          if (!cap.empty() && ni_diagnose_capture(&cap[0], cap.size(), (double)fs_now, pid, &d) == NI_OK) {
            std::wstring err;
            std::wstring where = save_capture_wav(cap, fs_now, &err);
            out = w8(d.verdict);
            out += fmt(L"\r\n\r\npeak %.1f dBFS   room %.1f dBFS   loudest %.1f dBFS   clipped %.2f %%"
                       L"\r\nband %.0f-%.0f Hz   shift %.0f ppm   modem %ls",
                       d.peak_dbfs, d.noise_dbfs, d.burst_dbfs, d.clip_pct, d.band_low_hz,
                       d.band_high_hz, d.shift_ppm,
                       d.decodes ? L"DECODES this recording" : L"cannot read a burst in it");
            // Keep the honest count in view: the button asked for eight seconds and the answer says how
            // many samples of it the microphone actually delivered (defect C). A run that keeps 2.25 s of
            // 8 s is a finding about the audio path, not a rounding detail.
            out += fmt(L"\r\nkept %zu of %zu samples in %.2f s of wall clock (%d Hz)",
                       cap.size(), want, seconds, fs_now);
            if (!where.empty())
              out += fmt(L"\r\nsaved as %ls", where.c_str());
            else
              out += fmt(L"\r\nNOT SAVED - no usable file could be written: %ls", err.c_str());
          } else if (cap.empty()) {
            out = L"nothing was recorded: the microphone handed the engine no samples at all in eight "
                  L"seconds, which is itself the answer (see the audio line in the status panel).";
          } else {
            out = fmt(L"the engine kept %zu samples in eight seconds but the analyser refused them (%d "
                      L"Hz); see the audio line in the status panel.", cap.size(), fs_now);
          }
        } else if (which == IDC_CAL) {
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
            out += L"\n(model suggestion - this machine's real choice comes from what it decodes on air)";
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
    const bool proxy_ticked = SendMessageW(proxy, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.enable_receiver_proxy = (cfg.role == NI_ROLE_RECEIVER) && proxy_ticked;
    // Named once so the token check below can ask whether a token is used at all (defect A).
    const bool wants_proxy = cfg.enable_receiver_proxy;
    cfg.enable_giver_fetch = (cfg.role == NI_ROLE_GIVER);
    cfg.proxy_port = 8471;
    cfg_proxy_port_ = (unsigned)cfg.proxy_port;
    // Defect 27: the window could show the generated token but never let the user choose one, so the
    // documented receiver workflow ("point your browser at 127.0.0.1:8471 with this token") could not
    // be set up from the GUI at all. Now the field is read here, checked before anything is opened, and
    // handed to the same NodeConfig the console's --proxy-token feeds.
    {
      std::string tok = edit_u8(token);
      // Only a receiver gateway uses a token at all: it is the credential that lets a browser on this
      // machine talk to the proxy. On a giver the field is unused, and refusing to start because it holds
      // four characters ended a real field run with "The proxy token is 4 characters long" on a machine
      // that was supplying the internet, not proxying (defect A).
      const bool token_matters = wants_proxy;
      if (token_matters && !tok.empty() && tok.size() < 8) {
        std::wstring m = fmt(L"The proxy token is %d characters long, and this window is started as a "
                             L"RECEIVER with the proxy ticked - so a token is used. Either leave the "
                             L"field empty (the engine then generates a 16-character token and the status "
                             L"panel shows it) or give at least 8 characters.\n\nA giver never uses this "
                             L"field and is not checked against it.\n\nNothing has been started.",
                             (int)tok.size());
        say(L"refused to start: " + m);
        SetFocus(token);
        MessageBoxW(win, m.c_str(), L"Noise Internet - proxy token too short", MB_OK | MB_ICONWARNING);
        return false;
      }
      cfg.proxy_token = tok;
      if (!tok.empty()) say(L"proxy token: the one typed in the field will be used (not logged here).");
    }
    // Assist: on for the window, because this is the build a person runs in a room with no meter and no
    // protocol knowledge. It sweeps our own output level while unpaired and lets a receiver beacon, so
    // that "nothing happens and nothing says why" cannot be the user's whole experience.
    cfg.assist = true;
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
    // The one question a user with a pair that will not work cannot answer alone, and the one that needs
    // no second device: does this machine hear its own speaker? Answered automatically, in words, eight
    // seconds after Start, because the answer is a sentence and not a number - and because a person who
    // has just pressed Start is not going to read bursts-seen and dBFS and work it out (defect 54).
    std::thread([this] { self_hear_after_start(); }).detach();
    return true;
  }

  // Eight seconds of the link's own counters. What it can see: the machine transmits continuously
  // (keepalives as giver, the assist beacon as receiver), so anything the microphone decodes in those
  // seconds came out of this machine's own speaker.
  void self_hear_after_start() {
    SelfHear before;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!node) return;
      before = node->self_hear_now();
    }
    // Sliced, and every slice checks quitting: a detached eight-second thread that wakes up after the
    // window is gone would read a destroyed App.
    for (int i = 0; i < 80 && !quitting.load(); ++i) Sleep(100);
    if (quitting.load()) return;
    SelfHear now;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!node || !running.load() || quitting.load()) return;
      now = node->self_hear_since(before, 8);
    }
    say(L"hear-myself: " + w8(now.verdict));
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
    // The engine's own sentence first, because it is the only line that tells the user what to do.
    if (!s.advice.empty()) t += w8(s.advice) + L"\r\n\r\n";
    t += fmt(L"input %.1f dBFS   bursts seen %llu   decoded %llu   callbacks with no signal %llu\r\n",
             s.in_dbfs, (unsigned long long)s.sync_hits, (unsigned long long)s.frames_received,
             (unsigned long long)s.rx_no_frame);
    // Its own voice, counted separately (defect 59) and with the repeat decodes named (defect H): "decoded
    // 48" with no peer anywhere used to read as a peer that answers and delivers nothing.
    if (s.self_echo_bursts)
      t += fmt(L"self-echo: %llu own burst(s) heard back through this machine's own microphone%s\r\n",
               (unsigned long long)s.self_echo_bursts,
               s.self_echo_repeats
                   ? fmt(L", plus %llu repeat decodes of the same burst", (unsigned long long)s.self_echo_repeats)
                         .c_str()
                   : L"");
    // A number the engine has never measured must not be printed as one: -120 dBFS and 100.00 % loss are
    // the snapshot's defaults, and they read as "dead microphone, everything lost" rather than "nothing
    // has been decoded yet" (defect F). The flags travel with the snapshot; both front ends obey them.
    if (s.phy_measured)
      t += fmt(L"snr %.2f dB   rssi %.1f dBFS   noise %.1f dBFS   evm %.2f %%   erle %.1f dB\r\n",
               s.snr_db, s.rssi_dbfs, s.noise_dbfs, s.evec_pct, s.erle_db);
    else
      t += L"snr / rssi / noise / evm: NOT MEASURED YET - no burst has been demodulated on this machine, "
           L"so these are the engine's default values and not readings\r\n";
    if (s.arq_measured)
      t += fmt(L"rtt %.1f ms   loss %.2f %%   retransmit rate %.1f %%\r\n", s.rtt_ms, s.loss_pct,
               s.frames_sent ? 100.0 * (double)s.retries / (double)s.frames_sent : 0.0);
    else
      t += L"rtt / loss: NOT MEASURED YET - nothing has been transmitted yet, so there is nothing whose "
           L"round trip or loss could be reported\r\n";
    t += fmt(L"mtu %d   link payload %.0f b/s   useful up %.0f / down %.0f b/s\r\n", s.mtu, s.link_bps,
             s.useful_bps_up, s.useful_bps_down);
    t += fmt(L"frames sent %llu  recv %llu  retries %llu  crc-fail %llu  malformed %llu\r\n",
             (unsigned long long)s.frames_sent, (unsigned long long)s.frames_received,
             (unsigned long long)s.retries, (unsigned long long)s.crc_fail,
             (unsigned long long)s.malformed);
    t += fmt(L"bytes sent %llu  received %llu\r\n", (unsigned long long)s.bytes_sent,
             (unsigned long long)s.bytes_received);
    t += fmt(L"output level        : %.2f of full scale%s\r\n", s.tx_gain,
             s.advice.empty() ? L"" : L"  (swept while unpaired)");
    t += fmt(L"tunnel %hs: %hs\r\n", s.tunnel_up ? "up" : "down", s.tunnel_detail.c_str());
    // The proxy refuses every client that does not present its bearer token, and the session's token
    // only exists once the node is running - so the window has to show it, or nobody could point a
    // browser at this proxy. (The console build can set one with --proxy-token; the window generated it
    // and, until this line, never told anyone what it was. Defect 27 for the Android side.)
    if (node && s.tunnel_up && !node->proxy_token().empty()) {
      // Masked unless the user asks to see it. A bearer token in a pane that gets photographed and
      // pasted is a token that leaks; a token the user cannot read is a proxy nobody can use. Hence the
      // checkbox, and never the log box (which is the pane people paste).
      bool show = SendMessageW(token_show, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (show)
        t += fmt(L"proxy 127.0.0.1:%u  token %hs\r\n", (unsigned)cfg_proxy_port_,
                 node->proxy_token().c_str());
      else
        t += fmt(L"proxy 127.0.0.1:%u  token = hidden (tick \"show\" to read it)   [%d characters]\r\n",
                 (unsigned)cfg_proxy_port_, (int)node->proxy_token().size());
      t += L"  anything that wants to use this proxy needs that token "
           L"(Proxy-Authorization: Noise <token>).\r\n";
    }
    // The device's own voice, counted by the engine as such instead of as a peer's frames. A lone window
    // reporting "recv 48" without this number reads like a peer that answers and delivers nothing; with
    // it, it reads like a device listening to itself, which is what it was (defect 59).
    t += fmt(L"self-echo bursts    : %llu  (of the frames above, this many were this device hearing "
             L"itself - not a peer)\r\n",
             (unsigned long long)s.self_echo_bursts);
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
    else if (s.in_dbfs <= -119.5)
      t += L"no audio has reached the engine at all (the level has never moved off -120 dBFS): this is "
           L"the capture path failing, not a quiet room. Open the audio line below - it names the "
           L"format and the device. A microphone that another program holds, or is disabled in "
           L"Windows privacy settings, reads exactly like this.";
    else if (s.in_dbfs < -70.0)
      t += fmt(L"the microphone hears almost nothing (%.1f dBFS): check which microphone is selected, "
               L"the other device's volume, and the distance.", s.in_dbfs);
    else
      t += fmt(L"listening: input %.1f dBFS, no burst decoded yet. Check both ends use the same "
               L"profile, session id and passphrase, and that both are started.", s.in_dbfs);
    t += L"\r\n";
    if (!s.audio_note.empty()) t += fmt(L"NOTE: %hs\r\n", s.audio_note.c_str());
    {
      // Which endpoints are open, in this pane rather than only in the log: it is the line that answers
      // "is it using the microphone I think it is" (defect I).
      std::string ad;
      {
        std::lock_guard<std::mutex> g(mu);
        if (audio) ad = audio->describe();
      }
      if (!ad.empty()) t += fmt(L"audio: %hs\r\n", ad.c_str());
    }
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
    // Layout audit. Counting controls proved they were created; it could not see that the record button
    // was drawn 8 px into the "measured status" label, because a control that is created but covered is
    // still a created control. Two things are checked for every child: it lies inside the client area,
    // and no two of them overlap. Combo boxes are measured at their closed height (their window rect
    // includes the drop-down list, which would otherwise "overlap" half the form by design).
    {
      RECT client;
      GetClientRect(win, &client);
      std::vector<LayoutItem> rs;
      LayoutDump collect{win, &rs};
      EnumChildWindows(win, layout_dump, (LPARAM)&collect);
      int outside = 0, overlaps = 0;
      for (size_t i = 0; i < rs.size(); ++i) {
        const RECT& a = rs[i].rc;
        if (a.left < client.left || a.top < client.top || a.right > client.right ||
            a.bottom > client.bottom) {
          ++outside;
          g_trace.line(fmt(L"layout: OUTSIDE the client area: %ls at %d,%d %dx%d", rs[i].what.c_str(),
                           (int)a.left, (int)a.top, (int)(a.right - a.left), (int)(a.bottom - a.top)));
        }
      }
      for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j) {
          RECT x;
          if (IntersectRect(&x, &rs[i].rc, &rs[j].rc)) {
            ++overlaps;
            g_trace.line(fmt(L"layout: OVERLAP between %ls and %ls (%dx%d at %d,%d)",
                             rs[i].what.c_str(), rs[j].what.c_str(), (int)(x.right - x.left),
                             (int)(x.bottom - x.top), (int)x.left, (int)x.top));
          }
        }
      g_trace.line(fmt(L"layout audit: %d controls, %d outside the client area, %d overlapping pairs",
                       (int)rs.size(), outside, overlaps));
      layout_ok = (outside == 0 && overlaps == 0);
    }
    ok_ui_check = (ctx.count >= 14) && layout_ok;
    g_trace.line(ok_ui_check
                     ? L"step 3: ui-check PASSED (window, controls, layout audit and inventory all good)"
                     : L"step 3: ui-check FAILED - fewer controls than the window defines, or a layout "
                       L"problem (see the layout lines above; the packaging step refuses to publish when "
                       L"this fails)");
    g_trace.close();
  }
  bool ok_ui_check = false;
  bool layout_ok = true;   // set by the audit in ui_check(): no control outside the client area, no overlap
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
      // "session id", not "session id (same on both ends)": that label was 150 px wide from x=304 and ran
      // into the passphrase label at x=374. The rectangles overlapped and the text sat on top of the
      // other label's text. The "(same on both ends)" instruction stays on the passphrase, where there is
      // room for it, and the window's own help line already says both ends must match. Found by the
      // --ui-check layout audit, which the packaging step now refuses to publish past.
      mk(hwnd, L"STATIC", L"session id", 0, 304, 108, 64, 16, a->font, 0);
      a->session = mk(hwnd, L"EDIT", L"7", ES_NUMBER | WS_BORDER, 304, 126, 60, 22, a->font, IDC_SESSION);
      mk(hwnd, L"STATIC", L"passphrase (same on both ends)", 0, 374, 108, 210, 16, a->font, 0);
      // ES_PASSWORD, at last: the pairing passphrase is a shared secret and this field used to print it
      // in clear text on a screen that gets photographed. It is masked by default and the user can reveal
      // it deliberately with the "show" box below (the same box also reveals the proxy token).
      a->secret = mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER, 374, 126, 210, 22,
                     a->font, IDC_SECRET);
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
      // The one button that answers "what is my microphone actually recording?" - the question that
      // three field reports could not answer, and the only one that needs no second device.
      // Row 7. The record button used to be drawn at y=240 with the "measured status" label at y=248 on
      // top of it - a real overlap, invisible to a check that only counted controls, and exactly what
      // --ui-check now refuses to let through (see the layout audit at the end of ui_check()).
      mk(hwnd, L"BUTTON", L"Record 8 s and analyse what the microphone hears", BS_PUSHBUTTON, 16, 244,
         380, 24, a->font, IDC_REC);
      mk(hwnd, L"BUTTON", L"Copy status", BS_PUSHBUTTON, 400, 244, 92, 24, a->font, IDC_COPY_STATUS);
      mk(hwnd, L"BUTTON", L"Copy log", BS_PUSHBUTTON, 496, 244, 88, 24, a->font, IDC_COPY_LOG);
      // Row 8: the receiver's proxy token. Blank means the engine generates one and shows it when
      // "show" is ticked; typing one means the user decides it. Masked unless asked (defect 27).
      mk(hwnd, L"STATIC", L"proxy token (receiver side; blank = the engine generates one)", 0, 16, 278, 260,
         16, a->font, 0);
      a->token = mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER, 280, 274, 224, 22,
                    a->font, IDC_TOKEN);
      a->token_show = mk(hwnd, L"BUTTON", L"show secrets", BS_AUTOCHECKBOX, 508, 276, 92, 18, a->font,
                         IDC_SHOW_TOKEN);
      mk(hwnd, L"STATIC", L"measured status", 0, 16, 306, 200, 16, a->font, 0);
      a->status = mk(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                     16, 324, 568, 150, a->mono, IDC_STATUS);
      mk(hwnd, L"STATIC", L"what the engine said", 0, 16, 482, 200, 16, a->font, 0);
      a->log = mk(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 16,
                  500, 568, 128, a->mono, IDC_LOG);

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
        case IDC_COPY_STATUS: {
          std::wstring text = edit_text(a->status);
          int n = copy_text_to_clipboard(a->win, text);
          // One line in the log, never the pane's contents: the log is what the user pastes, and the
          // status pane can contain a proxy token.
          a->say(n >= 0 ? fmt(L"copied the status panel (%d characters) to the clipboard", n)
                        : L"nothing to copy yet - the status panel is empty.");
          break;
        }
        case IDC_COPY_LOG: {
          std::wstring text = edit_text(a->log);
          int n = copy_text_to_clipboard(a->win, text);
          a->say(n >= 0 ? fmt(L"copied the engine log (%d characters) to the clipboard", n)
                        : L"nothing to copy yet - the engine log is empty.");
          break;
        }
        case IDC_SHOW_TOKEN: {
          // Both secrets, not just the token. The passphrase has to be read and compared on the two
          // screens ("both ends must use the same passphrase"), so a box that reveals one and hides the
          // other is no use - and both stay masked until it is ticked, because a photograph of this
          // window should not carry either of them.
          bool show = SendMessageW(a->token_show, BM_GETCHECK, 0, 0) == BST_CHECKED;
          const WPARAM mask = show ? 0 : (WPARAM)L'\u25CF';
          SendMessageW(a->token, EM_SETPASSWORDCHAR, mask, 0);
          SendMessageW(a->secret, EM_SETPASSWORDCHAR, mask, 0);
          InvalidateRect(a->token, nullptr, TRUE);    // the change is not repainted without this
          InvalidateRect(a->secret, nullptr, TRUE);
          a->say(show ? L"secrets visible: the passphrase field is unmasked and the status panel will "
                        L"print the proxy token while this is ticked"
                      : L"secrets hidden again");
          break;
        }
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
        case IDC_REC:
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
  RECT r = {0, 0, 620, 660};   // the client area the controls above are laid out for
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
    // "passes 8/8 unit tests" lived here and went stale the moment the ninth test was added: the number
    // is in the build log, not in a sentence inside the program (defect G).
    app.say(L"this window is a front end for the engine whose unit tests and loopback link test run in CI "
            L"on every push; the count is in that log, never in this sentence. Two machines have not yet "
            L"completed an end-to-end transfer over the air, so treat the first run as an experiment and "
            L"read the numbers it prints instead of the ones you hope for.");
  }
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  g_trace.close();
  return (ui_check && !app.ok_ui_check) ? 4 : 0;
}
