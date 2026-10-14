// NoiseInternet.exe — Windows giver/receiver front end for the shared acoustic core.
//
// One process, two roles, switchable at runtime: the giver runs the local proxy that fetches from
// the internet and pushes bytes over sound; the receiver pulls bytes out of sound and serves its own
// 127.0.0.1 proxy. Both roles are driven through ni::Node, the same class the CLI and the simulator
// use, and all audio goes through WasapiEngine. Nothing here pretends: if there is no capture
// endpoint, the status line says "NOT TESTED" and the transfer commands return the engine's error.
//
// Compiled and unit-tested by CI (MSVC, 8/8 ctest suites) - see windows/BUILD.md. It has still never
// been run against a sound card: no build host here has one. The console layout mirrors the field list
// the Android UI shows, so a physical test can be read off either machine.
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "ni/abi.h"
#include "ni/audio_io.h"
#include "ni/audio_wasapi.h"
#include "ni/node.h"
#include "ni/capture_diagnosis.h"

using namespace ni;

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_status_dirty{true};

struct App {
  NodeConfig cfg;
  std::string mic_id, spk_id;   // WASAPI endpoint ids from --list-devices
  Node* node = nullptr;
  WasapiEngine audio;
  bool using_wav = false;      // --wav-in/--wav-out development mode: file transport, NOT acoustic
  bool allow_unpaired = false; // --allow-unpaired: run TX/RX without a passphrase, explicitly
  std::string wav_in, wav_out;
  std::thread pump, reader;
};

// The one status screen. Every number on it comes from Node::snapshot(), which is computed from
// counters the engine actually maintains; fields with no measurement are printed as "-".
void print_status(App& app) {
  StatusSnapshot s = app.node->snapshot();
  printf("\n=== NOISE INTERNET (%s) =========================================\n", s.name.c_str());
  printf("role              : %s%s\n", s.role.c_str(),
         app.using_wav ? "   [FILE MODE - not an acoustic link]" : "");
  printf("state             : %s\n", s.state_name.c_str());
  printf("profile           : %s (id %d, %s)\n", s.profile_name.c_str(), s.profile_id,
         s.modulation.c_str());
  printf("sample rate       : %.0f Hz\n", s.sample_rate);
#if defined(_DLL)
  printf("c runtime         : dynamic (/MD) - vcruntime140.dll, vcruntime140_1.dll and msvcp140.dll\n"
         "                    must sit next to this exe; see redist/ in the downloaded zip\n");
#else
  printf("c runtime         : static (/MT) - standalone exe, no CRT DLLs needed\n");
#endif
  printf("audio devices     : mic=%s  speaker=%s\n", s.input_device.c_str(), s.output_device.c_str());
  printf("peer              : %s (last heard %lld ms ago)   pairing %s\n",
         s.peer_online ? "ONLINE" : "offline", (long long)s.peer_seen_ms, s.pairing.c_str());
  printf("match code        : %s   <- must read identically on the other device\n",
         s.sas.empty() ? "(not paired yet)" : s.sas.c_str());
  printf("input level       : %.1f dBFS   bursts seen %llu   decoded %llu   empty callbacks %llu\n",
         s.in_dbfs, (unsigned long long)s.sync_hits, (unsigned long long)s.frames_received,
         (unsigned long long)s.rx_no_frame);
  if (!s.pairing_note.empty()) printf("PAIRING NOTE      : %s\n", s.pairing_note.c_str());
  printf("measured SNR      : %.2f dB   RSSI %.1f dBFS   noise %.1f dBFS\n", s.snr_db, s.rssi_dbfs,
         s.noise_dbfs);
  printf("EVM               : %.2f %%\n", s.evec_pct);
  printf("RTT               : %.1f ms     frame loss %.2f %%\n", s.rtt_ms, s.loss_pct);
  printf("link payload rate : %.0f b/s\n", s.link_bps);
  printf("useful rate       : up %.0f b/s   down %.0f b/s\n", s.useful_bps_up, s.useful_bps_down);
  printf("frames            : sent %llu  recv %llu  retries %llu  crc-fail %llu  malformed %llu\n",
         (unsigned long long)s.frames_sent, (unsigned long long)s.frames_received,
         (unsigned long long)s.retries, (unsigned long long)s.crc_fail,
         (unsigned long long)s.malformed);
  printf("bytes             : sent %llu  received %llu      mtu %d\n",
         (unsigned long long)s.bytes_sent, (unsigned long long)s.bytes_received, s.mtu);
  printf("echo cancellation : ERLE %.2f dB (FDD experiment only; never tested on hardware)\n",
         s.erle_db);
  printf("tunnel            : %s  %s\n", s.tunnel_up ? "up" : "down", s.tunnel_detail.c_str());
  printf("internet          : %s  %s\n", s.internet_ok ? "verified" : "not verified",
         s.internet_detail.c_str());
  if (!s.advice.empty()) printf("WHAT TO DO        : %s\n", s.advice.c_str());
  if (!s.audio_note.empty()) printf("NOTE              : %s\n", s.audio_note.c_str());
  printf("=================================================================\n");
  fflush(stdout);
}

// Runs a command and prints whatever the engine measured. Nothing is inferred here: a negative
// return is the engine's NI_ERR_* and is printed with ni_strerror(), because "calibrate failed" is
// itself a result the user needs to see.
void run_command(App& app, const std::string& cmd) {
  Node& node = *app.node;
  int64_t now = (int64_t)GetTickCount64();
  if (cmd == "a") {
    Calibrator::Report rep;
    int rc = node.calibrate(now, &rep);
    if (rc != NI_OK) {
      printf("calibrate: %s (%d)\n", ni_strerror(rc), rc);
      return;
    }
    printf("calibrate report\n%s\n", rep.summary.c_str());
    printf("%-16s %7s %6s %9s %9s %8s  %s\n", "profile", "snr_dB", "ok%", "rawBER", "fecBER",
           "bps", "usable");
    for (size_t i = 0; i < rep.bands.size(); ++i) {
      const Calibrator::BandResult& b = rep.bands[i];
      printf("%-16s %7.2f %5.1f%% %9.2e %9.2e %8.0f  %s%s\n", b.name.c_str(), b.snr_db,
             b.frame_success * 100.0, b.raw_ber, b.fec_ber, b.payload_bps,
             b.usable ? "yes" : "no", b.clipped ? "  [CLIPPED]" : "");
    }
    printf("best profile id %d, suggested mtu %d (model suggestion, not a room measurement)\n",
           rep.best_profile, rep.suggested_mtu);
  } else if (cmd == "r") {
    if (app.using_wav) {
      printf("record: not with --wav-in (that mode feeds a file, it does not use a microphone)\n");
    } else {
      std::vector<float> cap;
      std::vector<float> buf(2048);
      int64_t until = (int64_t)GetTickCount64() + 8000;
      while ((int64_t)GetTickCount64() < until) {
        int got = app.audio.capture(&buf[0], (int)buf.size());
        if (got > 0) cap.insert(cap.end(), buf.begin(), buf.begin() + got);
        else Sleep(5);
      }
      double fs_now = app.audio.sample_rate();
      if (cap.empty()) {
        printf("record: the microphone delivered no samples in 8 s - that is the finding.\n");
      } else {
        ni::CaptureDiagnosis d;
        ni::ni_diagnose_capture(&cap[0], cap.size(), fs_now, 10, &d);
        if (ni_wav_write_f32("ni-last-capture.wav", &cap[0], cap.size(), fs_now, 16) == NI_OK)
          printf("saved %zu samples to ni-last-capture.wav\n", cap.size());
        printf("VERDICT: %s\n", d.verdict.c_str());
        printf("  peak %.1f  room %.1f  loudest %.1f dBFS  clipped %.2f %%  band %.0f-%.0f Hz  "
               "shift %.0f ppm  modem %s\n",
               d.peak_dbfs, d.noise_dbfs, d.burst_dbfs, d.clip_pct, d.band_low_hz, d.band_high_hz,
               d.shift_ppm, d.decodes ? "DECODES" : "no burst");
      }
    }
  } else if (cmd == "h") {
    // "hear myself": eight seconds of this machine listening to its own speaker. Detached, because the
    // pump has to keep running for the audio to move at all - and the answer is one sentence.
    SelfHear before = node.self_hear_now();
    std::thread([&node, before] {
      for (int i = 0; i < 80 && !g_quit.load(); ++i) Sleep(100);
      if (g_quit.load()) return;
      SelfHear now = node.self_hear_since(before, 8);
      printf("\nhear-myself: %s\n", now.verdict.c_str());
      fflush(stdout);
    }).detach();
    printf("listening to this machine's own speaker for 8 seconds...\n");
  } else if (cmd == "l") {
    std::string rep;
    int rc = node.test_link(now, 228, &rep);
    printf("link test: %s\n%s\n", rc == NI_OK ? "completed" : ni_strerror(rc), rep.c_str());
  } else if (cmd == "i") {
    InternetVerify iv;
    int rc = node.test_internet(now, "http://example.com/", &iv);
    printf("internet test: rc=%d ok=%s\n", rc, iv.ok ? "yes" : "no");
    for (size_t i = 0; i < iv.stages.size(); ++i)
      printf("  %-14s %-4s %.0f ms  %s\n", iv.stages[i].name.c_str(), iv.stages[i].ok ? "ok" : "FAIL",
             iv.stages[i].ms, iv.stages[i].detail.c_str());
    if (iv.ok)
      printf("  payload %u B, sha256 %s%s\n", (unsigned)iv.bytes, iv.got_sha256.c_str(),
             iv.expect_sha256 == iv.got_sha256 ? " (matches)" : " (MISMATCH)");
    else
      printf("  no internet path: which stage failed above is the result.\n");
  } else if (cmd == "r") {
    Role next = node.config().role == NI_ROLE_GIVER ? NI_ROLE_RECEIVER : NI_ROLE_GIVER;
    int rc = node.set_role(next);
    printf("role switch: %s (%s)\n", rc == NI_OK ? "accepted" : ni_strerror(rc),
           next == NI_ROLE_GIVER ? "giver" : "receiver");
  } else if (cmd == "p") {
    printf("%s\n", ni_abi_profiles_json());
  } else if (cmd == "s") {
    printf("%s\n", node.status_json().c_str());
  } else if (!cmd.empty()) {
    printf("unknown command: %s\n", cmd.c_str());
  }
}

void usage() {
  printf(
      "NoiseInternet - acoustic internet sharing over speaker and microphone\n"
      "\n"
      "  NoiseInternet.exe [--giver|--receiver] [--mic <endpoint-id>] [--spk <endpoint-id>]\n"
      "                    [--profile N] [--fs 48000] [--secret PASSPHRASE] [--session N]\n"
      "                    [--proxy-token T] [--allow-unpaired]   (a passphrase is required to pair:\n"
      "                    8+ characters, identical on both ends)\n"
      "                    [--proxy-port 8471] [--proxy-token TOKEN] [--duplex] [--list-devices]\n"
      "                    [--wav-in f.wav --wav-out g.wav]   (development: file transport, NOT\n"
      "                     an acoustic test; the status screen marks it as such)\n"
      "\n"
      "Then, interactively:  a = calibrate   l = link test (228 B)   i = internet test\n"
      "                     r = switch role   p = profiles            s = status json\n"
      "                     q = quit   (Enter alone reprints the status screen)\n"
      "\n"
      "A client on this machine reaches the receiver's proxy with\n"
      "  curl -x http://127.0.0.1:8471 -H \"Proxy-Authorization: Noise <token>\" http://example.com/\n"
      "Browsers cannot send that header (it is not Basic auth), so expect curl or ni.exe, not Chrome.\n"
      "\n"
      "Nothing here generates a result: every printed number comes from the engine.\n");
}

int list_devices() {
  std::vector<AudioDeviceInfo> dev;
  std::string why;
  int n = ni_audio_enumerate(&dev, &why);
  if (n < 0) {
    printf("device enumeration unavailable: %s\n", why.c_str());
    printf("NOT TESTED: this build has no audio backend (rebuild with -DNI_WITH_WASAPI=ON).\n");
    return n;
  }
  printf("%d endpoint(s):\n", n);
  for (size_t i = 0; i < dev.size(); ++i) {
    printf("  [%2d] %-3s %-22s %6.0f Hz %d ch  %s%s\n", (int)i, dev[i].is_capture ? "mic" : "spk",
           dev[i].name.substr(0, 22).c_str(), dev[i].nominal_rate, dev[i].channels,
           dev[i].api.c_str(), dev[i].loopback ? "  [LOOPBACK - unusable as a microphone]" : "");
    printf("       id: %s\n", dev[i].id.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  App app;
  app.cfg.role = NI_ROLE_GIVER;
  app.cfg.is_master = true;
  // Was the literal "change-me": a hard-coded passphrase that made --secret look optional, so a user
  // could run this end with one secret and the phone with another and never know why nothing paired.
  // Empty now, and the startup check below refuses to pretend it can pair without one.
  app.cfg.pairing_secret = "";
  app.cfg.profile_id = 10;   // FD-LOW-DBPSK: the profile every measured end-to-end run completes on
  app.cfg.session_id = 7;    // the same default the windowed app and the phone use
  app.cfg.name = "win";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) {
        usage();
        return 1;
      }
      dst = argv[++i];
      return 0;
    };
    std::string v;
    if (a == "--giver") {
      app.cfg.role = NI_ROLE_GIVER;
    } else if (a == "--receiver") {
      app.cfg.role = NI_ROLE_RECEIVER;
      app.cfg.is_master = false;
    } else if (a == "--mic") {
      if (next(v)) return 2;
      app.mic_id = v;
    } else if (a == "--spk") {
      if (next(v)) return 2;
      app.spk_id = v;
    } else if (a == "--profile") {
      if (next(v)) return 2;
      app.cfg.profile_id = atoi(v.c_str());
    } else if (a == "--fs") {
      if (next(v)) return 2;
      app.cfg.fs = atof(v.c_str());
    } else if (a == "--allow-unpaired") {
      app.allow_unpaired = true;
      // The core refuses a pairing-less start unless told it is intended, and the console app is
      // where that intention is expressed (the windowed app has no such flag: it always pairs).
      app.cfg.allow_unpaired = true;
    } else if (a == "--secret") {
      if (next(v)) return 2;
      app.cfg.pairing_secret = v;
    } else if (a == "--session") {
      if (next(v)) return 2;
      app.cfg.session_id = (uint16_t)atoi(v.c_str());
    } else if (a == "--proxy-port") {
      if (next(v)) return 2;
      app.cfg.proxy_port = atoi(v.c_str());
    } else if (a == "--proxy-token") {
      // The receiver's proxy authenticates clients with a bearer token ("Proxy-Authorization: Noise T").
      // Left unset the core generates one and only its 4-character prefix reaches the status screen,
      // which is right for security and useless for a client that needs the whole string - so the local
      // machine that wants to use the proxy sets its own token here. Documented as defect 27: the
      // Android build has no equivalent field, so its proxy can only be driven from inside the app.
      if (next(v)) return 2;
      app.cfg.proxy_token = v;
    } else if (a == "--duplex") {
      app.cfg.duplex = 1;
    } else if (a == "--wav-in") {
      if (next(v)) return 2;
      app.using_wav = true;
      app.wav_in = v;
    } else if (a == "--wav-out") {
      if (next(v)) return 2;
      app.using_wav = true;
      app.wav_out = v;
    } else if (a == "--list-devices") {
      return list_devices();
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      printf("unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  if (app.cfg.pairing_secret.size() < 8) {
    printf("\nNO PASSPHRASE: %s\n",
           app.cfg.pairing_secret.empty()
               ? "the passphrase is empty. Pairing is never attempted without one, so this end cannot "
                 "pair."
               : "the passphrase is under 8 characters. It will pair, but it is weak.");
    printf("Both ends need the SAME passphrase: this exe takes --secret PASSPHRASE and the Android app "
           "has a passphrase field.\n");
    if (app.cfg.pairing_secret.empty() && !app.allow_unpaired) {
      printf("Refusing to start: nothing would be measurable. Pass --secret 'a-long-passphrase' (and "
             "the same one on the phone), or --allow-unpaired to run this end without pairing.\n\n");
      return 2;
    }
    printf("\n");
  }

  // The receiver serves the local proxy; the giver is the one that can reach the internet.
  app.cfg.enable_receiver_proxy = (app.cfg.role == NI_ROLE_RECEIVER);
  app.cfg.enable_giver_fetch = true;

  if (app.using_wav) {
    printf("\nFILE MODE: audio is exchanged through WAV files. This exercises the codec and the\n"
           "link layer, and is NOT a speaker-to-microphone test. Do not report its timings as\n"
           "acoustic performance.\n\n");
  }

  Node node(app.cfg);
  app.node = &node;
  int rc = node.configure();
  if (rc != NI_OK) {
    printf("configure failed (%d): %s\n", rc, node.last_error().c_str());
    return 1;
  }
  node.start();

  // Real audio, unless the developer asked for the file path.
  if (!app.using_wav) {
    WasapiEngine::thread_init();
    rc = app.audio.open(app.spk_id, app.mic_id, app.cfg.fs);
    if (rc != NI_OK) {
      printf("audio open failed (%d): %s\n", rc, app.audio.last_error().c_str());
      printf("NOT TESTED: no usable capture/render pair, so no acoustic transfer can be claimed.\n");
      printf("Use --list-devices to see endpoint ids, or run with --wav-in/--wav-out for a\n"
             "codec-only test that is explicitly not over the air.\n");
      node.stop();
      return 3;
    }
    // The device decides the rate; if it refused the requested one, open() said so and failed.
    printf("%s\n", app.audio.describe().c_str());
  }

  std::thread pump;
  if (!app.using_wav) {
    pump = std::thread([&app, &node] {
      WasapiEngine::thread_init();
      std::vector<float> buf(4096);
      int64_t t = GetTickCount64();
      while (!g_quit.load()) {
        // capture -> engine
        int got = app.audio.capture(&buf[0], (int)buf.size());
        if (got > 0) node.feed_capture(&buf[0], (size_t)got);
        // engine -> render, with a small wait so we never spin the endpoint dry
        int need = (int)app.audio.render_buffer_frames();
        if (need < 240) need = 240;
        got = node.pull_playback(&buf[0], (size_t)need);
        if (got > 0) app.audio.render(&buf[0], got, 50);
        node.step(t);
        Sleep(5);
        t = GetTickCount64();
      }
    });
  } else {
    // File pump: read the whole input WAV once, feed it, then write whatever the engine wanted to
    // play. Deliberately simple - it exists so the codec can be exercised on Windows at all.
    pump = std::thread([&app, &node] {
      std::vector<float> in;
      double fs = 0;
      if (ni_wav_read_f32(app.wav_in, &in, &fs) < 0) printf("cannot read %s\n", app.wav_in.c_str());
      if (!in.empty()) node.feed_capture(&in[0], in.size());
      std::vector<float> out;
      std::vector<float> tmp(4096);
      int64_t t = (int64_t)GetTickCount64();
      for (int i = 0; i < 400; ++i) {
        int got = node.pull_playback(&tmp[0], tmp.size());
        if (got > 0) out.insert(out.end(), tmp.begin(), tmp.begin() + got);
        node.step(t + i * 10);
        Sleep(10);
      }
      if (!app.wav_out.empty() &&
          ni_wav_write_f32(app.wav_out, out.empty() ? nullptr : &out[0], out.size(),
                           fs > 0 ? fs : 48000.0) < 0)
        printf("cannot write %s\n", app.wav_out.c_str());
      printf("file mode: fed %d samples, produced %d samples\n", (int)in.size(), (int)out.size());
    });
  }

  printf("commands: a=calibrate(model sweep)  h=hear myself  r=record 8s+analyse  l=link test  "
         "i=internet test  p=profiles  s=status json  q=quit\n");
  print_status(app);
  std::string line;
  while (!g_quit.load() && std::getline(std::cin, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) {
      print_status(app);
      continue;
    }
    if (line == "q") break;
    run_command(app, line);
  }
  g_quit.store(true);
  if (pump.joinable()) pump.join();
  if (!app.using_wav) app.audio.close();
  node.stop();
  WasapiEngine::thread_shutdown();
  print_status(app);
  return 0;
}
