// sim.cpp — deterministic two-node simulation over the acoustic channel model, in virtual
// time. This is the harness behind every throughput / reliability number in reports/.
//
// What is real here: the full modulation chain, FEC, framing, ARQ, AEAD pairing, fragmentation
// and retransmission scheduling, plus a physical channel model (device band limits, multipath,
// AGC, clipping, noise, clock offset, 16-bit quantisation).
// What is NOT real: the transducers and the OS audio stack. See ADR-001 D8; every report row
// from this file carries channel="simulated".
#include <deque>
#include <math.h>
#include <sstream>
#include "ni/channel_sim.h"
#include "ni/crypto.h"
#include "ni/node.h"

namespace ni {

namespace {
struct Side {
  NodeConfig cfg;
  std::unique_ptr<Node> node;
  std::vector<uint8_t> got;
  uint64_t bytes_sent_target = 0;
  bool done = false;
  bool hash_ok = false;
  int64_t t_done = 0;
  std::string sha_expected, sha_got;
};

struct Ev {
  int64_t at;
  std::vector<float> pcm;
};
}  // namespace

LinkSimResult ni_link_sim(const LinkSimOptions& o) {
  LinkSimResult R;
  R.trials = o.trials;
  if (o.trials <= 0 || o.payload_bytes <= 0) {
    R.error = "bad options";
    return R;
  }
  const double fs = o.fs;
  const int per_ms = (int)(fs / 1000.0);

  auto mk = [&](Role role, bool master, int profile, const char* name) {
    NodeConfig c;
    c.role = role;
    c.is_master = master;
    c.profile_id = profile;
    c.fs = fs;
    c.tx_amplitude = o.tx_gain;
    c.session_id = 0x4e31;
    c.pairing_secret = "noise-internet-sim-pair-918273";
    c.sim_channel = true;
    c.name = name;
    c.allow_plaintext_test = false;
    c.enable_giver_fetch = false;
    c.enable_receiver_proxy = false;
    c.listen_ms = 1200;
    c.turnaround_ms = 40;
    c.arq_wait_ms = 100000;
    c.max_retransmit = 5;
    c.ch.fs = fs;
    c.ch.speaker = (role == NI_ROLE_GIVER) ? o.device_model : o.device_model;
    c.ch.microphone = NI_DEV_LAPTOP_MIC;
    c.ch.snr_db = o.snr_db;
    c.ch.sro_ppm = o.sro_ppm;
    c.ch.distance_m = o.distance_m;
    c.ch.multipath = o.multipath;
    c.ch.agc = o.agc;
    c.ch.agc_fast = o.agc_fast;
    c.ch.seed = o.seed + (role == NI_ROLE_GIVER ? 1 : 2);
    return c;
  };

  Side A, B;
  A.cfg = mk(NI_ROLE_GIVER, true, o.tx_profile_a, "giver");
  B.cfg = mk(NI_ROLE_RECEIVER, false, o.tx_profile_b, "receiver");
  A.node.reset(new Node(A.cfg));
  B.node.reset(new Node(B.cfg));
  if (A.node->start() != NI_OK || B.node->start() != NI_OK) {
    R.error = "node start failed: " + A.node->last_error() + " | " + B.node->last_error();
    return R;
  }
  A.node->set_pace_tx(o.pace_tx);
  B.node->set_pace_tx(o.pace_tx);
  A.node->link()->set_adaptive(o.adapt);
  B.node->link()->set_adaptive(o.adapt);
  if (A.node->link()->modemp() == nullptr || B.node->link()->modemp() == nullptr) {
    R.error = "modem unavailable for the requested profile";
    return R;
  }

  int maxpay = std::min(A.node->link()->modemp()->max_payload(),
                        B.node->link()->modemp()->max_payload()) - 44;
  int pb = std::min(o.payload_bytes, maxpay);
  if (pb < 16) {
    R.error = "profile cannot carry a useful test payload at this sample rate";
    return R;
  }

  // Both sides get a payload to send; each verifies the hash of what it received.
  Rng ra(o.seed), rb(o.seed ^ 0x5a5a);
  std::vector<uint8_t> payA((size_t)pb), payB((size_t)pb);
  ra.bytes(&payA[0], payA.size());
  rb.bytes(&payB[0], payB.size());
  uint8_t shaA[32], shaB[32];
  ni_sha256(&payA[0], payA.size(), shaA);
  ni_sha256(&payB[0], payB.size(), shaB);
  A.sha_expected = ni_hex(shaB, 32);  // A receives payB
  B.sha_expected = ni_hex(shaA, 32);  // B receives payA
  A.bytes_sent_target = payA.size();
  B.bytes_sent_target = payB.size();

  auto prepend = [](const uint8_t* sha, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> m;
    m.insert(m.end(), sha, sha + 32);
    m.insert(m.end(), body.begin(), body.end());
    return m;
  };
  std::vector<uint8_t> msgA = prepend(shaA, payA), msgB = prepend(shaB, payB);

  auto on_msg = [](Side* s) {
    return [s](const Delivered& d) {
      if (d.stream != 4242) return;
      if (d.type != NI_MSG_FILE) return;
      s->got.insert(s->got.end(), d.data.begin(), d.data.end());
      if (s->got.size() >= 32 && !s->done) {
        uint8_t want[32];
        memcpy(want, &s->got[0], 32);
        const uint8_t* body = &s->got[0] + 32;
        size_t blen = s->got.size() - 32;
        if (blen == s->bytes_sent_target) {
          uint8_t got[32];
          ni_sha256(body, blen, got);
          s->sha_got = ni_hex(got, 32);
          s->hash_ok = memcmp(want, got, 32) == 0;
          s->done = true;
        }
      }
    };
  };
  A.node->set_message_handler(on_msg(&A));
  B.node->set_message_handler(on_msg(&B));

  // scheduling state
  std::deque<Ev> wire_ab, wire_ba;
  double prop_ms = 1000.0 * o.distance_m / 343.0;
  const int64_t delay_ms = (int64_t)(25 + prop_ms);
  int64_t t = 0;
  int64_t t0 = 0;
  R.seconds = 0.0;
  int64_t pair_deadline = o.max_ms / 3;
  bool paired = false;
  float buf[8192];
  Rng jitter(o.seed ^ 0x77);

  while (t < o.max_ms) {
    A.node->step(t);
    B.node->step(t);
    // drain both playback paths into the (modelled) wire
    for (int which = 0; which < 2; which++) {
      Side& s = which ? B : A;
      int got = s.node->pull_playback(buf, per_ms);
      if (got > 0) {
        bool drop = o.burst_loss_prob > 0 && jitter.uniform() < o.burst_loss_prob;
        if (!drop) {
          Ev e;
          e.at = t + delay_ms;
          e.pcm.assign(buf, buf + got);
          (which ? wire_ba : wire_ab).push_back(std::move(e));
        }
      }
    }
    for (int which = 0; which < 2; which++) {
      std::deque<Ev>& q = which ? wire_ba : wire_ab;
      while (!q.empty() && q.front().at <= t) {
        Ev e = std::move(q.front());
        q.pop_front();
        Side& into = which ? A : B;
        into.node->feed_capture(e.pcm.data(), e.pcm.size());
      }
    }
    if (!paired) {
      Pairing* pa = A.node->link()->pairing();
      Pairing* pb2 = B.node->link()->pairing();
      if (pa && pb2 && pa->state() == NI_PAIR_DONE && pb2->state() == NI_PAIR_DONE) {
        paired = true;
        t0 = t;
        A.node->send_bytes(4242, NI_MSG_FILE, msgA.data(), msgA.size(), 0);
        B.node->send_bytes(4242, NI_MSG_FILE, msgB.data(), msgB.size(), 0);
        R.delivered = 0;
      } else if (t > pair_deadline) {
        {
          std::ostringstream os;
          os << "pairing did not complete within " << pair_deadline
             << " ms of virtual time. A{" << A.node->link()->diagnostics_json() << "} B{"
             << B.node->link()->diagnostics_json() << "}";
          R.error = os.str();
        }
        return R;
      }
    }
    if (paired) R.seconds = (double)(t - t0) / 1000.0;
    if (paired && A.done && B.done) {
      R.delivered = o.trials;
      break;
    }
    if (paired) {
      const ArqStats& sa = A.node->link()->arq().stats();
      const ArqStats& sb = B.node->link()->arq().stats();
      R.retries = (int)(sa.tx_retries + sb.tx_retries);
      R.lost = (int)(sa.lost_frames + sb.lost_frames);
      R.rtt_ms = (sa.rtt_ms_avg + sb.rtt_ms_avg) * 0.5;
      R.snr_db = (A.node->link()->estimator().band_snr_db() + B.node->link()->estimator().band_snr_db()) * 0.5;
      // Only bytes the peer actually reassembled count. Reporting what we queued would be the
      // sender's wishful thinking, not a measured link rate, and the 32-byte integrity prefix is
      // protocol overhead so it is excluded from "useful" throughput.
      double secs = R.seconds > 0.001 ? R.seconds : 0.001;
      double up = 8.0 * (double)(B.got.size() > 32 ? B.got.size() - 32 : 0) / secs;
      double down = 8.0 * (double)(A.got.size() > 32 ? A.got.size() - 32 : 0) / secs;
      R.throughput_up_bps = up;
      R.throughput_down_bps = down;
    }
    if (paired && t - t0 > o.max_ms / 2) {
      std::ostringstream os;
      os << "transfer timed out at " << (t - t0) << " ms (A done=" << A.done
         << " B done=" << B.done << ", A got " << A.got.size() << " B got " << B.got.size()
         << " of " << A.bytes_sent_target + 32 << " bytes). A{"
         << A.node->link()->diagnostics_json() << "} B{" << B.node->link()->diagnostics_json()
         << "}";
      R.error = os.str();
      return R;
    }
    t++;
  }
  R.ok = A.done && B.done && A.hash_ok && B.hash_ok;
  if (!R.ok && R.error.empty()) R.error = "payload hash mismatch or incomplete transfer";
  R.throughput_up_bps = 8.0 * (double)A.bytes_sent_target / std::max(0.001, R.seconds);
  R.throughput_down_bps = 8.0 * (double)B.bytes_sent_target / std::max(0.001, R.seconds);
  return R;
}

std::string LinkSimResult::to_json() const {
  std::ostringstream o;
  o << "{\"ok\": " << (ok ? "true" : "false") << ", \"trials\": " << trials
    << ", \"delivered\": " << delivered << ", \"retransmits\": " << retries
    << ", \"lost_frames\": " << lost << ", \"seconds\": " << seconds
    << ", \"up_bps\": " << throughput_up_bps << ", \"down_bps\": " << throughput_down_bps
    << ", \"rtt_ms\": " << rtt_ms << ", \"snr_db\": " << snr_db
    << ", \"error\": \"" << ni_json_escape(error) << "\"}";
  return o.str();
}

}  // namespace ni
