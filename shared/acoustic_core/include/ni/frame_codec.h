// frame_codec.h — the versioned MAC frame: 16-byte header, payload, CRC32 trailer.
//
// Wire layout (all multi-byte fields BIG ENDIAN — explicit, so endianness is never an
// assumption; see docs/PROTOCOL_SPECIFICATION.md):
//
//   off size field
//    0   1   version_type : (version<<4) | (frame_type & 0xF)
//    1   1   mod_id       : modulation profile in use for this burst
//    2   2   session_id   : 0 = unpaired/broadcast
//    4   4   seq          : send sequence number, wraps (32 bit)
//    8   4   ack          : cumulative ack (next seq we expect), valid iff ACK_VALID
//   12   2   payload_len   : 0..NI_MAX_PAYLOAD
//   14   1   flags
//   15   1   hdr_crc      : CRC8(poly 0x07, init 0xFF) over bytes 0..14
//   16   L   payload
//  16+L 4   crc32         : IEEE 802.3 over bytes 0..15+L
//
// Total MAC frame PDU on the PHY = 2 (pdu_len) + 16 + L + 4.
// Every accessor is bounds checked and rejects malformed/oversized input; nothing in this
// file writes outside the caller's buffer.
#ifndef NI_FRAME_CODEC_H
#define NI_FRAME_CODEC_H
#include <string>
#include <string.h>
#include <vector>
#include "ni/common.h"
namespace ni {

struct Frame {
  uint8_t version = NI_PROTO_VERSION;
  uint8_t type = NI_FT_DATA;
  uint8_t mod_id = 0;
  uint16_t session_id = 0;
  uint32_t seq = 0;
  uint32_t ack = 0;
  uint16_t payload_len = 0;
  uint8_t flags = 0;
  const uint8_t* payload = nullptr;  // borrowed pointer into the decode buffer

  bool has(uint8_t f) const { return (flags & f) != 0; }
};

// CRC8 used for the header (poly 0x07, init 0xFF, no reflection).
uint8_t ni_crc8(const void* data, size_t n);

// Encode into dst (cap must be >= ni_frame_size()). Returns total frame length or negative.
int ni_frame_encode(uint8_t* dst, size_t cap, const Frame& f);
size_t ni_frame_size(const Frame& f);

// Decode + validate. On failure, *out may be partially filled for diagnostics but the
// caller must trust only the return code. Never reads past src[0..n).
int ni_frame_decode(const uint8_t* src, size_t n, Frame* out);

// Header-only decode used when the payload length has to be known before validation.
int ni_frame_decode_header(const uint8_t* src, size_t n, Frame* out);

// ------------------------------------------------------------------ payload helpers
// Little struct writers for the typed payloads; they operate on a caller-owned buffer.
class Writer {
 public:
  Writer(uint8_t* p, size_t cap) : p_(p), cap_(cap) {}
  int error() const { return err_; }
  Writer& u8(uint8_t v);
  Writer& u16(uint16_t v);
  Writer& u32(uint32_t v);
  Writer& i16(int16_t v);
  Writer& bytes(const void* src, size_t n);
  Writer& zeros(size_t n);
  size_t size() const { return off_; }
  uint8_t* data() { return p_; }

 private:
  uint8_t* p_;
  size_t cap_, off_ = 0;
  int err_ = NI_OK;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  int error() const { return err_; }
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  int16_t i16();
  bool bytes(void* dst, size_t n);
  size_t remaining() const { return n_ - off_; }

 private:
  const uint8_t* p_;
  size_t n_, off_ = 0;
  int err_ = NI_OK;
};

// ---------------- typed payloads -------------------------------------------------
struct LinkReport {  // carried by ACK/NACK
  uint32_t next_seq = 0;
  uint32_t bitmap = 0;  // bit i set => seq (next_seq - 1 - i) already received
  int16_t snr_q8 = 0;   // received SNR, dB * 256
  uint8_t quality = 0;  // 0..255 composite link quality
  uint8_t profile_id = 0;
  uint16_t mtu = 0;  // receiver's currently acceptable payload
  void encode(Writer& w) const;
  int decode(Reader& r);
};

struct PairChallenge {
  uint8_t version = NI_PROTO_VERSION;
  uint8_t kdf_id = 0;
  uint16_t caps = 0;  // bitmask of supported profile ids
  uint32_t peer_id = 0;
  uint8_t nonce[16];
  void encode(Writer& w) const;
  int decode(Reader& r);
};
struct PairResponse {
  uint16_t caps = 0;
  uint32_t peer_id = 0;
  uint8_t nonce[16];
  uint8_t mac[16];
  void encode(Writer& w) const;
  int decode(Reader& r);
};

struct NetInfo {  // giver's real, measured uplink facts (never fabricated)
  uint8_t has_uplink = 0;
  uint8_t dns_ok = 0;
  uint8_t http_ok = 0;
  uint8_t https_ok = 0;
  uint16_t mtu = 0;
  uint32_t probe_latency_ms = 0;
  uint64_t uplink_kbps = 0;   // measured by the giver, 0 if not measured
  char probe_host[24];
  void encode(Writer& w) const;
  int decode(Reader& r);
};

struct CalProbe {
  uint8_t band_id = 0;
  uint8_t profile_id = 0;
  uint16_t index = 0;
  uint16_t frames_expected = 0;
  uint16_t tx_gain_q15 = 0;
  void encode(Writer& w) const;
  int decode(Reader& r);
};
struct CalReport {
  uint16_t sent = 0;
  uint16_t received = 0;
  uint16_t crc_fail = 0;
  int16_t snr_q8 = 0;
  int16_t noise_rms_dbfs_q8 = 0;
  int16_t sig_rms_dbfs_q8 = 0;
  uint8_t clipped = 0;
  uint8_t usable = 0;
  uint16_t payload_bytes = 0;
  void encode(Writer& w) const;
  int decode(Reader& r);
};

const char* ni_frame_type_name(uint8_t t);

}  // namespace ni
#endif
