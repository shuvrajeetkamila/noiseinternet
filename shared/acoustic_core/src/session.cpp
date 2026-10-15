#include <stdio.h>
#include <stdlib.h>
// session.cpp — AcousticLink: PHY glue, duplex scheduling, adaptation, echo cancellation.
//
// Half-duplex rule (default, prompt 5.6): the link MASTER (normally the giver) owns the
// schedule. It transmits a burst, then listens for `listen_ms`. The SLAVE transmits only in
// its reply window and only when it has something to say (an ACK, a NAK, or piggybacked
// data). No two devices ever transmit at the same time, so nothing here needs collision
// handling. Full-duplex (duplex=1) is a separate experiment: each direction uses its own band
// and our own playback is subtracted from our capture with an NLMS canceller.
#include <algorithm>
#include <math.h>
#include <sstream>
#include "ni/session.h"

namespace ni {

static ArqConfig arq_from_options(const LinkOptions& o, int modem_max_payload) {
  ArqConfig a = o.arq;
  a.mod_id = (uint8_t)o.modem.profile_id;
  a.session_id = o.modem.session_id;
  int mp = modem_max_payload - 40;  // leave room for the fragment header + AEAD tag
  if (a.max_payload > mp) a.max_payload = mp;
  if (a.max_payload < 24) a.max_payload = 24;
  return a;
}

AcousticLink::AcousticLink(const LinkOptions& opt)
    // declaration order (arq_, est_, adapt_) - not the order that reads nicely: a mismatch here is
    // a silent initialisation-order hazard, which the compiler is right to warn about.
    : opt_(opt), arq_(opt.arq), est_(0.3, 128), adapt_(opt.modem.profile_id) {}

AcousticLink::~AcousticLink() {
  delete modem_;
  delete modem_alt_;
}

int AcousticLink::start() {
  // Idempotent, and it has to be: a second call used to build a fresh Modem without releasing the
  // first (a leak that never frees, because the pointer was simply overwritten) and to re-issue
  // begin_pairing(), which throws away the nonces the peer is already working from. The link
  // simulator hit this because it started each node twice, and every pairing then failed its HMAC
  // check - the two ends derived keys from different nonce pairs.
  if (running_) return NI_OK;
  // The peer can ask us to change profile; the link is the only thing that owns a modem, so it is the
  // link that applies the change. Without this hook the far end's ARQ changed its bookkeeping while
  // its modem kept rendering and decoding the old profile.
  arq_.on_profile_request = [this](int pid) { apply_profile(pid); };
  modem_ = new Modem(opt_.modem);
  if (!modem_->ok()) {
    last_error_ = modem_->error();
    move_state(NI_ST_ERROR, last_error_);
    delete modem_;
    modem_ = nullptr;
    return NI_ERR_UNSUPPORTED;
  }
  if (opt_.duplex == 1) {
    ModemConfig alt = opt_.modem;
    alt.profile_id = opt_.rx_profile < 0 ? alt.profile_id : opt_.rx_profile;
    modem_alt_ = new Modem(alt);
    if (!modem_alt_->ok()) {
      last_error_ = std::string("rx band modem: ") + modem_alt_->error();
      move_state(NI_ST_ERROR, last_error_);
      return NI_ERR_UNSUPPORTED;
    }
    aec_.reset(new NlmsAec(opt_.aec_taps, (double)opt_.aec_step / 1000.0));
    played_.assign((size_t)(opt_.modem.sample_rate * 0.25), 0.f);
  }
  ArqConfig ac = arq_from_options(opt_, modem_->max_payload());
  arq_.config_mut() = ac;
  cap_.clear();
  cap_read_ = 0;
  running_ = true;
  txq_.clear();
  tx_off_ = 0;
  move_state(NI_ST_HARDWARE_OK,
              std::string("modem ready: ") + modem_->profile().name + " @ " +
                  std::to_string((int)opt_.modem.sample_rate) + " Hz, mtu " +
                  std::to_string(modem_->max_payload()));
  return NI_OK;
}

void AcousticLink::stop() {
  running_ = false;
  txq_.clear();
  tx_off_ = 0;
  cap_.clear();
  move_state(NI_ST_IDLE, "stopped by user");
}

void AcousticLink::move_state(int s, const std::string& why) {
  if (s == state_ && !on_state_) return;
  state_ = s;
  if (on_state_) on_state_(s, why);
}

double AcousticLink::burst_airtime_ms(int payload_bytes) const {
  if (!modem_) return 0.0;
  BurstPlan p = modem_->plan((size_t)payload_bytes + 20 + 2);
  return p.duration_ms;
}

// Move our own modem to `pid`. Only ever called when the peer is known to be moving too: either
// because the peer asked us (on_profile_request) or because it acknowledged our request.
int AcousticLink::apply_profile(int pid) {
  if (!modem_) return NI_ERR_STATE;
  ModemConfig c = opt_.modem;
  c.profile_id = pid;
  Modem* m = new Modem(c);
  if (!m->ok()) {
    std::string e = m->error();
    delete m;
    last_error_ = "cannot switch to profile " + std::to_string(pid) + ": " + e;
    return NI_ERR_UNSUPPORTED;
  }
  delete modem_;
  modem_ = m;
  opt_.modem.profile_id = pid;          // the burst descriptor now carries the new modulation id
  opt_.arq.mod_id = (uint8_t)pid;
  arq_.config_mut().mod_id = (uint8_t)pid;
  arq_.config_mut().max_payload =
      std::min(m->max_payload() - 40, opt_.arq.max_payload > 0 ? opt_.arq.max_payload : 228);
  bursts_since_adapt_ = 0;
  pending_profile_ = -1;
  last_adapt_reason_ = "switched to " + std::string(ni_profile(pid).name);
  return NI_OK;
}

int AcousticLink::begin_pairing(const std::string& secret) {
  if (!running_) return NI_ERR_STATE;
  PairingConfig pc;
  pc.secret = secret.empty() ? std::string("noise-internet-default-no-tunnel") : secret;
  pc.session_id = opt_.modem.session_id;
  pc.self_is_a = opt_.is_master;
  uint16_t caps = 0;
  for (int i = 0; i < ni_profile_count(); i++) caps |= (uint16_t)(1u << (i & 15));
  pc.caps = caps;
  pair_.reset(new Pairing(pc));
  int64_t now = now_ms_;
  Frame f;
  std::vector<uint8_t> pl;
  int rc = pair_->next_frame(now, &f, &pl);
  if (rc == NI_OK) arq_.send_frame((FrameType)f.type, pl.data(), pl.size(), true);
  else if (rc != NI_ERR_STATE) return rc;
  return NI_OK;
}

int AcousticLink::send_file(uint16_t stream, const uint8_t* data, size_t n) {
  return arq_.send_message(stream, NI_MSG_FILE, data, n, 0);
}

int AcousticLink::request_profile(int pid) {
  if (!modem_) return NI_ERR_STATE;
  // Validate the target before asking anyone to move: a request we cannot honour locally would leave
  // the peer on a profile we are unable to use.
  {
    ModemConfig probe_cfg = opt_.modem;
    probe_cfg.profile_id = pid;
    Modem probe(probe_cfg);
    if (!probe.ok()) {
      last_error_ = "cannot switch to profile " + std::to_string(pid) + ": " + probe.error();
      return NI_ERR_UNSUPPORTED;
    }
  }
  int rc = arq_.request_profile(pid);
  if (rc != NI_OK) return rc;
  // NOT applied here. The old code rebuilt our modem on the spot and merely asked the peer to do the
  // same; when that request was lost - and it is a best-effort frame under loss - this end was on a
  // new profile while the peer stayed on the old one, which is deafness with no recovery path. The
  // switch now happens in tick(), and only after the peer acknowledges (see apply_profile).
  pending_profile_ = pid;
  pending_profile_ms_ = now_ms_;
  last_adapt_reason_ = "requested " + std::to_string(pid) + ", waiting for the peer's ack";
  return NI_OK;
}

// ---------------------------------------------------------------- capture path
int AcousticLink::feed_pcm(const float* x, size_t n) {
  if (!running_ || !modem_) return NI_ERR_STATE;
  if (!n) return NI_OK;
  size_t keep = (size_t)(opt_.modem.sample_rate * 1.2);
  cap_.insert(cap_.end(), x, x + n);
  if (cap_.size() > keep * 2) {
    // never let the capture window grow unbounded; keep the newest 1.2 s
    cap_.erase(cap_.begin(), cap_.begin() + (cap_.size() - keep));
  }
  if (aec_ && !played_.empty()) {
    // subtract our own playback from the newest samples (full-duplex experiment)
    size_t off = cap_.size() > n ? cap_.size() - n : 0;
    std::vector<float> ref(n, 0.f), resid(n, 0.f);
    size_t pend = play_write_;
    for (size_t i = 0; i < n; i++) {
      size_t idx = (pend + i) % played_.size();
      ref[i] = played_[idx];
    }
    aec_->process(&ref[0], n, &cap_[off], &resid[0]);
    for (size_t i = 0; i < n; i++) cap_[off + i] = resid[i];
    erle_ = aec_->erle_db();
  }
  // ---- receive squelch -------------------------------------------------------
  // The burst detector is O(window): rescanning the whole 1.2 s capture buffer on every
  // 1 ms callback would be 1000x the work of the audio itself and would blow the deadline on a
  // phone. So the detector only runs when the newest block is clearly above the learned noise
  // floor, or while a burst is in flight (mid_burst_), which is set by the NI_ERR_SHORT
  // "burst still arriving" return and expires 400 ms after the last one.
  double e2 = 0.0;
  for (size_t i = 0; i < n; i++) e2 += (double)x[i] * (double)x[i];
  double new_rms = sqrt(e2 / (double)n);
  in_dbfs_ = 20.0 * log10(new_rms > 1e-9 ? new_rms : 1e-9);
  if (new_rms < noise_rms_)
    noise_rms_ = 0.2 * noise_rms_ + 0.8 * new_rms;   // noise floor falls fast
  else
    noise_rms_ = 0.9999 * noise_rms_ + 0.0001 * new_rms;  // and rises only slowly
  if (mid_burst_ && now_ms_ - burst_hold_ms_ > 400) { mid_burst_ = false; burst_start_ = 0; }
  Modem* demod = opt_.duplex == 1 && modem_alt_ ? modem_alt_ : modem_;
  RxResult r;
  if (cap_.size() < (size_t)(2 * (NI_N_FFT + modem_->cp())) ||
      (!mid_burst_ && new_rms < std::max(2e-4, noise_rms_ * 3.0)))
    return NI_ERR_NOFRAME;  // squelched: nothing to detect in this block
  int rc;
  if (mid_burst_ && burst_start_ < cap_.size()) {
    // We already know where this burst starts, so skip the detector: rx_decode reads the sync and
    // descriptor and returns NI_ERR_SHORT as soon as a symbol is missing, which costs a handful of
    // FFTs instead of a full correlation scan of the window.
    rc = demod->rx_decode(&cap_[0], cap_.size(), burst_start_, &r);
  } else {
    burst_start_ = 0;
    rc = demod->rx_capture(&cap_[0], cap_.size(), &r);
  }
  if (rc == NI_ERR_SHORT) {
    // Burst found but not fully received: keep it from its sync symbol onwards. That bounds the
    // work of every later callback AND stops the "newest 1.2 s" trim above from cutting the head
    // off a burst that is still arriving.
    mid_burst_ = true;
    burst_hold_ms_ = now_ms_;
    c_.sync_hits++;   // a burst is there; it just is not readable (yet, or at this profile)
    size_t head = r.start_pos > burst_start_ ? r.start_pos : burst_start_;
    if (head > 0 && head <= cap_.size()) {
      cap_.erase(cap_.begin(), cap_.begin() + head);
      burst_start_ = 0;
    }
    return rc;  // burst still arriving; try again on the next feed
  }
  mid_burst_ = false;
  burst_start_ = 0;
  if (rc == NI_ERR_NOFRAME) {
    c_.rx_no_frame++;
    if (cap_.size() > keep) cap_.erase(cap_.begin(), cap_.begin() + (cap_.size() - keep));
    return rc;
  }
  size_t adv = r.consumed ? r.consumed : 0;
  size_t pos = r.start_pos;
  last_peer_burst_ms_ = now_ms_;
  if (pos + adv <= cap_.size()) cap_.erase(cap_.begin(), cap_.begin() + (pos + adv));
  else cap_.clear();
  c_.rx_bursts++;
  if (rc != NI_OK) {
    if (rc == NI_ERR_CRC || rc == NI_ERR_DECODE || rc == NI_ERR_BITSLIP) c_.rx_crc_fail++;
    est_.observe_hard_failure();
    if (on_stats_) on_stats_(r.stats);
    return rc;
  }
  if (!r.pdu.empty()) handle_pdu(r.pdu.data(), r.pdu.size(), r.stats, now_ms_);
  if (on_stats_) on_stats_(r.stats);
  // feed the estimator with the per-bin channel
  if (!r.h_re.empty()) {
    double nv = r.stats.noise_var > 1e-18 ? r.stats.noise_var : 1e-18;
    est_.observe(&r.h_re[0], &r.h_im[0], nv, (int)r.h_re.size());
  }
  bursts_since_adapt_++;
  if (on_phy_) on_phy_(r);
  return NI_OK;
}

// Is this burst the echo of something this device just sent? Exact byte comparison against the ring of
// what went out, not a timing heuristic: a real peer cannot produce our PDU, and our own echo decodes to
// exactly the bytes we rendered. The window is generous because a slow renderer can put a burst on the
// air well after transmit() was called.
// FNV-1a: a few lines, no allocation, and this only ever compares hashes of bursts this device itself
// produced, so it does not need to be cryptographically anything.
static uint64_t ni_hash64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

bool AcousticLink::is_own_echo(const uint8_t* pdu, size_t n, int64_t now) const {
  if (!n) return false;
  const uint64_t h = ni_hash64(pdu, n);
  for (int i = 0; i < kTxEchoRing; ++i) {
    const TxEcho& e = tx_echo_[i];
    if (!e.ms) continue;
    // the same bytes as sent, or the same burst as the receiver sees it (without the length prefix)
    const bool same = e.hash == h || (e.hash_body && e.hash_body == h);
    const bool len_ok = (size_t)n == e.n || (size_t)n + NI_PDU_LEN_BYTES == e.n;
    if (same && len_ok && now - e.ms >= 0 && now - e.ms <= NI_SELF_ECHO_WINDOW_MS) return true;
  }
  return false;
}

void AcousticLink::handle_pdu(const uint8_t* pdu, size_t n, const PhyStats& st, int64_t now) {
  // Before the ARQ sees it, and before peer_seen_ms_ can be touched: this device's own voice.
  if (is_own_echo(pdu, n, now)) {
    // One physical echo, one count. The decoder is fed overlapping capture windows, so the same own
    // burst is routinely decoded more than once a few milliseconds apart; counting each decode made the
    // panel say "self-echo 128" for a single transmitted frame (field log, 2026-10-15). Repeats are kept
    // in their own counter rather than thrown away, because "the same burst decoded 128 times" is itself
    // evidence about the capture path.
    const uint64_t h = ni_hash64(pdu, n);
    bool counted_before = false;
    for (int i = 0; i < kEchoCountRing; ++i) {
      const EchoCount& e = echo_count_[i];
      if (e.hash == h && e.ms && now - e.ms >= 0 && now - e.ms <= NI_SELF_ECHO_WINDOW_MS) {
        counted_before = true;
        break;
      }
    }
    if (counted_before) {
      c_.self_echo_repeats++;
    } else {
      c_.self_echo++;
      echo_count_[echo_count_next_].hash = h;
      echo_count_[echo_count_next_].ms = now;
      echo_count_next_ = (echo_count_next_ + 1) % kEchoCountRing;
    }
    last_error_ = "own transmission decoded back through the local microphone (self-echo)";
    return;
  }
  int accepted = arq_.on_pdu(pdu, n, now);
  if (accepted > 0) {
    peer_seen_ms_ = now;
    if (state_ < NI_ST_FRAME_OK) move_state(NI_ST_FRAME_OK, "valid frame decoded");
    if (arq_.paired() && state_ >= NI_ST_FRAME_OK) move_state(NI_ST_SESSION_OK, "ARQ running");
  }
  int16_t snr = (int16_t)lround(st.snr_db * 256.0);
  arq_.set_snr(snr, (uint8_t)std::min(255.0, std::max(0.0, st.snr_db * 12.0)));
  // adaptive modulation: evaluate every few bursts, act on the decision
  // Never before the link is up: the pairing handshake must not be disturbed by either end deciding
  // to change profile in the middle of it.
  if (adaptive_ && arq_.paired() && arq_.link_up() && bursts_since_adapt_ >= 6) {
    bursts_since_adapt_ = 0;
    double tot = (double)(c_.rx_bursts + c_.rx_crc_fail);
    double success = tot > 0 ? (double)c_.rx_bursts / tot : 1.0;
    auto d = adapt_.update(est_, success, st.snr_db, arq_.active_profile());
    if (d.changed && d.profile_id != arq_.active_profile()) request_profile(d.profile_id);
    last_adapt_reason_ = d.reason;
  }
  if (on_deliver_) {
    Delivered dm;
    while (arq_.next_delivery(&dm)) on_deliver_(dm);
  } else {
    Delivered dm;
    while (arq_.next_delivery(&dm)) { /* drained so queues cannot grow unbounded */ }
  }
}

// ---------------------------------------------------------------- transmit path
void AcousticLink::clamp_burst_to_phy() {
  if (!modem_) return;
  int cap = modem_->max_payload() - (int)NI_PDU_LEN_BYTES;
  if (cap < (int)(NI_HEADER_BYTES + NI_CRC32_BYTES + NI_PDU_LEN_BYTES))
    cap = (int)(NI_HEADER_BYTES + NI_CRC32_BYTES + NI_PDU_LEN_BYTES);
  ArqConfig& ac = arq_.config_mut();
  if (cap < ac.max_burst_bytes) ac.max_burst_bytes = cap;
}

// ---------------------------------------------------------------- transmit path
int AcousticLink::render_burst(const uint8_t* pdu, size_t n) {
  std::vector<float> wave;
  int rc = modem_->tx_render(pdu, n, &wave);
  if (rc != NI_OK) {
    // This used to `return;` in silence. The ARQ had already marked the frames in this burst as
    // in flight and the caller counted the burst, so a message whose packing exceeded the modem's
    // per-burst capacity was dropped on the floor while both endpoints' counters said it had been
    // transmitted - the exact shape of the "acks flow, nothing is delivered" failure in
    // reports/RESULTS.md. Now it is a counted, named failure and the frames go back in the queue.
    c_.tx_render_fail++;
    last_error_ = "tx_render refused a " + std::to_string(n) + "-byte burst: " + ni_strerror(rc);
    arq_.cancel_burst();
    return rc;
  }
  txq_.insert(txq_.end(), wave.begin(), wave.end());
  if (aec_ && !played_.empty()) {
    for (size_t i = 0; i < wave.size(); i++) {
      played_[play_write_] = wave[i];
      play_write_ = (play_write_ + 1) % played_.size();
    }
  }
  return NI_OK;
}

void AcousticLink::transmit(const uint8_t* pdu, size_t n, int64_t now_ms) {
  if (render_burst(pdu, n) != NI_OK) return;  // no wave, no burst, no "sent" bookkeeping
  pending_tx_frames_ = last_packed_frames_;
  tx_played_ms_ = (double)now_ms;
  c_.tx_bursts++;
  // Remember what went out, so hearing it come back can be recognised as hearing ourselves rather than
  // as the far end finally answering (defect 59).
  tx_echo_[tx_echo_next_].hash = ni_hash64(pdu, n);
  tx_echo_[tx_echo_next_].hash_body =
      n > NI_PDU_LEN_BYTES ? ni_hash64(pdu + NI_PDU_LEN_BYTES, n - NI_PDU_LEN_BYTES) : 0;
  tx_echo_[tx_echo_next_].ms = now_ms;
  tx_echo_[tx_echo_next_].n = n;
  tx_echo_next_ = (tx_echo_next_ + 1) % kTxEchoRing;
}

int AcousticLink::take_pcm(float* out, size_t cap) {
  size_t avail = txq_.size() - tx_off_;
  if (!avail) return 0;
  size_t take = std::min(avail, cap);
  memcpy(out, &txq_[tx_off_], sizeof(float) * take);
  tx_off_ += take;
  if (tx_off_ >= txq_.size()) {
    // burst is fully handed to the audio pipeline
    txq_.clear();
    tx_off_ = 0;
  }
  return (int)take;
}

// How often an assisting slave says something when it has heard nothing. Long enough not to eat the
// master's airtime, short enough that a person watching the panel sees activity within a few seconds.
static const int64_t kAssistBeaconMs = 4000;

void AcousticLink::set_tx_amplitude(double a) {
  opt_.modem.tx_amplitude = a < 0.02 ? 0.02 : (a > 1.0 ? 1.0 : a);
  if (modem_) modem_->set_tx_amplitude(opt_.modem.tx_amplitude);
  if (modem_alt_) modem_alt_->set_tx_amplitude(opt_.modem.tx_amplitude);
}

int AcousticLink::tick(int64_t now_ms) {
  now_ms_ = now_ms;
  if (!running_ || !modem_) return NI_ERR_STATE;
  if (txq_.empty() && tx_backlog_ == 0) {
    if (pending_tx_frames_) {
      double airtime = 1000.0 * (double)tx_samples_total_ / opt_.modem.sample_rate;
      arq_.note_emitted(pending_tx_frames_, now_ms, airtime);
      last_tx_end_ms_ = now_ms;
      listen_until_ms_ = now_ms + opt_.listen_ms + (int)airtime;
      pending_tx_frames_ = 0;
      tx_samples_total_ = 0;
    }
  }
  // "playing" must include what the transport has queued but not yet put on the speaker: a burst
  // is not finished when it is copied out of our buffer, only when it has been played. Otherwise a
  // caller that drains faster than real time (the simulator, or an AudioTrack with a deep buffer)
  // is handed burst after burst, the TDD turn-taking no longer matches the wire, and the RTT and
  // throughput figures measure how fast the host can render rather than how fast sound travels.
  bool playing = !txq_.empty() || tx_backlog_ > 0;
  // A profile change is committed only when the peer has confirmed it, and it is abandoned if that
  // confirmation never arrives: a lost request then costs a few seconds instead of the session. Both
  // ends end up on the same profile, or both stay on the old one.
  if (pending_profile_ >= 0) {
    if (arq_.take_profile_ack()) {
      apply_profile(pending_profile_);
    } else if (now_ms - pending_profile_ms_ > 5000) {
      arq_.cancel_profile_request();
      last_adapt_reason_ = "profile request timed out; staying on the current profile";
      pending_profile_ = -1;
    }
  }
  arq_.on_timer(now_ms);

  // A receiver that has heard nothing used to be silent in the literal sense - no burst, no sound, no
  // evidence that its transmitter works - which is indistinguishable from an app whose audio is dead.
  // With assist on it emits a KEEPALIVE beacon every few seconds even when it has heard nothing, so
  // the other end can measure this direction and the user can hear the receiver is alive.
  Pairing* pr = pairing();
  const bool paired_now = pr != nullptr && pr->state() == NI_PAIR_DONE;
  bool beacon_due = false;
  bool may_tx = false;
  if (!playing) {
    if (opt_.duplex == 1) {
      may_tx = true;  // FDD: our band is ours
    } else if (opt_.is_master) {
      may_tx = now_ms - last_tx_end_ms_ >= opt_.turnaround_ms;
    } else {
      // slave: speak in the reply window after the master's burst; and, when assisting, speak anyway
      // on the beacon interval, so the pair is never a silent mystery.
      beacon_due = opt_.assist && !paired_now && now_ms - last_tx_end_ms_ >= kAssistBeaconMs;
      const bool heard_recently =
          now_ms - last_peer_burst_ms_ >= opt_.turnaround_ms &&
          now_ms - last_peer_burst_ms_ < opt_.turnaround_ms + 4 * std::max(200, (int)airtime_est_ms_);
      may_tx = (arq_.tx_pending() || ack_due_ || beacon_due) && heard_recently;
      if (!may_tx && (arq_.tx_pending() || beacon_due) && now_ms - last_peer_burst_ms_ > opt_.listen_ms + 2000)
        may_tx = true;  // master has gone quiet: take a turn so ACKs are not stuck
    }
  }
  if (may_tx) {
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    size_t len = 0;
    // The burst can never be bigger than what one burst of this profile carries. Clamping only in
    // ArqConfig was not enough - the node assigns its own ArqConfig after the link is built, which
    // put max_burst_bytes back to 1024 - so the limit is applied here, at the buffer, where nothing
    // can overwrite it. Without it, every multi-frame burst was refused by tx_render (NI_ERR_OVERFLOW)
    // while the ARQ had already marked those frames in flight: the frames never went out, never got
    // acked, and were re-packed on every tick. See reports/RESULTS.md 4.6.
    size_t phy = (size_t)modem_->max_payload();
    size_t cap = phy < sizeof buf ? phy : sizeof buf;
    int n = arq_.build_pdu(now_ms, buf, cap, &len, opt_.is_master || beacon_due);
    if (n > 0 && len >= 2) {
      last_packed_frames_ = (size_t)n;
      tx_samples_total_ = (size_t)modem_->burst_samples_for(len);
      transmit(buf, len, now_ms);
      airtime_est_ms_ = 1000.0 * (double)tx_samples_total_ / opt_.modem.sample_rate;
      if (!opt_.is_master) ack_due_ = false;
    }
  }
  // liveness: the peer counts as offline after 4 missed polls. Frames stay queued so the
  // session recovers by itself when the peer comes back (prompt 5.5 "session recovery").
  return 0;
}

std::string AcousticLink::diagnostics_json() const {
  std::ostringstream o;
  o << "{\"role\": \"" << ni_role_name(opt_.role) << "\", \"duplex\": \""
    << (opt_.duplex == 1 ? "FDD-full" : "TDD-half") << "\", \"link_master\": "
    << (opt_.is_master ? "true" : "false") << ", \"state\": " << state_ << ", \"state_name\": \""
    << ni_state_name(state_) << "\", \"profile\": \""
    << (modem_ ? modem_->profile().name : "?") << "\", \"sample_rate\": "
    << opt_.modem.sample_rate << ", \"tx_gain\": " << opt_.modem.tx_amplitude
    << ", \"mtu\": " << (modem_ ? modem_->max_payload() : 0)
    << ", \"turnaround_ms\": " << opt_.turnaround_ms << ", \"listen_ms\": " << opt_.listen_ms
    << ", \"measured_airtime_ms\": " << airtime_est_ms_ << ", \"peer_online\": "
    << (peer_seen_ms_ && now_ms_ - peer_seen_ms_ < 8000 ? "true" : "false")
    << ", \"aec_erle_db\": " << erle_ << ", \"band_snr_db\": " << est_.band_snr_db()
    << ", \"bad_bins\": " << est_.bad_bins(4.0) << ", \"coherence\": " << est_.coherence()
    << ", \"adaptation\": \"" << last_adapt_reason_ << "\""
    << ", \"pairing\": \"" << (pair_ ? pair_->state_name() : "none") << "\""
    << (pair_ ? ", \"pairing_failures\": " + std::to_string((int)pair_->failures()) : "")
    << ", \"self_echo_bursts\": " << c_.self_echo
    << ", \"counters\": {\"rx_bursts\": " << c_.rx_bursts << ", \"tx_bursts\": " << c_.tx_bursts
    << ", \"rx_crc_fail\": " << c_.rx_crc_fail << ", \"rx_no_frame\": " << c_.rx_no_frame
    << ", \"tx_render_fail\": " << c_.tx_render_fail << ", \"self_echo\": " << c_.self_echo
    << ", \"self_echo_repeats\": " << c_.self_echo_repeats << "}"
    << ", \"arq\": " << arq_.stats().to_json() << "}";
    ;
  return o.str();
}

}  // namespace ni
