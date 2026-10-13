// t_frame.cpp — the MAC frame: exact wire layout, validation and rejection of every malformed
// input the radio can hand us. The byte-for-byte layout assertions are what the Kotlin and
// (future) other-language implementations are checked against, so they are written from
// docs/PROTOCOL_SPECIFICATION.md and include/ni/frame_codec.h, not from the encoder.
#include <string.h>
#include <string>
#include <vector>

#include "ni/common.h"
#include "ni/crc.h"
#include "ni/frame_codec.h"
#include "ntest.h"

using namespace ni;

namespace {
uint8_t ref_crc8(const uint8_t* p, size_t n) {  // poly 0x07, init 0xFF, no reflection
  uint8_t c = 0xFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
  }
  return c;
}
std::string bytes_hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) s += std::string{d[p[i] >> 4], d[p[i] & 15]};
  return s;
}
}  // namespace

int main() {
  printf("[t_frame] mac frame codec\n");

  // ---- 1. a known frame, byte for byte ----------------------------------------
  {
    std::vector<uint8_t> pl = {'h', 'e', 'l', 'l', 'o'};
    Frame f;
    f.version = NI_PROTO_VERSION;
    f.type = NI_FT_DATA;
    f.mod_id = 2;
    f.session_id = 0x1234;
    f.seq = 0xDEADBEEF;
    f.ack = 0x00000010;
    f.flags = NI_FLAG_ACK_VALID | NI_FLAG_ENCRYPTED;
    f.payload = pl.data();
    f.payload_len = (uint16_t)pl.size();
    NT_EQ(ni_frame_size(f), NI_HEADER_BYTES + 5 + NI_CRC32_BYTES);
    std::vector<uint8_t> buf(NI_MAX_FRAME, 0);
    int n = ni_frame_encode(buf.data(), buf.size(), f);
    NT_EQ(n, 25);
    NT_EQ(buf[0], (uint8_t)((NI_PROTO_VERSION << 4) | (NI_FT_DATA & 0xF)));
    NT_EQ(buf[1], 2);
    NT_EQ(buf[2], 0x12);
    NT_EQ(buf[3], 0x34);
    NT_EQ(buf[4], 0xDE);  // seq, big endian
    NT_EQ(buf[7], 0xEF);
    NT_EQ(buf[8], 0x00);
    NT_EQ(buf[11], 0x10);  // ack
    NT_EQ(buf[12], 0x00);
    NT_EQ(buf[13], 5);  // payload_len
    NT_EQ(buf[14], (uint8_t)(NI_FLAG_ACK_VALID | NI_FLAG_ENCRYPTED));
    NT_EQ(buf[15], ref_crc8(buf.data(), 15));  // header crc over bytes 0..14
    NT_CHECK(memcmp(&buf[16], "hello", 5) == 0);
    uint32_t c = ni_crc32(buf.data(), 21, 0);  // header + payload, seed 0
    NT_EQ(buf[21], (uint8_t)(c >> 24));
    NT_EQ(buf[22], (uint8_t)(c >> 16));
    NT_EQ(buf[23], (uint8_t)(c >> 8));
    NT_EQ(buf[24], (uint8_t)c);
    printf("  note: encoded frame = %s\n", bytes_hex(buf.data(), (size_t)n).c_str());

    Frame g;
    NT_EQ(ni_frame_decode(buf.data(), (size_t)n, &g), NI_OK);
    NT_EQ((int)g.version, NI_PROTO_VERSION);
    NT_EQ((int)g.type, NI_FT_DATA);
    NT_EQ((int)g.mod_id, 2);
    NT_EQ((int)g.session_id, 0x1234);
    NT_EQ(g.seq, 0xDEADBEEFu);
    NT_EQ(g.ack, 16u);
    NT_EQ((int)g.flags, (int)(NI_FLAG_ACK_VALID | NI_FLAG_ENCRYPTED));
    NT_EQ((int)g.payload_len, 5);
    NT_CHECK(g.payload == &buf[16]);  // borrowed pointer, documented
    NT_CHECK(memcmp(g.payload, "hello", 5) == 0);

    // every frame type round-trips, including the 4-bit wrap of the type field
    for (int t = 0; t < NI_FT_COUNT; t++) {
      Frame s = f;
      s.type = (uint8_t)t;
      std::vector<uint8_t> b(NI_MAX_FRAME);
      int m = ni_frame_encode(b.data(), b.size(), s);
      NT_CHECK(m > 0);
      Frame r;
      NT_EQ(ni_frame_decode(b.data(), (size_t)m, &r), NI_OK);
      NT_EQ((int)r.type, t);
      NT_EQ(b[0], (uint8_t)((NI_PROTO_VERSION << 4) | (t & 0xF)));
    }
  }

  // ---- 2. zero-length payload and maximum payload ------------------------------
  {
    Frame f;
    f.type = NI_FT_PING;
    f.payload = nullptr;
    f.payload_len = 0;
    std::vector<uint8_t> b(NI_MAX_FRAME);
    int n = ni_frame_encode(b.data(), b.size(), f);
    NT_EQ(n, NI_HEADER_BYTES + NI_CRC32_BYTES);
    Frame g;
    NT_EQ(ni_frame_decode(b.data(), (size_t)n, &g), NI_OK);
    NT_EQ((int)g.payload_len, 0);

    std::vector<uint8_t> big(NI_MAX_PAYLOAD, 0x5A);
    f.payload = big.data();
    f.payload_len = NI_MAX_PAYLOAD;
    n = ni_frame_encode(b.data(), b.size(), f);
    NT_EQ(n, NI_MAX_FRAME);
    NT_EQ(ni_frame_decode(b.data(), (size_t)n, &g), NI_OK);
    NT_EQ((int)g.payload_len, NI_MAX_PAYLOAD);
    NT_CHECK(memcmp(g.payload, big.data(), NI_MAX_PAYLOAD) == 0);

    // one byte over the bound must be refused, by both endpoints of the call
    f.payload_len = NI_MAX_PAYLOAD + 1;
    NT_CHECK_MSG(ni_frame_encode(b.data(), b.size(), f) < 0,
                 "encoder accepted an oversized payload (memory safety of the caller)");
    NT_CHECK(ni_frame_size(f) > NI_MAX_FRAME);  // the size helper still reports the truth
  }

  // ---- 3. truncation and tampering are always rejected -------------------------
  {
    std::vector<uint8_t> pl(40, 7);
    Frame f;
    f.type = NI_FT_DATA;
    f.payload = pl.data();
    f.payload_len = (uint16_t)pl.size();
    std::vector<uint8_t> b(NI_MAX_FRAME);
    int n = ni_frame_encode(b.data(), b.size(), f);
    NT_CHECK(n > 0);
    Frame g;
    for (int cut = 0; cut < n; cut++)
      NT_CHECK_MSG(ni_frame_decode(b.data(), (size_t)cut, &g) != NI_OK,
                   "a truncated frame was accepted");
    // header crc catches a change to every single header byte
    for (int i = 0; i < NI_HEADER_BYTES; i++) {
      std::vector<uint8_t> e = b;
      e[i] ^= 0x01;
      NT_CHECK_MSG(ni_frame_decode(e.data(), e.size(), &g) != NI_OK, "header byte flip accepted");
    }
    // and the trailer crc catches the payload
    for (int i = 0; i < 6; i++) {
      std::vector<uint8_t> e = b;
      e[NI_HEADER_BYTES + i] ^= 0x80;
      NT_CHECK(ni_frame_decode(e.data(), e.size(), &g) != NI_OK);
    }
    // declared length larger than the buffer
    {
      std::vector<uint8_t> e = b;
      ni_wr16(&e[12], 200);
      NT_CHECK(ni_frame_decode(e.data(), e.size(), &g) != NI_OK);
    }
    // wrong version nibble
    {
      std::vector<uint8_t> e = b;
      e[0] = (uint8_t)(((NI_PROTO_VERSION + 1) << 4) | (e[0] & 0xF));
      int rc = ni_frame_decode(e.data(), e.size(), &g);
      NT_CHECK_MSG(rc != NI_OK, "an unknown protocol version must be refused");
      printf("  note: version mismatch reported as %d (%s)\n", rc, ni_strerror(rc));
    }
    // buffer longer than the frame: trailing garbage must not be read as part of it
    {
      std::vector<uint8_t> e = b;
      e.resize((size_t)n + 13, 0xFF);
      NT_EQ(ni_frame_decode(e.data(), e.size(), &g), NI_OK);  // tolerant by design: extra bytes
      // belong to the next frame in the same burst (pdu_len delimits them)
    }
  }

  // ---- 4. header-only decode ---------------------------------------------------
  {
    std::vector<uint8_t> pl(9, 3);
    Frame f;
    f.type = NI_FT_ACK;
    f.payload = pl.data();
    f.payload_len = (uint16_t)pl.size();
    std::vector<uint8_t> b(NI_MAX_FRAME);
    int n = ni_frame_encode(b.data(), b.size(), f);
    Frame g;
    NT_EQ(ni_frame_decode_header(b.data(), (size_t)n, &g), NI_OK);
    NT_EQ((int)g.payload_len, 9);
    NT_EQ((int)g.type, NI_FT_ACK);
    NT_CHECK(g.seq == f.seq);
    // too short even for a header
    NT_CHECK(ni_frame_decode_header(b.data(), NI_HEADER_BYTES - 1, &g) != NI_OK);
    // a corrupt header crc is still caught by the header-only decode (that is what it is for)
    std::vector<uint8_t> e = b;
    e[1] ^= 0x10;
    NT_CHECK(ni_frame_decode_header(e.data(), (size_t)n, &g) != NI_OK);
  }

  // ---- 5. crc8 of the header ----------------------------------------------------
  {
    uint8_t d[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    NT_EQ(ni_crc8(d, 9), ref_crc8(d, 9));
    NT_EQ(ni_crc8(d, 0), 0xFF);  // init value, no bytes processed
    for (int i = 0; i < 9; i++) {
      uint8_t e[9];
      memcpy(e, d, 9);
      e[i] ^= 1;
      NT_CHECK(ni_crc8(e, 9) != ni_crc8(d, 9));
    }
  }

  // ---- 6. Writer / Reader -------------------------------------------------------
  {
    uint8_t buf[64];
    Writer w(buf, sizeof buf);
    w.u8(0xAB).u16(0x1234).u32(0xDEADBEEF).i16(-2).bytes("xyz", 3).zeros(2);
    NT_EQ(w.error(), NI_OK);
    NT_EQ(w.size(), (size_t)(1 + 2 + 4 + 2 + 3 + 2));
    NT_EQ(buf[0], 0xAB);
    NT_EQ(buf[1], 0x12);
    NT_EQ(buf[2], 0x34);
    NT_EQ(buf[6], (uint8_t)0xEF);  // u32 big endian tail (starts at offset 3)
    NT_EQ(buf[7], (uint8_t)0xFF);  // i16 -2 -> 0xFFFE
    NT_EQ(buf[9], 'x');
    NT_EQ(buf[11], 'z');
    NT_EQ(buf[12], 0);
    Reader r(buf, w.size());
    NT_EQ(r.u8(), 0xAB);
    NT_EQ(r.u16(), 0x1234);
    NT_EQ(r.u32(), 0xDEADBEEFu);
    NT_EQ(r.i16(), -2);
    char s[4] = {0};
    NT_CHECK(r.bytes(s, 3));
    NT_STR_EQ(std::string(s, 3), "xyz");
    NT_EQ(r.remaining(), (size_t)2);
    uint8_t z[2];
    NT_CHECK(r.bytes(z, 2));
    NT_EQ(r.remaining(), (size_t)0);
    NT_CHECK(!r.bytes(z, 1));  // nothing left
    NT_CHECK(r.error() != NI_OK);

    // overflow must be reported, not written
    uint8_t tiny[4];
    Writer w2(tiny, 3);
    w2.u32(1);
    NT_CHECK(w2.error() != NI_OK);
    Reader r2(tiny, 2);
    NT_EQ(r2.u32(), 0u);
    NT_CHECK(r2.error() != NI_OK);
  }

  // ---- 7. typed payloads round-trip (these are the interop surfaces) -----------
  {
    uint8_t buf[64];
    LinkReport lr;
    lr.next_seq = 0x11223344;
    lr.bitmap = 0xF0F0F0F0;
    lr.snr_q8 = (int16_t)-1536;  // -6.0 dB
    lr.quality = 200;
    lr.profile_id = 5;
    lr.mtu = 240;
    {
      Writer w(buf, sizeof buf);
      lr.encode(w);
      NT_EQ(w.error(), NI_OK);
      LinkReport x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_CHECK(!r.error());
      NT_EQ(x.next_seq, lr.next_seq);
      NT_EQ(x.bitmap, lr.bitmap);
      NT_EQ((int)x.snr_q8, (int)lr.snr_q8);
      NT_EQ((int)x.quality, 200);
      NT_EQ((int)x.profile_id, 5);
      NT_EQ((int)x.mtu, 240);
    }
    {
      PairChallenge pc;
      pc.caps = 0xAAAA;
      pc.peer_id = 0x0BADC0DE;
      for (int i = 0; i < 16; i++) pc.nonce[i] = (uint8_t)(i * 17);
      Writer w(buf, sizeof buf);
      pc.encode(w);
      PairChallenge x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_EQ((int)x.version, NI_PROTO_VERSION);
      NT_EQ(x.caps, pc.caps);
      NT_EQ(x.peer_id, pc.peer_id);
      NT_CHECK(memcmp(x.nonce, pc.nonce, 16) == 0);
    }
    {
      PairResponse pr;
      pr.caps = 0x000F;
      pr.peer_id = 42;
      for (int i = 0; i < 16; i++) {
        pr.nonce[i] = (uint8_t)i;
        pr.mac[i] = (uint8_t)(200 - i);
      }
      Writer w(buf, sizeof buf);
      pr.encode(w);
      PairResponse x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_CHECK(memcmp(x.nonce, pr.nonce, 16) == 0);
      NT_CHECK(memcmp(x.mac, pr.mac, 16) == 0);
    }
    {
      NetInfo ni;
      ni.has_uplink = 1;
      ni.dns_ok = 1;
      ni.http_ok = 1;
      ni.https_ok = 0;
      ni.mtu = 1450;
      ni.probe_latency_ms = 123456;
      ni.uplink_kbps = 987654;
      memcpy(ni.probe_host, "example.invalid", 15);
      ni.probe_host[15] = 0;
      Writer w(buf, sizeof buf);
      ni.encode(w);
      NT_EQ(w.error(), NI_OK);
      NetInfo x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_EQ((int)x.has_uplink, 1);
      NT_EQ((int)x.dns_ok, 1);
      NT_EQ((int)x.https_ok, 0);
      NT_EQ((int)x.mtu, 1450);
      NT_EQ(x.probe_latency_ms, 123456u);
      NT_EQ((long long)x.uplink_kbps, 987654ll);
      NT_STR_EQ(std::string(x.probe_host), "example.invalid");
    }
    {
      CalProbe cp;
      cp.band_id = 3;
      cp.profile_id = 11;
      cp.index = 7;
      cp.frames_expected = 200;
      cp.tx_gain_q15 = 27000;
      Writer w(buf, sizeof buf);
      cp.encode(w);
      CalProbe x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_EQ((int)x.band_id, 3);
      NT_EQ((int)x.profile_id, 11);
      NT_EQ(x.frames_expected, 200);
      NT_EQ(x.tx_gain_q15, 27000);
    }
    {
      CalReport cr;
      cr.sent = 100;
      cr.received = 93;
      cr.crc_fail = 4;
      cr.snr_q8 = 5120;
      cr.noise_rms_dbfs_q8 = -12000;
      cr.sig_rms_dbfs_q8 = -18000;
      cr.clipped = 1;
      cr.usable = 1;
      cr.payload_bytes = 224;
      Writer w(buf, sizeof buf);
      cr.encode(w);
      CalReport x;
      Reader r(buf, w.size());
      NT_EQ(x.decode(r), NI_OK);
      NT_EQ(x.sent, 100);
      NT_EQ(x.received, 93);
      NT_EQ(x.crc_fail, 4);
      NT_EQ((int)x.snr_q8, 5120);
      NT_EQ((int)x.noise_rms_dbfs_q8, -12000);
      NT_EQ((int)x.clipped, 1);
      NT_EQ(x.payload_bytes, 224);
    }
    // a truncated typed payload must report an error rather than read uninitialised memory
    {
      LinkReport lr2;
      Writer w(buf, sizeof buf);
      lr.encode(w);
      Reader r(buf, 6);
      NT_CHECK(lr2.decode(r) != NI_OK);
    }
  }

  // ---- 8. frame type names are present and distinct ----------------------------
  {
    std::vector<std::string> seen;
    for (int t = 0; t < NI_FT_COUNT; t++) {
      std::string s = ni_frame_type_name((uint8_t)t);
      NT_CHECK(!s.empty());
      seen.push_back(s);
    }
    for (size_t i = 0; i < seen.size(); i++)
      for (size_t j = i + 1; j < seen.size(); j++) NT_CHECK(seen[i] != seen[j]);
    NT_STR_EQ(std::string(ni_frame_type_name(200)), std::string(ni_frame_type_name(200)));
    printf("  note: names:");
    for (size_t i = 0; i < seen.size(); i++) printf(" %s", seen[i].c_str());
    printf("\n");
  }

  return ntest::summary("t_frame");
}
