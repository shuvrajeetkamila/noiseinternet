// node.cpp — the whole engine for one device. Every platform shell (Android service, Windows
// app, CLI, simulator, WASM) drives exactly these methods.
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
  o.arq.require_crypto = !cfg_.allow_plaintext_test;
  o.arq.max_retries = cfg_.max_retransmit;
  o.arq.session_id = cfg_.session_id;
  link_.reset(new AcousticLink(o));
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
  if (!cfg_.pairing_secret.empty()) link_->begin_pairing(cfg_.pairing_secret);
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
  if (!link_) return NI_ERR_STATE;
  return link_->feed_pcm(x, n);
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
  if (chan_ && pace_tx_)
    link_->set_tx_backlog(raw_tx_.size() +
                          (ready_tx_.size() > ready_off_ ? ready_tx_.size() - ready_off_ : 0));
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
      uint8_t rep[40];
      memcpy(rep, got, 32);
      ni_wr32(rep + 32, (uint32_t)blen);
      rep[36] = lt_in_.hash_ok ? 1 : 0;
      send_bytes((uint16_t)(kTestStream + kRespBase), NI_MSG_FILE, rep, 37, 0);
    }
    return;
  }
  if (d.stream == (uint16_t)(kTestStream + kRespBase)) {
    if (d.data.size() >= 37 && lt_.pending) {
      lt_.t1 = now_ms;
      lt_.answered = true;
      lt_.got_sha.assign((const char*)d.data.data(), 32);
      lt_.hash_ok = d.data[36] != 0 && lt_.got_sha == lt_.expect_sha;
      lt_.bytes = ni_rd32(&d.data[32]);
      lt_.pending = false;
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
int Node::test_link(int64_t now, int payload_bytes, std::string* report) {
  int rc = ensure_built();
  if (rc != NI_OK) return rc;
  if (!running_) return NI_ERR_STATE;
  if (payload_bytes > link_->max_payload() - 40) payload_bytes = link_->max_payload() - 40;
  Rng r((uint64_t)time(nullptr) ^ 0x2b5u);
  std::vector<uint8_t> body((size_t)payload_bytes);
  r.bytes(&body[0], body.size());
  uint8_t sh[32];
  ni_sha256(&body[0], body.size(), sh);
  std::vector<uint8_t> msg;
  msg.insert(msg.end(), sh, sh + 32);
  msg.insert(msg.end(), body.begin(), body.end());
  lt_.pending = true;
  lt_.answered = false;
  lt_.hash_ok = false;
  lt_.t0 = now;
  lt_.expect_sha = ni_hex(sh, 32);
  link_->arq().send_message(kTestStream, NI_MSG_FILE, msg.data(), msg.size(), 0);
  // pump until answered or timeout; the caller's transport must also be moving PCM here, so
  // this helper is only used by the tests/CLI that drive both endpoints themselves.
  int waited = 0;
  while (!lt_.answered && waited < cfg_.arq_wait_ms) {
    step(now + waited);
    waited += 10;
  }
  std::ostringstream o;
  o << "{\"payload_bytes\": " << payload_bytes << ", \"answered\": " << (lt_.answered ? "true" : "false")
    << ", \"hash_match\": " << (lt_.hash_ok ? "true" : "false") << ", \"elapsed_ms\": " << waited
    << ", \"expect_sha256\": \"" << lt_.expect_sha << "\", \"got_sha256\": \"" << lt_.got_sha << "\"}";
  *report = o.str();
  if (report) test_report = *report;
  return lt_.answered && lt_.hash_ok ? NI_OK : NI_ERR_TIMEOUT;
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
  std::ostringstream s;
  s << (hw_present_ ? "measured on device audio" : "NOT TESTED on hardware: no audio device here")
    << "; profile numbers come from the " << (cfg_.sim_channel ? "simulated acoustic channel" : "loopback waveform path");
  out->summary += "; " + s.str();
  return NI_OK;
}

// ------------------------------------------------------------------ internet test
int Node::test_internet(int64_t now, const std::string& url, InternetVerify* out) {
  if (!rproxy_) return NI_ERR_STATE;
  return ni_verify_internet(url, *rproxy_, now, out);
}

// ------------------------------------------------------------------ snapshots
StatusSnapshot Node::snapshot() const {
  StatusSnapshot s;
  s.name = cfg_.name;
  s.role = ni_role_name(cfg_.role);
  s.state = link_ ? link_->state() : NI_ST_IDLE;
  s.state_name = ni_state_name(s.state);
  s.running = running_;
  s.profile_id = cfg_.profile_id;
  s.profile_name = ni_profile(cfg_.profile_id).name;
  s.modulation = ni_mod_name(ni_profile(cfg_.profile_id).mod);
  s.sample_rate = cfg_.fs;
  s.mtu = link_ ? link_->arq().config().max_payload : 0;
  s.audio_note = audio_note_;
  if (link_) {
    const ArqStats& a = link_->arq().stats();
    s.peer_online = link_->peer_online();
    s.peer_seen_ms = link_->peer_seen_ms();
    s.frames_sent = a.tx_frames;
    s.frames_received = a.rx_frames;
    s.bytes_sent = a.tx_bytes_payload;
    s.bytes_received = a.rx_bytes_payload;
    s.retries = a.tx_retries;
    s.crc_fail = a.rx_crc_fail + a.rx_malformed;
    s.rtt_ms = a.rtt_ms_avg;
    s.loss_pct = 100.0 * (double)a.lost_frames / (double)std::max<uint64_t>(1, a.tx_frames);
    s.useful_bps_down = a.throughput_down_bps;
    s.useful_bps_up = a.throughput_up_bps;
    s.session_ms = a.last_rx_ms ? a.last_rx_ms - a.session_start_ms : 0;
    s.erle_db = link_->erle_db();
    s.snr_db = link_->estimator().band_snr_db();
    auto& l = *link_;
    s.rssi_dbfs = last_phy_.rssi_dbfs;
    s.noise_dbfs = last_phy_.noise_dbfs;
    s.evec_pct = last_phy_.evec;
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
  s.tunnel_up = rproxy_ != nullptr && rproxy_->streams_open() >= 0 && running_;
  s.tunnel_detail = rproxy_ ? rproxy_->describe() : std::string("no local proxy on this role");
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
  o << "{\"name\": \"" << name << "\", \"role\": \"" << role << "\", \"state\": " << state
    << ", \"state_name\": \"" << state_name << "\", \"running\": " << (running ? "true" : "false")
    << ", \"profile\": \"" << profile_name << "\", \"profile_id\": " << profile_id
    << ", \"modulation\": \"" << modulation << "\", \"sample_rate\": " << sample_rate
    << ", \"input_device\": \"" << input_device << "\", \"output_device\": \"" << output_device
    << "\", \"peer_online\": " << (peer_online ? "true" : "false") << ", \"peer_seen_ms\": " << peer_seen_ms
    << ", \"session_ms\": " << session_ms << ", \"snr_db\": " << snr_db << ", \"rssi_dbfs\": " << rssi_dbfs
    << ", \"noise_dbfs\": " << noise_dbfs << ", \"evec_pct\": " << evec_pct << ", \"link_bps\": " << link_bps
    << ", \"useful_up_bps\": " << useful_bps_up << ", \"useful_down_bps\": " << useful_bps_down
    << ", \"loss_pct\": " << loss_pct << ", \"rtt_ms\": " << rtt_ms << ", \"erle_db\": " << erle_db
    << ", \"bytes_sent\": " << bytes_sent << ", \"bytes_received\": " << bytes_received
    << ", \"frames_sent\": " << frames_sent << ", \"frames_received\": " << frames_received
    << ", \"retransmits\": " << retries << ", \"crc_fail\": " << crc_fail << ", \"mtu\": " << mtu
    << ", \"tunnel_up\": " << (tunnel_up ? "true" : "false") << ", \"tunnel_detail\": \"" << tunnel_detail
    << "\", \"internet_ok\": " << (internet_ok ? "true" : "false") << ", \"internet_detail\": \""
    << internet_detail << "\", \"audio_note\": \"" << audio_note << "\"}";
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
