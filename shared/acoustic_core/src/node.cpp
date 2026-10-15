// node.cpp — the whole engine for one device. Every platform shell (Android service, Windows
// app, CLI, simulator, WASM) drives exactly these methods.
#include <chrono>

#include "ni/node.h"
#include <math.h>
#include <sstream>
#include <time.h>
#include "ni/audio_io.h"
#include "ni/crypto.h"

namespace ni {

static const uint16_t kTestStream = 16383;      // receiver -> giver probe
static const uint16_t kTestAckBase = 16384;     // giver -> receiver answer (16383+16384 wraps -> see below)
static const uint16_t kRespBase = 16384;        // giver responses live at request_stream + 16384

Node::Node(const NodeConfig& cfg) : cfg_(cfg) {}
Node::~Node() { stop(); }

int Node::ensure_built() {
  if (built_) return NI_OK;
  if (cfg_.fs < 16000 || cfg_.fs > 192000) return NI_ERR_PARAM;
  LinkOptions o;
  o.modem.sample_rate = cfg_.fs;
  o.modem.profile_id = cfg_.profile_id;
  o.modem.session_id = cfg_.session_id;
  o.modem.tx_amplitude = cfg_.tx_amplitude;
  o.modem.label = cfg_.name;
  o.modem.max_payload = NI_MAX_PAYLOAD;
  o.role = cfg_.role;
  o.is_master = cfg_.is_master;
  o.duplex = cfg_.duplex;
  o.tx_profile = cfg_.profile_id;
  o.rx_profile = cfg_.rx_profile_id;
  o.aec_taps = cfg_.aec_taps;
  o.listen_ms = cfg_.listen_ms;
  o.turnaround_ms = cfg_.turnaround_ms;
  o.assist = cfg_.assist;
  o.arq.require_crypto = !cfg_.allow_plaintext_test;
  o.arq.max_retries = cfg_.max_retransmit;
  o.arq.session_id = cfg_.session_id;
  link_.reset(new AcousticLink(o));
  // The per-burst PHY statistics (RSSI, noise floor, EVM) have to be kept somewhere, and until now they
  // were not kept at all: `last_phy_` was declared, read by snapshot() and never written, so the status
  // panel printed the struct's defaults - rssi -120 dBFS, noise -120 dBFS, EVM 0.00 % - as if they were
  // readings, on every run, on both platforms (defect F). The link already offers the callback; this
  // keeps the newest one and records that a burst was actually demodulated.
  link_->on_stats_ = [this](const PhyStats& st) {
    std::lock_guard<std::mutex> g(phy_mu_);
    last_phy_ = st;
    if (st.bins_used > 0 || st.rssi_dbfs > -119.0) phy_measured_ = true;
  };
  if (cfg_.sim_channel) chan_.reset(new AcousticChannel(cfg_.ch));
  std::vector<AudioDeviceInfo> dev;
  std::string why;
  int n = ni_audio_enumerate(&dev, &why);
  hw_present_ = n > 0 && !dev.empty();
  audio_note_ = hw_present_ ? "" : why;
  if (cfg_.enable_receiver_proxy) {
    ReceiverGateway::Options po;
    po.bind_addr = cfg_.proxy_bind;
    po.port = cfg_.proxy_port;
    po.token = cfg_.proxy_token;
    rproxy_.reset(new ReceiverGateway(po));
    tok_ = rproxy_->token();
    int rc = rproxy_->start();
    if (rc != NI_OK) {
      err_ = "receiver proxy could not bind " + cfg_.proxy_bind + ":" +
             std::to_string(cfg_.proxy_port) + " (in use?)";
      return rc;
    }
    rproxy_->set_sender([this](uint16_t s, int t, const uint8_t* p, size_t n, uint16_t f) -> int {
      return (int)send_bytes(s, (MsgType)t, p, n, f);
    });
  }
  if (cfg_.role == NI_ROLE_GIVER && cfg_.enable_giver_fetch) {
    GiverGateway::Options go;
    gproxy_.reset(new GiverGateway(go));
    gproxy_->set_sender([this](uint16_t s, int t, const uint8_t* p, size_t n, uint16_t f) -> int {
      return (int)send_bytes(s, (MsgType)t, p, n, f);
    });
  }
  link_->set_delivery_handler([this](const Delivered& d) { on_delivered(d); });
  built_ = true;
  return NI_OK;
}

int Node::configure() { return ensure_built(); }

int Node::start() {
  if (running_) return NI_OK;  // see AcousticLink::start(): starting twice is not a restart
  int rc = ensure_built();
  if (rc != NI_OK) return rc;
  // Refuse a start that cannot possibly pair. Without this the engine transmitted nothing at all (no
  // challenge is sent when the passphrase is empty), the peer stayed "offline" for ever, and no error
  // was reported anywhere - which is exactly the report that came back from the first field test
  // ("it never paired, from both the exe and the apk"). The front ends check the field themselves and
  // say so in their own words; this is the guarantee behind them, for every future caller too.
  if (!cfg_.allow_unpaired && (int)cfg_.pairing_secret.size() < cfg_.min_secret_len) {
    pairing_note_ = "no pairing is possible: the passphrase is " +
                    std::to_string(cfg_.pairing_secret.size()) + " characters and at least " +
                    std::to_string(cfg_.min_secret_len) +
                    " are needed. Both devices must be given the SAME passphrase; without one the "
                    "engine never sends a pairing challenge, so the two ends can never pair.";
    err_ = pairing_note_;
    return NI_ERR_PARAM;
  }
  rc = link_->start();
  if (rc != NI_OK) {
    err_ = link_->last_error();
    return rc;
  }
  link_->arq().on_pair = [this](const Frame& f, int64_t t) -> int {
    Pairing* p = link_->pairing();
    if (!p) return (int)NI_ERR_STATE;
    int r = p->on_frame(f, t);
    if (r == (int)NI_OK) {
      if (p->state() == NI_PAIR_DONE) {
        link_->arq().set_keys(&p->keys());
        link_->move_state(NI_ST_PEER_OK, "paired; sas=" + p->sas());
      }
      Frame rf;
      std::vector<uint8_t> pl;
      if (p->next_frame(t, &rf, &pl) == NI_OK)
        link_->arq().send_frame((FrameType)rf.type, pl.data(), pl.size(), true,
                               (uint16_t)(rf.flags & NI_FLAG_PAIR_CONFIRM));
      // The key hand-off has to be re-checked *after* next_frame, not only before it: the
      // initiator flips to DONE inside next_frame (it has nothing left to prove once its own
      // confirmation is on the wire). With the check only above, A never installed its session
      // keys while B did, B then demanded an authenticated MAC on every control frame, A sent
      // unauthenticated ones, and each side silently discarded the other's frames for ever.
      if (p->state() == NI_PAIR_DONE) {
        link_->arq().set_keys(&p->keys());
        link_->move_state(NI_ST_PEER_OK, "paired; sas=" + p->sas());
      }
    }
    return r;
  };
  if (!cfg_.pairing_secret.empty()) {
    link_->begin_pairing(cfg_.pairing_secret);
    if (cfg_.pairing_secret.size() < 8)
      pairing_note_ = "passphrase is only " + std::to_string(cfg_.pairing_secret.size()) +
                      " characters: it works, but it is weak";
  } else {
    // The trap that produced "it never paired on either end": with an empty passphrase this
    // function is never called, so no challenge is ever sent, no match code ever appears, and
    // nothing anywhere says why. The front ends refuse to start in this state; this note is for
    // every other caller, and it travels to the status JSON.
    pairing_note_ = "no passphrase: pairing was never attempted, so this endpoint cannot pair. Both "
                    "ends must be given the same passphrase (8+ characters) before Start.";
  }
  running_ = true;
  return NI_OK;
}

void Node::stop() {
  if (link_) link_->stop();
  if (rproxy_) rproxy_->stop();
  running_ = false;
}

int Node::set_role(Role r) {
  if (running_ && link_ && link_->arq().link_up())
    return NI_ERR_STATE;  // prompt 4: refuse to switch roles in the middle of a session
  if (r == cfg_.role) return NI_OK;
  cfg_.role = r;
  cfg_.is_master = (r == NI_ROLE_GIVER);
  built_ = false;
  link_.reset();
  rproxy_.reset();
  gproxy_.reset();
  chan_.reset();
  if (cfg_.role == NI_ROLE_RECEIVER) cfg_.enable_giver_fetch = false;
  return ensure_built();
}

int Node::set_profile(int pid) {
  if (pid < 0 || pid >= ni_profile_count()) return NI_ERR_PARAM;
  cfg_.profile_id = pid;
  if (link_ && link_->running()) return link_->request_profile(pid);
  built_ = false;
  return ensure_built();
}

int Node::feed_capture(const float* x, size_t n) {
  // A null buffer is a caller bug; a zero-length one is not - "this callback captured nothing" is normal,
  // is what the FFI boundary documents as NI_OK, and t_abi checks it (the tap below must not change that).
  if (!x) return NI_ERR_PARAM;
  if (!n) return NI_OK;
  {
    // The tap, before the link sees anything: this is exactly what the microphone delivered to the
    // engine, which is what the GUI's recorder analyses and saves (see rx_tap_begin).
    std::lock_guard<std::mutex> g(rx_tap_mu_);
    if (rx_tap_on_) rx_tap_.insert(rx_tap_.end(), x, x + n);
  }
  if (!link_) return NI_ERR_STATE;
  return link_->feed_pcm(x, n);
}

void Node::rx_tap_begin() {
  std::lock_guard<std::mutex> g(rx_tap_mu_);
  rx_tap_.clear();
  rx_tap_on_ = true;
}

std::vector<float> Node::rx_tap_end() {
  std::lock_guard<std::mutex> g(rx_tap_mu_);
  rx_tap_on_ = false;
  std::vector<float> out;
  out.swap(rx_tap_);
  return out;
}

// Pull the samples this device must play. With sim_channel enabled the raw burst is run
// through the acoustic model first, which is what makes the two-process and in-process
// emulations physically meaningful: the only bytes that ever leave this node are the
// modelled sound samples.
int Node::pull_playback(float* out, size_t cap) {
  if (!link_) return 0;
  if (!chan_) {
    int got = link_->take_pcm(out, cap);
    if (got && tx_dump_on_)
      tx_dump_.insert(tx_dump_.end(), out, out + got);
    return got;
  }
  float tmp[4096];
  for (;;) {
    int got = link_->take_pcm(tmp, sizeof tmp / sizeof(float));
    if (got <= 0) break;
    raw_tx_.insert(raw_tx_.end(), tmp, tmp + got);
  }
  if (raw_tx_.size() && !link_->tx_ready()) {
    std::vector<float> processed;
    chan_->process(&raw_tx_[0], raw_tx_.size(), &processed);
    ready_tx_.insert(ready_tx_.end(), processed.begin(), processed.end());
    raw_tx_.clear();
  }
  if (ready_off_ >= ready_tx_.size()) {
    ready_tx_.clear();
    ready_off_ = 0;
    return 0;
  }
  size_t avail = ready_tx_.size() - ready_off_;
  size_t take = avail < cap ? avail : cap;
  memcpy(out, &ready_tx_[ready_off_], sizeof(float) * take);
  ready_off_ += take;
  if (tx_dump_on_) tx_dump_.insert(tx_dump_.end(), out, out + take);
  return (int)take;
}

int Node::step(int64_t now) {
  now_ms = now;
  if (!running_) return NI_ERR_STATE;
  // Level hunt while unpaired (NodeConfig::assist). The failure this exists for is the most common one
  // in a real room and the least diagnosable from a panel: the far end hears *something*, but the
  // burst is either too quiet to decode or loud enough to distort in the speaker. Nothing here knows
  // which, and the user has no meter for it, so the level walks a small ladder - 0.85, 0.5, 0.3, 1.0 -
  // every 8 s until the link pairs, and then stays where it landed. Both ends do this independently;
  // they do not have to agree, because this is only about loudness.
  if (cfg_.assist && link_) {
    Pairing* pr = link_->pairing();
    const bool paired = pr != nullptr && pr->state() == NI_PAIR_DONE;
    if (!paired && now_ms >= hunt_next_ms_) {
      static const double kLadder[] = {0.85, 0.5, 0.3, 1.0};
      tx_gain_ = kLadder[(size_t)(hunt_step_++ % 4)];

      link_->set_tx_amplitude(tx_gain_);
      hunt_next_ms_ = now_ms + 8000;
    }
  }
  // Turn-taking is timed against played sound, not handed-out sound. With a modelled channel the
  // node knows the whole queue itself. Without one - that is, in the Windows and Android apps - the
  // only place that knows is the audio backend, which is why the apps call set_tx_backlog() with
  // their device's own queue depth. Until they do, this stays 0 and the link will believe a burst is
  // over the moment the samples left the library, which is what a sound card's buffer is for.
  // Both halves of "audio that has not been played yet": what is still inside this library, and what
  // the audio backend has already taken but not yet put on the speaker. In the apps the first half is
  // always empty (no modelled channel means no internal queue) and the second is whatever the audio
  // device reports; in the simulator it is the other way round plus, when a device queue is modelled,
  // the samples still in flight to the peer.
  size_t own = 0;
  if (chan_ && pace_tx_)
    own = raw_tx_.size() + (ready_tx_.size() > ready_off_ ? ready_tx_.size() - ready_off_ : 0);
  link_->set_tx_backlog(own + device_backlog_);
  link_->tick(now);
  if (rproxy_) rproxy_->pump(now);
  if (gproxy_) gproxy_->pump(now);
  // the peer's NETINFO tells us whether the giver really has internet
  link_->arq().on_netinfo = [this](const NetInfo& ni) -> int {
    peer_netinfo_ = ni;
    peer_netinfo_valid_ = true;
    peer_netinfo_ms_ = now_ms;
    return NI_OK;
  };
  link_->arq().on_cal = [this](const Frame& f, int64_t t) {
    if (f.type == NI_FT_CAL_PROBE) {
      // slave side: count probes and answer with a report
      cal_.got++;
      if (cal_.got % 10 == 0) {
        CalReport rep;
        rep.received = (uint16_t)cal_.got;
        rep.sent = (uint16_t)cal_.sent;
        rep.snr_q8 = (int16_t)lround(link_->estimator().band_snr_db() * 256.0);
        rep.usable = 1;
        rep.payload_bytes = (uint16_t)link_->arq().config().max_payload;
        uint8_t buf[64];
        Writer w(buf, sizeof buf);
        rep.encode(w);
        link_->arq().send_frame(NI_FT_CAL_REPORT, buf, w.size(), true);
      }
      return NI_OK;
    }
    if (f.type == NI_FT_CAL_REPORT) {
      CalReport rep;
      Reader r(f.payload, f.payload_len >= 4 ? f.payload_len - 4 : f.payload_len);
      if (rep.decode(r) == NI_OK) {
        cal_.got = rep.received;
        cal_.done = true;
      }
      return NI_OK;
    }
    (void)t;
    return NI_OK;
  };
  return 0;
}

void Node::on_delivered(const Delivered& d) {
  if (d.stream == kTestStream) {
    // giver side of the link test: verify the hash the sender embedded, echo the verdict
    if (d.data.size() >= 32) {
      uint8_t want[32];
      memcpy(want, d.data.data(), 32);
      const uint8_t* body = d.data.data() + 32;
      size_t blen = d.data.size() - 32;
      uint8_t got[32];
      ni_sha256(body, blen, got);
      lt_in_.bytes = blen;
      lt_in_.expect_sha = ni_hex(want, 32);
      lt_in_.got_sha = ni_hex(got, 32);
      lt_in_.hash_ok = memcmp(want, got, 32) == 0;
      lt_in_.answered = true;
      // The reply carries three things now: the hash this end computed over the far end's payload, the
      // length, and a payload of this end's own - a fresh random nonce and its SHA-256. The first two
      // prove the forward direction; the nonce proves the reverse direction with content the initiator
      // has never seen, so "both directions work" is a measured statement rather than an inference from
      // an empty acknowledgement. (A verdict-only reply could be a valid answer whose own bytes were
      // mangled; this cannot.)
      uint8_t rep[88];
      memset(rep, 0, sizeof rep);
      memcpy(rep, got, 32);
      ni_wr32(rep + 32, (uint32_t)blen);
      rep[36] = lt_in_.hash_ok ? 1 : 0;
      Rng rr((uint64_t)(now_ms * 2654435761u) ^ 0x9e3779b9u);
      rr.bytes(rep + 37, kRevNonce);
      uint8_t rsh[32];
      ni_sha256(rep + 37, kRevNonce, rsh);
      memcpy(rep + 37 + kRevNonce, rsh, 32);
      send_bytes((uint16_t)(kTestStream + kRespBase), NI_MSG_FILE, rep, 37 + kRevNonce + 32, 0);
      // The responder's own view of the forward leg, so a failed comparison can be told apart from a
      // short or padded delivery without guessing (this is what the command prints when the test fails).
      std::ostringstream ro;
      ro << "{\"role\": \"responder\", \"payload_bytes\": " << blen
         << ", \"hash_match\": " << (lt_in_.hash_ok ? "true" : "false")
         << ", \"expect_sha256\": \"" << lt_in_.expect_sha << "\", \"got_sha256\": \""
         << lt_in_.got_sha << "\"}";
      test_report = ro.str();
    }
    return;
  }
  if (d.stream == (uint16_t)(kTestStream + kRespBase)) {
    if (d.data.size() >= 37 && lt_.pending) {
      lt_.t1 = now_ms;
      lt_.answered = true;
      // Hex, not the raw bytes: the check below compares this with expect_sha, which is hex. Assigning
      // the 32 raw bytes here made the comparison impossible to satisfy, so the link test reported
      // hash_match false even when the payload had crossed the air perfectly and the far end had
      // confirmed it. Every "the forward leg failed" reading this command ever produced was that one
      // line. (Defect 66. `reverse_hash_match` was computed with memcmp and stayed correct, which is why
      // the return leg verified and the forward leg "did not".)
      lt_.got_sha = ni_hex(d.data.data(), 32);
      lt_.hash_ok = d.data[36] != 0 && lt_.got_sha == lt_.expect_sha;
      lt_.bytes = ni_rd32(&d.data[32]);
      lt_.pending = false;
      // the reverse leg: the far end's own nonce and the hash it claims for it. Recomputed here, so a
      // corrupted return trip cannot pass by repeating a hash.
      if (d.data.size() >= 37 + kRevNonce + 32) {
        uint8_t want_r[32];
        memcpy(want_r, d.data.data() + 37 + kRevNonce, 32);
        uint8_t got_r[32];
        ni_sha256(d.data.data() + 37, kRevNonce, got_r);
        lt_.reverse_bytes = kRevNonce;
        lt_.reverse_sha = ni_hex(got_r, 32);
        lt_.reverse_ok = memcmp(want_r, got_r, 32) == 0;
      }
    }
    return;
  }
  if (d.stream & kRespBase) {
    if (rproxy_) rproxy_->on_stream(d.stream, d.type, d.data.data(), d.data.size(), d.flags);
  } else if (gproxy_) {
    gproxy_->on_stream(d.stream, d.type, d.data.data(), d.data.size(), d.flags);
  }
  if (msg_) msg_(d);
}

int Node::send_bytes(uint16_t stream, MsgType t, const uint8_t* p, size_t n, uint16_t flags) {
  if (!link_) return NI_ERR_STATE;
  return link_->arq().send_message(stream, t, p, n, flags);
}

void Node::set_message_handler(std::function<void(const Delivered&)> h) { msg_ = h; }

// ------------------------------------------------------------------ self test (no peer needed)
int Node::selftest(int payload_bytes, uint64_t seed, PhyTestResult* out) {
  int rc = ensure_built();
  if (rc != NI_OK) return rc;
  Modem* m = link_->modemp();
  if (!m) return NI_ERR_STATE;
  if (payload_bytes > m->max_payload()) payload_bytes = m->max_payload();
  if (payload_bytes < 1) payload_bytes = 1;
  Rng r(seed ? seed : 1234);
  std::vector<uint8_t> pl((size_t)payload_bytes);
  r.bytes(&pl[0], pl.size());
  ChannelConfig ch = cfg_.ch;
  ch.fs = cfg_.fs;
  return ni_phy_test(*m, *m, &pl[0], pl.size(), ch, out);
}

// ------------------------------------------------------------------ link test (real peer)
// Split into start / poll / report because the blocking form can only work for a caller that keeps
// moving audio between the two ends *while* the test runs. The first version's internal loop called
// step() on this node alone, so in the two-node harness the test could never be answered: it burned its
// whole timeout and reported "not answered" no matter how healthy the link was. A GUI has the same
// problem for a different reason - it must not freeze for the length of a link test.
int Node::start_link_test(int64_t now, int payload_bytes) {
  int rc = ensure_built();
  if (rc != NI_OK) return rc;
  if (!running_) return NI_ERR_STATE;
  if (payload_bytes > link_->max_payload() - 40) payload_bytes = link_->max_payload() - 40;
  if (payload_bytes < 8) payload_bytes = link_->max_payload() - 40;
  // A payload that is unique per run, which is the point of the test: three runs inside the same second
  // used to be seeded from time() alone and therefore tested byte-identical payloads against identical
  // hashes. The seed now mixes the wall clock, a high-resolution tick and a per-process address so two
  // runs cannot share it by accident.
  static uint64_t counter = 0;
  const uint64_t tick =
      (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
  const uint64_t addr = (uint64_t)(uintptr_t)this;
  Rng r((uint64_t)time(nullptr) ^ (tick * 0x9e3779b97f4a7c15ull) ^ (addr << 17) ^ (++counter * 0x2b5u));
  lt_body_.assign((size_t)payload_bytes, 0);
  r.bytes(&lt_body_[0], lt_body_.size());
  uint8_t sh[32];
  ni_sha256(&lt_body_[0], lt_body_.size(), sh);
  std::vector<uint8_t> msg;
  msg.insert(msg.end(), sh, sh + 32);
  msg.insert(msg.end(), lt_body_.begin(), lt_body_.end());
  lt_.pending = true;
  lt_.answered = false;
  lt_.hash_ok = false;
  lt_.reverse_ok = false;
  lt_.reverse_bytes = 0;
  lt_.reverse_sha.clear();
  lt_.got_sha.clear();
  lt_.bytes = 0;
  lt_.t0 = now;
  lt_.expect_sha = ni_hex(sh, 32);
  return link_->arq().send_message(kTestStream, NI_MSG_FILE, msg.data(), msg.size(), 0);
}

bool Node::link_test_finished() const { return lt_.answered; }

std::string Node::link_test_report(int64_t waited) const {
  std::ostringstream o;
  o << "{\"payload_bytes\": " << lt_body_.size() << ", \"answered\": "
    << (lt_.answered ? "true" : "false")
    << ", \"hash_match\": " << (lt_.hash_ok ? "true" : "false")
    << ", \"reverse_payload_bytes\": " << lt_.reverse_bytes
    << ", \"reverse_hash_match\": " << (lt_.reverse_ok ? "true" : "false")
    << ", \"reverse_sha256\": \"" << lt_.reverse_sha << "\""
    << ", \"directions_verified\": " << ((lt_.hash_ok && lt_.reverse_ok) ? 2 : (lt_.hash_ok ? 1 : 0))
    << ", \"elapsed_ms\": " << waited
    << ", \"expect_sha256\": \"" << lt_.expect_sha << "\", \"got_sha256\": \"" << lt_.got_sha
    << "\"}";
  return o.str();
}

int Node::test_link(int64_t now, int payload_bytes, std::string* report) {
  int rc = start_link_test(now, payload_bytes);
  if (rc != NI_OK) return rc;
  // Pump until answered or timeout. The caller's transport must also be moving PCM between the two
  // ends here - that is the whole point of the split API above, and it is why cmd_linktest() drives the
  // pump itself instead of calling this.
  int waited = 0;
  while (!lt_.answered && waited < cfg_.arq_wait_ms) {
    step(now + waited);
    waited += 10;
  }
  if (report) *report = link_test_report(waited);
  test_report = link_test_report(waited);
  // Success means BOTH directions, each verified against content that crossed the air: the payload this
  // end sent, and the far end's own nonce that came back. A run that only got the forward leg is a
  // failure, and the JSON says which leg failed.
  return (lt_.answered && lt_.hash_ok && lt_.reverse_ok) ? NI_OK : NI_ERR_TIMEOUT;
}

// ------------------------------------------------------------------ calibration
int Node::calibrate(int64_t now, Calibrator::Report* out) {
  (void)now;
  int rc = ensure_built();
  if (rc != NI_OK) return rc;
  out->sample_rate = cfg_.fs;
  out->hardware_ok = hw_present_;
  out->input_ok = hw_present_;
  out->output_ok = hw_present_;
  out->permission_ok = hw_present_;
  out->max_output_gain = cfg_.tx_amplitude;
  bool peer = link_ && link_->peer_online();
  bool ultra = false;  // unknown until a device reports it; sim model decides by device profile
  if (cfg_.sim_channel)
    ultra = cfg_.ch.speaker != NI_DEV_PHONE_SPK && cfg_.ch.speaker != NI_DEV_SMART_SPEAKER;
  for (int pid : Calibrator::candidate_profiles(ultra)) {
    Calibrator::BandResult br;
    br.profile_id = pid;
    br.band_id = pid;
    br.name = ni_profile(pid).name;
    int trials_ok = 0, trials = 0;
    double snr_sum = 0, bps_sum = 0, raw_sum = 0, fec_sum = 0;
    bool clipped = false;
    for (double snr : {18.0, 12.0, 7.0}) {
      for (int t = 0; t < cfg_.calibrate_trials; t++) {
        ModemConfig mc;
        mc.sample_rate = cfg_.fs;
        mc.profile_id = pid;
        mc.tx_amplitude = cfg_.tx_amplitude;
        Modem m(mc);
        if (!m.ok()) {
          br.note = m.error();
          trials = -1;
          break;
        }
        int pb = std::min(64, m.max_payload());
        Rng rr(0x99ull + (uint64_t)(pid * 1000 + t));
        std::vector<uint8_t> pl((size_t)pb);
        rr.bytes(&pl[0], pl.size());
        ChannelConfig ch = cfg_.ch;
        ch.fs = cfg_.fs;
        ch.snr_db = snr;
        PhyTestResult pr;
        ni_phy_test(m, m, &pl[0], pl.size(), ch, &pr);
        trials++;
        if (pr.payload_match) trials_ok++;
        snr_sum += pr.snr_db;
        bps_sum += pr.payload_bps;
        raw_sum += pr.raw_ber;
        fec_sum += pr.fec_ber;
        if (pr.error.find("clip") != std::string::npos) clipped = true;
      }
      if (trials < 0) break;
    }
    if (trials > 0) {
      br.frame_success = (double)trials_ok / (double)trials;
      br.snr_db = snr_sum / trials;
      br.payload_bps = bps_sum / trials;
      br.raw_ber = raw_sum / trials;
      br.fec_ber = fec_sum / trials;
      br.clipped = clipped;
      char b[160];
      snprintf(b, sizeof b, "%d/%d frames ok over the simulated channel", trials_ok, trials);
      br.note = b;
    }
    out->bands.push_back(br);
  }
  if (peer) {
    // A real peer also gets an end-to-end probe exchange; the numbers above stay the basis
    // for the profile choice, the peer report is appended as corroboration.
    cal_.active = true;
    cal_.sent = 0;
    cal_.got = 0;
    link_->arq().send_frame(NI_FT_CAL_START, nullptr, 0, true);
  }
  Calibrator::evaluate(out->bands, out);
  // What these numbers actually are: the modem run against its own simulated channel at three signal
  // levels, once per profile. Nothing here touches a microphone or a speaker, and the line used to say
  // "measured on device audio" merely because this machine has sound devices - which is exactly the kind
  // of claim this project forbids, and a field report took it for a room measurement. (Defect 53.)
  std::ostringstream s;
  s << "MODEL SWEEP, NOT A ROOM MEASUREMENT: the modem against its own simulated channel at 18, 12 and " \
       "7 dB SNR, once per profile. No microphone or speaker was involved, and the profile suggestion " \
       "below comes from the model - a real pair chooses its profile from what it actually decodes"
    << (hw_present_ ? "." : ". NOT TESTED on hardware: no audio device on this build host.");
  out->summary += "; " + s.str();
  return NI_OK;
}

// ------------------------------------------------------------------ internet test
int Node::test_internet(int64_t now, const std::string& url, InternetVerify* out) {
  if (!rproxy_) return NI_ERR_STATE;
  return ni_verify_internet(url, *rproxy_, now, out);
}

// ------------------------------------------------------------------ snapshots
// The panel sentence. Written once here so the Windows window, the console and the Android panel all
// say the same thing about the same numbers; the alternative is three UIs each guessing, which is how
// "it does not work" survives three builds.
// The three numbers the two ends must agree on, in the words of whichever end is asking. A person holding
// two devices cannot compare what neither screen shows, and "both ends must match" was advice the panels
// made impossible to follow: the session id was not printed anywhere and the sample rate only in some
// builds (round 6, "make sure both sides match").
static std::string this_ends_numbers(const StatusSnapshot& s) {
  std::ostringstream n;
  n << " THIS end is running profile " << s.profile_id << " (" << s.profile_name << "), session "
    << s.session_id << ", " << (long)(s.sample_rate + 0.5) << " Hz - the other device must show the SAME "
       "three numbers, or nothing can decode.";
  return n.str();
}

static std::string status_advice(const StatusSnapshot& s) {
  std::ostringstream o;
  if (!s.running) {
    return "the engine is not running: press Start. Nothing is transmitted until you do.";
  }
  if (s.pairing == "failed" || s.pairing == "locked") {
    // This is not a guess: the engine reached the cryptographic part of the handshake and the proof did
    // not match, which has exactly one cause - the two passphrases differ. It also used to be reported
    // with no words at all, which is why a mismatch looked identical to "nothing happened".
    return "the two passphrases do not match. The link got as far as checking the proof and it failed, " \
           "so the words typed on the two devices are not the same string (capital letters, spaces and " \
           "punctuation all count). Type the same passphrase on both ends and press Start again.";
  }
  if (s.pairing == "paired") {
    return "paired: both ends show the same match code and data can flow both ways. The rate on this " \
           "panel is what this session delivered, not an internet speed.";
  }
  if (s.in_dbfs <= -119.5) {
    return "no sound has reached the engine at all (the microphone level has never moved off -120 " \
           "dBFS): the microphone path is the problem, not the room. Check the microphone permission, " \
           "close any other app that may hold the microphone, and read the audio line below - it names " \
           "the capture format and the input source that is running.";
  }
  if (s.sync_hits == 0 && s.frames_received == 0) {
    if (s.in_dbfs < -65.0) {
      o << "the microphone hears only a very quiet room (" << (int)(s.in_dbfs + 0.5)
        << " dBFS) and no bursts. Move the two devices 20-30 cm apart, raise the other device's " \
           "volume, and make sure it is started as GIVER - a receiver stays silent until it hears " \
           "something."
        << this_ends_numbers(s);
      return o.str();
    }
    if (s.in_dbfs > -10.0) {
      o << "the microphone input is very loud (" << (int)(s.in_dbfs + 0.5)
        << " dBFS) and no burst decodes: lower the other device's volume or move it further away. A " \
           "too-loud signal distorts in the speaker and is as unusable as a silent one.";
      return o.str();
    }
    o << "the microphone hears the room (" << (int)(s.in_dbfs + 0.5)
      << " dBFS) but no bursts decode: check that the other device is running as GIVER and that no "
         "Bluetooth headset or speaker is in the path."
      << this_ends_numbers(s);
    return o.str();
  }
  // Self-echo is a measured fact now, not an inference: the link compares every burst it decodes with
  // the bursts this device put on the air and drops the ones that are its own voice. So this branch
  // fires on evidence, and frames_received no longer contains echoes (defect 59).
  if (s.self_echo_bursts > 0 && !s.peer_online) {
    o << "the microphone decoded " << s.self_echo_bursts
      << " burst(s) that were this device's OWN transmission coming back (self-echo";
    // "128 decodes of 1 burst" and "128 bursts heard back" are different findings, and the panel used to
    // print the first as if it were the second (defect H).
    if (s.self_echo_repeats)
      o << ", plus " << s.self_echo_repeats
        << " repeat decodes of the same burst - the decoder finding it again in the audio it still holds";
    o << "), and no burst " \
         "from any other device. Speaker, microphone, profile and modem all work on THIS end - the " \
         "proof is that it hears itself clearly enough to decode itself. What has not happened is the " \
         "other device being heard at all: check that it is started, that it is transmitting (a " \
         "receiver is silent until it hears something), and that its speaker actually faces this " \
         "microphone."
      << this_ends_numbers(s);
    return o.str();
  }
  if (s.frames_received == 0) {
    o << "bursts are arriving and their sync is found, but nothing decodes ("  << s.sync_hits
      << " so far): the two ends most likely disagree on the profile number, the session id or the " \
         "passphrase."
      << this_ends_numbers(s);
    return o.str();
  }
  o << "frames are decoding (" << s.frames_received
    << " so far) but pairing has not completed: compare the passphrases - both ends must have the " \
       "SAME passphrase, at least 8 characters. The match code appears on this panel as soon as " \
       "pairing succeeds."
    << this_ends_numbers(s);
  return o.str();
}

std::string SelfHear::to_json() const {
  std::ostringstream o;
  o << "{\"bursts_heard\": " << bursts_heard << ", \"frames_decoded\": " << frames_decoded
    << ", \"in_dbfs\": " << in_dbfs << ", \"verdict\": \"" << verdict << "\"}";
  return o.str();
}

SelfHear Node::self_hear_now() const {
  SelfHear h;
  StatusSnapshot s = snapshot();
  h.bursts_heard = s.sync_hits;
  h.frames_decoded = s.frames_received;
  h.in_dbfs = s.in_dbfs;
  return h;
}

SelfHear Node::self_hear_since(const SelfHear& before, int seconds) const {
  SelfHear h = self_hear_now();
  const uint64_t bursts = h.bursts_heard >= before.bursts_heard ? h.bursts_heard - before.bursts_heard : 0;
  const uint64_t frames = h.frames_decoded >= before.frames_decoded ? h.frames_decoded - before.frames_decoded : 0;
  h.bursts_heard = bursts;
  h.frames_decoded = frames;
  std::ostringstream o;
  if (!link_ || !running_) {
    h.verdict = "the engine is not running, so nothing was listened to.";
    return h;
  }
  if (h.in_dbfs <= -119.5) {
    h.verdict = "YOUR MICROPHONE IS NOT DELIVERING ANY SOUND AT ALL: the level never moved off -120 " \
                "dBFS in " + std::to_string(seconds) + " seconds, so no amount of speaker volume can " \
                "help until that is fixed. Check the microphone permission, close any other app that " \
                "may hold the microphone, and read the audio line below - it names the format and the " \
                "input source that is running.";
    return h;
  }
  if (bursts == 0) {
    o << "NOTHING WAS HEARD: this machine transmitted for " << seconds << " seconds and its own " \
         "microphone did not catch a single burst (newest level " << (int)(h.in_dbfs + 0.5) << " dBFS). " \
         "Either the microphone is not the one you think it is, or the speaker is muted or pointing " \
         "away, or the two devices are too far apart - the sound has to be loud enough to come back.";
    h.verdict = o.str();
    return h;
  }
  if (frames == 0) {
    o << "HEARD BUT NOT READABLE: the microphone caught " << bursts << " burst(s) in " << seconds
      << " seconds and not one of them decoded. The audio is being altered between the speaker and the " \
         "microphone - Windows sound effects (noise suppression, echo cancellation, 'audio " \
         "enhancements') or a device that resamples - or the level is wrong at either end.";
    h.verdict = o.str();
    return h;
  }
  o << "THIS END WORKS: the microphone decoded " << frames << " of this machine's own bursts in "
    << seconds << " seconds while no other device answered. Speaker, microphone and modem are all " \
       "fine here, so whatever still fails is the other device reaching this one.";
  h.verdict = o.str();
  return h;
}

StatusSnapshot Node::snapshot() const {
  StatusSnapshot s;
  s.name = cfg_.name;
  s.role = ni_role_name(cfg_.role);
  s.build = NI_BUILD_STAMP;
  s.session_id = (int)cfg_.session_id;
  s.state = link_ ? link_->state() : NI_ST_IDLE;
  s.state_name = ni_state_name(s.state);
  s.running = running_;
  s.profile_id = cfg_.profile_id;
  s.profile_name = ni_profile(cfg_.profile_id).name;
  s.modulation = ni_mod_name(ni_profile(cfg_.profile_id).mod);
  s.sample_rate = cfg_.fs;
  s.mtu = link_ ? link_->arq().config().max_payload : 0;
  s.audio_note = audio_note_;
  s.tx_gain = tx_gain_;
  if (link_) {
    const ArqStats& a = link_->arq().stats();
    s.peer_online = link_->peer_online();
    s.peer_seen_ms = link_->peer_seen_ms();
    s.frames_sent = a.tx_frames;
    s.frames_received = a.rx_frames;
    s.self_echo_bursts = link_->counters().self_echo;
    s.self_echo_repeats = link_->counters().self_echo_repeats;
    s.bytes_sent = a.tx_bytes_payload;
    s.bytes_received = a.rx_bytes_payload;
    s.retries = a.tx_retries;
    s.crc_fail = a.rx_crc_fail + a.rx_malformed;
    s.rtt_ms = a.rtt_ms_avg;
    s.loss_pct = 100.0 * (double)a.lost_frames / (double)std::max<uint64_t>(1, a.tx_frames);
    // Nothing has been transmitted, so "loss 100.00 %" is not a measurement of anything: it is what
    // 0 lost of max(1,0) transmits prints. The panel says "not measured yet" instead (defect F).
    s.arq_measured = a.tx_frames > 0;
    s.useful_bps_down = a.throughput_down_bps;
    s.useful_bps_up = a.throughput_up_bps;
    s.session_ms = a.last_rx_ms ? a.last_rx_ms - a.session_start_ms : 0;
    s.erle_db = link_->erle_db();
    s.snr_db = link_->estimator().band_snr_db();
    auto& l = *link_;
    {
      // Under the same lock the stats callback takes: PHY numbers are written on the audio thread and
      // read here from the UI thread (see phy_mu_ in node.h).
      std::lock_guard<std::mutex> g(phy_mu_);
      s.rssi_dbfs = last_phy_.rssi_dbfs;
      s.noise_dbfs = last_phy_.noise_dbfs;
      s.evec_pct = last_phy_.evec;
      s.phy_measured = phy_measured_;
    }
    (void)l;
  }
  // link bitrate: derived from the plan of a full-size frame, never from the symbol rate alone
  {
    ModemConfig mc;
    mc.sample_rate = cfg_.fs;
    mc.profile_id = cfg_.profile_id;
    Modem m(mc);
    if (m.ok()) {
      BurstPlan p = m.plan(m.max_payload());
      s.link_bps = p.payload_bitrate;
    }
  }
  // "tunnel up" has to mean a tunnel that is carrying something, not a proxy object that exists.
  // streams_open() counts the sockets currently using the receiver gateway and is never negative, so the
  // old test (>= 0) was true whenever a proxy object had been built and the node was running - which is
  // how one end printed "tunnel up" while the other end of the same link printed "tunnel not requested"
  // (defect E). A tunnel now requires the link to be paired and at least one client to actually be using
  // the proxy; tunnel_detail still says what the proxy is listening for either way.
  const bool paired_now = link_ && link_->pairing() && link_->pairing()->state_name() == std::string("paired");
  s.tunnel_up = rproxy_ != nullptr && running_ && paired_now && rproxy_->streams_open() > 0;
  if (link_ && link_->pairing()) {
    s.pairing = link_->pairing()->state_name();
    s.sas = link_->pairing()->sas();
  }
  s.pairing_note = pairing_note_;
  if (link_) {
    s.in_dbfs = link_->input_dbfs();
    s.sync_hits = link_->counters().sync_hits;
    s.rx_no_frame = link_->counters().rx_no_frame;
  }
  s.tunnel_detail = rproxy_ ? rproxy_->describe() : std::string("no local proxy on this role");
  s.advice = status_advice(s);   // last, because it reads every field above
  if (gproxy_) s.tunnel_detail += " [giver: " + gproxy_->describe() + "]";
  s.internet_ok = peer_netinfo_valid_ && peer_netinfo_.http_ok;
  std::ostringstream io;
  if (!last_verify_.empty())
    io << last_verify_;
  else if (peer_netinfo_valid_)
    io << "giver reports uplink=" << (int)peer_netinfo_.has_uplink << " dns=" << (int)peer_netinfo_.dns_ok
       << " probe=" << peer_netinfo_.probe_host;
  else
    io << "not verified (no NETINFO received yet)";
  s.internet_detail = io.str();
  return s;
}

std::string StatusSnapshot::to_json() const {
  std::ostringstream o;
  o << "{\"name\": \"" << name << "\", \"role\": \"" << role << "\", \"build\": \"" << build
    << "\", \"session_id\": " << session_id << ", \"state\": " << state
    << ", \"state_name\": \"" << state_name << "\", \"running\": " << (running ? "true" : "false")
    << ", \"profile\": \"" << profile_name << "\", \"profile_id\": " << profile_id
    << ", \"modulation\": \"" << modulation << "\", \"sample_rate\": " << sample_rate
    << ", \"pairing\": \"" << pairing << "\", \"sas\": \"" << sas << "\""
    << ", \"pairing_note\": \"" << pairing_note << "\""
    << ", \"in_dbfs\": " << in_dbfs << ", \"sync_hits\": " << sync_hits
    << ", \"rx_no_frame\": " << rx_no_frame
    << ", \"input_device\": \"" << input_device << "\", \"output_device\": \"" << output_device
    << "\", \"peer_online\": " << (peer_online ? "true" : "false") << ", \"peer_seen_ms\": " << peer_seen_ms
    << ", \"session_ms\": " << session_ms << ", \"snr_db\": " << snr_db << ", \"rssi_dbfs\": " << rssi_dbfs
    << ", \"noise_dbfs\": " << noise_dbfs << ", \"evec_pct\": " << evec_pct << ", \"link_bps\": " << link_bps
    << ", \"useful_up_bps\": " << useful_bps_up << ", \"useful_down_bps\": " << useful_bps_down
    << ", \"loss_pct\": " << loss_pct << ", \"rtt_ms\": " << rtt_ms << ", \"erle_db\": " << erle_db
    << ", \"bytes_sent\": " << bytes_sent << ", \"bytes_received\": " << bytes_received
    << ", \"frames_sent\": " << frames_sent << ", \"frames_received\": " << frames_received
    << ", \"self_echo_bursts\": " << self_echo_bursts
    << ", \"self_echo_repeats\": " << self_echo_repeats
    << ", \"retransmits\": " << retries << ", \"crc_fail\": " << crc_fail << ", \"mtu\": " << mtu
    // Both front ends read these to decide whether to print a number or the words "not measured yet"
    // (defect F). They are part of the status contract now, not an extra.
    << ", \"phy_measured\": " << (phy_measured ? "true" : "false")
    << ", \"arq_measured\": " << (arq_measured ? "true" : "false")
    << ", \"tunnel_up\": " << (tunnel_up ? "true" : "false") << ", \"tunnel_detail\": \"" << tunnel_detail
    << "\", \"internet_ok\": " << (internet_ok ? "true" : "false") << ", \"internet_detail\": \""
    << internet_detail << "\", \"tx_gain\": " << tx_gain
    << ", \"advice\": \"" << advice << "\", \"audio_note\": \"" << audio_note << "\"}";
  return o.str();
}

std::string Node::export_report_json(int64_t now) const {
  std::ostringstream o;
  o << "{\n \"node\": " << status_json() << ",\n \"diagnostics\": "
    << (link_ ? link_->diagnostics_json() : "{}") << ",\n \"now_ms\": " << now << ",\n";
  o << " \"link_test\": " << (test_report.empty() ? std::string("{}") : test_report) << ",\n";
  o << " \"proxy_receiver\": {\"requests_ok\": " << (rproxy_ ? rproxy_->requests_ok : 0)
    << ", \"requests_fail\": " << (rproxy_ ? rproxy_->requests_fail : 0) << ", \"abuse\": "
    << (rproxy_ ? rproxy_->abuse : 0) << ", \"bytes_relayed\": " << (rproxy_ ? rproxy_->bytes_relayed : 0)
    << "},\n";
  o << " \"proxy_giver\": {\"fetches_ok\": " << (gproxy_ ? gproxy_->fetches_ok : 0)
    << ", \"fetches_fail\": " << (gproxy_ ? gproxy_->fetches_fail : 0) << ", \"connect_ok\": "
    << (gproxy_ ? gproxy_->connect_ok : 0) << ", \"connect_fail\": " << (gproxy_ ? gproxy_->connect_fail : 0)
    << "}\n}\n";
  return o.str();
}

int Node::start_calibration_probe(int profile_id, int frames, uint16_t* tag) {
  if (!link_) return NI_ERR_STATE;
  cal_.active = true;
  cal_.sent = 0;
  cal_.got = 0;
  cal_.profile = profile_id;
  cal_.frames = frames;
  if (tag) cal_.tag = *tag;
  for (int i = 0; i < frames; i++) {
    CalProbe cp;
    cp.band_id = (uint8_t)profile_id;
    cp.profile_id = (uint8_t)profile_id;
    cp.index = (uint16_t)i;
    cp.frames_expected = (uint16_t)frames;
    cp.tx_gain_q15 = (uint16_t)(cfg_.tx_amplitude * 32768.0);
    uint8_t buf[32];
    Writer w(buf, sizeof buf);
    cp.encode(w);
    uint8_t filler[64];
    Rng r(0x1234u + (uint32_t)i);
    r.bytes(filler, sizeof filler);
    w.bytes(filler, sizeof filler);
    link_->arq().send_frame(NI_FT_CAL_PROBE, buf, w.size(), false);
    cal_.sent++;
  }
  return NI_OK;
}

}  // namespace ni
