// arq.cpp — reliability layer: fragmentation, sequence numbers, cumulative ACK + selective
// NAK bitmap, bounded retransmission, duplicate/replay rejection, control-frame
// authentication, AEAD payload privacy.
//
// Sequence rules (docs/PROTOCOL_SPECIFICATION.md sec. 4):
//  * one reliable counter per direction starting at 0; it never rolls back while a session key
//    is live (soft_reset() clears queues only), because the AEAD nonce is derived from it.
//    At 2^32 reliable frames the engine refuses to send and a re-pair is required.
//  * ACK/NACK/PONG/NOP/CAL/KEEPALIVE frames use a separate best-effort counter and are never
//    acknowledged (they are re-sent by the state machine if the situation persists).
//  * the header `ack` field on every frame carries the cumulative next-expected reliable
//    sequence number of the peer, so acknowledgements piggyback on ordinary traffic.
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <sstream>
#include "ni/bits.h"
#include "ni/crc.h"
#include "ni/session.h"

namespace ni {

ArqEngine::ArqEngine(const ArqConfig& cfg)
    : cfg_(cfg), replay_((uint32_t)std::max(64, cfg.dup_window)) {}

void ArqEngine::set_keys(const SessionKeys* k) {
  keys_ = k;
  if (!k) replay_.reset();
}

int ArqEngine::send_message(uint16_t stream, MsgType type, const uint8_t* data, size_t n,
                           uint16_t mflags) {
  if (type >= NI_MSG_COUNT) return NI_ERR_TYPE;
  if (!keys_ && cfg_.require_crypto) return NI_ERR_NOPEER;
  const int frag_hdr = 12;
  const int overhead = frag_hdr + (keys_ ? (int)NI_TAG_BYTES : 0);
  if (cfg_.max_payload <= overhead) return NI_ERR_OVERFLOW;
  const int chunk = cfg_.max_payload - overhead;
  if (n > (size_t)chunk * 4096) return NI_ERR_OVERFLOW;  // bounded per message
  size_t off = 0;
  // FIRST/LAST belong to the message, and only the FIRST fragment may claim FIRST and only the LAST
  // fragment may claim LAST. The caller's flags used to be copied into every fragment and then OR'd
  // with the per-fragment bits, so a message that the transport had to split arrived at its consumer
  // as a series of messages that each said "this is the whole thing": a 60-byte HTTP request became
  // three 14-byte "complete" requests, the gateway marked its head complete after the first one and
  // could never parse it. Every proxied HTTP request failed that way (found through the gateway trace
  // added this session, defect 37). Strip the two bits from what the caller asks for and set them
  // per fragment.
  const uint16_t keep = (uint16_t)(mflags & ~(uint16_t)(NI_MFLAG_FIRST | NI_MFLAG_LAST));
  if (n == 0) {
    Fragment fr;
    fr.stream = stream;
    fr.type = (uint8_t)type;
    fr.flags = (uint8_t)((keep & 0xFF) | NI_MFLAG_FIRST | NI_MFLAG_LAST);
    fr.offset = 0;
    fr.len = 0;
    uint8_t buf[12];
    fr.encode(buf);
    return send_frame(NI_FT_DATA, buf, 12, true);
  }
  while (off < n) {
    size_t take = ni_min((size_t)chunk, n - off);
    Fragment fr;
    fr.stream = stream;
    fr.type = (uint8_t)type;
    fr.flags = (uint8_t)(keep & 0xFF);
    if (off == 0) fr.flags |= NI_MFLAG_FIRST;
    if (off + take >= n) fr.flags |= NI_MFLAG_LAST;
    fr.offset = (uint32_t)off;
    fr.len = (uint16_t)take;
    std::vector<uint8_t> buf((size_t)12 + take);
    fr.encode(&buf[0]);
    memcpy(&buf[12], data + off, take);
    int rc = send_frame(NI_FT_DATA, &buf[0], buf.size(), true);
    if (rc != NI_OK) return rc;
    off += take;
  }
  return NI_OK;
}

int ArqEngine::send_frame(FrameType ft, const uint8_t* payload, size_t n, bool want_ack,
                         uint16_t extra_flags) {
  if (ft >= NI_FT_COUNT) return NI_ERR_TYPE;
  const size_t extra = (want_ack && keys_ && ft != NI_FT_DATA) ? 4 : 0;
  if (n + extra > NI_MAX_PAYLOAD) return NI_ERR_OVERFLOW;
  TxItem it;
  it.f.version = NI_PROTO_VERSION;
  it.f.type = (uint8_t)ft;
  it.f.mod_id = cfg_.mod_id;
  it.f.session_id = cfg_.session_id;
  it.f.seq = want_ack ? (uint32_t)(tx_seq_ & 0xFFFFFFFFu) : (uint32_t)(tx_ctl_++ & 0x7FFFFFFFu);
  if (want_ack) {
    if (tx_seq_ > 0xFFFFFFFEull) return NI_ERR_OVERFLOW;  // re-pairing required, by design
    tx_seq_++;
  }
  it.f.flags = (uint16_t)(NI_FLAG_ACK_VALID | (want_ack ? 0 : NI_FLAG_CTRL) | extra_flags);
  if (ft == NI_FT_DATA && keys_) it.f.flags |= NI_FLAG_ENCRYPTED;
  it.f.ack = (uint32_t)rx_next_;
  it.want_ack = want_ack;
  it.payload.assign(payload, payload + n);
  txq_.push_back(std::move(it));
  st_.pending = txq_.size();
  return NI_OK;
}

void Fragment::encode(uint8_t* p) const {
  ni_wr16(p + 0, stream);
  p[2] = type;
  p[3] = flags;
  ni_wr32(p + 4, offset);
  ni_wr16(p + 8, len);
  ni_wr16(p + 10, reserved);
}
int Fragment::decode(const uint8_t* p, size_t n) {
  if (n < 12) return NI_ERR_SHORT;
  stream = ni_rd16(p + 0);
  type = p[2];
  flags = p[3];
  offset = ni_rd32(p + 4);
  len = ni_rd16(p + 8);
  reserved = ni_rd16(p + 10);
  if (type >= NI_MSG_COUNT) return NI_ERR_TYPE;
  if ((size_t)12 + len > n) return NI_ERR_LEN;
  return NI_OK;
}

namespace {
// AEAD nonce = session || seq || type || pad. Unique per (key, type, seq): the reliable
// counter is monotonic and best-effort frames carry no user data and are never encrypted.
void nonce_for(uint32_t seq, uint8_t type, const uint8_t sid_be[4], uint8_t out[12]) {
  memcpy(out, sid_be, 4);
  ni_wr32(out + 4, seq);
  out[8] = type;
  out[9] = out[10] = out[11] = 0;
}
uint32_t ctl_tag(const uint8_t key[32], const uint8_t* hdr, size_t hlen, const uint8_t* pl,
                 size_t plen) {
  uint8_t buf[NI_HEADER_BYTES + 32];
  memcpy(buf, hdr, hlen);
  memcpy(buf + hlen, key, 32);
  uint64_t a = ni_fnv1a(buf, hlen + 32);
  uint64_t b = ni_fnv1a(pl, plen);
  return (uint32_t)((a * 2654435761ull) ^ (b * 40503ull) ^ (a >> 17));
}
// Finalises one frame: applies the security transform then encodes it into dst.
int seal_and_encode(const SessionKeys* keys, Frame& f, std::vector<uint8_t>& body, bool is_data,
                    uint8_t* dst, size_t cap) {
  if (is_data && keys) {
    std::vector<uint8_t> ct(body.size());
    uint8_t tag[16], nonce[12];
    nonce_for(f.seq, NI_FT_DATA, keys->sid_be, nonce);
    ni_aead_seal(keys->tx, nonce, nullptr, 0, body.data(), body.size(), ct.data(), tag);
    ct.insert(ct.end(), tag, tag + 16);
    body.swap(ct);
    f.flags |= NI_FLAG_ENCRYPTED;
  } else if (!is_data && keys && (f.flags & NI_FLAG_CTRL) == 0) {
    // acked control frames get a truncated MAC (integrity without encryption: no user data)
    uint8_t hdr[NI_HEADER_BYTES];
    Frame tmp = f;
    tmp.payload = body.data();
    tmp.payload_len = (uint16_t)(body.size() + 4);
    if (ni_frame_encode(hdr, sizeof hdr, tmp) == (int)sizeof hdr) {
      uint8_t t[4];
      ni_wr32(t, ctl_tag(keys->tx, hdr, sizeof hdr, body.data(), body.size()));
      body.insert(body.end(), t, t + 4);
    }
  }
  f.payload = body.data();
  f.payload_len = (uint16_t)body.size();
  return ni_frame_encode(dst, cap, f);
}
}  // namespace

LinkReport ArqEngine::make_report() const {
  LinkReport rep;
  rep.next_seq = (uint32_t)rx_next_;
  for (auto& kv : ooo_) {
    if (kv.first < 32) rep.bitmap |= (1u << kv.first);
  }
  rep.snr_q8 = last_snr_q8_;
  rep.quality = last_quality_;
  rep.profile_id = cfg_.mod_id;
  rep.mtu = (uint16_t)cfg_.max_payload;
  return rep;
}

// ------------------------------------------------------------------ TX packing
int ArqEngine::pack_frames(int64_t now_ms, uint8_t* pdu, size_t cap, size_t* out_len,
                           int max_frames, int max_bytes) {
  size_t off = 0;
  int n = 0;
  *out_len = 0;
  burst_frames_.clear();
  const size_t limit = ni_min(cap, (size_t)max_bytes);
  if (ack_dirty_) {
    Frame f;
    memset(&f, 0, sizeof f);
    f.version = NI_PROTO_VERSION;
    f.type = nack_pending_ ? NI_FT_NACK : NI_FT_ACK;
    f.mod_id = cfg_.mod_id;
    f.session_id = cfg_.session_id;
    f.seq = tx_ctl_++ & 0x7FFFFFFFu;
    f.ack = (uint32_t)rx_next_;
    f.flags = NI_FLAG_ACK_VALID | (nack_pending_ ? NI_FLAG_NACK : 0);
    LinkReport rep = make_report();
    std::vector<uint8_t> body(64, 0);
    Writer w(body.data(), body.size());
    rep.encode(w);
    body.resize(w.size());
    if (w.error()) return w.error();
    // The acknowledgement frame is 16 + body + 4 (+4 tag) bytes; if it does not fit in what is
    // left of this burst, that is not an error - the ack is simply still pending for the next one.
    size_t need = NI_HEADER_BYTES + body.size() + NI_CRC32_BYTES + (keys_ ? 4u : 0u);  // + MAC
    // An acknowledgement that does not fit in the remaining room is *deferred*, not fatal. Returning
    // 0 here used to abandon the data frames already encoded into this burst, whose senders had
    // already been told the burst was in flight - so with a small per-burst capacity (the FDD profiles
    // carry 80 bytes of PDU) a burst could sit in that state indefinitely. Now the ack stays pending
    // and the data goes out.
    if (need <= limit - off) {
      int rc = seal_and_encode(keys_, f, body, false, pdu + off, limit - off);
      if (rc > 0) {
        off += (size_t)rc;
        n++;
        st_.acks_sent++;
        if (nack_pending_) st_.nacks_sent++;
        ack_dirty_ = false;
        nack_pending_ = false;
      }
    }
  }
  while (n < max_frames && off + NI_HEADER_BYTES + NI_CRC32_BYTES < limit) {
    int pick = -1;
    for (size_t i = 0; i < txq_.size(); i++) {
      const TxItem& it = txq_[i];
      if (it.acked || it.in_flight) continue;
      if (it.retries && it.next_retry_ms > now_ms) continue;
      pick = (int)i;
      break;
    }
    if (pick < 0) break;
    TxItem& it = txq_[(size_t)pick];
    std::vector<uint8_t> body = it.payload;
    Frame f = it.f;
    f.ack = (uint32_t)rx_next_;
    f.flags |= NI_FLAG_ACK_VALID;
    f.mod_id = cfg_.mod_id;
    if (it.retries) f.flags |= NI_FLAG_DUP_WIN;  // marks a retransmission, useful in reports
    // The burst is full when the *encoded* frame no longer fits, and that is the normal end of a
    // burst, not a failure. This used to return the encoder's NI_ERR_SHORT to the caller, which
    // treats a negative as "nothing to send" - so any message needing more than one burst was
    // never transmitted at all, on any profile, and the queue just sat there. It is also why a
    // 2500-byte transfer over the modelled link made no progress for seconds at a time.
    size_t need = NI_HEADER_BYTES + it.payload.size() + NI_CRC32_BYTES;
    if (keys_) need += (f.type == NI_FT_DATA) ? NI_TAG_BYTES : 4;
    if (need > limit - off) break;
    int rc = seal_and_encode(keys_, f, body, f.type == NI_FT_DATA, pdu + off, limit - off);
    if (rc <= 0) break;  // never lose the frame: it stays queued for the next burst
    it.in_flight = true;
    it.sent_ms = now_ms;
    // A best-effort frame has nothing to wait for: the peer's cumulative acknowledgement counts the
    // reliable sequence space, so a frame queued with want_ack=false can never be acknowledged and
    // would sit in this queue for the life of the session (an unbounded leak, and `pending` lying
    // about how much work is outstanding). One copy on the wire is the contract, so release it now.
    if (!it.want_ack) it.acked = true;
    off += (size_t)rc;
    n++;
    burst_frames_.push_back(f.seq);
    if (!it.retries) st_.tx_frames++;
    st_.tx_bytes_payload += f.payload_len;
  }
  if (n > 0) {
    txq_.erase(std::remove_if(txq_.begin(), txq_.end(), [](const TxItem& i) { return i.acked; }),
               txq_.end());
    st_.pending = txq_.size();
  }
  *out_len = off;
  return n;
}

int ArqEngine::build_pdu(int64_t now_ms, uint8_t* pdu, size_t cap, size_t* out_len,
                         bool allow_poll) {
  int n = pack_frames(now_ms, pdu, cap, out_len, cfg_.max_frames_per_burst,
                      cfg_.max_burst_bytes);
  if (n < 0) return n;
  if (n > 0) {
    size_t total = *out_len;
    if (total + NI_PDU_LEN_BYTES > cap) return NI_ERR_OVERFLOW;
    std::vector<uint8_t> tmp(pdu, pdu + total);
    memcpy(pdu + NI_PDU_LEN_BYTES, &tmp[0], total);
    ni_wr16(pdu, (uint16_t)total);
    *out_len = total + NI_PDU_LEN_BYTES;
    st_.bursts_tx++;
    last_emit_ms_ = now_ms;
    return n;
  }
  if (!allow_poll) return 0;
  // idle: the master emits a KEEPALIVE so the slave gets a transmission opportunity
  memset(pdu, 0, 2 + NI_HEADER_BYTES + NI_CRC32_BYTES);
  Frame f;
  memset(&f, 0, sizeof f);
  f.version = NI_PROTO_VERSION;
  f.type = NI_FT_KEEPALIVE;
  f.mod_id = cfg_.mod_id;
  f.session_id = cfg_.session_id;
  f.seq = tx_ctl_++ & 0x7FFFFFFFu;
  f.ack = (uint32_t)rx_next_;
  f.flags = NI_FLAG_ACK_VALID;
  std::vector<uint8_t> body;
  int rc = seal_and_encode(keys_, f, body, false, pdu + 2, cap - 2);
  if (rc <= 0) return rc;
  ni_wr16(pdu, (uint16_t)rc);
  *out_len = (size_t)rc + 2;
  burst_frames_.assign(1, f.seq);
  st_.bursts_tx++;
  last_emit_ms_ = now_ms;
  return 1;
}

// Undo the in-flight marking of a burst that never made it onto the wire. The frames are the ones
// pack_frames just took; putting them back is what lets the next tick retry them immediately instead
// of waiting for an ack that can never arrive.
void ArqEngine::cancel_burst() {
  for (uint32_t seq : burst_frames_)
    for (auto& it : txq_)
      if (it.f.seq == seq && it.in_flight) { it.in_flight = false; break; }
  burst_frames_.clear();
  st_.pending = txq_.size();
}

void ArqEngine::note_emitted(size_t frames_in_burst, int64_t now_ms, double airtime_ms) {
  (void)frames_in_burst;
  int to = cfg_.ack_timeout_ms + (int)(2.0 * airtime_ms) + (int)(0.5 * airtime_ms);
  for (auto& it : txq_) {
    if (!it.in_flight || it.acked) continue;
    it.next_retry_ms = now_ms + (to > 150 ? to : 150);
  }
  txq_.erase(std::remove_if(txq_.begin(), txq_.end(), [](const TxItem& i) { return i.acked; }),
             txq_.end());
  st_.pending = txq_.size();
}

// ------------------------------------------------------------------ RX
int ArqEngine::deliver_data(const Frame& f, int64_t now_ms) {
  if (cfg_.session_id && f.session_id && f.session_id != cfg_.session_id) return NI_ERR_NOPEER;
  std::vector<uint8_t> plain;
  const uint8_t* p = f.payload;
  size_t n = f.payload_len;
  if (f.has(NI_FLAG_ENCRYPTED)) {
    if (!keys_) return NI_ERR_CRYPTO;
    if (n < NI_TAG_BYTES) return NI_ERR_SHORT;
    size_t clen = n - NI_TAG_BYTES;
    plain.resize(clen);
    uint8_t nonce[12];
    nonce_for(f.seq, NI_FT_DATA, keys_->sid_be, nonce);
    int rc = ni_aead_open(keys_->rx, nonce, nullptr, 0, p, clen, plain.data(), p + clen);
    if (rc != NI_OK) return rc;  // the caller classifies and counts the refusal
    p = plain.data();
    n = plain.size();
  } else if (cfg_.require_crypto) {
    // Refuse unencrypted DATA whenever the link requires crypto. Gating this on this endpoint
    // having keys was a downgrade hole: during the pairing race the side that had not installed
    // its keys yet accepted any plaintext DATA an on-path attacker chose to inject.
    return NI_ERR_CRYPTO;
  }
  Fragment fr;
  if (fr.decode(p, n) != NI_OK) return NI_ERR_TYPE;
  Delivered d;
  d.stream = fr.stream;
  d.type = fr.type;
  d.flags = fr.flags;
  d.offset = fr.offset;
  d.len = fr.len;
  d.seq = f.seq;
  d.data.assign(p + 12, p + 12 + fr.len);
  deliverq_.push_back(std::move(d));
  st_.rx_bytes_payload += f.payload_len;
  if (!st_.session_start_ms) st_.session_start_ms = now_ms;
  return NI_OK;
}

void ArqEngine::apply_report(uint32_t next_seq, uint32_t bitmap, int64_t now_ms) {
  uint64_t want = next_seq;
  bool changed = false;
  for (auto& it : txq_) {
    if (!it.want_ack || it.acked) continue;
    uint32_t s = it.f.seq;
    bool acked = false;
    if ((uint64_t)s < want) {
      acked = true;  // already covered by the cumulative ack (we are behind: peer moved on)
    } else if ((uint64_t)s < want + 32u && (bitmap & (1u << (s - (uint32_t)want)))) {
      acked = true;
    }
    if (!acked) continue;
    if (it.f.type == NI_FT_CTRL && it.f.payload_len >= 4 && ni_rd16(it.f.payload) == 1) {
      // our own SET_PROFILE was acknowledged: the peer has switched (or is about to), so the link
      // may now move its own modem without splitting the two ends across profiles.
      profile_acked_ = true;
      profile_req_active_ = false;
    }
    if (it.in_flight && it.sent_ms && now_ms >= it.sent_ms) {
      double rtt = (double)(now_ms - it.sent_ms);
      st_.rtt_ms_last = rtt;
      if (rtt > st_.rtt_ms_max) st_.rtt_ms_max = rtt;
      rtt_sum_ += (int64_t)rtt;
      rtt_n_++;
      st_.rtt_ms_avg = (double)rtt_sum_ / (double)rtt_n_;
    }
    it.acked = true;
    changed = true;
  }
  if (changed) {
    txq_.erase(std::remove_if(txq_.begin(), txq_.end(), [](const TxItem& i) { return i.acked; }),
               txq_.end());
    st_.pending = txq_.size();
  }
  link_up_ = true;
  last_rx_ms_ = now_ms;
  st_.last_rx_ms = now_ms;
  if (state_ < NI_ST_SESSION_OK) state_ = NI_ST_SESSION_OK;
}

bool ArqEngine::on_timer(int64_t now_ms) {
  bool due = false;
  bool lost_any = false;
  for (auto& it : txq_) {
    if (!it.in_flight || it.acked) continue;
    if (now_ms < it.next_retry_ms) continue;
    it.in_flight = false;
    it.retries++;
    st_.tx_retries++;
    due = true;
    if (it.retries > cfg_.max_retries) {
      it.giveup = true;
      lost_any = true;
      st_.lost_frames++;
    }
  }
  if (lost_any) {
    txq_.erase(std::remove_if(txq_.begin(), txq_.end(), [](const TxItem& i) { return i.giveup; }),
               txq_.end());
    link_up_ = false;
    state_ = NI_ST_ERROR;
  }
  st_.pending = txq_.size();
  return due;
}

int ArqEngine::request_profile(int profile_id) {
  if (profile_id < 0 || profile_id >= ni_profile_count()) return NI_ERR_PARAM;
  uint8_t pl[4];
  ni_wr16(pl, 1);  // op SET_PROFILE
  ni_wr16(pl + 2, (uint16_t)profile_id);
  ctrl_profile_ = (uint16_t)profile_id;
  profile_req_pending_ = true;
  profile_req_active_ = true;
  profile_req_ms_ = 0;   // stamped by the caller's clock on the next tick
  return send_frame(NI_FT_CTRL, pl, 4, true);
}

int ArqEngine::next_event_ms(int64_t now_ms) const {
  int best = cfg_.poll_interval_ms;
  if (ack_dirty_) return 1;
  for (auto& it : txq_) {
    if (it.acked) continue;
    if (!it.in_flight) {
      if (it.retries && it.next_retry_ms > now_ms) {
        best = std::min(best, (int)(it.next_retry_ms - now_ms));
      } else {
        return 1;
      }
      continue;
    }
    int d = (int)(it.next_retry_ms - now_ms);
    if (d < best) best = d < 0 ? 0 : d;
  }
  if (best < 1) best = 1;
  return best;
}

void ArqEngine::reset_counters() {
  st_ = ArqStats();
  rtt_sum_ = rtt_n_ = 0;
}

void ArqEngine::soft_reset() {
  // Clears outstanding work but never the counters: sequence numbers and AEAD nonces must not
  // repeat for a live key. Re-pairing is what resets those.
  txq_.clear();
  deliverq_.clear();
  ooo_.clear();
  ack_dirty_ = false;
  link_up_ = false;
  st_.pending = 0;
}

int ArqEngine::on_pdu(const uint8_t* pdu, size_t n, int64_t now_ms) {
  int accepted = 0;
  size_t off = 0;
  while (off + NI_HEADER_BYTES + NI_CRC32_BYTES <= n) {
    std::vector<uint8_t> img(pdu + off, pdu + n);  // copy so out-of-order frames stay valid
    Frame f;
    int rc = ni_frame_decode(&img[0], img.size(), &f);
    if (rc != NI_OK) {
      st_.rx_malformed++;
      st_.rx_err = 3;
      return accepted ? accepted : rc;
    }
    if (f.version != NI_PROTO_VERSION) {
      st_.rx_malformed++;
      st_.rx_err = 4;
      break;
    }
    // verify the control-frame MAC before trusting anything
    if (keys_ && f.type != NI_FT_DATA && f.payload_len >= 4) {
      uint8_t hdr[NI_HEADER_BYTES];
      Frame tmp = f;
      tmp.payload_len = (uint16_t)(f.payload_len - 4);
      std::vector<uint8_t> body(f.payload, f.payload + tmp.payload_len);
      tmp.payload = body.data();
      if (ni_frame_encode(hdr, sizeof hdr, tmp) == (int)sizeof hdr) {
        uint32_t want = ni_rd32(f.payload + tmp.payload_len);
        if (ctl_tag(keys_->rx, hdr, sizeof hdr, body.data(), body.size()) != want) {
          st_.rx_malformed++;
          st_.rx_err = 5;
          st_.rx_err_type = f.type;
          break;  // forged or corrupted control frame: stop treating this burst as trusted
        }
      }
    }
    off += NI_HEADER_BYTES + f.payload_len + NI_CRC32_BYTES;
    st_.rx_frames++;
    accepted++;
    last_rx_ms_ = now_ms;
    st_.last_rx_ms = now_ms;
    if (f.has(NI_FLAG_ACK_VALID) && f.type != NI_FT_DATA) apply_report(f.ack, 0, now_ms);
    switch (f.type) {
      case NI_FT_DATA: {
        uint64_t s = f.seq;
        if (s < rx_next_) {
          st_.rx_frames_dup++;
          break;
        }
        if (s >= rx_next_ + (uint64_t)cfg_.dup_window) {
          st_.rx_malformed++;
          st_.rx_err = 6;
          st_.rx_err_type = f.type;
          break;  // far outside the window
        }
        if (s == rx_next_) {
          Frame cur = f;
          // `hold` owns the bytes that cur.payload points at *for as long as the loop needs them*.
          // Declaring it inside the loop body was a use-after-free: the vector died at the end of an
          // iteration while the next iteration was still passing its buffer to deliver_data(), which
          // glibc hid (the freed block kept its contents) and MSVC did not - t_arq passed on Linux
          // and failed the payload comparison on Windows, with the byte count correct (defect 25).
          std::vector<uint8_t> hold;
          for (;;) {
            int drc = deliver_data(cur, now_ms);
            if (drc != NI_OK) {
              st_.rx_malformed++;
              // 1 = the frame claimed to be encrypted and the tag did not verify, 2 = it arrived in
              // plaintext on a link that requires crypto, 7 = the fragment header itself is junk.
              st_.rx_err = cur.has(NI_FLAG_ENCRYPTED) ? 1 : (drc == NI_ERR_CRYPTO ? 2 : 7);
              st_.rx_err_type = cur.type;
              st_.rx_rc = drc;
              break;
            }
            rx_next_++;
            if (!ooo_.empty()) {  // stored keys are relative: rebase by one
              std::map<uint32_t, std::vector<uint8_t>> moved;
              for (auto& kv : ooo_)
                if (kv.first) moved[kv.first - 1] = std::move(kv.second);
              ooo_ = std::move(moved);
            }
            auto it = ooo_.find(0u);
            if (it == ooo_.end()) break;
            hold = std::move(it->second);
            ooo_.erase(it);
            if (ni_frame_decode(hold.data(), hold.size(), &cur) != NI_OK) break;
          }
        } else {
          ooo_[(uint32_t)(s - rx_next_)] = img;
        }
        ack_dirty_ = true;
        break;
      }
      case NI_FT_ACK:
      case NI_FT_NACK: {
        LinkReport rep;
        Reader r(f.payload, f.payload_len >= 4 ? f.payload_len - 4 : f.payload_len);
        if (rep.decode(r) == NI_OK) {
          apply_report(rep.next_seq, rep.bitmap, now_ms);
          last_snr_q8_ = rep.snr_q8;
          last_quality_ = rep.quality;
          if (rep.profile_id != cfg_.mod_id && rep.profile_id < ni_profile_count())
            cfg_.mod_id = rep.profile_id;
          if (rep.mtu && rep.mtu < (uint16_t)cfg_.max_payload) cfg_.max_payload = rep.mtu;
        }
        if (f.type == NI_FT_NACK) {
          for (auto& it : txq_)
            if (it.in_flight) {
              it.in_flight = false;
              it.retries++;
              it.next_retry_ms = now_ms;
              st_.tx_retries++;
            }
        }
        break;
      }
      case NI_FT_PING: {
        std::vector<uint8_t> echo(f.payload, f.payload + f.payload_len);
        send_frame(NI_FT_PONG, echo.data(), echo.size(), true);
        break;
      }
      case NI_FT_PONG: {
        if (f.payload_len >= 4) {
          uint32_t t0 = ni_rd32(f.payload);
          int64_t rtt = (int64_t)((uint32_t)now_ms - t0);
          if (rtt > 0 && rtt < 600000) {
            st_.rtt_ms_last = (double)rtt;
            st_.rtt_ms_avg = st_.rtt_ms_avg ? (st_.rtt_ms_avg * 3.0 + (double)rtt) / 4.0
                                            : (double)rtt;
            if (rtt > st_.rtt_ms_max) st_.rtt_ms_max = (double)rtt;
          }
        }
        break;
      }
      case NI_FT_KEEPALIVE:
        ack_dirty_ = true;  // give the peer our state as soon as we get a slot
        break;
      case NI_FT_CTRL: {
        if (f.payload_len >= 8) {
          uint16_t op = ni_rd16(f.payload), arg = ni_rd16(f.payload + 2);
          if (op == 1 && arg < ni_profile_count()) {
            cfg_.mod_id = (uint8_t)arg;
            profile_req_pending_ = false;
            // Changing the ARQ's idea of the profile is not enough: the modem that renders and
            // decodes is owned by the link, and without this hook the far end kept transmitting on
            // one profile and listening with another - the peers silently stopped being able to
            // read each other after any adaptation.
            if (on_profile_request) on_profile_request((int)arg);
            ack_dirty_ = true;   // acknowledge the request so the requester knows to move too
          }
        }
        break;
      }
      case NI_FT_PAIR_CHALLENGE:
      case NI_FT_PAIR_RESPONSE:
        // A handshake frame queued with want_ack consumes a slot in the reliable transmit
        // sequence space just like DATA does, so the cumulative acknowledgement has to move for it
        // as well. If it does not, the sender's queue entry is never released - and because an
        // in-flight entry is never re-sent, the handshake stalls with one copy of each frame on
        // the wire and no way to recover. Frames flagged NI_FLAG_CTRL are best-effort and keep
        // their sequence number out of this space, which is what the !CTRL test is for.
        if (!f.has(NI_FLAG_CTRL)) {
          if ((uint64_t)f.seq == rx_next_) {
            rx_next_++;
            ack_dirty_ = true;
          }
        }
        if (on_pair) on_pair(f, now_ms);
        break;
      case NI_FT_NETINFO: {
        NetInfo ni;
        size_t use = f.payload_len >= 4 ? f.payload_len - 4 : f.payload_len;
        Reader r(f.payload, use);
        if (ni.decode(r) == NI_OK && on_netinfo) on_netinfo(ni);
        break;
      }
      case NI_FT_CAL_START:
      case NI_FT_CAL_PROBE:
      case NI_FT_CAL_REPORT:
        if (on_cal) on_cal(f, now_ms);
        break;
      case NI_FT_RESYNC:
        soft_reset();
        break;
      case NI_FT_NOP:
      case NI_FT_ERROR:
      default:
        break;
    }
  }
  if (accepted && state_ < NI_ST_FRAME_OK) state_ = NI_ST_FRAME_OK;
  st_.bursts_rx++;
  return accepted;
}

bool ArqEngine::next_delivery(Delivered* out) {
  if (deliverq_.empty()) return false;
  *out = deliverq_.front();
  deliverq_.erase(deliverq_.begin());
  bytes_up_win_ += out->data.size();
  return true;
}

std::string ArqStats::to_json() const {
  std::ostringstream o;
  o << "{\"tx_frames\": " << tx_frames << ", \"tx_retries\": " << tx_retries
    << ", \"tx_payload_bytes\": " << tx_bytes_payload << ", \"rx_frames\": " << rx_frames
    << ", \"rx_payload_bytes\": " << rx_bytes_payload << ", \"rx_duplicates\": " << rx_frames_dup
    << ", \"rx_crc_fail\": " << rx_crc_fail << ", \"rx_malformed\": " << rx_malformed
    << ", \"bursts_tx\": " << bursts_tx << ", \"bursts_rx\": " << bursts_rx
    << ", \"lost_frames\": " << lost_frames << ", \"pending\": " << pending
    << ", \"acks_sent\": " << acks_sent << ", \"nacks_sent\": " << nacks_sent
    << ", \"rx_err\": " << rx_err << ", \"rx_err_type\": " << rx_err_type
    << ", \"rtt_ms_avg\": " << rtt_ms_avg << ", \"rtt_ms_max\": " << rtt_ms_max
    << ", \"down_bps\": " << throughput_down_bps << ", \"up_bps\": " << throughput_up_bps
    << ", \"session_ms\": " << (last_rx_ms ? last_rx_ms - session_start_ms : 0) << "}";
  return o.str();
}

std::string ni_json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 32) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
          o += b;
        } else
          o += c;
    }
  }
  return o;
}

}  // namespace ni
