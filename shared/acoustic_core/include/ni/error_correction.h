// error_correction.h — rate-1/2 K=7 convolutional code (NASA generators 133/171 octal),
// 3-bit soft-decision Viterbi decoder, and the block interleaver.
//
// Why this code: it is the most-studied soft-decision inner code available, needs no
// table distribution (64 states), gives ~5 dB coding gain at BER 1e-3, and its decoder
// is trivially testable (encode->channel->decode must recover the input). Reed-Solomon
// was considered and rejected: for burst-prone acoustic channels an interleaved
// convolutional code with the ARQ layer on top is simpler and gives the decoder
// reliability information the adaptive controller needs.
#ifndef NI_ERROR_CORRECTION_H
#define NI_ERROR_CORRECTION_H
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "ni/common.h"
namespace ni {

enum : int {
  NI_V_K = 7,               // constraint length
  NI_V_STATES = 64,
  NI_V_TAIL = 6,            // flush bits
  NI_SOFT_BITS = 3,
  NI_SOFT_MAX = (1 << NI_SOFT_BITS) - 1,  // 7
};

// Encoder: input is 0/1 valued bytes (n_in bits), output 2*n_in coded bits (0/1 bytes).
// Appends NI_V_TAIL flush bits, so output has 2*(n_in+6) bits.
size_t ni_conv_out_bits(size_t n_in_bits);
void ni_conv_encode(const uint8_t* in, size_t n_in, uint8_t* out);

// Decoder: soft[i] in [0,7]; 0 = certainly 0, 7 = certainly 1, 4 = unknown.
// n_coded must be even and >= 12. Returns number of information bits decoded
// (= n_coded/2 - 6) or a negative error.
int ni_conv_decode_soft(const uint8_t* soft, size_t n_coded, uint8_t* info_bits);
// Hard-decision convenience wrapper (uses 0/7 metrics).
int ni_conv_decode_hard(const uint8_t* hard, size_t n_coded, uint8_t* info_bits);

// ---- block interleaver ------------------------------------------------------
// Rows = data symbols of the burst, columns = bits per symbol. Written row-wise,
// read column-wise; pads the last row with zeros to n_rows*n_cols bits.
struct Interleave {
  int rows, cols;
  std::vector<uint32_t> perm;  // perm[i] = source index of output i
  bool ok;
};
void ni_interleave_init(Interleave* il, int rows, int cols);
inline size_t ni_interleave_size(const Interleave& il) {
  return (size_t)il.rows * (size_t)il.cols;
}
void ni_interleave(const Interleave* il, const uint8_t* in, size_t n_in, uint8_t* out);
void ni_deinterleave(const Interleave* il, const uint8_t* in, uint8_t* out /*rows*cols*/);

uint8_t ni_soft_from_llr(double llr);  // +ve llr favours bit 1 -> 0..7 metric

// ---- test helper: theoretical/unencoded BER bookkeeping ---------------------
struct BerCounters {
  uint64_t raw_bits = 0, raw_err = 0;         // after demodulation, before FEC
  uint64_t decoded_bits = 0, decoded_err = 0;  // after FEC
  uint64_t frames = 0, frames_ok = 0, frames_crc_fail = 0, frames_missing = 0;
  double ber_raw() const { return raw_bits ? (double)raw_err / (double)raw_bits : 0.0; }
  double ber_decoded() const {
    return decoded_bits ? (double)decoded_err / (double)decoded_bits : 0.0;
  }
  double frame_success() const { return frames ? (double)frames_ok / (double)frames : 0.0; }
};

}  // namespace ni
#endif
