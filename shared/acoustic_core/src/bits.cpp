// bits.cpp — bit packing, multiplicative scrambler, Gray maps.
#include "ni/bits.h"
#include "ni/common.h"

namespace ni {

void ni_bits_from_bytes(const uint8_t* src, size_t n_bytes, uint8_t* bits) {
  for (size_t i = 0; i < n_bytes; i++)
    for (int b = 7; b >= 0; b--) *bits++ = (uint8_t)((src[i] >> b) & 1);
}
void ni_bytes_from_bits(const uint8_t* bits, size_t n_bits, uint8_t* dst) {
  size_t nb = n_bits / 8;
  for (size_t i = 0; i < nb; i++) {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | (bits[i * 8 + b] & 1));
    dst[i] = v;
  }
}

// out[n] = in[n] ^ out[n-14] ^ out[n-15], register preloaded with NI_SCRAM_INIT.
void ni_scramble(const uint8_t* in, size_t n, uint8_t* out) {
  uint32_t r = NI_SCRAM_INIT & NI_SCRAM_MASK;  // r bit0 == out[n-1], bit14 == out[n-15]
  for (size_t i = 0; i < n; i++) {
    uint32_t fb = ((r >> 13) ^ (r >> 14)) & 1u;  // out[n-14] is bit13, out[n-15] bit14
    uint32_t o = (in[i] & 1u) ^ fb;
    out[i] = (uint8_t)o;
    r = ((r << 1) | o) & NI_SCRAM_MASK;
  }
}
void ni_descramble(const uint8_t* in, size_t n, uint8_t* out) {
  uint32_t r = NI_SCRAM_INIT & NI_SCRAM_MASK;
  for (size_t i = 0; i < n; i++) {
    uint32_t fb = ((r >> 13) ^ (r >> 14)) & 1u;
    uint32_t y = in[i] & 1u;
    out[i] = (uint8_t)(y ^ fb);
    r = ((r << 1) | y) & NI_SCRAM_MASK;  // register holds *received* (= transmitted) bits
  }
}

uint32_t ni_gray_enc(uint32_t v) { return v ^ (v >> 1); }
uint32_t ni_gray_dec(uint32_t v) {
  uint32_t r = v;
  while (v >>= 1) r ^= v;
  return r;
}

}  // namespace ni
