// t_crc_bits.cpp — checksums, bit order, scrambler, Gray code. Every expectation is computed
// independently in this file (or taken from a published reference), never from the library.
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ni/bits.h"
#include "ni/common.h"
#include "ni/crc.h"
#include "ntest.h"

using namespace ni;

namespace {
// Independent bitwise implementations, written straight from the definitions.
uint16_t ref_crc16_ccitt(const uint8_t* d, size_t n, uint16_t init) {
  uint16_t c = init;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}
uint32_t ref_crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
  }
  return ~c;
}
uint32_t ref_fnv1a(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 16777619u;
  }
  return h;
}
}  // namespace

int main() {
  printf("[t_crc_bits] crc / bit order / scrambler / gray\n");
  ni_crc_tables_init();

  const char* s1234 = "123456789";
  std::vector<uint8_t> d(s1234, s1234 + 9);

  // Check values from published references, cross-checked against an independent implementation
  // of each definition before being hard-coded here (CRC-32 and CRC-16/CCITT-FALSE both use the
  // string "123456789" as their standard test input; the FNV reference vectors are the empty
  // string and "a"). An expectation invented from memory is worse than no test at all - it turns
  // a correct library into a failing test, which is exactly how this line was wrong once.
  NT_EQ((long long)ni_fnv1a((const uint8_t*)"", 0), (long long)0x811c9dc5u);
  NT_EQ((long long)ni_fnv1a((const uint8_t*)"a", 1), (long long)0xe40c292cu);
  NT_EQ((long long)ni_crc32(d.data(), 9), (long long)0xCBF43926u);
  NT_EQ((long long)ni_crc16(d.data(), 9, 0xFFFF), (long long)0x29B1u);
  NT_EQ((long long)ni_fnv1a(d.data(), 9), (long long)0xbb86b11cu);
  NT_EQ((long long)ni_fnv1a(d.data(), 9), (long long)ref_fnv1a(d.data(), 9));
  NT_EQ((long long)ni_crc32(d.data(), 9), (long long)ref_crc32(d.data(), 9));
  NT_EQ((long long)ni_crc16(d.data(), 9, 0xFFFF), (long long)ref_crc16_ccitt(d.data(), 9, 0xFFFF));

  // empty and single-byte cases, plus the seed parameter actually being used
  NT_EQ((long long)ni_crc32(nullptr, 0, 0), (long long)0x00000000u);
  NT_EQ((long long)ni_crc16(nullptr, 0, 0xFFFF), (long long)0xFFFFu);
  NT_CHECK(ni_crc16(d.data(), 9, 0) != ni_crc16(d.data(), 9, 0xFFFF));
  uint8_t one = 0x00;
  NT_EQ((long long)ni_crc32(&one, 1, 0), (long long)ref_crc32(&one, 1));

  // error detection: one flipped bit must change both checksums, for every bit position
  for (size_t i = 0; i < 9; i++) {
    for (int b = 0; b < 8; b++) {
      std::vector<uint8_t> e = d;
      e[i] ^= (uint8_t)(1u << b);
      NT_CHECK_MSG(ni_crc32(e.data(), e.size(), 0) != ni_crc32(d.data(), d.size(), 0),
                   "crc32 blind to a single bit flip");
      NT_CHECK(ni_crc16(e.data(), e.size(), 0xFFFF) != ni_crc16(d.data(), d.size(), 0xFFFF));
      NT_CHECK(ni_fnv1a(e.data(), e.size()) != ni_fnv1a(d.data(), d.size()));
    }
  }
  // and a length-preserving reorder must be caught too (CRC is not a checksum of sums)
  {
    std::vector<uint8_t> sw = d;
    std::swap(sw[0], sw[8]);
    NT_CHECK(ni_crc32(sw.data(), sw.size(), 0) != ni_crc32(d.data(), d.size(), 0));
  }

  // ---- bit order: MSB first, byte bit 7 is the first bit on the wire ----
  {
    uint8_t in[3] = {0x80, 0x01, 0xA5};
    uint8_t bits[24], back[3];
    ni_bits_from_bytes(in, 3, bits);
    NT_EQ(bits[0], 1);
    NT_EQ(bits[1], 0);
    NT_EQ(bits[7], 0);
    NT_EQ(bits[8], 0);
    NT_EQ(bits[15], 1);
    NT_EQ(bits[16], 1);  // 0xA5 = 1010 0101
    NT_EQ(bits[17], 0);
    NT_EQ(bits[23], 1);
    ni_bytes_from_bits(bits, 24, back);
    NT_CHECK(memcmp(in, back, 3) == 0);
    NT_EQ(ni_bits_count(7), (size_t)56);
  }

  // ---- scrambler: invertible, and self-synchronising after 15 bits ----
  {
    std::mt19937 rng(12345);
    for (int len : {1, 2, 15, 16, 100, 1000, 4096}) {
      std::vector<uint8_t> a(len), b(len), c(len);
      for (int i = 0; i < len; i++) a[i] = (uint8_t)(rng() & 1);
      ni_scramble(a.data(), a.size(), b.data());
      ni_descramble(b.data(), b.size(), c.data());
      NT_CHECK_MSG(memcmp(a.data(), c.data(), a.size()) == 0, "scrambler is not invertible");
      if (len >= 32) {
        // Self-synchronising property: a receiver that starts reading at an arbitrary point in
        // the *scrambled* stream has a wrong shift register for at most 15 bits, then recovers
        // exactly. This is what lets the modem drop a leading symbol and still decode.
        std::vector<uint8_t> tail((size_t)len - 15);
        ni_descramble(b.data() + 15, tail.size(), tail.data());
        size_t checked = 0, bad = 0;
        for (size_t j = 15; j < tail.size(); j++) {
          bad += (tail[j] != a[15 + j]) ? 1u : 0u;
          checked++;
        }
        NT_CHECK_MSG(checked > 0 && bad == 0, "descrambler does not re-synchronise after 15 bits");
      }
      // and it must actually spread the data (an all-ones burst must not pass through unchanged)
      if (len >= 100) {
        std::vector<uint8_t> ones((size_t)len, 1), scr((size_t)len, 0), back((size_t)len, 0);
        ni_scramble(ones.data(), ones.size(), scr.data());
        size_t n1 = 0;
        for (int i = 0; i < len; i++) n1 += scr[i];
        NT_CHECK(n1 > (size_t)len / 4 && n1 < (size_t)len * 3 / 4);  // roughly balanced
        ni_descramble(scr.data(), scr.size(), back.data());
        NT_CHECK(memcmp(ones.data(), back.data(), ones.size()) == 0);
      }
    }
    // fixed init state => deterministic, which is what lets a vector be pinned
    std::vector<uint8_t> z(32, 0), s1(32), s2(32);
    ni_scramble(z.data(), z.size(), s1.data());
    ni_scramble(z.data(), z.size(), s2.data());
    NT_CHECK(memcmp(s1.data(), s2.data(), 32) == 0);
    printf("  note: scrambler of 32 zero bits starts %02x %02x %02x %02x\n", s1[0], s1[1], s1[2],
           s1[3]);
  }

  // ---- Gray code: bijective, and adjacent symbols differ in exactly one bit ----
  for (uint32_t v = 0; v < 64; v++) {
    uint32_t g = ni_gray_enc(v);
    NT_EQ(ni_gray_dec(g), v);
    if (v + 1 < 64) {
      uint32_t x = ni_gray_enc(v) ^ ni_gray_enc(v + 1);
      NT_EQ((int)(x & (x - 1)), 0);  // power of two => one bit differs
    }
  }

  // ---- endianness helpers and json escaping (reports depend on these) ----
  {
    uint8_t p[4];
    ni_wr16(p, 0x1234);
    NT_EQ(p[0], 0x12);
    NT_EQ(p[1], 0x34);
    NT_EQ(ni_rd16(p), 0x1234);
    ni_wr32(p, 0xDEADBEEFu);
    NT_EQ(ni_rd32(p), 0xDEADBEEFu);
    NT_EQ(p[0], 0xDE);
    NT_EQ(p[3], 0xEF);
    NT_STR_EQ(ni_json_escape("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
    std::string ctrl;
    ctrl.push_back((char)1);
    ctrl.push_back((char)31);
    NT_CHECK(ni_json_escape(ctrl).find('\x01') == std::string::npos);
  }

  // ---- error strings: every code in the enum has its own non-empty description, because the
  // UI and the logs print these verbatim (a duplicated message hides which failure happened).
  {
    std::vector<std::string> seen;
    for (int c = 0; c >= -21; c--) {
      std::string s2 = ni_strerror(c);
      NT_CHECK_MSG(!s2.empty(), "ni_strerror returned an empty string");
      bool dup = false;
      for (size_t i = 0; i < seen.size(); i++)
        if (seen[i] == s2) dup = true;
      NT_CHECK_MSG(!dup, "two error codes share a description");
      seen.push_back(s2);
    }
    NT_STR_EQ(ni_strerror(NI_OK), ni_strerror(0));
  }

  return ntest::summary("t_crc_bits");
}
