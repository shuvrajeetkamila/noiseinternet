// frame_codec.cpp — MAC frame encode/decode and the typed control payloads.
#include "ni/frame_codec.h"
#include "ni/crc.h"
#include <math.h>
#include <stdio.h>

namespace ni {

static const char* kTypeName[NI_FT_COUNT] = {
    "DATA",         "ACK",        "NACK",     "PING",     "PONG",      "CAL_START",
    "CAL_PROBE",    "CAL_REPORT", "PAIR_CHL", "PAIR_RESP", "NETINFO",  "CTRL",
    "RESYNC",       "KEEPALIVE",  "ERROR",    "NOP"};

const char* ni_frame_type_name(uint8_t t) { return t < NI_FT_COUNT ? kTypeName[t] : "?"; }

uint8_t ni_crc8(const void* data, size_t n) {  // poly 0x07, init 0xFF, no reflection
  const uint8_t* p = (const uint8_t*)data;
  uint8_t c = 0xFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
  }
  return c;
}

size_t ni_frame_size(const Frame& f) {
  return NI_HEADER_BYTES + (size_t)f.payload_len + NI_CRC32_BYTES;
}

int ni_frame_encode(uint8_t* dst, size_t cap, const Frame& f) {
  if (!dst) return NI_ERR_PARAM;
  if (f.version != NI_PROTO_VERSION) return NI_ERR_VERSION;
  if (f.type >= NI_FT_COUNT) return NI_ERR_TYPE;
  if (f.payload_len > NI_MAX_PAYLOAD) return NI_ERR_LEN;
  if (f.payload_len && !f.payload) return NI_ERR_PARAM;
  size_t need = ni_frame_size(f);
  if (cap < need) return NI_ERR_SHORT;
  dst[0] = (uint8_t)(((f.version & 0xF) << 4) | (f.type & 0xF));
  dst[1] = f.mod_id;
  ni_wr16(dst + 2, f.session_id);
  ni_wr32(dst + 4, f.seq);
  ni_wr32(dst + 8, f.ack);
  ni_wr16(dst + 12, f.payload_len);
  dst[14] = f.flags;
  dst[15] = 0;
  dst[15] = ni_crc8(dst, 15);
  if (f.payload_len) memcpy(dst + NI_HEADER_BYTES, f.payload, f.payload_len);
  uint32_t c = ni_crc32(dst, NI_HEADER_BYTES + f.payload_len, 0);
  ni_wr32(dst + NI_HEADER_BYTES + f.payload_len, c);
  return (int)need;
}

int ni_frame_decode_header(const uint8_t* src, size_t n, Frame* out) {
  if (!src || !out) return NI_ERR_PARAM;
  if (n < NI_HEADER_BYTES) return NI_ERR_SHORT;
  uint8_t vt = src[0];
  Frame f;
  f.version = (uint8_t)(vt >> 4);
  f.type = (uint8_t)(vt & 0xF);
  f.mod_id = src[1];
  f.session_id = ni_rd16(src + 2);
  f.seq = ni_rd32(src + 4);
  f.ack = ni_rd32(src + 8);
  f.payload_len = ni_rd16(src + 12);
  f.flags = src[14];
  if (f.version != NI_PROTO_VERSION) return NI_ERR_VERSION;
  if (f.type >= NI_FT_COUNT) return NI_ERR_TYPE;
  if (ni_crc8(src, 15) != src[15]) return NI_ERR_CRC;
  if (f.payload_len > NI_MAX_PAYLOAD) return NI_ERR_LEN;
  *out = f;
  return NI_OK;
}

int ni_frame_decode(const uint8_t* src, size_t n, Frame* out) {
  Frame f;
  int rc = ni_frame_decode_header(src, n, &f);
  if (rc != NI_OK) return rc;
  size_t total = NI_HEADER_BYTES + (size_t)f.payload_len + NI_CRC32_BYTES;
  if (n < total) return NI_ERR_SHORT;
  uint32_t want = ni_rd32(src + NI_HEADER_BYTES + f.payload_len);
  uint32_t got = ni_crc32(src, NI_HEADER_BYTES + f.payload_len, 0);
  if (want != got) return NI_ERR_CRC;
  f.payload = src + NI_HEADER_BYTES;
  *out = f;
  return NI_OK;
}

// ------------------------------------------------------------------ Writer / Reader
Writer& Writer::u8(uint8_t v) {
  if (off_ + 1 > cap_) err_ = NI_ERR_OVERFLOW;
  else p_[off_++] = v;
  return *this;
}
Writer& Writer::u16(uint16_t v) {
  if (off_ + 2 > cap_) err_ = NI_ERR_OVERFLOW;
  else { ni_wr16(p_ + off_, v); off_ += 2; }
  return *this;
}
Writer& Writer::u32(uint32_t v) {
  if (off_ + 4 > cap_) err_ = NI_ERR_OVERFLOW;
  else { ni_wr32(p_ + off_, v); off_ += 4; }
  return *this;
}
Writer& Writer::i16(int16_t v) { return u16((uint16_t)v); }
Writer& Writer::bytes(const void* src, size_t n) {
  if (off_ + n > cap_) err_ = NI_ERR_OVERFLOW;
  else { memcpy(p_ + off_, src, n); off_ += n; }
  return *this;
}
Writer& Writer::zeros(size_t n) {
  if (off_ + n > cap_) err_ = NI_ERR_OVERFLOW;
  else { memset(p_ + off_, 0, n); off_ += n; }
  return *this;
}
uint8_t Reader::u8() {
  if (off_ + 1 > n_) { err_ = NI_ERR_SHORT; return 0; }
  return p_[off_++];
}
uint16_t Reader::u16() {
  if (off_ + 2 > n_) { err_ = NI_ERR_SHORT; return 0; }
  uint16_t v = ni_rd16(p_ + off_);
  off_ += 2;
  return v;
}
uint32_t Reader::u32() {
  if (off_ + 4 > n_) { err_ = NI_ERR_SHORT; return 0; }
  uint32_t v = ni_rd32(p_ + off_);
  off_ += 4;
  return v;
}
int16_t Reader::i16() { return (int16_t)u16(); }
bool Reader::bytes(void* dst, size_t n) {
  if (off_ + n > n_) { err_ = NI_ERR_SHORT; return false; }
  memcpy(dst, p_ + off_, n);
  off_ += n;
  return true;
}

// ------------------------------------------------------------------ typed payloads
void LinkReport::encode(Writer& w) const {
  w.u32(next_seq).u32(bitmap).i16(snr_q8).u8(quality).u8(profile_id).u16(mtu);
}
int LinkReport::decode(Reader& r) {
  next_seq = r.u32();
  bitmap = r.u32();
  snr_q8 = r.i16();
  quality = r.u8();
  profile_id = r.u8();
  mtu = r.u16();
  return r.error();
}
void PairChallenge::encode(Writer& w) const {
  w.u8(version).u8(kdf_id).u16(caps).u32(peer_id).bytes(nonce, 16);
}
int PairChallenge::decode(Reader& r) {
  version = r.u8();
  kdf_id = r.u8();
  caps = r.u16();
  peer_id = r.u32();
  if (!r.bytes(nonce, 16)) return r.error();
  if (version != NI_PROTO_VERSION) return NI_ERR_VERSION;
  if (kdf_id != 0) return NI_ERR_UNSUPPORTED;
  return r.error();
}
void PairResponse::encode(Writer& w) const {
  w.u16(caps).u32(peer_id).bytes(nonce, 16).bytes(mac, 16);
}
int PairResponse::decode(Reader& r) {
  caps = r.u16();
  peer_id = r.u32();
  if (!r.bytes(nonce, 16)) return r.error();
  if (!r.bytes(mac, 16)) return r.error();
  return r.error();
}
void NetInfo::encode(Writer& w) const {
  w.u8(has_uplink).u8(dns_ok).u8(http_ok).u8(https_ok).u16(mtu).u32(probe_latency_ms);
  w.u32((uint32_t)(uplink_kbps & 0xFFFFFFFFu));
  char b[24] = {0};
  size_t i = 0;
  for (; i < sizeof(b) - 1 && probe_host[i]; i++) b[i] = probe_host[i];
  w.bytes(b, 24);
}
int NetInfo::decode(Reader& r) {
  has_uplink = r.u8();
  dns_ok = r.u8();
  http_ok = r.u8();
  https_ok = r.u8();
  mtu = r.u16();
  probe_latency_ms = r.u32();
  uplink_kbps = r.u32();
  char b[24] = {0};
  if (!r.bytes(b, 24)) return r.error();
  b[23] = 0;
  memcpy(probe_host, b, 24);
  return r.error();
}
void CalProbe::encode(Writer& w) const {
  w.u8(band_id).u8(profile_id).u16(index).u16(frames_expected).u16(tx_gain_q15);
}
int CalProbe::decode(Reader& r) {
  band_id = r.u8();
  profile_id = r.u8();
  index = r.u16();
  frames_expected = r.u16();
  tx_gain_q15 = r.u16();
  return r.error();
}
void CalReport::encode(Writer& w) const {
  w.u16(sent).u16(received).u16(crc_fail).i16(snr_q8).i16(noise_rms_dbfs_q8)
      .i16(sig_rms_dbfs_q8).u8(clipped).u8(usable).u16(payload_bytes);
}
int CalReport::decode(Reader& r) {
  sent = r.u16();
  received = r.u16();
  crc_fail = r.u16();
  snr_q8 = r.i16();
  noise_rms_dbfs_q8 = r.i16();
  sig_rms_dbfs_q8 = r.i16();
  clipped = r.u8();
  usable = r.u8();
  payload_bytes = r.u16();
  return r.error();
}

// ------------------------------------------------------------------ PHY wrap
int ni_phy_wrap(const uint8_t* frame, size_t n, std::vector<uint8_t>* out) {
  if (!frame || !out) return NI_ERR_PARAM;
  if (n < NI_HEADER_BYTES + NI_CRC32_BYTES || n > NI_MAX_FRAME) return NI_ERR_LEN;
  out->resize(n + NI_PDU_LEN_BYTES);
  ni_wr16(&(*out)[0], (uint16_t)n);
  memcpy(&(*out)[NI_PDU_LEN_BYTES], frame, n);
  return NI_OK;
}
int ni_phy_unwrap(const uint8_t* pdu, size_t n, const uint8_t** frame, size_t* frame_len) {
  if (!pdu || n < NI_PDU_LEN_BYTES) return NI_ERR_SHORT;
  uint16_t fl = ni_rd16(pdu);
  if ((size_t)fl + NI_PDU_LEN_BYTES > n) return NI_ERR_LEN;
  if (fl < NI_HEADER_BYTES + NI_CRC32_BYTES || fl > NI_MAX_FRAME) return NI_ERR_LEN;
  *frame = pdu + NI_PDU_LEN_BYTES;
  *frame_len = fl;
  return NI_OK;
}

double ni_peak(const float* x, size_t n) {
  double p = 0;
  for (size_t i = 0; i < n; i++) {
    double a = fabs((double)x[i]);
    if (a > p) p = a;
  }
  return p;
}
double ni_rms(const float* x, size_t n) {
  if (!n) return 0.0;
  double s = 0;
  for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
  return sqrt(s / (double)n);
}
int ni_clip_count(const float* x, size_t n, double th) {
  int c = 0;
  for (size_t i = 0; i < n; i++)
    if (fabs((double)x[i]) >= th) c++;
  return c;
}
void ni_scale(float* x, size_t n, double g) {
  for (size_t i = 0; i < n; i++) x[i] = (float)((double)x[i] * g);
}

}  // namespace ni
