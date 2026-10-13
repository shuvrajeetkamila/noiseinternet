// bits.h — bit packing plus the self-synchronising scrambler used in front of the FEC.
//
// The scrambler is the multiplicative LFSR  1/(1 + x^14 + x^15)  (ITU V.32 style,
// 0x12000 taps at 14/15). Purpose: (a) remove long runs that would let a differential
// decoder drift, (b) remove DC/tone-like patterns that a device AGC reacts to, (c) make
// the burst look noise-like to the *other* device's noise gate. Descrambling is exact
// inverse regardless of error propagation being burst-limited (the CRC32/FEC handle that).
#ifndef NI_BITS_H
#define NI_BITS_H
#include <stddef.h>
#include <stdint.h>
namespace ni {

// MSB-first bit order everywhere in this protocol: byte bit 7 is the first bit.
void ni_bits_from_bytes(const uint8_t* src, size_t n_bytes, uint8_t* bits /*8*n*/);
void ni_bytes_from_bits(const uint8_t* bits, size_t n_bits, uint8_t* dst);
inline size_t ni_bits_count(size_t n_bytes) { return n_bytes * 8; }

// 15-bit scrambler state; state is part of the burst (fixed init state => deterministic).
enum : uint32_t { NI_SCRAM_INIT = 0x1AAAu, NI_SCRAM_MASK = 0x7FFFu };
void ni_scramble(const uint8_t* in, size_t n_bits, uint8_t* out);
void ni_descramble(const uint8_t* in, size_t n_bits, uint8_t* out);

// Gray mapping helpers (bit-exact, tables built at init).
uint32_t ni_gray_enc(uint32_t v);
uint32_t ni_gray_dec(uint32_t v);

}  // namespace ni
#endif
