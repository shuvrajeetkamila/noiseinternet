// session.h — reliability layer (ARQ) + duplex scheduling.
//
// The ArqEngine is pure logic over MAC frames: no floats, no audio, no wall clock. It is
// therefore exhaustively unit-testable (loss, retransmission, duplicates, out-of-order,
// timeouts, MTU fragmentation, sequence wrap, replay protection, bounded retries) and the
// same object is what the Android and Windows apps run.
//
// AcousticLink ties ArqEngine + Modem + an AudioTransport and implements the turn-taking
// half-duplex scheduler (master polls / slave answers on ACK) and, as a separate mode, the
// frequency-division full-duplex experiment with AEC.
#ifndef NI_SESSION_H
#define NI_SESSION_H
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ni/aec.h"
#include "ni/channel_estimator.h"
#include "ni/common.h"
#include "ni/crypto.h"
#include "ni/frame_codec.h"
#include "ni/modem.h"

namespace ni {

// ---------------- message layer carried inside DATA frames ----------------
enum MsgType : uint8_t {
  NI_MSG_STREAM = 0,   // opaque bytes for a stream
  NI_MSG_OPEN = 1,     // open stream
  NI_MSG_CLOSE = 2,    // close stream (peer half)
  NI_MSG_ABORT = 3,    // stream aborted (error)
  NI_MSG_FILE = 4,     // labelled bulk transfer (test payload, hash check)
  NI_MSG_SHELL = 5,    // reserved: never enabled
  NI_MSG_COUNT = 6
};
enum : uint16_t { NI_MFLAG_FIRST = 1, NI_MFLAG_LAST = 2, NI_MFLAG_URGENT = 4 };

struct Fragment {  // 12 bytes at the front of a DATA payload (inside the AEAD)
  uint16_t stream;
  uint8_t type;
  uint8_t flags;
  uint32_t offset;
  uint16_t len;
  uint16_t reserved;
  void encode(uint8_t* p) const;
  int decode(const uint8_t* p, size_t n);
};

struct Delivered {
  uint16_t stream = 0;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint32_t offset = 0;
  uint16_t len = 0;
  std::vector<uint8_t> data;
  uint32_t seq = 0;
};

struct ArqStats {
  uint64_t tx_frames = 0, tx_retries = 0, tx_bytes_payload = 0;
  uint64_t rx_frames = 0, rx_frames_dup = 0, rx_crc_fail = 0, rx_malformed = 0;
  // Why the last frame was refused. rx_malformed on its own tells an operator nothing usable, and
  // "the peer is not sending DATA" and "the AEAD tag does not verify" look identical from outside.
  // 1 aead-open, 2 plaintext refused after pairing, 3 frame decode, 4 version, 5 control-frame
  // tag, 6 sequence far outside the duplicate window, 7 fragment decode/reassembly.
  int rx_err = 0;
  int rx_err_type = 0;  // FrameType of the frame that was refused
  int rx_rc = 0;        // the error code the refusal came with (NI_ERR_*), for the log/CLI
  uint64_t rx_bytes_payload = 0;
  uint64_t acks_sent = 0, nacks_sent = 0, bursts_tx = 0, bursts_rx = 0;
  uint64_t lost_frames = 0, pending = 0;
  double rtt_ms_avg = 0.0, rtt_ms_last = 0.0, rtt_ms_max = 0.0;
  double throughput_up_bps = 0.0, throughput_down_bps = 0.0;
  int64_t session_start_ms = 0, last_rx_ms = 0;
  double erasure_rate() const {
    uint64_t t = rx_frames + lost_frames;
    return t ? (double)lost_frames / (double)t : 0.0;
  }
  std::string to_json() const;
};

struct ArqConfig {
  int max_payload = 228;      // DATA fragment payload limit (post-encryption budget)
  int ack_timeout_ms = 1200;  // scaled from the measured burst airtime by AcousticLink
  int max_retries = 5;
  int poll_interval_ms = 1500;  // master keepalive/poll when idle
  int max_frames_per_burst = 6;
  int max_burst_bytes = 1024;
  bool require_crypto = true;   // refuse DATA when unpaired (no plaintext fallback)
  uint16_t session_id = 0;
  uint8_t mod_id = 2;
  int dup_window = 512;         // replay window / unordered acceptance span
};

class ArqEngine {
 public:
  explicit ArqEngine(const ArqConfig& cfg);

  const ArqConfig& config() const { return cfg_; }
  ArqConfig& config_mut() { return cfg_; }

  // ---- transmit -------------------------------------------------------------
  // Fragment and queue an application message. Returns NI_ERR_OVERFLOW if it exceeds the
  // per-stream cap. `more` lets a large body span several DATA frames.
  int send_message(uint16_t stream, MsgType type, const uint8_t* data, size_t n,
                   uint16_t mflags = 0);
  // `extra_flags` carries pairing bits that must survive onto the wire (NI_FLAG_PAIR_CONFIRM);
  // 0 keeps the previous behaviour.
  int send_frame(FrameType ft, const uint8_t* payload, size_t n, bool want_ack,
                 uint16_t extra_flags = 0);
  bool tx_pending() const { return !txq_.empty(); }
  uint64_t tx_seq_next() const { return tx_seq_; }

  // callbacks the owning session installs
  std::function<int(const Frame&, int64_t)> on_pair;
  std::function<int(const NetInfo&)> on_netinfo;
  // The peer asked us to change modulation profile. The LINK installs this (it owns the modem); the
  // ARQ only knows that a request arrived. Applying it here is what keeps both ends on one profile.
  std::function<void(int)> on_profile_request;
  std::function<int(const Frame&, int64_t)> on_cal;
  void set_snr(int16_t snr_q8, uint8_t quality) { last_snr_q8_ = snr_q8; last_quality_ = quality; }
  void flag_nack() { nack_pending_ = true; ack_dirty_ = true; }
  bool profile_request_pending() const { return profile_req_pending_; }

  // Build a burst PDU (concatenated MAC frames, pdu_len prefix excluded). Respects
  // `cap` and the airtime bound. Returns frames packed, or 0 when there is nothing to say
  // (`allow_poll` adds a POLL/KEEPALIVE so the slave can reply).
  int build_pdu(int64_t now_ms, uint8_t* pdu, size_t cap, size_t* out_len, bool allow_poll);
  // Called by the transport once the waveform really went out (starts RTT/timeout timers).
  void note_emitted(size_t frames_in_burst, int64_t now_ms, double airtime_ms);
  // Release the frames of a burst that was never rendered (see AcousticLink::render_burst).
  void cancel_burst();

  // ---- receive --------------------------------------------------------------
  // Parse a decoded PDU: validates every frame, applies the ack/bitmap state, and queues
  // deliveries. `bad_crc` counts are for the report. Returns number of frames accepted.
  int on_pdu(const uint8_t* pdu, size_t n, int64_t now_ms);
  bool next_delivery(Delivered* out);

  // ---- crypto / pairing ------------------------------------------------------
  void set_keys(const SessionKeys* k);  // nullptr clears (unpaired)
  const SessionKeys* keys() const { return keys_; }
  bool paired() const { return keys_ != nullptr; }
  void set_rng_seed(uint64_t s) { rng_.s = s ? s : 1; }
  ReplayWindow& replay() { return replay_; }

  // ---- timers / state --------------------------------------------------------
  // Returns true when a retransmission became due.
  bool on_timer(int64_t now_ms);
  bool link_up() const { return link_up_; }
  int state() const { return state_; }
  void set_state(int s) { state_ = s; }
  const ArqStats& stats() const { return st_; }
  ArqStats& stats_mut() { return st_; }
  // Request the peer to change profile (returns a CTRL frame payload).
  int request_profile(int profile_id);
  // True once, after the peer acknowledged our profile request. The link waits for this before it
  // moves its own modem, so a lost request costs nothing instead of the whole session.
  bool take_profile_ack() {
    bool a = profile_acked_;
    profile_acked_ = false;
    return a;
  }
  void cancel_profile_request() { profile_req_active_ = false; }
  bool profile_request_in_flight() const { return profile_req_active_; }
  int active_profile() const { return cfg_.mod_id; }
  // Time until the next event the caller must service (ms). -1 = no timer.
  int next_event_ms(int64_t now_ms) const;
  void reset_counters();
  // Forced re-sync (used after a link-down -> up transition).
  void soft_reset();

 private:
  struct TxItem {
    Frame f;
    std::vector<uint8_t> payload;  // plaintext fragment (encrypted at pack time)
    bool want_ack = false;
    int64_t sent_ms = 0;
    int64_t next_retry_ms = 0;
    int retries = 0;
    bool in_flight = false;
    bool acked = false;
    bool giveup = false;
  };
  int pack_frames(int64_t now_ms, uint8_t* pdu, size_t cap, size_t* out_len, int max_frames,
                  int max_bytes);
  int deliver_data(const Frame& f, int64_t now_ms);
  void apply_report(uint32_t next_seq, uint32_t bitmap, int64_t now_ms);
  LinkReport make_report() const;

  ArqConfig cfg_;
  std::vector<TxItem> txq_;
  uint64_t tx_seq_ = 0;   // reliable counter (32 bit on the wire)
  uint64_t rx_next_ = 0;  // next in-order seq we expect from the peer
  uint32_t rx_bitmap_ = 0;
  std::vector<Delivered> deliverq_;
  std::map<uint32_t, std::vector<uint8_t>> ooo_;  // (seq - rx_next_) -> whole frame image
  bool ack_dirty_ = false;
  bool nack_pending_ = false;
  int16_t last_snr_q8_ = 0;
  uint8_t last_quality_ = 0;
  const SessionKeys* keys_ = nullptr;
  uint64_t tx_nonce_ = 1;
  ReplayWindow replay_;
  ArqStats st_;
  int state_ = NI_ST_IDLE;
  bool link_up_ = false;
  Rng rng_{0x5eed1234ull};
  int64_t last_emit_ms_ = 0;
  int64_t last_rx_ms_ = 0;
  int64_t last_poll_ms_ = 0;
  int64_t rtt_sum_ = 0;
  uint64_t rtt_n_ = 0;
  int64_t bytes_up_win_start_ = 0;
  uint64_t bytes_up_win_ = 0;
  uint16_t ctrl_profile_ = 0xFFFF;
  // A profile change has to be a two-sided event. The endpoint that asks for one must not switch its
  // own modem before the peer has confirmed, or the two ends can end up on different profiles with no
  // way back - which is what used to happen, on both apps, because adaptation was enabled by default.
  // The request travels as a reliable CTRL frame; `profile_acked_` is set when that frame is
  // acknowledged, and AcousticLink switches its own modem only then. See defect 33.
  bool profile_req_active_ = false;
  bool profile_acked_ = false;
  int64_t profile_req_ms_ = 0;
  bool profile_req_pending_ = false;
  uint32_t tx_ctl_ = 0;
  std::vector<uint32_t> burst_frames_;
};

// ---------------- PCM interface ----------------
// The engine is deliberately transport-agnostic and pull/push based, because that is the
// shape every real platform can offer:
//   * Android: AudioTrack thread calls take_pcm(), AudioRecord thread calls feed_pcm()
//   * Windows: the WASAPI render callback calls take_pcm(), the capture callback feed_pcm()
//   * tests / two-process emulation: a runner moves PCM between two engines (optionally
//     through AcousticChannel), which is the ONLY place bytes change hands.
// A backend that cannot be opened returns NI_ERR_NOAUDIO; nothing here fakes a device.

struct LinkOptions {
  ModemConfig modem;
  ArqConfig arq;
  bool is_master = true;   // link master = the device that polls (default: the giver)
  Role role = NI_ROLE_GIVER;
  int duplex = 0;          // 0 = half duplex turn-taking, 1 = FDD full duplex (experiment)
  int tx_profile = -1;     // full duplex only: profile used for our TX band
  int rx_profile = -1;     // full duplex only: profile used for our RX band
  int turnaround_ms = 60;  // guard between our burst end and the peer's reply
  int listen_ms = 900;     // how long we stay in RX after a burst
  int aec_taps = 1024;     // full duplex only: NLMS taps
  int aec_step = 30;       // mu*1000
  bool allow_plaintext_test = false;  // tests only
  // Real-hardware assistance, off by default so that every measured sweep in reports/ keeps meaning
  // what it meant. When on: the slave emits a KEEPALIVE beacon in its reply window even when it has
  // heard nothing (otherwise a receiver that hears nothing is completely silent, which is
  // indistinguishable from a broken app), and the app sweeps its own output level while unpaired
  // (NodeConfig::assist).
  bool assist = false;
};

// Drives one endpoint. Thread-safety: call from one thread; feed_pcm may be called from an
// audio callback thread (it is mutex-protected).
class AcousticLink {
 public:
  explicit AcousticLink(const LinkOptions& opt);
  ~AcousticLink();

  int start();   // validates the configuration, moves state to HARDWARE_OK when told so
  void stop();
  bool running() const { return running_; }

  // Feed captured samples (called by the backend or the emulator).
  int feed_pcm(const float* x, size_t n);
  // Pull PCM that must be played now. Returns samples copied (0 when idle).
  int take_pcm(float* out, size_t cap);
  bool tx_ready() const { return txq_.size() > tx_off_; }
  // The transport calls this with the number of samples of ours that are still queued for
  // playback (its own buffers, not just ours). See AcousticLink::tick: turn-taking has to be
  // measured against what has actually gone out over the wire.
  void set_tx_backlog(size_t samples) { tx_backlog_ = (int64_t)samples; }
  // Runtime output level in full-scale fractions (see Modem::set_tx_amplitude).
  void set_tx_amplitude(double a);
  double tx_amplitude() const { return opt_.modem.tx_amplitude; }
  // Service timers, do ARQ work, decide to transmit. Called every ~5-10 ms of *device* time.
  int tick(int64_t now_ms);

  ArqEngine& arq() { return arq_; }
  const ArqEngine& arq() const { return arq_; }
  Modem& modem() { return *modem_; }
  Modem* modemp() { return modem_; }
  const Modem* modem_rx() const { return modem_; }
  ChannelEstimator& estimator() { return est_; }
  const RateAdaptation& adaptation() const { return adapt_; }
  int state() const { return state_; }
  std::string last_error() const { return last_error_; }

  // Application-facing
  int send_file(uint16_t stream, const uint8_t* data, size_t n);
  void set_delivery_handler(std::function<void(const Delivered&)> h) { on_deliver_ = h; }
  void set_state_handler(std::function<void(int, const std::string&)> h) { on_state_ = h; }
  // Diagnostics export (JSON), used by both UIs and by the report generator.
  std::string diagnostics_json() const;
  const ArqStats& stats() const { return arq_.stats(); }
  double measured_latency_ms() const { return rtt_display_; }
  int max_payload() const { return modem_ ? modem_->max_payload() : 0; }
  double burst_airtime_ms(int payload_bytes) const;
  // Full-duplex: last measured ERLE in dB (0 when AEC not active).
  double erle_db() const { return erle_; }
  bool peer_online() const { return peer_seen_ms_ > 0; }
  int64_t peer_seen_ms() const { return peer_seen_ms_; }
  // Request the peer to switch modulation (and apply locally once acked).
  int request_profile(int pid);
  // Commit a profile change locally. Public because the ARQ's peer-request hook calls it; both
  // directions of the negotiation end up here, and neither end moves without the other.
  int apply_profile(int pid);
  bool adaptive() const { return adaptive_; }
  void set_adaptive(bool on) { adaptive_ = on; }
  // Pairing access for the UI.
  Pairing* pairing() { return pair_.get(); }
  int begin_pairing(const std::string& secret);

  struct Counters {
    uint64_t rx_bursts = 0, tx_bursts = 0, rx_crc_fail = 0, rx_no_frame = 0, dup = 0;
    uint64_t tx_render_fail = 0;  // bursts the modem refused to render (see clamp_burst_to_phy)
    // A burst whose sync sequence was found but which could not be read: the single most useful
    // number when "it never pairs" (see this count next to rx_bursts and rx_no_frame).
    //   sync_hits > 0, rx_bursts == 0  -> signal is arriving, nothing decodes (wrong profile on one
    //                                    end, or the level is too high/low)
    //   sync_hits == 0, rx_no_frame    -> the receiver hears no burst at all (route, volume, mic)
    uint64_t sync_hits = 0;
    // Bursts this device decoded that turned out to be ITS OWN transmission arriving back through its
    // own speaker-to-microphone path. They are not a peer, they must not move the pairing state, and
    // they used to be counted as received frames: the phone reported "decoded 48" with "bytes received
    // 0" because 48 of the 48 were the device hearing itself. (Defect 59.)
    uint64_t self_echo = 0;
    // The same own burst decoded again, usually because the capture ring still holds the audio and the
    // decoder finds it a second time on the next pass. Counted apart from self_echo so that number
    // answers "how many of my own bursts came back" (one) instead of "how many times did the decoder
    // look at it" (128 in the 2026-10-15 field log, from a single transmitted frame). (Defect H.)
    uint64_t self_echo_repeats = 0;
  };
  // The last few burst PDUs this device put on the air, newest last. An echo of our own transmission
  // decodes to the bytes we sent, so an incoming PDU that matches one of these (within
  // NI_SELF_ECHO_WINDOW_MS) is this device hearing itself - an exact test, unlike a timing guess that
  // would have to decide whether a fast peer answered or the room did. The ring is what makes the
  // distinction possible at all before pairing, when there is no key to validate against.
  // Two hashes per slot: the bytes as transmitted (with the 2-byte burst-length prefix) and the bytes
  // the *receiver* sees, because the decoder strips that prefix before the ARQ ever sees the PDU.
  // Comparing only the transmitted form is why the first version of this detector never fired.
  struct TxEcho { uint64_t hash = 0, hash_body = 0; int64_t ms = 0; size_t n = 0; };
  // 32 slots, not a handful: the detector has to survive a transmitter that gets chatty (a master polls
  // every 1.5 s and retries on top of that) and a capture buffer that holds 1.2 s of the past, so the
  // ring must span several seconds of transmissions or the echo of an older burst falls off the end and
  // is believed as a peer. 32 x 32 bytes is nothing.
  static const int kTxEchoRing = 32;
  // How long a burst we sent stays eligible to be recognised coming back. Long enough for a retransmit
  // of the same PDU (a retry re-sends identical bytes, so the match stays valid), short enough that the
  // ring never holds something the room could plausibly still be returning.
  static const int64_t NI_SELF_ECHO_WINDOW_MS = 15000;
  bool is_own_echo(const uint8_t* pdu, size_t n, int64_t now) const;

  // UI hooks
  std::function<void(const PhyStats&)> on_stats_;
  std::function<void(const RxResult&)> on_phy_;
  const Counters& counters() const { return c_; }

  // Newest capture block's level, dBFS, as measured in feed_pcm() before the squelch. This is the
  // "is the microphone hearing anything at all" number a field test needs, and it is measured on
  // every block rather than only on decoded bursts - the whole point is to have an answer when
  // nothing decodes.
  double input_dbfs() const { return in_dbfs_; }

  void move_state(int s, const std::string& why);

 private:
  void transmit(const uint8_t* pdu, size_t n, int64_t now_ms);
  int render_burst(const uint8_t* pdu, size_t n);  // NI_OK, or the modem's refusal (counted)
  void clamp_burst_to_phy();  // keeps a burst within what the modem can carry
  void handle_pdu(const uint8_t* pdu, size_t n, const PhyStats& st, int64_t now_ms);

  LinkOptions opt_;
  Modem* modem_ = nullptr;
  Modem* modem_alt_ = nullptr;  // full duplex: second modem for the other band
  std::vector<float> txq_;
  size_t tx_off_ = 0;
  double tx_played_ms_ = 0.0;
  ArqEngine arq_;
  ChannelEstimator est_;
  RateAdaptation adapt_;
  std::unique_ptr<Pairing> pair_;
  std::function<void(const Delivered&)> on_deliver_;
  std::function<void(int, const std::string&)> on_state_;
  std::vector<float> cap_;      // capture accumulation
  std::vector<float> aec_ref_;  // our own last played signal (FDD)
  size_t cap_read_ = 0;
  double noise_rms_ = 1e-4;  // learned receive noise floor, for the detector squelch
  double in_dbfs_ = -120.0;  // newest capture block level (see input_dbfs)
  bool mid_burst_ = false;   // a burst is arriving: keep the detector running every callback
  int64_t burst_hold_ms_ = 0;
  // Index inside cap_ where the burst we are waiting for begins. Once the detector has found the
  // sync pair we do not run the detector again per callback (it is O(window x symbol)); we decode
  // from this fixed offset, which aborts cheaply while the burst is still arriving.
  size_t burst_start_ = 0;
  int state_ = NI_ST_IDLE;
  bool running_ = false;
  std::string last_error_;
  int64_t listen_until_ms_ = 0, last_tx_end_ms_ = 0, peer_seen_ms_ = 0, last_peer_burst_ms_ = 0;
  TxEcho tx_echo_[kTxEchoRing];
  int tx_echo_next_ = 0;
  // Deduplication for the echo counter: which own bursts have already been counted as heard back, over
  // the same window in which an echo is recognised at all. This is almost always the same burst decoded
  // again - the decoder is fed overlapping capture windows and finds it a second and third time - but it
  // is also what a retransmission of the same frame looks like coming back, and both are one frame of
  // ours being heard, not several. A distinct frame has a distinct hash, so this cannot merge two real
  // echoes into one. (Defect H: "self-echo 128" was one transmitted frame.)
  static const int kEchoCountRing = 64;
  struct EchoCount { uint64_t hash = 0; int64_t ms = 0; };
  EchoCount echo_count_[kEchoCountRing];
  int echo_count_next_ = 0;
  int pending_profile_ = -1;            // requested, waiting for the peer's ack (see apply_profile)
  int64_t pending_profile_ms_ = 0;
  int64_t tx_backlog_ = 0;  // samples the transport still owes the speaker (set_tx_backlog)
  // Off by default, and that default is the fix for defect 33: adaptation changes the modulation
  // profile, and the apps had it enabled with no way for the two ends to agree - one end's estimator
  // could therefore move itself and leave the peer behind, permanently. It is opt-in now
  // (LinkSimOptions::adapt, or set_adaptive by a caller that knows what it is doing), and even then the
  // switch is only committed after the peer acknowledges it.
  bool adaptive_ = false;
  int64_t next_tick_ms_ = 0;
  double rtt_display_ = 0.0, erle_ = 0.0;
  Counters c_;
  uint64_t rx_bit_total_ = 0, rx_bit_err_ = 0;
  int64_t now_ms_ = 0;
  size_t pending_tx_frames_ = 0, tx_samples_total_ = 0, last_packed_frames_ = 0;
  double airtime_est_ms_ = 0.0;
  bool ack_due_ = false;
  int bursts_since_adapt_ = 0;
  std::string last_adapt_reason_ = "-";
  std::unique_ptr<NlmsAec> aec_;
  std::vector<float> played_;
  size_t play_write_ = 0;
  friend struct LinkSimFriend;
};

// ---------------- in-process two-endpoint simulation (virtual time) ----------------
// Runs two AcousticLink-free ArqEngines plus Modems over an acoustic channel model.
// Used for deterministic protocol tests and for the benchmark sweeps.
struct LinkSimResult {
  bool ok = false;
  int trials = 0, delivered = 0, retries = 0, lost = 0;
  double seconds = 0.0;
  double throughput_up_bps = 0.0, throughput_down_bps = 0.0;
  double raw_ber = 0.0, fec_ber = 0.0;
  double rtt_ms = 0.0;
  double snr_db = 0.0;
  std::string error;
  std::string to_json() const;
};

struct LinkSimOptions {
  int tx_profile_a = 2, tx_profile_b = 2;
  double fs = 48000.0;
  double snr_db = 20.0;
  double sro_ppm = 0.0;
  double distance_m = 0.5;
  int device_model = 0;  // see channel_sim.h
  int trials = 10;
  int payload_bytes = 200;
  uint64_t seed = 12345;
  double tx_gain = 0.85;
  bool noise_gate = false;
  double burst_loss_prob = 0.0;  // fraction of bursts the model drops entirely (fades / interruptions)
  bool multipath = true;
  bool agc = false;
  bool agc_fast = false;  // pathological AGC loop, for demonstrating what kills the link
  // Rate adaptation is off by default in the link simulator, and that is not laziness: the burst
  // descriptor carries the modulation id but the demodulator still runs on its own configured
  // profile, so if one end adapts and the other does not, they stop being able to read each other
  // and never converge again. See docs/STATUS.md ("adaptive modulation") - until the preamble is
  // made profile-independent, adaptation must be negotiated, not decided locally.
  bool adapt = false;
  // true = each end waits for its own burst to finish playing before the next one (how a sound
  // card behaves); false = the simulator drains the render queue as fast as it can compute, which
  // is much faster and was the original default, but it lets an endpoint speak over the peer's
  // reply and it inflates every timing figure. Measured both ways in reports/RESULTS.md.
  bool pace_tx = true;
  // The sound card's own queue: `device_ms` of audio sits between the moment an endpoint hands
  // samples to its audio backend and the moment they come out of the speaker / are handed to the
  // peer. With device_ms = 0 nothing changes (the model keeps its original 25 ms + propagation
  // floor). This exists because the apps have a real queue and the model did not, so a turn-taking
  // bug that only appears against a real device could not be reproduced here at all.
  int device_ms = 0;
  // Whether each endpoint is TOLD how much of its audio is still in flight, the way an app reading
  // GetCurrentPadding()/playbackHeadPosition() can tell its own link. device_report = false models
  // the apps as they were before this was wired up: the queue exists, the transmitter does not know.
  bool device_report = true;
  int max_ms = 60000;
};
LinkSimResult ni_link_sim(const LinkSimOptions& o);

}  // namespace ni
#endif
