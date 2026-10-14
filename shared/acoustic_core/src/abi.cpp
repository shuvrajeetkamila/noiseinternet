// abi.cpp — the flat C ABI. Android's JNI bridge and any Windows shell (Win32 C++ or a
// P/Invoke wrapper) call exactly these functions, so both platforms drive the identical core.
//
// The config is JSON-in so the shells never have to mirror C++ structs across the FFI; the
// parser below is a deliberate "flat scanner", not a JSON implementation: it finds the first
// "key" at any depth and reads its scalar. Anything unparseable falls back to the default in
// NodeConfig, and ni_abi_create reports an error if the text is not an object at all.
#include <stdlib.h>
#include <algorithm>
#include <string.h>
#include <string>
#include <vector>
#include "ni/abi.h"
#include "ni/audio_io.h"
#include "ni/channel_sim.h"
#include "ni/crypto.h"
#include "ni/node.h"

using namespace ni;

namespace {

// ------------------------------------------------------------------ flat JSON scanning
std::string jstring(const std::string& j, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t p = j.find(pat);
  if (p == std::string::npos) return "";
  p = j.find(':', p + pat.size());
  if (p == std::string::npos) return "";
  p++;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
  if (p >= j.size()) return "";
  if (j[p] == '"') {
    size_t e = p + 1;
    std::string out;
    while (e < j.size() && j[e] != '"') {
      if (j[e] == '\\' && e + 1 < j.size()) e++;
      out.push_back(j[e]);
      e++;
    }
    return out;
  }
  size_t e = p;
  while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']' && j[e] != '\n') e++;
  std::string out = j.substr(p, e - p);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\r' || out.back() == '\t'))
    out.pop_back();
  return out;
}

double jnum(const std::string& j, const std::string& key, double deflt) {
  std::string s = jstring(j, key);
  if (s.empty()) return deflt;
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) {
    if (s == "true") return 1;
    if (s == "false") return 0;
    return deflt;
  }
  return v;
}

bool jbool(const std::string& j, const std::string& key, bool deflt) {
  std::string s = jstring(j, key);
  if (s.empty()) return deflt;
  if (s == "false" || s == "0") return false;
  if (s == "true" || s == "1") return true;
  return deflt;
}

// Extract the sub-object of "key": { ... } with balanced-brace matching, so nested keys
// (proxy.port) do not collide with top-level ones.
std::string jobj(const std::string& j, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t p = j.find(pat);
  if (p == std::string::npos) return "";
  p = j.find('{', p);
  if (p == std::string::npos) return "";
  int depth = 0;
  for (size_t i = p; i < j.size(); i++) {
    if (j[i] == '{') depth++;
    else if (j[i] == '}') {
      depth--;
      if (depth == 0) return j.substr(p, i - p + 1);
    }
  }
  return "";
}

int dev_model_index(const std::string& name, int deflt) {
  for (int i = 0; i < NI_DEV_MODEL_COUNT; i++)
    if (name == ni_device_name(i)) return i;
  return deflt;
}

}  // namespace

struct NiNode {
  NodeConfig cfg;
  Node* node = nullptr;
  std::string scratch;
};

extern "C" {

NiNode* ni_abi_create(const char* config_json, char* err, int err_cap) {
  if (err && err_cap) err[0] = 0;
  std::string j = config_json ? config_json : "";
  if (j.empty()) j = "{}";
  size_t a = j.find('{');
  size_t b = j.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b < a) {
    if (err && err_cap) snprintf(err, err_cap, "config must be a JSON object");
    return nullptr;
  }
  NiNode* n = new NiNode();
  NodeConfig& c = n->cfg;
  std::string proxy = jobj(j, "proxy");
  if (proxy.empty()) proxy = j;
  c.name = jstring(j, "name").empty() ? "node" : jstring(j, "name");
  std::string role = jstring(j, "role");
  c.role = (role == "giver") ? NI_ROLE_GIVER : NI_ROLE_RECEIVER;
  if (role.empty()) c.role = NI_ROLE_GIVER;
  c.is_master = jbool(j, "is_master", c.role == NI_ROLE_GIVER);
  std::string prof = jstring(j, "profile");
  if (!prof.empty()) {
    int pid = ni_profile_lookup(prof.c_str());
    char* end = nullptr;
    long v = strtol(prof.c_str(), &end, 10);
    c.profile_id = pid >= 0 ? pid : (end != prof.c_str() ? (int)v : 2);
  }
  c.fs = jnum(j, "fs", 48000);
  c.tx_amplitude = jnum(j, "tx_amplitude", 0.85);
  c.session_id = (uint16_t)(int)jnum(j, "session_id", 0x2A2A);
  c.pairing_secret = jstring(j, "pairing_secret");
  c.allow_plaintext_test = jbool(j, "allow_plaintext_test", false);
  c.duplex = (int)jnum(j, "duplex", 0);
  c.rx_profile_id = (int)jnum(j, "rx_profile", -1);
  c.aec_taps = (int)jnum(j, "aec_taps", 1024);
  c.listen_ms = (int)jnum(j, "listen_ms", 900);
  c.turnaround_ms = (int)jnum(j, "turnaround_ms", 60);
  c.max_retransmit = (int)jnum(j, "max_retransmit", 5);
  c.enable_giver_fetch = jbool(j, "giver_fetch", true);
  c.enable_receiver_proxy = jbool(proxy, "enable", c.role == NI_ROLE_RECEIVER);
  c.proxy_port = (int)jnum(proxy, "port", 8471);
  std::string bind = jstring(proxy, "bind");
  if (!bind.empty()) c.proxy_bind = bind;
  c.proxy_token = jstring(proxy, "token");
  c.sim_channel = jbool(j, "sim_channel", false);
  c.arq_wait_ms = (int)jnum(j, "arq_wait_ms", c.arq_wait_ms);
  c.calibrate_trials = (int)jnum(j, "calibrate_trials", c.calibrate_trials);
  c.ch.fs = c.fs;
  c.ch.snr_db = jnum(j, "snr_db", 22);
  c.ch.distance_m = jnum(j, "distance_m", 0.6);
  c.ch.sro_ppm = jnum(j, "sro_ppm", 0);
  std::string spk = jstring(j, "speaker_model");
  if (!spk.empty()) c.ch.speaker = dev_model_index(spk, c.ch.speaker);
  std::string mic = jstring(j, "mic_model");
  if (!mic.empty()) c.ch.microphone = dev_model_index(mic, c.ch.microphone);
  if (c.proxy_bind != "127.0.0.1" && c.proxy_bind != "localhost") {
    if (err && err_cap)
      snprintf(err, err_cap,
               "proxy_bind must be 127.0.0.1: exposing the acoustic proxy on a routable "
               "interface is refused by design");
    delete n;
    return nullptr;
  }
  n->node = new Node(c);
  int rc = n->node->configure();
  if (rc != NI_OK) {
    if (err && err_cap) snprintf(err, err_cap, "%s", n->node->last_error().c_str());
    delete n->node;
    delete n;
    return nullptr;
  }
  return n;
}

int ni_abi_destroy(NiNode* n) {
  if (!n) return NI_ERR_PARAM;
  delete n->node;
  delete n;
  return NI_OK;
}

int ni_abi_start(NiNode* n) {
  if (!n) return NI_ERR_PARAM;
  return n->node->start();
}
int ni_abi_stop(NiNode* n) {
  if (!n) return NI_ERR_PARAM;
  n->node->stop();
  return NI_OK;
}
int ni_abi_set_tx_backlog(NiNode* n, unsigned frames) {
  if (!n || !n->node) return NI_ERR_PARAM;
  n->node->set_tx_backlog((size_t)frames);
  return NI_OK;
}

int ni_abi_step(NiNode* n, int64_t now_ms) {
  if (!n) return NI_ERR_PARAM;
  return n->node->step(now_ms);
}
int ni_abi_feed_pcm(NiNode* n, const float* pcm, int n_frames) {
  if (!n || !pcm || n_frames < 0) return NI_ERR_PARAM;
  return n->node->feed_capture(pcm, (size_t)n_frames);
}
int ni_abi_pull_pcm(NiNode* n, float* out, int cap) {
  if (!n || !out || cap <= 0) return NI_ERR_PARAM;
  return n->node->pull_playback(out, (size_t)cap);
}
int ni_abi_pull_pcm_needed(NiNode* n) {
  if (!n) return NI_ERR_PARAM;
  return n->node->playback_ready() ? 1 : 0;
}

static char* dup_cstr(const std::string& s) {
  char* p = (char*)malloc(s.size() + 1);
  if (!p) return nullptr;
  memcpy(p, s.data(), s.size());
  p[s.size()] = 0;
  return p;
}

char* ni_abi_status_json(NiNode* n) {
  if (!n) return dup_cstr("{}");
  return dup_cstr(n->node->status_json());
}
char* ni_abi_report_json(NiNode* n) {
  if (!n) return dup_cstr("{}");
  return dup_cstr(n->node->export_report_json(n->node->now_ms));
}
void ni_abi_free(char* p) { free(p); }

int ni_abi_send_bytes(NiNode* n, int stream, int type, const uint8_t* data, int len,
                      int flags) {
  if (!n) return NI_ERR_PARAM;
  if (!data && len != 0) return NI_ERR_PARAM;
  return n->node->send_bytes((uint16_t)stream, (MsgType)type, data, (size_t)len,
                             (uint16_t)flags);
}

int ni_abi_calibrate(NiNode* n, int64_t now_ms, char* out_json, int cap) {
  if (!n || !out_json || cap <= 0) return NI_ERR_PARAM;
  Calibrator::Report rep;
  int rc = n->node->calibrate(now_ms, &rep);
  std::string s = rep.to_json();
  int m = std::min<int>((int)s.size(), cap - 1);
  memcpy(out_json, s.data(), (size_t)m);
  out_json[m] = 0;
  return rc;
}

int ni_abi_test_link(NiNode* n, int64_t now_ms, int payload_bytes, char* out_json, int cap) {
  if (!n || !out_json || cap <= 0) return NI_ERR_PARAM;
  std::string rep;
  int rc = n->node->test_link(now_ms, payload_bytes, &rep);
  int m = std::min<int>((int)rep.size(), cap - 1);
  memcpy(out_json, rep.data(), (size_t)m);
  out_json[m] = 0;
  return rc;
}

int ni_abi_test_internet(NiNode* n, int64_t now_ms, const char* url, char* out_json, int cap) {
  if (!n || !url || !out_json || cap <= 0) return NI_ERR_PARAM;
  InternetVerify iv;
  int rc = n->node->test_internet(now_ms, url, &iv);
  std::string s = iv.to_json();
  int m = std::min<int>((int)s.size(), cap - 1);
  memcpy(out_json, s.data(), (size_t)m);
  out_json[m] = 0;
  return rc;
}

int ni_abi_render(int profile, double fs, double amplitude, uint16_t session_id,
                  const uint8_t* payload, int payload_len, float* out, int out_cap) {
  if (!out || out_cap <= 0) return NI_ERR_PARAM;
  ModemConfig mc;
  mc.sample_rate = fs > 0 ? fs : 48000;
  mc.profile_id = profile;
  mc.tx_amplitude = amplitude > 0 ? amplitude : 0.85;
  mc.session_id = session_id;
  Modem m(mc);
  if (!m.ok()) return NI_ERR_PARAM;
  if (payload_len < 0 || payload_len > m.max_payload()) return NI_ERR_OVERFLOW;
  Frame f;
  f.type = NI_FT_DATA;
  f.mod_id = (uint8_t)profile;
  f.session_id = session_id;
  f.payload = payload;
  f.payload_len = (uint16_t)payload_len;
  std::vector<uint8_t> fr(NI_MAX_FRAME), pdu;
  int fl = ni_frame_encode(&fr[0], fr.size(), f);
  if (fl <= 0) return fl;
  ni_phy_wrap(&fr[0], (size_t)fl, &pdu);
  std::vector<float> w;
  int rc = m.tx_render(pdu.data(), pdu.size(), &w);
  if (rc != NI_OK) return rc;
  if ((int)w.size() > out_cap) return NI_ERR_OVERFLOW;
  memcpy(out, w.data(), w.size() * sizeof(float));
  return (int)w.size();
}

int ni_abi_decode(int profile, double fs, uint16_t session_id, const float* pcm, int n,
                 uint8_t* out, int out_cap, double* snr_out, double* peak_out) {
  if (!pcm || n <= 0) return NI_ERR_PARAM;
  (void)session_id;
  ModemConfig mc;
  mc.sample_rate = fs > 0 ? fs : 48000;
  mc.profile_id = profile;
  Modem m(mc);
  if (!m.ok()) return NI_ERR_PARAM;
  RxResult r;
  int rc = m.rx_capture(pcm, (size_t)n, &r);
  if (rc != NI_OK) return rc;
  Frame f;
  rc = ni_frame_decode(r.pdu.data(), r.pdu.size(), &f);
  if (rc != NI_OK) return rc;
  if (out && out_cap >= f.payload_len) memcpy(out, f.payload, f.payload_len);
  if (!out && f.payload_len > 0) return NI_ERR_OVERFLOW;
  if (snr_out) *snr_out = r.stats.snr_db;
  if (peak_out) *peak_out = ni_peak(pcm, (size_t)n);
  return f.payload_len;
}

const char* ni_abi_version(void) {
  static std::string v = std::string("noise-internet core 0.1, protocol v") +
                         std::to_string(NI_PROTO_VERSION) + ", " __DATE__;
  return v.c_str();
}

const char* ni_abi_profiles_json(void) {
  static std::string cache;
  if (cache.empty()) {
    std::string s = "[";
    for (int i = 0; i < ni_profile_count(); i++) {
      const Profile& p = ni_profile(i);
      CarrierMap cm;
      ni_build_carrier_map(p, 48000, NI_N_FFT, &cm);
      int mp = ni_profile_max_payload(p, 48000);
      char buf[512];
      snprintf(buf, sizeof buf,
               "%s{\"id\":%d,\"name\":\"%s\",\"mod\":\"%s\",\"f_low\":%.0f,\"f_high\":%.0f,"
               "\"bins\":%d,\"bits_per_symbol\":%d,\"max_payload\":%d}",
               i ? "," : "", i, p.name, ni_mod_name(p.mod), p.f_low, p.f_high, cm.n_bins,
               cm.bits_per_symbol, mp);
      s += buf;
    }
    cache = s + "]";
  }
  return cache.c_str();
}

int ni_abi_audio_enumerate(char* out_json, int cap) {
  if (!out_json || cap <= 0) return NI_ERR_PARAM;
  std::vector<AudioDeviceInfo> dev;
  std::string why;
  int n = ni_audio_enumerate(&dev, &why);
  std::string s = "{\"backend\": ";
  s += ni_audio_backend_available() ? "true" : "false";
  s += ", \"count\": ";
  s += std::to_string(n < 0 ? 0 : n);
  s += ", \"note\": \"" + ni_json_escape(why) + "\", \"devices\": [";
  for (int i = 0; i < n; i++) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s{\"i\":%d", i ? "," : "", i);
    s += buf;
    s += ", \"name\": \"" + ni_json_escape(dev[i].name) + "\"";
    s += ", \"api\": \"" + ni_json_escape(dev[i].api) + "\"";
    s += dev[i].is_capture ? ", \"dir\": \"in\"" : ", \"dir\": \"out\"";
    snprintf(buf, sizeof buf, ", \"rate\": %.0f, \"ch\": %d, \"loopback\": %s}",
             dev[i].nominal_rate, dev[i].channels, dev[i].loopback ? "true" : "false");
    s += buf;
  }
  s += "]}";
  if ((int)s.size() + 1 > cap) return NI_ERR_OVERFLOW;
  memcpy(out_json, s.data(), s.size());
  out_json[s.size()] = 0;
  return (int)s.size();
}

}  // extern "C"
