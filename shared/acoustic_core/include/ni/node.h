// node.h — the whole engine for one device: role, modem, link, proxy, calibration,
// statistics, and the JSON the UIs render. This is the object the Android service, the
// Windows app, the CLI and the simulator all drive, so behaviour cannot diverge.
#ifndef NI_NODE_H
#define NI_NODE_H
#include <memory>
#include <string>
#include <vector>
#include "ni/aec.h"
#include "ni/channel_sim.h"
#include "ni/proxy.h"
#include "ni/session.h"

namespace ni {

struct NodeConfig {
  Role role = NI_ROLE_GIVER;
  bool is_master = true;       // link master (polls). Default: the giver.
  int profile_id = 10;         // FD-LOW-DBPSK: the profile every measured end-to-end run delivered on
  double fs = 48000.0;
  double tx_amplitude = 0.85;
  // Assist: what makes this app work in a real room without the user knowing any of the numbers.
  // It sweeps our own output level while unpaired (the right value depends on the speaker, the
  // microphone and the distance) and lets a receiver emit a keepalive beacon so that it is never
  // silently indistinguishable from a dead app. Off by default: the measured sweeps in reports/ were
  // taken without it, and turning it on by default would quietly change every one of them.
  bool assist = false;
  // 0x0007 in both apps, and both apps must agree on it or the two ends are talking to different
  // sessions. Two apps whose defaults differ cannot pair by accident, and that was one of the ways a
  // first field test failed silently: the phone defaulted to profile 2 / session 0 while the PC
  // defaulted to profile 11 / session 7.
  uint16_t session_id = 7;
  std::string pairing_secret;   // empty = unpaired (DATA refused unless allow_plaintext)
  bool allow_plaintext_test = false;
  // A passphrase is what makes pairing possible at all: Node::start() refuses to start without one
  // (see the check there) unless allow_unpaired is set deliberately. min_secret_len is the length the
  // front ends also enforce, because a silent "nothing happens" is the worst possible failure mode for
  // a first run.
  int min_secret_len = 8;
  int duplex = 0;               // 0 half, 1 FDD full-duplex experiment
  int rx_profile_id = -1;       // duplex=1 only
  int aec_taps = 1024;
  int listen_ms = 900;
  int turnaround_ms = 60;
  int max_retransmit = 5;
  // proxy
  bool enable_receiver_proxy = false;
  bool enable_giver_fetch = true;
  int proxy_port = 8471;
  std::string proxy_bind = "127.0.0.1";
  std::string proxy_token;      // empty -> generated
  // simulation channel (only used by the simulator / self-test)
  bool sim_channel = false;
  ChannelConfig ch;
  std::string name = "node";
  int calibrate_trials = 3;   // per band, per SNR point
  int arq_wait_ms = 12000;    // how long test_link pumps before giving up
  // Set only by tools that have no peer by design (calibration) or that deliberately measure the
  // plaintext path. Both apps leave it false, so neither can start without a passphrase.
  bool allow_unpaired = false;
};

struct StatusSnapshot {
  std::string name, role, state_name;
  // Pairing is part of the status a UI has to show, not an extra: the match code (SAS words) is the
  // only thing a human can compare between the two ends to know the "encrypted" claim means something.
  // The Windows GUI and the Android panel both read these, and the fields used to exist only inside the
  // diagnostics JSON - which is why neither UI could show them (they are here so both can).
  std::string pairing = "none";   // "none" | "wait-challenge" | "wait-response" | "paired" | "failed"
  std::string sas;                // 4-word short authentication string, empty until paired
  // Why the link is not pairing, in the engine's own words, whenever that can be answered locally -
  // today that means "the passphrase is empty, so no challenge was ever sent". Both front ends show
  // this verbatim; the alternative is a user staring at "peer offline" forever.
  std::string pairing_note;
  int state = NI_ST_IDLE;
  bool running = false;
  int profile_id = 0;
  std::string profile_name, modulation;
  double sample_rate = 0;
  std::string input_device = "not selected", output_device = "not selected";
  bool peer_online = false;
  int64_t peer_seen_ms = 0;
  int64_t session_ms = 0;
  double snr_db = 0, rssi_dbfs = -120, noise_dbfs = -120, evec_pct = 0;
  // Diagnostics that answer "why is nothing decoding": the newest capture block's level, how many
  // bursts had their sync found but could not be read, and how many callbacks saw no signal at all.
  double in_dbfs = -120;          // newest microphone block, dBFS (measured every callback)
  uint64_t sync_hits = 0;         // bursts whose sync was detected
  uint64_t rx_no_frame = 0;       // callbacks the squelch found empty
  double link_bps = 0, useful_bps_up = 0, useful_bps_down = 0;
  double loss_pct = 0, rtt_ms = 0, erle_db = 0;
  uint64_t bytes_sent = 0, bytes_received = 0, frames_sent = 0, frames_received = 0;
  // Bursts this device decoded that were its own transmission returning through its own microphone.
  // Counted separately since defect 59: adding them to frames_received made "decoded 48, bytes
  // received 0" look like a peer that answers but never delivers, when in fact no peer had ever been
  // heard. They also no longer make the peer look online, because the link drops them before the ARQ
  // can see them.
  uint64_t self_echo_bursts = 0;
  uint64_t retries = 0, crc_fail = 0, malformed = 0;
  bool tunnel_up = false, internet_ok = false;
  std::string tunnel_detail, internet_detail;
  int mtu = 0;
  double tx_gain = 0;          // the output level actually in use (assist sweeps it)
  // One sentence, in plain words, saying what this end sees and what to do about it - computed in the
  // engine from its own counters so that both front ends print the same diagnosis instead of each
  // inventing its own wording. Empty only when there is nothing useful to say (paired and healthy).
  std::string advice;
  std::string audio_note;      // "NOT TESTED: no audio hardware in this environment" etc.
  // Filled by Node::self_hear_*: counts taken over a measured interval, never a prediction.
  std::string to_json() const;
};

// A self-hearing measurement: see Node::self_hear_now().
struct SelfHear {
  uint64_t bursts_heard = 0;    // bursts whose sync was found in the window
  uint64_t frames_decoded = 0;  // frames of ours that came back through the microphone
  double in_dbfs = -120.0;      // the newest capture block's level
  std::string verdict;          // one sentence, empty until self_hear_since() fills it
  std::string to_json() const;
};

class Node {
 public:
  explicit Node(const NodeConfig& cfg);
  ~Node();

  int configure();     // builds modem/link; NI_ERR_PARAM on invalid input
  int start();
  void stop();
  bool running() const { return running_; }

  // PCM plumbing
  int feed_capture(const float* x, size_t n);  // microphone samples in
  int pull_playback(float* out, size_t cap);    // speaker samples out
  bool playback_ready() const { return link_ ? link_->tx_ready() : false; }

  // Advance all state machines. now_ms is this device's monotonic clock.
  int step(int64_t now_ms);
  // When a modelled channel sits between the two ends, the link can be told to wait for its own
  // audio to be played before it starts the next burst (paced, like a real sound card) or to ignore
  // that and let the simulation drain the queue as fast as the host can compute (unpaced). Paced is
  // the honest model; unpaced runs 100x faster and was the default while the simulator was written,
  // which is why LinkSimOptions::pace_tx exists - see reports/RESULTS.md on what changes between
  // them. `Node::step` reports the backlog to the link only when pacing is on.
  void set_pace_tx(bool on) { pace_tx_ = on; }

  const NodeConfig& config() const { return cfg_; }
  ArqEngine& arq() { return link_->arq(); }
  AcousticLink* link() { return link_.get(); }
  int state() const { return link_ ? link_->state() : NI_ST_IDLE; }
  StatusSnapshot snapshot() const;

  // ---- user actions (all measured, none of them a stub) ----
  int calibrate(int64_t now_ms, Calibrator::Report* out);   // runs a full calibration exchange
  int test_link(int64_t now_ms, int payload_bytes, std::string* report);  // hash-compared ping
  // Does this machine hear its own speaker? The one question a user with a pair that will not work
  // cannot answer alone, and the one that needs no second device: a running node is always transmitting
  // (keepalive bursts, or the assist beacon), so whatever its own microphone decodes during those
  // seconds came out of its own speaker. Take a reading, wait, take another with the first as the
  // baseline; the second call fills in the deltas and the sentence.
  SelfHear self_hear_now() const;
  SelfHear self_hear_since(const SelfHear& before, int seconds) const;
  int test_internet(int64_t now_ms, const std::string& url, InternetVerify* out);
  // How many frames the audio device still has queued and unplayed. The apps sample this from
  // their own audio backend (WASAPI GetCurrentPadding, AudioTrack head vs written) and hand it to
  // the link, so turn-taking is timed against the sound that has actually been played rather than
  // against the sound that has been copied out of this library. See Node::step.
  void set_tx_backlog(size_t samples) { device_backlog_ = samples; }
  size_t tx_backlog() const { return device_backlog_; }

  // Loopback self-check usable with no peer: render -> channel model -> decode.
  int selftest(int payload_bytes, uint64_t seed, PhyTestResult* out);
  int start_calibration_probe(int profile_id, int frames, uint16_t* tag);

  // ---- data plane for the UIs / tools ----
  int send_bytes(uint16_t stream, MsgType t, const uint8_t* p, size_t n, uint16_t flags = 0);
  void set_message_handler(std::function<void(const Delivered&)> h);
  ReceiverGateway* receiver_proxy() { return rproxy_.get(); }
  GiverGateway* giver() { return gproxy_.get(); }
  std::string status_json() const { return snapshot().to_json(); }
  std::string diagnostics_json() const { return link_ ? link_->diagnostics_json() : "{}"; }
  std::string export_report_json(int64_t now_ms) const;
  int64_t now_ms = 0;               // maintained by step() for the JSON exporters
  // link test bookkeeping (used by test_link and the FILE handler)
  struct LinkTest {
    bool pending = false;
    bool answered = false;
    bool hash_ok = false;
    int64_t t0 = 0, t1 = 0;
    size_t bytes = 0;
    std::string expect_sha, got_sha;
  } lt_, lt_in_;
  std::string test_report;

  const std::string& proxy_token() const { return tok_; }
  std::string last_error() const { return err_; }
  // Role switch with safety checks (prompt 4: refuse incompatible role/simultaneous session)
  int set_role(Role r);
  int set_profile(int pid);
  bool audio_hardware_present() const { return hw_present_; }
  // Deliberate payload-leak self-check used by the tests (see ADR D8).
  const std::vector<uint8_t>& debug_tx_bytes() const { return tx_dump_; }
  void enable_tx_dump(bool on) { tx_dump_on_ = on; }

 private:
  int ensure_built();
  NodeConfig cfg_;
  bool built_ = false, running_ = false, hw_present_ = false;
  std::string err_, tok_;
  std::unique_ptr<AcousticLink> link_;
  std::unique_ptr<ReceiverGateway> rproxy_;
  std::unique_ptr<GiverGateway> gproxy_;
  bool pace_tx_ = true;                     // see set_pace_tx
  size_t device_backlog_ = 0;               // frames the real audio device still owes (set_tx_backlog)
  std::string pairing_note_;
  std::unique_ptr<AcousticChannel> chan_;   // sim channel applied to our own TX
  std::unique_ptr<NlmsAec> aec_;
  std::function<void(const Delivered&)> msg_;
  void on_delivered(const Delivered& d);
  std::vector<float> rx_hold_;
  std::vector<float> raw_tx_, ready_tx_;
  size_t ready_off_ = 0;
  std::string audio_note_;
  NetInfo peer_netinfo_{};
  bool peer_netinfo_valid_ = false;
  int64_t peer_netinfo_ms_ = 0;
  PhyStats last_phy_{};
  std::string last_verify_;
  // assist bookkeeping: the output level in use and when to move to the next rung
  double tx_gain_ = 0.85;
  int hunt_step_ = 0;
  int64_t hunt_next_ms_ = 0;
  std::vector<uint8_t> tx_dump_;
  bool tx_dump_on_ = false;
  // calibration bookkeeping
  struct Cal {
    bool active = false;
    int stage = 0;
    int profile = 0;
    int frames = 0;
    int sent = 0, got = 0;
    int64_t t0 = 0;
    uint16_t tag = 0;
    std::vector<Calibrator::BandResult> results;
    bool done = false;
    double noise_dbfs = -120, sig_dbfs = -120;
    bool clipped = false;
  } cal_;
  friend class AcousticLink;
};

}  // namespace ni
#endif
