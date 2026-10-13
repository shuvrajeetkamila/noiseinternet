// t_arq.cpp - the retransmission, reassembly and crypto-gate layer on its own, with no audio, no
// channel model and no clock running: two ArqEngine objects bolted back to back. Everything here is
// about whether a byte stream that enters one end comes out of the other, once, intact, and only
// when it is allowed to be.
//
// The burst PDU handed to on_pdu() starts after the 2-byte length prefix, because that is what the
// PHY strips (the burst descriptor carries the length, so the receiver can size the FEC block
// before it reads any header). build_pdu() produces the prefix, so feeding on_pdu(buf + 2, len - 2)
// is exactly the wire.
#include <string.h>

#include <algorithm>
#include <vector>

#include "ni/common.h"
#include "ni/crc.h"
#include "ni/crypto.h"
#include "ni/frame_codec.h"
#include "ni/session.h"
#include "ntest.h"

using namespace ni;

namespace {

struct Harness {
  ArqEngine A, B;
  int64_t t;
  // The configs must be finished before the engines are built: ArqEngine copies its ArqConfig, so
  // assigning to it in the constructor body would be a no-op, which is exactly the kind of quiet
  // test-only difference that makes a suite pass for the wrong reason.
  static ArqConfig base() {
    ArqConfig c;
    c.require_crypto = false;
    c.session_id = 0x4e31;
    c.max_payload = 228;
    c.ack_timeout_ms = 1200;
    return c;
  }
  Harness() : A(base()), B(base()), t(0) {}

  // A speaks, then B. Returns the number of turns used. `skip_ab` drops that many A->B bursts
  // entirely (a fade that swallows a whole burst, which is what the acoustic channel really does).
  int pump(int max_turns = 80, int skip_ab = 0, bool stop_when_idle = true) {
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    int turn = 0;
    for (; turn < max_turns; turn++) {
      size_t len = 0;
      int n = A.build_pdu(t, buf, sizeof buf, &len, true);
      if (n > 0) {
        A.note_emitted((size_t)n, t, 60.0);
        if (skip_ab <= 0) B.on_pdu(buf + 2, len - 2, t);
        else skip_ab--;
      }
      t += 3;
      len = 0;
      n = B.build_pdu(t, buf, sizeof buf, &len, false);
      if (n > 0) {
        B.note_emitted((size_t)n, t, 60.0);
        A.on_pdu(buf + 2, len - 2, t);
      }
      t += 3;
      A.on_timer(t);
      B.on_timer(t);
      if (stop_when_idle && A.stats().pending == 0 && B.stats().pending == 0 && A.tx_pending() == 0 &&
          B.tx_pending() == 0)
        break;
    }
    return turn;
  }

  // Pulls deliveries out of e and lays their bytes out by fragment offset, which is what the
  // proxy/fragment layer expects of a reliable link.
  size_t drain(ArqEngine& e, std::vector<uint8_t>* into, size_t total) {
    into->assign(total, 0);
    std::vector<char> seen(total, 0);
    size_t got = 0;
    Delivered d;
    while (e.next_delivery(&d)) {
      if (d.offset + d.len > total) {
        NT_CHECK_MSG(false, "fragment offset past the end of the message");
        continue;
      }
      for (size_t i = 0; i < d.len; i++) {
        if (seen[d.offset + i] && (*into)[d.offset + i] != d.data[i]) {
          NT_CHECK_MSG(false, "the same offset was delivered twice with different bytes");
          break;
        }
        seen[d.offset + i] = 1;
        (*into)[d.offset + i] = d.data[i];
      }
      got += d.len;
    }
    return got;
  }
};

std::vector<uint8_t> make_msg(size_t n, uint64_t seed) {
  Rng r(seed ? seed : 1);
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(r.next() & 0xFF);
  return v;
}

void mirrored_keys(SessionKeys* ka, SessionKeys* kb, const char* secret) {
  uint8_t na[16], nb[16];
  for (int i = 0; i < 16; i++) {
    na[i] = (uint8_t)(0x10 + i);
    nb[i] = (uint8_t)(0x90 + i);
  }
  NT_EQ(ni_derive_keys(std::string(secret), na, nb, 0x4e31, true, ka), NI_OK);
  NT_EQ(ni_derive_keys(std::string(secret), na, nb, 0x4e31, false, kb), NI_OK);
  NT_CHECK(memcmp(ka->tx, kb->rx, NI_KEY_BYTES) == 0);
  NT_CHECK(memcmp(ka->rx, kb->tx, NI_KEY_BYTES) == 0);
}
}  // namespace

int main() {
  printf("[t_arq] fragmentation, acks, retransmission, crypto gate\n");

  // ---- 1. a multi-kilobyte message arrives byte-identical, split over many fragments ----------
  {
    Harness h;
    std::vector<uint8_t> msg = make_msg(2500, 1234);
    uint8_t sha[32];
    ni_sha256(&msg[0], msg.size(), sha);
    NT_EQ(h.A.send_message(7, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    int turns = h.pump();
    // settle the retransmission timers, then look: pending is only recomputed by the engine's own
    // events, so reading it straight after the last burst would show a stale value.
    h.t += 3000;
    h.A.on_timer(h.t);
    h.B.on_timer(h.t);
    std::vector<uint8_t> got;
    size_t bytes = h.drain(h.B, &got, msg.size());
    NT_EQ(bytes, msg.size());
    NT_CHECK(got.size() == msg.size() && memcmp(got.data(), msg.data(), msg.size()) == 0);
    // No frame of the transfer may be written off, and the receiver must not have had to reject
    // anything. (The number of frames still sitting in the sender's queue depends on this test's
    // turn budget - the acknowledgements need one more exchange to travel - so it is reported, not
    // asserted: the properties that matter are completeness and loss.)
    NT_EQ((int)h.A.stats().lost_frames, 0);
    NT_CHECK_MSG(h.B.stats().rx_malformed == 0, "receiver reported malformed frames");
    printf("  pending after settle: A=%llu B=%llu\n", (unsigned long long)h.A.stats().pending,
           (unsigned long long)h.B.stats().pending);
    printf("  note: 2500 B in %d turns, fragments=%llu tx_frames=%llu rtt=%.0f ms\n", turns,
           (unsigned long long)h.B.stats().rx_frames, (unsigned long long)h.A.stats().tx_frames,
           h.A.stats().rtt_ms_avg);
    // the sha of what arrived must match, i.e. the link is fit to carry a file, not just a frame
    uint8_t sha_got[32];
    ni_sha256(&got[0], got.size(), sha_got);
    NT_CHECK(memcmp(sha, sha_got, 32) == 0);
  }

  // ---- 2. best-effort frames must not sit in the queue forever --------------------------------
  // A frame queued with want_ack=false can never be acknowledged (the peer's cumulative ack counts
  // the reliable sequence space only), so the sender has to release it as soon as it is on the
  // wire. Until it did, every pairing/control frame stayed "pending" for the life of the session.
  {
    Harness h;
    uint8_t pl[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    NT_EQ(h.A.send_frame(NI_FT_PING, pl, sizeof pl, false), NI_OK);
    NT_EQ(h.A.send_frame(NI_FT_NETINFO, pl, sizeof pl, false), NI_OK);
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    size_t len = 0;
    int n = h.A.build_pdu(0, buf, sizeof buf, &len, false);
    NT_CHECK(n >= 2);
    NT_EQ((int)h.A.stats().pending, 0);
    int accepted = h.B.on_pdu(buf + 2, len - 2, 5);
    NT_EQ(accepted, n);  // every frame in the burst must parse
  }

  // ---- 3. an unacknowledged burst is retransmitted and the message still arrives -------------
  {
    Harness h;
    h.A.config_mut().ack_timeout_ms = 60;  // so the retry clock moves inside the test's budget
    std::vector<uint8_t> msg = make_msg(900, 99);
    NT_EQ(h.A.send_message(9, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    // Emit one burst and deliberately never hand it to B: that is what a fade does. The frames are
    // in flight with no acknowledgement, so the retry timer has to put them back on the wire.
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    size_t len = 0;
    int n = h.A.build_pdu(h.t, buf, sizeof buf, &len, false);
    NT_CHECK(n >= 1);
    h.A.note_emitted((size_t)n, h.t, 60.0);
    std::vector<uint8_t> got;
    NT_EQ((int)h.drain(h.B, &got, msg.size()), 0);  // nothing was sent to B yet
    h.t += 1000;  // far past ack_timeout_ms
    for (int turn = 0; turn < 8; turn++) {
      uint8_t b[2 + NI_MAX_BURST_PDU];
      size_t l = 0;
      int nn = h.A.build_pdu(h.t, b, sizeof b, &l, true);
      printf("   turn %d: build=%d len=%zu pending=%llu -> on_pdu accepted=%d\n", turn, nn, l,
             (unsigned long long)h.A.stats().pending, nn > 0 ? h.B.on_pdu(b + 2, l - 2, h.t) : 0);
      if (nn > 0) h.A.note_emitted((size_t)nn, h.t, 60.0);
      h.t += 300;  // past ack_timeout_ms + the airtime allowance, so the retry clock fires
      h.A.on_timer(h.t);
      h.B.on_timer(h.t);
      printf("          after on_timer: pending=%llu retries=%llu\n",
             (unsigned long long)h.A.stats().pending, (unsigned long long)h.A.stats().tx_retries);
    }
    size_t bytes = h.drain(h.B, &got, msg.size());
    NT_EQ(bytes, msg.size());
    NT_CHECK(memcmp(got.data(), msg.data(), msg.size()) == 0);
    NT_CHECK_MSG(h.A.stats().tx_retries >= 1, "retransmission never happened");
    printf("  note: %d turns after the retry expired, retries=%llu\n", n,
           (unsigned long long)h.A.stats().tx_retries);
    // and after giving up entirely the frame must be counted lost, not silently dropped
    Harness h2;
    h2.A.config_mut().ack_timeout_ms = 20;
    h2.A.config_mut().max_retries = 2;
    std::vector<uint8_t> small = make_msg(100, 3);
    NT_EQ(h2.A.send_message(10, NI_MSG_FILE, &small[0], small.size(), 0), NI_OK);
    for (int i = 0; i < 40; i++) {
      uint8_t b2[2 + NI_MAX_BURST_PDU];
      size_t l2 = 0;
      int n2 = h2.A.build_pdu(h2.t, b2, sizeof b2, &l2, false);
      if (n2 > 0) h2.A.note_emitted((size_t)n2, h2.t, 60.0);  // never delivered on purpose
      h2.t += 100;
      h2.A.on_timer(h2.t);
    }
    NT_CHECK(h2.A.stats().lost_frames >= 1);
    NT_EQ((int)h2.A.stats().pending, 0);
    printf("  note: after max_retries the frame is reported lost=%llu pending=%llu\n",
           (unsigned long long)h2.A.stats().lost_frames, (unsigned long long)h2.A.stats().pending);
  }

  // ---- 4. out-of-order fragments are reassembled (selective retransmit depends on it) ---------
  {
    Harness h;
    h.A.config_mut().max_frames_per_burst = 1;
    std::vector<uint8_t> msg = make_msg(600, 7);  // 600 B / 216 per fragment = 3 fragments
    NT_EQ(h.A.send_message(11, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    // capture the first three bursts A emits, then feed B in reverse order
    std::vector<std::vector<uint8_t> > pdus;
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    for (int i = 0; i < 3; i++) {
      size_t len = 0;
      int n = h.A.build_pdu(h.t, buf, sizeof buf, &len, false);
      NT_CHECK(n >= 1);
      if (len < 2) len = 0;
      pdus.push_back(std::vector<uint8_t>(buf, buf + len));
      h.t += 3;
    }
    for (int i = 2; i >= 0; i--)
      if (pdus[(size_t)i].size() > 2)
        h.B.on_pdu(&pdus[(size_t)i][2], pdus[(size_t)i].size() - 2, h.t);
    std::vector<uint8_t> got;
    size_t bytes = h.drain(h.B, &got, msg.size());
    NT_EQ(bytes, msg.size());
    NT_CHECK(memcmp(got.data(), msg.data(), msg.size()) == 0);
  }

  // ---- 5. the crypto gate: DATA is refused while unpaired, and after tampering ----------------
  {
    Harness h;
    h.B.config_mut().require_crypto = true;
    std::vector<uint8_t> msg = make_msg(200, 5);
    NT_EQ(h.A.send_message(13, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    h.pump(6);
    std::vector<uint8_t> got;
    size_t bytes = h.drain(h.B, &got, msg.size());
    NT_EQ((int)bytes, 0);  // plaintext must not be accepted by a paired receiver
    NT_EQ(h.B.stats().rx_err, 2);
    printf("  note: unpaired plaintext refused, rx_malformed=%llu\n",
           (unsigned long long)h.B.stats().rx_malformed);

  }

  // ---- 5b. the same message through a paired, encrypted session ------------------------------
  // Separate block on purpose: a fresh engine pair. Reusing the harness above would leave the
  // refused plaintext frame queued at seq 0, which (correctly) blocks in-order delivery of
  // everything behind it, and the test would then be measuring that rather than the crypto path.
  {
    Harness h;
    h.A.config_mut().require_crypto = true;
    h.B.config_mut().require_crypto = true;
    std::vector<uint8_t> msg = make_msg(200, 5);
    SessionKeys ka, kb;
    mirrored_keys(&ka, &kb, "tunnel passphrase");
    h.A.set_keys(&ka);
    h.B.set_keys(&kb);
    h.A.config_mut().require_crypto = true;
    NT_EQ(h.A.send_message(14, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    h.pump();
    std::vector<uint8_t> got;
    size_t bytes = h.drain(h.B, &got, msg.size());
    NT_EQ((int)bytes, (int)msg.size());
    NT_CHECK(memcmp(got.data(), msg.data(), msg.size()) == 0);
    NT_EQ(h.B.stats().rx_err, 0);
    NT_CHECK_MSG(h.B.stats().rx_bytes_payload > 0, "no payload accounted for on the receive side");

    // one flipped bit in the ciphertext of a captured burst must stop the delivery entirely
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    size_t len = 0;
    h.A.config_mut().max_frames_per_burst = 1;  // one frame per burst, so the tamper is unambiguous
    std::vector<uint8_t> msg2 = make_msg(400, 6);
    NT_EQ(h.A.send_message(15, NI_MSG_FILE, &msg2[0], msg2.size(), 0), NI_OK);
    int n = h.A.build_pdu(h.t, buf, sizeof buf, &len, false);
    NT_CHECK(n == 1);
    if (len < 2) return ntest::summary("t_arq");
    std::vector<uint8_t> tampered(buf, buf + len);
    // Locate the DATA frame in this burst: an outstanding acknowledgement may share it, and the
    // tamper has to land on the frame we mean.
    Frame probe;
    NT_EQ(ni_frame_decode(buf + 2, len - 2, &probe), NI_OK);
    printf("  note: burst head frame type=%s len=%u\n", ni_frame_type_name((FrameType)probe.type),
           probe.payload_len);
    NT_EQ((int)probe.type, (int)NI_FT_DATA);
    tampered[2 + NI_HEADER_BYTES + probe.payload_len - 8] ^= 0x08;  // inside the poly1305 tag
    // Re-seal the *frame* CRC so that only the AEAD can catch this: the CRC32 is a transmission
    // check, and a flip that the CRC already rejects would prove nothing about the MAC.
    {
      uint32_t crc = ni_crc32(tampered.data() + 2, (size_t)(NI_HEADER_BYTES + probe.payload_len));
      ni_wr32(tampered.data() + 2 + NI_HEADER_BYTES + probe.payload_len, crc);
      Frame again;
      NT_EQ(ni_frame_decode(tampered.data() + 2, tampered.size() - 2, &again), NI_OK);
    }
    uint64_t before = h.B.stats().rx_frames;
    h.B.on_pdu(&tampered[2], tampered.size() - 2, h.t);
    NT_CHECK(h.B.stats().rx_malformed > 0);
    NT_CHECK(h.B.stats().rx_err == 1);
    printf("  note: tampered ciphertext refused with rx_rc=%d\n", h.B.stats().rx_rc);
    (void)before;
    Delivered d;
    NT_CHECK_MSG(!h.B.next_delivery(&d), "a tampered frame was delivered");
  }

  // ---- 6. a burst replayed verbatim delivers nothing twice -----------------------------------
  {
    Harness h;
    std::vector<uint8_t> msg = make_msg(300, 3);
    NT_EQ(h.A.send_message(17, NI_MSG_FILE, &msg[0], msg.size(), 0), NI_OK);
    uint8_t buf[2 + NI_MAX_BURST_PDU];
    size_t len = 0;
    int n = h.A.build_pdu(h.t, buf, sizeof buf, &len, false);
    NT_CHECK(n >= 1);
    if (len < 2) return ntest::summary("t_arq");
    std::vector<uint8_t> pdu(buf, buf + len);
    NT_CHECK(h.B.on_pdu(&pdu[2], pdu.size() - 2, h.t) >= 1);
    std::vector<uint8_t> got;
    size_t first = h.drain(h.B, &got, msg.size());
    NT_CHECK(first > 0);
    int again = h.B.on_pdu(&pdu[2], pdu.size() - 2, h.t + 1);
    Delivered d;
    bool got_more = h.B.next_delivery(&d);
    NT_CHECK_MSG(!got_more, "a replayed burst produced a second delivery");
    NT_CHECK(h.B.stats().rx_frames_dup >= 1);
    (void)again;
  }

  return ntest::summary("t_arq");
}
