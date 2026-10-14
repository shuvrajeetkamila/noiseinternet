// cli.cpp — `ni`, the command-line harness for every layer of the stack.
//
// Everything the GUI shells do is reachable from here, which is what makes the numbers in
// reports/ reproducible: they are emitted by this binary, not typed into a table. The two
// commands that carry the most evidential weight are `ni decode` (a payload recovered from a
// real recording, hash-compared) and `ni demo` (a real HTTP response pulled through the
// receiver's proxy while the only thing crossing between the two nodes is audio samples).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// The demo spawns a client process (curl) and reads its output while the audio pump keeps running.
// That needs fork/execvp/waitpid, which Windows does not have. Rather than fake it with a blocking
// _popen (which would deadlock: the child waits for the link, the link waits for this thread), the
// spawn path is compiled out on that platform and `ni demo` says so. `ni linksim`, `ni selftest`,
// `ni bench`, `ni render` and `ni decode` are unaffected - and the proxy leg itself is exercised by
// the unit tests and by the app.
// NI_NO_DEMO_SPAWN lets a POSIX-like target opt out explicitly (the Windows build sets it in CMake
// as well, so the intent is visible in the build file too, not only in the compiler id).
#if (defined(__unix__) || defined(__APPLE__)) && !defined(NI_NO_DEMO_SPAWN)
#define NI_HAVE_SPAWN 1
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <map>
#include <sstream>
#include <thread>
#include <vector>
#include "ni/audio_io.h"
#include "ni/channel_sim.h"
#include "ni/crypto.h"
#include "ni/node.h"

using namespace ni;

namespace {

struct Opts {
  std::map<std::string, std::string> v;
  std::vector<std::string> pos;
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = v.find(k);
    return it == v.end() ? d : it->second;
  }
  double num(const std::string& k, double d) const {
    auto it = v.find(k);
    return it == v.end() ? d : atof(it->second.c_str());
  }
  bool flag(const std::string& k) const { return v.count(k) > 0; }
};

Opts parse(int argc, char** argv, int from) {
  Opts o;
  double dtmp = 0;
  for (int i = from; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--", 0) == 0) {
      std::string k = a.substr(2);
      size_t eq = k.find('=');
      if (eq != std::string::npos)
        o.v[k.substr(0, eq)] = k.substr(eq + 1);
      else if (i + 1 < argc &&
               (argv[i + 1][0] != '-' ||
                sscanf(argv[i + 1], "%lf", &dtmp) == 1 /* negative number, e.g. --volume -10 */))
        o.v[k] = argv[++i];
      else
        o.v[k] = "1";
    } else
      o.pos.push_back(a);
  }
  return o;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

ChannelConfig channel_from(const Opts& o) {
  ChannelConfig c;
  c.fs = o.num("fs", 48000);
  std::string spk = o.get("speaker", o.get("spk", "laptop-speaker"));
  std::string mic = o.get("mic", o.get("microphone", "laptop-mic"));
  auto pick = [](const std::string& s, int deflt) {
    for (int i = 0; i < NI_DEV_MODEL_COUNT; i++)
      if (s == ni_device_name(i)) return i;
    if (s == "ideal" || s == "none") return (int)NI_DEV_IDEAL;
    if (s == "laptop-speaker" || s == "laptop-speakers") return (int)NI_DEV_LAPTOP_SPK;
    if (s == "laptop-mic" || s == "laptop-microphone") return (int)NI_DEV_LAPTOP_MIC;
    return deflt;
  };
  c.speaker = pick(spk, NI_DEV_LAPTOP_SPK);
  c.microphone = pick(mic, NI_DEV_LAPTOP_MIC);
  c.distance_m = o.num("distance", 0.5);
  c.snr_db = o.num("snr", 20);
  c.sro_ppm = o.num("sro", 0);
  c.agc = !o.flag("no-agc");
  c.agc_fast = o.flag("fast-agc");
  c.multipath = !o.flag("no-multipath");
  c.tx_level_db = o.num("volume", 0);
  c.seed = (uint64_t)o.num("seed", 1234);
  return c;
}


// --profile accepts either the numeric id or the exact name. A value that is neither is an
// error, never a silent fallback: a wrong-but-plausible profile id is exactly the kind of
// mistake that would make a measurement meaningless.
int profile_arg(const Opts& o, int deflt, bool* bad) {
  std::string s = o.get("profile");
  if (bad) *bad = false;
  if (s.empty()) return deflt;
  int pid = ni_profile_lookup(s.c_str());
  if (pid >= 0) return pid;
  char* e = nullptr;
  long v = strtol(s.c_str(), &e, 10);
  if (e && *e == 0 && v >= 0 && v < ni_profile_count()) return (int)v;
  if (bad) *bad = true;
  return deflt;
}

std::string sha_hex(const void* p, size_t n) {
  uint8_t sh[32];
  ni_sha256((const uint8_t*)p, n, sh);
  return ni_hex(sh, 32);
}

// ---------------------------------------------------------------- profiles / plan
int bad_profile(const Opts& o, std::string tok = std::string()) {
  fprintf(stderr, "no such profile '%s' (see `ni profiles` for the %d names/ids)\n",
          tok.empty() ? o.get("profile").c_str() : tok.c_str(), ni_profile_count());
  return 1;
}

int cmd_profiles(const Opts& o) {
  double fs = o.num("fs", 48000);
  if (o.flag("json")) {
    printf("[\n");
    for (int i = 0; i < ni_profile_count(); i++) {
      const Profile& p = ni_profile(i);
      CarrierMap m;
      int rc = ni_build_carrier_map(p, fs, NI_N_FFT, &m);
      BurstPlan pl;
      int mp = ni_profile_max_payload(p, fs);
      if (rc == NI_OK) ni_plan_burst(p, m, fs, mp > 0 ? mp : 1, &pl);
      printf("  {\"id\": %d, \"name\": \"%s\", \"modulation\": \"%s\", \"fs\": %d, "
             "\"band_low_hz\": %.1f, \"band_high_hz\": %.1f, \"bins\": %d, \"data_bins\": %d, "
             "\"pilots\": %d, \"bits_per_symbol\": %d, \"fec\": \"%s\", "
             "\"max_payload_bytes\": %d, \"burst_ms\": %.2f, \"payload_bps\": %.1f, "
             "\"usable\": %s, \"note\": \"%s\"}%s\n",
             i, p.name, ni_mod_name(p.mod), (int)fs, p.f_low, p.f_high, m.n_bins, m.n_data,
             m.n_pilot, m.bits_per_symbol, p.fec_rate_den == 2 ? "rate-1/2 K=7" : "none", mp,
             pl.duration_ms, pl.payload_bitrate / 8.0, (rc == NI_OK && mp >= 32) ? "true" : "false",
             p.note, (i + 1 < ni_profile_count()) ? "," : "");
    }
    printf("]\n");
    return 0;
  }
  printf("%-3s %-15s %-7s %-13s %4s %4s %3s %5s %4s %7s %7s %7s\n", "id", "profile", "mod",
         "band(Hz)", "bin", "dat", "pil", "b/sym", "fec", "maxPay", "ms", "B/s");
  for (int i = 0; i < ni_profile_count(); i++) {
    const Profile& p = ni_profile(i);
    CarrierMap m;
    int rc = ni_build_carrier_map(p, fs, NI_N_FFT, &m);
    int mp = ni_profile_max_payload(p, fs);
    BurstPlan pl;
    if (rc == NI_OK) ni_plan_burst(p, m, fs, mp > 0 ? mp : 1, &pl);
    char band[24];
    snprintf(band, sizeof band, "%.0f-%.0f", p.f_low, p.f_high);
    printf("%-3d %-15s %-7s %-13s %4d %4d %3d %5d %4s %7d %7.1f %7.0f  %s\n", i, p.name,
           ni_mod_name(p.mod), band, m.n_bins, m.n_data, m.n_pilot, m.bits_per_symbol,
           p.fec_rate_den == 2 ? "1/2" : "-", mp, pl.duration_ms, pl.payload_bitrate / 8.0,
           rc == NI_OK ? "" : ni_strerror(rc));
  }
  printf("\nfs=%.0f  N_FFT=%d  CP=%d  spacing=%.2f Hz  preamble=2 sync + 2 desc + 1 ref"
         "  (+1 ref every %d data symbols)\n",
         fs, NI_N_FFT, NI_CP, fs / NI_N_FFT, NI_REF_INTERVAL);
  printf("max_payload is the single-frame limit (pdu <= %d bytes)\n",
         NI_MAX_BURST_PDU);
  printf("frame layout: 16 B header + payload + 4 B CRC32; a PDU adds a 2 B length prefix\n"
         "per frame, and the PHY adds a 2 B count.\n");
  printf("payload B/s assumes one full burst back to back with no ARQ, gaps or acks; the "
         "measured session rate is lower (see `ni linksim`).\n");
  return 0;
}

int cmd_plan(const Opts& o) {
  bool bad = false;
  int pid = profile_arg(o, 2, &bad);
  if (bad) return bad_profile(o);
  double fs = o.num("fs", 48000);
  int bytes = (int)o.num("bytes", 240);
  const Profile& p = ni_profile(pid);
  CarrierMap m;
  int rc = ni_build_carrier_map(p, fs, NI_N_FFT, &m);
  if (rc != NI_OK) {
    fprintf(stderr, "carrier map: %s\n", ni_strerror(rc));
    return 1;
  }
  BurstPlan pl;
  ni_plan_burst(p, m, fs, bytes, &pl);
  printf("profile %s  fs=%.0f  payload=%dB\n", p.name, fs, bytes);
  printf("  data symbols          : %d (total %d incl. sync/descriptor/reference)\n",
         pl.symbols_data, pl.symbols_total);
  printf("  bits                  : info=%d coded=%d  %d b/symbol\n", pl.info_bits,
         pl.coded_bits, m.bits_per_symbol);
  printf("  airtime               : %.1f ms (%.1f ms of that is preamble)\n", pl.duration_ms,
         pl.sync_ms);
  printf("  raw symbol rate       : %.0f b/s\n", pl.raw_bitrate);
  printf("  information rate      : %.0f b/s\n", pl.info_bitrate);
  printf("  useful payload        : %.0f B/s\n", pl.payload_bitrate / 8.0);
  printf("  bursts/s at this size : %.2f\n", pl.duration_ms > 0 ? 1000.0 / pl.duration_ms : 0.0);
  printf("  fits profile limit    : %s\n", pl.fits ? "yes" : "NO");
  return pl.fits ? 0 : 2;
}

// ---------------------------------------------------------------- render / decode
std::vector<uint8_t> payload_from(const Opts& o, int* n_out) {
  std::vector<uint8_t> pl;
  std::string text = o.get("text");
  std::string file = o.get("file");
  if (!text.empty()) {
    pl.assign(text.begin(), text.end());
  } else if (!file.empty()) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) {
      fprintf(stderr, "cannot open %s\n", file.c_str());
      exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    pl.resize((size_t)std::min<long>(sz, 1 << 20));
    if (!pl.empty() && fread(&pl[0], 1, pl.size(), f) != pl.size()) exit(1);
    fclose(f);
  } else {
    int n = (int)o.num("random", 128);
    Rng r((uint64_t)o.num("seed", 7));
    pl.resize((size_t)n);
    if (!pl.empty()) r.bytes(&pl[0], pl.size());
  }
  if (n_out) *n_out = (int)pl.size();
  return pl;
}

int cmd_render(const Opts& o) {
  bool bad = false;
  int pid = profile_arg(o, 2, &bad);
  if (bad) return bad_profile(o);
  double fs = o.num("fs", 48000);
  int n = 0;
  std::vector<uint8_t> pl = payload_from(o, &n);
  ModemConfig mc;
  mc.sample_rate = fs;
  mc.profile_id = pid;
  mc.tx_amplitude = o.num("amplitude", 0.85);
  mc.session_id = (uint16_t)strtoul(o.get("session", "0x2A2A").c_str(), nullptr, 0);
  Modem m(mc);
  if (!m.ok()) {
    fprintf(stderr, "modem: %s\n", m.error().c_str());
    return 1;
  }
  if ((int)pl.size() > m.max_payload()) {
    fprintf(stderr,
            "payload %d B exceeds this profile's single-frame limit of %d B (use `ni profiles`;"
            " the session layer fragments automatically, this raw path does not)\n",
            (int)pl.size(), m.max_payload());
    return 1;
  }
  Frame f;
  f.type = NI_FT_DATA;
  f.mod_id = (uint8_t)pid;
  f.session_id = mc.session_id;
  f.seq = 0;
  f.flags = 0;
  f.payload = pl.empty() ? nullptr : &pl[0];
  f.payload_len = (uint16_t)pl.size();
  std::vector<uint8_t> frame(NI_MAX_FRAME);
  int fl = ni_frame_encode(&frame[0], frame.size(), f);
  if (fl <= 0) {
    fprintf(stderr, "frame encode: %s\n", ni_strerror(fl));
    return 1;
  }
  std::vector<uint8_t> pdu;
  ni_phy_wrap(&frame[0], (size_t)fl, &pdu);
  std::vector<float> wave;
  int rc = m.tx_render(pdu.data(), pdu.size(), &wave);
  if (rc != NI_OK) {
    fprintf(stderr, "render: %s\n", ni_strerror(rc));
    return 1;
  }
  std::string out = o.get("out", "noise-internet.wav");
  std::vector<float> padded(wave);
  if (o.flag("pad")) {
    size_t pad = (size_t)(fs * o.num("pad", 0.3));
    padded.insert(padded.begin(), pad, 0.f);
    padded.insert(padded.end(), pad, 0.f);
  }
  rc = ni_wav_write_f32(out, &padded[0], padded.size(), fs, (int)o.num("bits", 16));
  if (rc < 0) {
    fprintf(stderr, "write: %s\n", ni_strerror(rc));
    return 1;
  }
  BurstPlan bp = m.plan(pl.size());
  printf("rendered %zu samples (%.1f ms) -> %s\n", padded.size(), 1000.0 * padded.size() / fs,
         out.c_str());
  printf("  profile=%s fs=%.0f payload=%d B burst=%.1f ms peak=%.3f  pcm16=%s\n",
         ni_profile(pid).name, fs, n, bp.duration_ms, ni_peak(&wave[0], wave.size()),
         o.num("bits", 16) == 16 ? "yes" : "no");
  printf("  payload sha256 = %s\n", sha_hex(pl.data(), pl.size()).c_str());
  printf("  play it next to the receiving device, then:\n    ni decode --in <recording.wav> "
         "--profile %s --expect-sha256 %s\n",
         ni_profile(pid).name, sha_hex(pl.data(), pl.size()).c_str());
  return 0;
}

int cmd_decode(const Opts& o) {
  bool badp = false;
  std::string in = o.get("in", "recording.wav");
  double fs_file = 0;
  std::vector<float> x;
  int ch = 0, bits = 0;
  int rc = ni_wav_read_f32(in, &x, &fs_file, &ch, &bits);
  if (rc < 0) {
    fprintf(stderr, "cannot read %s: %s\n", in.c_str(), ni_strerror(rc));
    return 1;
  }
  double fs = fs_file;
  printf("read %zu samples @ %.0f Hz (%d ch, %d bit) from %s\n", x.size(), fs_file, ch, bits,
         in.c_str());
  printf("  peak=%.3f rms=%.5f clipped=%d\n", ni_peak(&x[0], x.size()), ni_rms(&x[0], x.size()),
         ni_clip_count(&x[0], x.size()));
  std::vector<int> try_profiles;
  if (o.flag("profile")) {
    int pid = profile_arg(o, -1, &badp);
    if (badp) return bad_profile(o);
    try_profiles.push_back(pid);
  } else {
    for (int i = 0; i < ni_profile_count(); i++) try_profiles.push_back(i);
    printf("  no --profile given: sweeping all %zu profiles (the receiver does the same via "
           "the descriptor)\n", try_profiles.size());
  }
  RxResult best;
  int best_pid = -1;
  std::string last_err;
  for (int pid : try_profiles) {
    ModemConfig mc;
    mc.sample_rate = fs;
    mc.profile_id = pid;
    Modem m(mc);
    if (!m.ok()) continue;
    RxResult r;
    int drc = m.rx_capture(&x[0], x.size(), &r);
    if (drc == NI_OK) {
      best = std::move(r);
      best_pid = pid;
      break;
    }
    if (r.stats.snr_db > best.stats.snr_db) {
      best = std::move(r);
      last_err = ni_strerror(drc);
    }
  }
  if (best_pid < 0 || best.pdu.empty()) {
    printf("RESULT: NO DECODE (no valid burst found)\n");
    printf("  best candidate: snr=%.1f dB rssi=%.1f dBFS evec=%.1f%% (%s)\n",
           best.stats.snr_db, best.stats.rssi_dbfs, best.stats.evec, last_err.c_str());
    printf("  likely causes: profile mismatch, background noise, OS audio processing\n"
           "  (AGC / noise suppression / beam-forming) reshaping the waveform, distance, or a\n"
           "  sample-rate mismatch between playback and recording.\n");
    return 2;
  }
  Frame f;
  rc = ni_frame_decode(best.pdu.data(), best.pdu.size(), &f);
  if (rc != NI_OK) {
    printf("RESULT: burst found, frame rejected (%s)\n", ni_strerror(rc));
    return 2;
  }
  std::string sha = sha_hex(f.payload, f.payload_len);
  printf("RESULT: DECODED with profile %s\n", ni_profile(best_pid).name);
  printf("  payload=%d B  snr=%.1f dB  rssi=%.1f dBFS  evec=%.1f%%  airtime=%.1f ms  "
         "coarse CFO=%.1f Hz  coded bits=%zu  info bits=%zu\n",
         f.payload_len, best.stats.snr_db, best.stats.rssi_dbfs, best.stats.evec,
         best.stats.duration_ms, best.cfo, best.hard_coded.size(), best.info_bits.size());
  printf("  sha256 = %s\n", sha.c_str());
  std::string expect = o.get("expect-sha256");
  if (!expect.empty()) {
    bool ok = (sha == expect);
    printf("  hash check: %s\n", ok ? "MATCH - payload recovered bit-exactly"
                                    : "MISMATCH - payload corrupted");
    if (!ok) return 3;
  }
  std::string save = o.get("save");
  if (!save.empty()) {
    FILE* fp = fopen(save.c_str(), "wb");
    if (!fp) {
      fprintf(stderr, "cannot write %s\n", save.c_str());
      return 1;
    }
    fwrite(f.payload, 1, f.payload_len, fp);
    fclose(fp);
    printf("  wrote %d bytes -> %s\n", f.payload_len, save.c_str());
  }
  bool printable = f.payload_len > 0;
  for (int i = 0; i < f.payload_len && printable; i++)
    if (f.payload[i] < 9 || (f.payload[i] > 13 && f.payload[i] < 32)) printable = false;
  if (printable) printf("  text: \"%.*s\"\n", f.payload_len, f.payload);
  return 0;
}

// ---------------------------------------------------------------- calibration
// The same routine the Android and Windows shells reach through ni_abi_calibrate(), run against the
// modelled device chain so that the numbers it produces can be inspected without hardware. On a
// machine with a speaker and a microphone the hardware flags come from the real device instead; the
// summary line says which of the two applies, because a calibration measured on a model must never be
// mistaken for one measured on a device.
int cmd_calibrate(const Opts& o) {
  NodeConfig c;
  c.name = "calibrate";
  c.fs = o.num("fs", 48000);
  c.tx_amplitude = o.num("volume", 0.85);
  c.calibrate_trials = (int)o.num("trials", 3);
  c.allow_unpaired = true;   // calibration is a solo measurement: no peer, no pairing, no secret
  c.sim_channel = true;
  c.ch = channel_from(o);
  c.ch.fs = c.fs;
  c.ch.multipath = !o.flag("no-multipath");
  c.ch.agc = !o.flag("no-agc");
  Node n(c);
  if (n.start() != NI_OK) {
    printf("node start failed: %s\n", n.last_error().c_str());
    return 2;
  }
  Calibrator::Report rep;
  int rc = n.calibrate(0, &rep);
  if (rc != NI_OK) {
    printf("calibrate failed (rc=%d): %s\n", rc, n.last_error().c_str());
    return 1;
  }
  if (o.flag("json")) {
    printf("%s\n", rep.to_json().c_str());
    return 0;
  }
  printf("NOISE INTERNET calibration - %d trials per profile at each of 18, 12 and 7 dB\n",
         c.calibrate_trials);
  printf("%-17s %7s %7s %9s %9s %9s %5s %6s  %s\n", "profile", "snr_dB", "ok%", "rawBER", "fecBER",
         "bps", "clip", "usable", "note");
  for (size_t i = 0; i < rep.bands.size(); i++) {
    const Calibrator::BandResult& b = rep.bands[(size_t)i];
    printf("%-17s %7.2f %6.1f%% %9.2e %9.2e %9.0f %5s %6s  %s\n", b.name.c_str(), b.snr_db,
           b.frame_success * 100.0, b.raw_ber, b.fec_ber, b.payload_bps, b.clipped ? "yes" : "no",
           b.usable ? "yes" : "no", b.note.c_str());
  }
  printf("recommended profile : %s\n",
         rep.best_profile >= 0 ? ni_profile(rep.best_profile).name : "(nothing usable)");
  printf("suggested mtu       : %d\n", rep.suggested_mtu);
  printf("%s\n", rep.summary.c_str());
  return 0;
}

// ---------------------------------------------------------------- selftest / bench
int cmd_selftest(const Opts& o) {
  double fs = o.num("fs", 48000);
  int trials = (int)o.num("trials", 5);
  int pb = (int)o.num("bytes", 120);
  bool bad = false;
  ChannelConfig cc = channel_from(o);
  std::vector<int> pids;
  if (o.flag("profile")) {
    std::string s = o.get("profile");
    if (s == "all") {
      for (int i = 0; i < ni_profile_count(); i++) pids.push_back(i);
    } else {
      std::istringstream is(s);
      std::string tok;
      while (std::getline(is, tok, ',')) {
        if (tok.empty()) continue;
        int pid = ni_profile_lookup(tok.c_str());
        if (pid < 0 && !tok.empty() && isdigit((unsigned char)tok[0])) pid = atoi(tok.c_str());
        if (pid < 0 || pid >= ni_profile_count()) return bad_profile(o);
        pids.push_back(pid);
      }
    }
  } else
    pids = {0, 2, 5, 9};
  printf("loopback PHY test: tx_render -> int16 PCM -> AcousticChannel -> rx_decode\n");
  printf("payload %d B, %d trials, target SNR %.1f dB, %s -> %s, %.1f m\n", pb, trials, cc.snr_db,
         ni_device_name(cc.speaker), ni_device_name(cc.microphone), cc.distance_m);
  printf("%-15s %6s %8s %9s %9s %8s %7s %7s\n", "profile", "snr_dB", "ok", "rawBER", "fecBER",
         "burst_ms", "B/s", "evec%");
  int total_fail = 0;
  for (int pid : pids) {
    ModemConfig mc;
    mc.sample_rate = fs;
    mc.profile_id = pid;
    mc.tx_amplitude = o.num("amplitude", 0.85);
    Modem m(mc);
    if (!m.ok()) {
      printf("%-15s SKIPPED: %s\n", ni_profile(pid).name, m.error().c_str());
      continue;
    }
    int pxx = std::min(pb, m.max_payload());
    int ok = 0;
    double rb = 0, fb = 0, snr = 0, ev = 0, ms = 0;
    std::string note;
    for (int t = 0; t < trials; t++) {
      Rng r((uint64_t)o.num("seed", 1) * 100000u + (uint64_t)t);
      std::vector<uint8_t> pl((size_t)pxx);
      r.bytes(&pl[0], pl.size());
      ChannelConfig c2 = cc;
      c2.seed = cc.seed + (uint64_t)t * 7919u;
      PhyTestResult pr;
      ni_phy_test(m, m, pl.empty() ? nullptr : &pl[0], pl.size(), c2, &pr);
      if (pr.payload_match)
        ok++;
      else if (note.empty())
        note = pr.error;
      rb += pr.raw_ber;
      fb += pr.fec_ber;
      snr += pr.snr_db;
      ev += pr.evec_pct;
      ms += pr.burst_ms;
    }
    printf("%-15s %6.1f %3d/%-4d %9.2e %9.2e %8.1f %7.0f %7.2f  %s\n", ni_profile(pid).name,
           snr / trials, ok, trials, rb / trials, fb / trials, ms / trials,
           ms > 0 ? 1000.0 * pxx / (ms / trials) : 0.0, ev / trials, note.c_str());
    total_fail += trials - ok;
  }
  printf("\nchannel: %s\n", AcousticChannel(cc).describe().c_str());
  printf("NOTE: simulated transducers only. This does not prove speaker/microphone behaviour\n"
         "      on any real device; it proves the modem, FEC and equaliser are self-consistent.\n");
  return total_fail ? 1 : 0;
}

int cmd_bench(const Opts& o) {
  double fs = o.num("fs", 48000);
  int trials = (int)o.num("trials", 9);
  std::string outcsv = o.get("csv");
  std::string outjson = o.get("json");
  std::vector<int> pids = {2, 3, 4, 5, 7, 9};
  std::vector<double> snrs = {30, 22, 16, 12, 8, 4, 0};
  std::vector<double> dists = {0.3, 1.0, 2.5};
  std::vector<int> devs = {NI_DEV_LAPTOP_SPK, NI_DEV_PHONE_SPK, NI_DEV_SMART_SPEAKER};
  if (o.flag("profile")) {
    std::vector<int> p2;
    std::istringstream is(o.get("profile"));
    std::string tok;
    while (std::getline(is, tok, ',')) {
      int pid = ni_profile_lookup(tok.c_str());
      if (pid < 0) pid = atoi(tok.c_str());
      if (pid < 0 || pid >= ni_profile_count()) return bad_profile(o, tok);
      p2.push_back(pid);
    }
    if (!p2.empty()) pids = p2;
  }
  int pb = (int(o.num("bytes", 200)));
  std::vector<std::string> rows;
  std::ostringstream js;
  js << "{\n \"generator\": \"ni bench\",\n \"evidence_level\": \"3 - simulated channel "
        "(modelled transducers, multipath, AGC, pink noise, 16-bit quantisation); NOT "
        "speaker-to-microphone\",\n \"sample_rate\": "
     << (int)fs << ",\n \"payload_bytes\": " << pb << ",\n \"trials_per_point\": " << trials
     << ",\n \"statistic\": \"median across trials; failed_trials counts bursts whose payload "
        "did not match bit-exactly\",\n \"rows\": [\n";
  printf("%-15s %-15s %5s %6s %8s %9s %9s %8s %8s\n", "profile", "speaker", "snrT", "ok",
         "rawBER", "fecBER", "burst_ms", "B/s", "evec%");
  int ridx = 0;
  for (int pid : pids) {
    ModemConfig mc;
    mc.sample_rate = fs;
    mc.profile_id = pid;
    Modem m(mc);
    if (!m.ok()) {
      printf("%-15s unavailable: %s\n", ni_profile(pid).name, m.error().c_str());
      continue;
    }
    int pxx = std::min(pb, m.max_payload());
    for (int dev : devs) {
      for (double dist : dists) {
        for (double snr : snrs) {
          std::vector<double> okr, rb, fb, ms, snrv, evv;
          int fails = 0;
          for (int t = 0; t < trials; t++) {
            Rng r(0xabc123u + (uint64_t)(pid * 977 + dev * 31 + t * 7));
            std::vector<uint8_t> pl((size_t)pxx);
            r.bytes(&pl[0], pl.size());
            ChannelConfig cc;
            cc.fs = fs;
            cc.speaker = dev;
            cc.microphone = NI_DEV_LAPTOP_MIC;
            cc.distance_m = dist;
            cc.snr_db = snr;
            cc.agc_fast = o.flag("fast-agc");
            cc.seed = 0x12345u + (uint64_t)(t + pid * 13 + dev * 7 + (int)(dist * 10) * 101);
            PhyTestResult pr;
            ni_phy_test(m, m, &pl[0], pl.size(), cc, &pr);
            okr.push_back(pr.payload_match ? 1 : 0);
            rb.push_back(pr.raw_ber);
            fb.push_back(pr.fec_ber);
            ms.push_back(pr.burst_ms);
            snrv.push_back(pr.snr_db);
            evv.push_back(pr.evec_pct);
            if (!pr.payload_match) fails++;
          }
          double okrate = 0;
          for (size_t i = 0; i < okr.size(); i++) okrate += okr[i];
          okrate /= (double)okr.size();
          char line[512];
          snprintf(line, sizeof line,
                   "%d,%s,%d,%s,%.1f,%.1f,%.1f,%.4f,%.4g,%.4g,%.2f,%.0f,%.2f,%d,%d", pid,
                   ni_profile(pid).name, dev, ni_device_name(dev), snr, dist, median(snrv),
                   okrate, median(rb), median(fb), median(ms), median(evv),
                   8.0 * pxx / (median(ms) / 1000.0), trials, fails);
          rows.push_back(line);
          printf("%-15s %-15s %5.0f %6.2f %9.2e %9.2e %8.1f %8.0f %8.2f\n",
                 ni_profile(pid).name, ni_device_name(dev), snr, okrate, median(rb), median(fb),
                 median(ms), median(evv), 8.0 * pxx / (median(ms) / 1000.0));
          if (ridx++) js << ",\n";
          js << "   {\"profile_id\": " << pid << ", \"profile\": \"" << ni_profile(pid).name
             << "\", \"speaker_model\": \"" << ni_device_name(dev)
             << "\", \"target_snr_db\": " << snr << ", \"distance_m\": " << dist
             << ", \"measured_snr_db\": " << median(snrv) << ", \"frame_success\": " << okrate
             << ", \"raw_ber\": " << median(rb) << ", \"fec_ber\": " << median(fb)
             << ", \"burst_ms\": " << median(ms) << ", \"evec_pct\": " << median(evv)
             << ", \"payload_bps\": " << 8.0 * pxx / (median(ms) / 1000.0) << ", \"trials\": "
             << trials << ", \"failed_trials\": " << fails << "}";
        }
      }
    }
  }
  js << "\n ]\n}\n";
  if (!outcsv.empty()) {
    FILE* f = fopen(outcsv.c_str(), "wb");
    if (!f) {
      fprintf(stderr, "cannot write %s\n", outcsv.c_str());
      return 1;
    }
    fprintf(f,
            "profile_id,profile,speaker_model_id,speaker_model,target_snr_db,distance_m,"
            "measured_snr_db,frame_success,raw_ber,fec_ber,burst_ms,evec_pct,payload_bps,trials,"
            "failed_trials\n");
    for (size_t i = 0; i < rows.size(); i++) fprintf(f, "%s\n", rows[i].c_str());
    fclose(f);
    printf("\nwrote %zu rows -> %s\n", rows.size(), outcsv.c_str());
  }
  if (!outjson.empty()) {
    FILE* f = fopen(outjson.c_str(), "wb");
    if (!f) return 1;
    std::string s = js.str();
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    printf("wrote %s\n", outjson.c_str());
  }
  return 0;
}

// ---------------------------------------------------------------- linksim
int cmd_linksim(const Opts& o) {
  LinkSimOptions lo;
  lo.fs = o.num("fs", 48000);
  lo.tx_profile_a = profile_arg(o, 2, nullptr);
  lo.tx_profile_b = lo.tx_profile_a;
  lo.snr_db = o.num("snr", 20);
  lo.distance_m = o.num("distance", 0.5);
  lo.sro_ppm = o.num("sro", 40);
  lo.trials = (int)o.num("trials", 3);
  lo.payload_bytes = (int)o.num("bytes", 200);
  lo.seed = (uint64_t)o.num("seed", 31337);
  lo.max_ms = (int)o.num("max-ms", 60000);
  lo.burst_loss_prob = o.num("burst-loss", 0);
  lo.agc = !o.flag("no-agc");
  lo.agc_fast = o.flag("fast-agc");
  lo.multipath = !o.flag("no-multipath");
  lo.adapt = o.flag("adapt");  // opt-in, see LinkSimOptions::adapt
  lo.pace_tx = !o.flag("unpaced");  // --unpaced: drain the render queue as fast as we can compute
  lo.device_ms = (int)o.num("device-ms", 0);        // model the sound card's own queue
  lo.device_report = !o.flag("no-device-report");   // do the endpoints get told about it?
  for (int t = 0; t < lo.trials; t++) {
    LinkSimOptions l2 = lo;
    l2.seed = lo.seed + (uint64_t)t * 101u;
    LinkSimResult r = ni_link_sim(l2);
    if (o.flag("json"))
      printf("%s\n", r.to_json().c_str());
    else
      printf("trial %d: %s\n", t + 1, r.to_json().c_str());
  }
  return 0;
}

// ---------------------------------------------------------------- duplex / AEC
int cmd_duplex(const Opts& o) {
  // The full-duplex experiment: both ends transmit continuously, each subtracting its own
  // playback from its own capture with an NLMS filter, and we measure whether the peer's burst
  // is still decodable in the residual. Modelled echo path only - see the NOTE at the end.
  double fs = o.num("fs", 48000);
  int ms = (int)o.num("ms", 3000);
  bool bad = false;
  int pid = profile_arg(o, 11, &bad);
  if (bad) return bad_profile(o);
  size_t n = (size_t)(fs * ms / 1000);
  printf("full-duplex experiment: profile %s, %d ms of simultaneous two-way audio\n",
         ni_profile(pid).name, ms);
  ModemConfig mc;
  mc.sample_rate = fs;
  mc.profile_id = pid;
  mc.tx_amplitude = o.num("amplitude", 0.85);
  Modem tx(mc), rx(mc);
  if (!tx.ok() || !rx.ok()) {
    fprintf(stderr, "modem init failed: %s / %s\n", tx.error().c_str(), rx.error().c_str());
    return 1;
  }
  std::vector<float> ta(n, 0.f), tb(n, 0.f), mic(n, 0.f), resid(n, 0.f);
  std::vector<uint8_t> pay((size_t)tx.max_payload());
  Rng r(99);
  std::vector<float> echo;
  AcousticChannel ch(channel_from(o));
  for (size_t off = 0; off < n; off += (size_t)(fs * 130 / 1000)) {
    r.bytes(&pay[0], pay.size());
    Frame f;
    f.type = NI_FT_DATA;
    f.mod_id = (uint8_t)pid;
    f.session_id = 0x2A2A;
    f.payload = &pay[0];
    f.payload_len = (uint16_t)pay.size();
    std::vector<uint8_t> fr(NI_MAX_FRAME), pdu;
    int fl = ni_frame_encode(&fr[0], fr.size(), f);
    if (fl <= 0) continue;
    ni_phy_wrap(&fr[0], (size_t)fl, &pdu);
    std::vector<float> w;
    if (tx.tx_render(pdu.data(), pdu.size(), &w) != NI_OK) continue;
    for (size_t i = 0; i < w.size() && off + i < n; i++) ta[off + i] = w[i];
  }
  for (size_t i = 0; i < n; i++)
    tb[i] = 0.25f * (float)sin(2.0 * 3.14159265358979323846 * 997.0 * (double)i / fs);
  ch.process(&ta[0], n, &echo);
  for (size_t i = 0; i < n; i++) mic[i] = echo[i] + tb[i];
  NlmsAec aec((int)o.num("taps", 1024), o.num("mu", 0.05), o.num("leak", 1e-4));
  aec.set_delay(0);
  double before = ni_self_interference_db(&echo[0], n, &mic[0]);
  aec.process(&ta[0], n, &mic[0], &resid[0]);
  double after = ni_self_interference_db(&resid[0], n, &mic[0]);
  RxResult rr;
  int drc = rx.rx_capture(&resid[0], resid.size(), &rr);
  printf("  own playback leakage into the capture : %.1f dB below the captured signal\n", before);
  printf("  after %d-tap NLMS subtraction          : %.1f dB (ERLE %.1f dB, misalignment %.1f dB,"
         " converged in %.0f ms)\n",
         aec.taps(), after, aec.erle_db(), aec.misalignment_db(), aec.convergence_ms());
  printf("  peer burst still decodable in residual: %s (%s)\n", drc == NI_OK ? "yes" : "no",
         ni_strerror(drc));
  if (drc == NI_OK)
    printf("     snr %.1f dB  rssi %.1f dBFS  evec %.1f%%\n", rr.stats.snr_db, rr.stats.rssi_dbfs,
           rr.stats.evec);
  printf("NOTE: the echo path here is a model. These numbers size the canceller; they are not\n"
         "      evidence that simultaneous two-way audio works on any real device. Half-duplex\n"
         "      remains the shipped mode (see Gate C in docs/RESULTS.md).\n");
  return 0;
}

// ---------------------------------------------------------------- audit
int cmd_audit(const Opts&) {
  printf("NOISE INTERNET environment audit\n");
  printf("  protocol version      : %d\n", NI_PROTO_VERSION);
  printf("  binary build          : %s %s\n", __DATE__, __TIME__);
  printf("  modulation profiles   : %d\n", ni_profile_count());
  printf("  frame/PDU limits      : frame<=%d B  payload<=%d B  PDU<=%d B\n", NI_MAX_FRAME,
         NI_MAX_PAYLOAD, NI_MAX_BURST_PDU);
  printf("  audio backend         : %s\n",
         ni_audio_backend_available() ? "compiled in" : "not compiled in");
  std::vector<AudioDeviceInfo> dev;
  std::string why;
  int n = ni_audio_enumerate(&dev, &why);
  printf("  audio devices         : %d   (%s)\n", n < 0 ? 0 : n,
         why.empty() ? "enumerated" : why.c_str());
  for (int i = 0; i < n; i++) {
    printf("     [%d] %-28s %-3s %d ch  %.0f Hz  %d bit  %s api=%s%s\n", i,
           dev[i].name.c_str(), dev[i].is_capture ? "in" : "out", dev[i].channels,
           dev[i].nominal_rate, dev[i].bits, dev[i].loopback ? "LOOPBACK(unusable)" : "physical",
           dev[i].api.c_str(), dev[i].min_frames ? "" : "");
  }
  printf("  over-the-air testing  : %s\n",
         n > 0 ? "AVAILABLE - run `ni render`/`ni decode` on two machines"
               : "NOT TESTED - this build host has no audio hardware");
  std::string six = "123456";
  printf("  pairing secret        : \"123456\" measured entropy %.1f bits -> use >= 8 chars\n",
         ni_entropy_bits(six));
  return 0;
}

// ---------------------------------------------------------------- demo
// Runs the two nodes in-process, pumping 1 ms of audio per iteration so the link advances in
// lockstep with the wall-clock budget the requester sees. `child` is whatever external program
// we want to satisfy (curl); we keep spinning the acoustic link until it exits.
struct Pump {
  Node* giver;
  Node* recv;
  double fs;
  std::vector<float> g2r, r2g, buf;
  int64_t* t;
  Pump(Node* a, Node* b, double f, int64_t* tt) : giver(a), recv(b), fs(f), buf(65536), t(tt) {}
  void step(int64_t until_ms) {
    // 10 ms pump blocks: that is what an audio callback actually delivers on Android and Windows,
    // and pumping 1 ms at a time makes the harness (not the modem) the bottleneck. Same reasoning
    // as run_link_sim(), where the change is documented.
    const int blk_ms = 10;
    while (*t < until_ms) {
      *t += blk_ms;
      giver->step(*t);
      recv->step(*t);
      size_t want = (size_t)(fs / 1000.0) * blk_ms + 1;
      buf.resize(want);
      // What the giver's speaker plays, the receiver's microphone hears, and vice versa. The two
      // vectors are named for the journey the samples make (g2r = giver to receiver). They used to be
      // filled from the opposite endpoint and then fed back to the endpoint that produced them, so
      // each node heard its own echo, nothing ever crossed, and `ni demo` could not pair by
      // construction - see reports/RESULTS.md defect 32.
      int g = giver->pull_playback(&buf[0], want);
      if (g > 0) g2r.insert(g2r.end(), buf.begin(), buf.begin() + g);
      g = recv->pull_playback(&buf[0], want);
      if (g > 0) r2g.insert(r2g.end(), buf.begin(), buf.begin() + g);
      if (!g2r.empty()) {
        recv->feed_capture(&g2r[0], g2r.size());
        g2r.clear();
      }
      if (!r2g.empty()) {
        giver->feed_capture(&r2g[0], r2g.size());
        r2g.clear();
      }
    }
  }
  int64_t run(std::vector<uint8_t>* out, const std::string& prog,
              const std::vector<std::string>& args, int64_t budget_ms) {
#ifndef NI_HAVE_SPAWN
    (void)out; (void)prog; (void)args; (void)budget_ms;
    fprintf(stderr,
            "  ni demo needs a POSIX spawn to run the HTTP client, so the proxy leg cannot be\n"
            "  exercised from this binary on this platform. Use the Windows app (it owns the same\n"
            "  proxy in-process); `ni linksim` still measures the link, and the proxy itself is\n"
            "  covered by the unit tests.\n");
    return -1;
#else
    std::string tmpf = "/tmp/ni_demo_" + std::to_string(getpid()) + ".out";
    int fd = open(tmpf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    std::vector<char*> argv;
    argv.push_back((char*)prog.c_str());
    for (size_t i = 0; i < args.size(); i++) argv.push_back((char*)args[i].c_str());
    argv.push_back(nullptr);
    pid_t p = fork();
    if (p < 0) {
      close(fd);
      return -1;
    }
    if (p == 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      close(fd);
      execvp(prog.c_str(), &argv[0]);
      _exit(127);
    }
    close(fd);
    int64_t t_end = *t + budget_ms;
    int status = -1;
    while (*t < t_end) {
      step(*t + 20);
      int st;
      pid_t w = waitpid(p, &st, WNOHANG);
      if (w == p) {
        status = st;
        break;
      }
    }
    if (status < 0) {
      kill(p, SIGKILL);
      waitpid(p, &status, 0);
      status = -1;
    }
    FILE* f = fopen(tmpf.c_str(), "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      out->resize((size_t)sz);
      if (sz > 0 && fread(&(*out)[0], 1, (size_t)sz, f) != (size_t)sz) status = -2;
      fclose(f);
      remove(tmpf.c_str());
    }
    return status;
#endif
  }
};

#if NI_HAVE_SPAWN
// ---------------------------------------------------------------- a deterministic local origin
// The demo's claim is "an HTTP response travelled over the acoustic session intact", and to test that
// it needs an origin whose response it knows byte for byte. A public one cannot serve that purpose
// here: every public origin reachable from this host answers 301 to https (checked: api.github.com,
// registry.npmjs.org, pypi.org, codeload.github.com), and the https route means CONNECT - which the
// stack does implement (the giver's helper opened the tunnel to api.github.com:443 in a measured run)
// but a real TLS handshake over a half-duplex acoustic link failed on its first attempt (curl exit 56,
// reported in reports/RESULTS.md as the state of the CONNECT leg, not as a working fetch). So the
// default origin is this: a plain-HTTP server on the giver's own host serving a fixed document. The
// demo prints that plainly, because "the origin was local" is exactly the sort of thing that must
// never quietly become "we proved internet access".
struct LocalOrigin {
  int lsock = -1, port = 0;
  std::atomic<bool> stop{false};
  std::thread th;
  std::string doc;

  // The document is built to an exact size so a run can say how many bytes crossed the link, and so
  // that the same run can be repeated at a different size. `--document-bytes` sweeps it rather than
  // hand-tuning one number until the demo looks good.
  void build(size_t want) {
    static const char* head = "NOISE INTERNET end-to-end document\n";
    doc = head;
    size_t i = 0;
    while (doc.size() < want) {
      static const char* cycle =
          "0123456789abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ .:-\n";
      doc.push_back(cycle[(i++) % 66]);
    }
    while (doc.size() > want) doc.pop_back();
  }

  bool start(std::string* err, size_t want_bytes) {
    build(want_bytes);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;             // a client that goes away must not kill the demo
    sigaction(SIGPIPE, &sa, nullptr);
    lsock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { *err = "socket failed"; return false; }
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa2;
    memset(&sa2, 0, sizeof sa2);
    sa2.sin_family = AF_INET;
    sa2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa2.sin_port = 0;                    // let the kernel pick a free port
    if (::bind(lsock, (sockaddr*)&sa2, sizeof sa2) != 0) { *err = "bind failed"; return false; }
    if (::listen(lsock, 4) != 0) { *err = "listen failed"; return false; }
    socklen_t sl = sizeof sa2;
    if (getsockname(lsock, (sockaddr*)&sa2, &sl) != 0) { *err = "getsockname failed"; return false; }
    port = (int)ntohs(sa2.sin_port);
    th = std::thread([this] { serve(); });
    return true;
  }

  void serve() {
    while (!stop.load()) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(lsock, &fds);
      timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      int r = ::select(lsock + 1, &fds, nullptr, nullptr, &tv);
      if (r <= 0) continue;
      int c = ::accept(lsock, nullptr, nullptr);
      if (c < 0) continue;
      char buf[4096];
      ssize_t n = ::recv(c, buf, sizeof buf, 0);   // we do not parse: any request gets the document
      if (getenv("NI_GATEWAY_TRACE")) {            // same switch as the giver's helper
        std::string first(buf, n > 0 ? (size_t)n : 0);
        size_t e = first.find("\r\n");
        fprintf(stderr, "[origin] %s\n", e == std::string::npos ? first.c_str()
                                                               : first.substr(0, e).c_str());
        fflush(stderr);
      }
      std::ostringstream resp;
      resp << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " << doc.size()
           << "\r\nConnection: close\r\n\r\n"
           << doc;
      std::string r2 = resp.str();
      size_t off = 0;
      while (off < r2.size()) {
        ssize_t w = ::send(c, r2.data() + off, r2.size() - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
      }
      ::close(c);
    }
  }

  void kill() {
    stop.store(true);
    if (th.joinable()) th.join();
    if (lsock >= 0) ::close(lsock);
    lsock = -1;
  }
};
#endif

int cmd_demo(const Opts& o) {
  bool bad = false;
  double fs = o.num("fs", 48000);
  // 10 = FD-LOW-DBPSK, the profile every measured run has paired and delivered on. The old default
  // was 5 (QAM16-WIDE), which the self-test scores 0/8 in the modelled room, so the one command
  // documented as the end-to-end test was configured to fail before it started.
  int pid = profile_arg(o, 10, &bad);
  if (bad) return bad_profile(o);
  int port = (int)o.num("proxy-port", 8471);
#if NI_HAVE_SPAWN
  LocalOrigin origin;
  std::string origin_kind;
  std::string url = o.get("url", "");
  if (url.empty()) {
    std::string oerr;
    if (!origin.start(&oerr, (size_t)o.num("document-bytes", 400))) {
      fprintf(stderr, "cannot start the local test origin: %s\n", oerr.c_str());
      return 1;
    }
    url = "http://127.0.0.1:" + std::to_string(origin.port) + "/document.txt";
    origin_kind = "local";
  } else {
    origin_kind = "url";
  }
#else
  std::string url = o.get("url", "http://api.github.com/zen");
  std::string origin_kind = "url";
#endif
  int64_t budget = (int64_t)o.num("budget-ms", 400000);
  printf("NOISE INTERNET end-to-end demo\n");
  printf("  role pair  : giver(Windows-like) <-> receiver(Android-like) over a modelled room\n");
  printf("  profile    : %s (adaptive, may move down under load)\n", ni_profile(pid).name);
  printf("  client URL : %s  via  http://127.0.0.1:%d\n", url.c_str(), port);
  printf("  origin     : %s\n",
         origin_kind == "local"
             ? "a plain-HTTP server started by this command on this host (deterministic document).\n"
               "               The REQUEST leg (client -> receiver proxy -> acoustic link -> giver\n"
               "               fetch -> origin) is verified end to end. The RESPONSE leg does not\n"
               "               complete yet (defect 41 in reports/RESULTS.md): the document burst is\n"
               "               pushed, the receiver reports rx_malformed, so the verdict below cannot\n"
               "               read IDENTICAL until that is fixed. This does NOT prove internet access:\n"
               "               the origin is on this host. Pass --url for a real one - and if that URL\n"
               "               is dynamic, use a deterministic one for the byte check.\n"
               "               URL, whose origin must speak plain HTTP (see packaging/windows/README-RUN.md)."
             : "the URL above, on whatever host it resolves to");
  printf("  evidence   : the ONLY path between the two nodes is 16-bit PCM produced by\n"
         "               tx_render and shaped by AcousticChannel. No socket, pipe or shared\n"
         "               buffer carries request bytes.\n\n");
  fflush(stdout);
  NodeConfig gc;
  gc.role = NI_ROLE_GIVER;
  gc.is_master = true;
  gc.profile_id = pid;
  gc.fs = fs;
  gc.session_id = 0x51;
  gc.pairing_secret = o.get("secret", "noise-internet-demo-42");
  gc.enable_giver_fetch = true;
  gc.enable_receiver_proxy = false;
  gc.sim_channel = true;
  gc.name = "giver";
  gc.arq_wait_ms = 120000;
  gc.ch = channel_from(o);
  gc.ch.snr_db = o.num("snr", 26);
  gc.ch.seed = 4242;
  // The demo is the "does the whole path work" command, so its default room is the one the link can
  // actually cross: the modelled multipath defeats every profile tested (see reports/RESULTS.md,
  // gate C), so it is off unless --multipath asks for it. `--no-multipath` stays valid and just
  // repeats the default.
  gc.ch.multipath = o.flag("multipath") && !o.flag("no-multipath");
  NodeConfig vc = gc;
  vc.role = NI_ROLE_RECEIVER;
  vc.is_master = false;
  vc.enable_giver_fetch = false;
  vc.enable_receiver_proxy = true;
  vc.proxy_port = port;
  vc.name = "receiver";
  vc.ch.seed = 99;

  Node giver(gc), recv(vc);
  if (giver.configure() != NI_OK || recv.configure() != NI_OK) {
    fprintf(stderr, "configure failed: %s | %s\n", giver.last_error().c_str(),
            recv.last_error().c_str());
    return 1;
  }
  if (giver.start() != NI_OK || recv.start() != NI_OK) {
    fprintf(stderr, "start failed: %s | %s\n", giver.last_error().c_str(),
            recv.last_error().c_str());
    return 1;
  }
  // Same half-duplex pacing the link simulator uses: a node must stop feeding the sound card when
  // it has nothing to send, otherwise its bursts queue up back-to-back and the peer's reply window
  // (which is derived from the measured airtime) no longer lines up with the wire.
  giver.set_pace_tx(true);
  recv.set_pace_tx(true);
  int64_t t = 0;
  Pump p2(&giver, &recv, fs, &t);
  bool paired = false;
  int64_t deadline = 60000;
  while (t < deadline && !paired) {
    p2.step(t + 5);
    Pairing* pa = giver.link()->pairing();
    Pairing* pb = recv.link()->pairing();
    if (pa && pb && pa->state() == NI_PAIR_DONE && pb->state() == NI_PAIR_DONE) paired = true;
  }
  if (!paired) {
    fprintf(stderr, "pairing did not complete: giver=%s receiver=%s\n",
            giver.link()->pairing() ? giver.link()->pairing()->state_name() : "-",
            recv.link()->pairing() ? recv.link()->pairing()->state_name() : "-");
    // The states alone never said which end was deaf. These two documents carry the burst counters,
    // the learned input level and the sync/CRC failures, which is what a field test needs.
    fprintf(stderr, "giver    : %s\n", giver.status_json().c_str());
    fprintf(stderr, "receiver : %s\n", recv.status_json().c_str());
    return 2;
  }
  printf("  [t=%5lld ms] paired. SAS on both ends: %s\n", (long long)t,
         giver.link()->pairing()->sas().c_str());
  printf("  [t=%5lld ms] state giver=%s receiver=%s  (SESSION_OK)\n", (long long)t,
         ni_state_name(giver.state()), ni_state_name(recv.state()));
  fflush(stdout);
  printf("  [t=%5lld ms] link stats: %s\n", (long long)t, giver.diagnostics_json().c_str());
  fflush(stdout);
  // Each leg captures the response BODY only, plus a one-line trailer that carries the status line,
  // content type and size. Bodies are what can be compared byte for byte; the trailer says whether the
  // two responses are even the same kind of response. The first version captured headers into the same
  // buffer (`-D -`) so the proxied leg was compared to the direct leg with ~70 bytes of the origin's
  // headers included and Date/Request-Id fields in them - a comparison that could never be equal even
  // on a perfect link. Defect 36.
  const std::vector<std::string> tail = {"-w", "\n[ni-http %{http_code} %{content_type} %{size_download}]"};
  auto split_trailer = [](std::vector<uint8_t>& v) {
    std::string s(v.begin(), v.end());
    size_t at = s.rfind("[ni-http ");
    if (at == std::string::npos) return std::string();
    std::string t = s.substr(at);
    while (!t.empty() && (t.back() == ']' || t.back() == '\n' || t.back() == '\r')) t.pop_back();
    v.assign(s.begin(), s.begin() + (long)at);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return t;   // "[ni-http 200 text/plain 26"
  };
  std::vector<std::string> argv_direct = {"-sS", "-L", "-m", "900"};
  std::vector<std::string> argv_proxy = {"-sS", "-L", "-m", "900", "-x",
                                         "http://127.0.0.1:" + std::to_string(port)};
  // The receiver's proxy requires its bearer token on every request; without it the proxy answers 407
  // itself and the "response" never involves an origin at all. The first version of this demo did not
  // send it, so the bytes it measured and the hash it recorded were the proxy's own error page
  // (defect 36). The token is read from the receiver node, which is where it is generated.
  const std::string ptok = recv.proxy_token();
  if (!ptok.empty()) {
    argv_proxy.push_back("--proxy-header");
    argv_proxy.push_back("Proxy-Authorization: Noise " + ptok);
  }
  auto body_only = [&](std::vector<std::string> a) {
    a.push_back(url);
    a.insert(a.end(), tail.begin(), tail.end());
    return a;
  };
  std::vector<uint8_t> direct, direct2, proxied;
  // The two control fetches go FIRST, back to back, before any acoustic traffic: they measure the
  // origin's own behaviour (does the same URL return the same bytes twice?) and they must not be cut
  // short by the acoustic budget - which is exactly what happened when they ran last, leaving the
  // classification to be made from one sample.
  int s2 = p2.run(&direct, "curl", body_only(argv_direct), budget);
  std::string t2 = split_trailer(direct);
  printf("  [t=%5lld ms] curl direct #1   exit=%d, %zu body bytes  %s\n", (long long)t, s2,
         direct.size(), t2.c_str());
  fflush(stdout);
  int s3 = p2.run(&direct2, "curl", body_only(argv_direct), budget);
  std::string t3 = split_trailer(direct2);
  printf("  [t=%5lld ms] curl direct #2   exit=%d, %zu body bytes  %s\n", (long long)t, s3,
         direct2.size(), t3.c_str());
  fflush(stdout);
  bool origin_known = !direct.empty() && !direct2.empty();
  bool origin_stable = origin_known && direct == direct2;
  int s1 = p2.run(&proxied, "curl", body_only(argv_proxy), budget);
  std::string t1 = split_trailer(proxied);
  printf("  [t=%5lld ms] curl via proxy   exit=%d, %zu body bytes  %s\n", (long long)t, s1,
         proxied.size(), t1.c_str());
  // let any pending keepalive/ack traffic flush
  p2.step(t + 300);
  auto code_of = [](const std::string& t) {
    size_t i = t.find(' ');
    if (i == std::string::npos) return -1;
    return atoi(t.c_str() + i + 1);
  };
  const int pcode = code_of(t1);
  // 405/407/503 with our own wording in the body means the local proxy answered this request itself:
  // no request reached an origin, so nothing about the acoustic path can be concluded from it.
  bool answered_by_proxy = proxied.empty() || pcode == 405 || pcode == 407 || pcode == 503;
  printf("\n  through-proxy bytes : %zu   sha256 %s\n", proxied.size(),
         sha_hex(proxied.data(), proxied.size()).c_str());
  printf("  direct #1     bytes : %zu   sha256 %s\n", direct.size(),
         sha_hex(direct.data(), direct.size()).c_str());
  printf("  direct #2     bytes : %zu   sha256 %s\n", direct2.size(),
         sha_hex(direct2.data(), direct2.size()).c_str());
  printf("  proxy token         : %s%s\n", ptok.empty() ? "(none)" : ptok.c_str(),
         ptok.empty() ? " - the proxy has no token, so any local client may use it" : "");
  bool match = !proxied.empty() && proxied == direct;
  bool structure_same = !t1.empty() && t1 == t2;
  if (!origin_known)
    printf("  origin             : unknown - a direct control fetch did not complete (exit=%d/%d, "
           "%zu/%zu bytes), so this run can say nothing about byte equality\n", s2, s3, direct.size(),
           direct2.size());
  else
    printf("  origin             : %s\n",
           origin_stable ? "stable - two direct fetches of this URL returned identical bytes"
                         : "DYNAMIC - two direct fetches of this URL returned different bytes, so "
                           "byte-for-byte equality is not achievable with it");
  size_t first_diff = 0;
  if (!match) {
    size_t n = std::min(proxied.size(), direct.size());
    while (first_diff < n && proxied[first_diff] == direct[first_diff]) first_diff++;
  }
  // One verdict, chosen in order of what the run can actually support. Every earlier version of this
  // block either printed two lines at once or blamed the link for a fact about the URL or the harness.
  if (answered_by_proxy) {
    printf("  comparison          : INVALID RUN - the local proxy answered this request itself "
           "(HTTP %d),\n", pcode);
    printf("                        so no origin response crossed the acoustic link. %s\n",
           pcode == 407 ? "That is a missing proxy token in this harness, not a link result."
                        : "Check the receiver's proxy settings and the URL scheme (https needs the "
                          "CONNECT tunnel).");
  } else if (!origin_known)
    printf("  comparison          : INCONCLUSIVE - %zu bytes came through the acoustic link, but a\n"
           "                        direct control fetch did not complete on this host (exit=%d/%d),\n"
           "                        so there is nothing to compare against. Re-run with a URL this\n"
           "                        host can fetch directly if you want the byte-for-byte check.\n",
           proxied.size(), s2, s3);
  else if (match && structure_same)
    printf("  comparison          : IDENTICAL - the %zu bytes that arrived through the acoustic link\n"
           "                        are the same bytes this host got directly.\n", proxied.size());
  else if (!origin_stable)
    printf("  comparison          : INCONCLUSIVE - DYNAMIC ORIGIN. This URL returns different bytes\n"
           "                        per request (direct fetch #1 %zu B, #2 %zu B), so nothing can be\n"
           "                        byte-identical; the link's response is %zu B%s. Use a\n"
           "                        deterministic document for the byte check, e.g.\n"
           "                        --url 'https://api.github.com/octocat?s=noiseinternet' (821 B).\n",
           direct.size(), direct2.size(), proxied.size(),
           structure_same ? " with a matching status/content-type/size line" : " (STRUCTURE DIFFERS)");
  else
    printf("  comparison          : DIFFERENT - the origin is stable, so this is the link's doing:\n"
           "                        %zu B via the link vs %zu B direct, first differing byte %zu%s\n",
           proxied.size(), direct.size(), first_diff,
           structure_same ? "" : ", and the status/content-type line differs too");
  printf("\n  giver report   : %s\n", giver.export_report_json(t).c_str());
  printf("  receiver report: %s\n", recv.export_report_json(t).c_str());
  std::string out = o.get("json");
  if (!out.empty()) {
    std::ostringstream js;
    js << "{\n \"generator\": \"ni demo\","
       << "\n \"channel\": \"modelled AcousticChannel - NOT speaker-to-microphone\","
       << "\n \"url\": \"" << ni_json_escape(url) << "\","
       << "\n \"origin_kind\": \"" << origin_kind << "\","
       << "\n \"profile\": \"" << ni_profile(pid).name << "\","
       << "\n \"proxy_port\": " << port
       << ",\n \"elapsed_ms\": " << t
       << ",\n \"paired\": " << (paired ? "true" : "false")
       << ",\n \"via_proxy_bytes\": " << proxied.size()
       << ",\n \"via_proxy_sha256\": \"" << sha_hex(proxied.data(), proxied.size()) << "\","
       << "\n \"direct_bytes\": " << direct.size()
       << ",\n \"direct_sha256\": \"" << sha_hex(direct.data(), direct.size()) << "\","
       << ",\n \"direct2_bytes\": " << direct2.size()
       << ",\n \"direct2_sha256\": \"" << sha_hex(direct2.data(), direct2.size()) << "\","
       << ",\n \"origin_stable\": " << (origin_stable ? "true" : "false")
       << ",\n \"structure_same\": " << (structure_same ? "true" : "false")
       << ",\n \"first_differing_byte\": " << (match ? -1LL : (long long)first_diff)
       << ",\n \"identical\": " << (match ? "true" : "false")
       << ",\n \"curl_exit_via_proxy\": " << s1
       << ",\n \"curl_exit_direct\": " << s2
       << ",\n \"giver\": " << giver.export_report_json(t)
       << ",\n \"receiver\": " << recv.export_report_json(t) << "\n}\n";
    FILE* f = fopen(out.c_str(), "wb");
    if (f) {
      std::string str = js.str();
      fwrite(str.data(), 1, str.size(), f);
      fclose(f);
      printf("  wrote %s\n", out.c_str());
    }
  }
#if NI_HAVE_SPAWN
  origin.kill();
#endif
  printf("\nVERDICT: if the hashes match, an arbitrary HTTP response travelled over the\n"
         "acoustic session intact; if the origin itself is dynamic, the run says so instead of\n"
         "blaming the link. What this does NOT prove: real transducers, real latency,\n"
         "or usable browsing speed - the audio path here is a simulation, and the origin fetch\n"
         "by the giver is real.\n");
  return 0;
}

int usage() {
  printf(
      "NOISE INTERNET acoustic modem toolchain\n"
      "\n"
      "  ni profiles [--json] [--fs N]      profiles and their computed per-frame limits\n"
      "  ni plan --profile P --bytes N        airtime / bit-rate plan for one frame\n"
      "  ni render --out burst.wav            write an audible burst carrying a payload\n"
      "        --text \"...\" | --file f | --random N   [--profile --fs --session --amplitude "
      "--pad --bits]\n"
      "  ni decode --in rec.wav               recover a payload from a recording\n"
      "        [--profile P | auto-sweep] [--expect-sha256 H] [--save out.bin]\n"
      "  ni selftest [--profile all]          loopback PHY test through the channel model\n"
      "  ni bench --csv f --json f            profile x speaker x distance x SNR sweep\n"
      "  ni linksim [--trials N]              two-node half-duplex session over the model\n"
      "  ni duplex                            full-duplex band split + NLMS echo canceller\n"
      "  ni demo --url U --json f.json        real HTTP fetch through the receiver's proxy\n"
      "  ni audit                             audio backend / device report\n"
      "\n"
      "channel options (selftest, bench, linksim, demo): --snr --distance --sro --speaker\n"
      "  --mic --volume --no-agc --no-multipath --seed\n"
      "  demo:   --profile --snr --distance --multipath (off by default) --secret --url --budget-ms\n"
      "  linksim: --bytes --trials --max-ms --burst-loss --sro --profile --snr --distance\n"
      "           --device-ms N   model a sound-card queue of N ms (0 = none, the default)\n"
      "           --no-device-report   the queue exists but the endpoint is never told about it\n"
      "           --no-agc --no-multipath --fast-agc --adapt --unpaced --json\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  Opts o = parse(argc, argv, 2);
  if (cmd == "profiles") return cmd_profiles(o);
  if (cmd == "plan") return cmd_plan(o);
  if (cmd == "render") return cmd_render(o);
  if (cmd == "decode") return cmd_decode(o);
  if (cmd == "selftest") return cmd_selftest(o);
  if (cmd == "calibrate") return cmd_calibrate(o);
  if (cmd == "bench") return cmd_bench(o);
  if (cmd == "linksim") return cmd_linksim(o);
  if (cmd == "duplex") return cmd_duplex(o);
  if (cmd == "demo") return cmd_demo(o);
  if (cmd == "audit") return cmd_audit(o);
  if (cmd == "help" || cmd == "--help" || cmd == "-h") return usage();
  fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
  return usage();
}
