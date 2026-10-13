// error_correction.cpp — rate-1/2 K=7 convolutional encoder, soft-decision Viterbi,
// block interleaver.
#include "ni/error_correction.h"
#include <limits.h>
#include <math.h>
#include <string.h>

namespace ni {
namespace {
// Generators written in the same bit order as the register: reg bit6 is the newest input.
// (133, 171) octal, the rate-1/2 K=7 code with free distance 10 - the value the coding-budget
// claim in docs/RESULTS.md rests on. t_fec recomputes d_free from these two bytes by an
// independent shortest-path search and fails if they are ever changed to a weaker pair; this
// test is what caught an earlier revision using 0x6B (octal 153), whose d_free is 8.
const uint8_t kG1 = 0x5B;  // 1011011b -> octal 133 = x^6+x^4+x^3+x+1
const uint8_t kG2 = 0x79;  // 1111001b -> octal 171 = x^6+x^5+x^4+x^3+1

inline int parity8(uint8_t v) {
  v ^= (uint8_t)(v >> 4);
  v ^= (uint8_t)(v >> 2);
  v ^= (uint8_t)(v >> 1);
  return v & 1;
}
struct Tables {
  uint8_t next_state[NI_V_STATES][2];
  uint8_t outbits[NI_V_STATES][2];
  int inited = 0;
};
Tables T;
void init_tables() {
  if (T.inited) return;
  for (int s = 0; s < NI_V_STATES; s++) {
    for (int b = 0; b < 2; b++) {
      int reg = ((s << 1) | b) & 0x7F;
      int o1 = parity8((uint8_t)(reg & kG1));
      int o2 = parity8((uint8_t)(reg & kG2));
      T.next_state[s][b] = (uint8_t)(reg & (NI_V_STATES - 1));
      T.outbits[s][b] = (uint8_t)((o1 << 1) | o2);
    }
  }
  T.inited = 1;
}
}  // namespace

size_t ni_conv_out_bits(size_t n_in_bits) { return 2 * (n_in_bits + NI_V_TAIL); }

void ni_conv_encode(const uint8_t* in, size_t n, uint8_t* out) {
  init_tables();
  int s = 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    int b = in[i] & 1;
    uint8_t ob = T.outbits[s][b];
    out[o++] = (uint8_t)((ob >> 1) & 1);
    out[o++] = (uint8_t)(ob & 1);
    s = T.next_state[s][b];
  }
  for (int i = 0; i < NI_V_TAIL; i++) {
    uint8_t ob = T.outbits[s][0];
    out[o++] = (uint8_t)((ob >> 1) & 1);
    out[o++] = (uint8_t)(ob & 1);
    s = T.next_state[s][0];
  }
}

uint8_t ni_soft_from_llr(double llr) {
  // llr = log(P(1)/P(0)). Saturation at +-3.5 nats keeps 3-bit metrics well conditioned.
  double s = 4.0 + 2.0 * llr;
  if (!(s > 0.0)) return 0;
  if (s > 7.0) return 7;
  return (uint8_t)(s + 0.5);
}

int ni_conv_decode_soft(const uint8_t* soft, size_t n_coded, uint8_t* info_bits) {
  init_tables();
  if (n_coded < 12 || (n_coded & 1)) return NI_ERR_PARAM;
  const size_t n_steps = n_coded / 2;
  if (n_steps <= (size_t)NI_V_TAIL) return NI_ERR_SHORT;
  const size_t n_info = n_steps - NI_V_TAIL;
  if (n_info > NI_MAX_PDU_BITS + 8) return NI_ERR_OVERFLOW;

  const int32_t NEG = INT_MIN / 8;
  std::vector<int32_t> pm(NI_V_STATES, NEG), npm(NI_V_STATES, NEG);
  std::vector<uint8_t> prev((n_steps + 1) * NI_V_STATES, 0);
  pm[0] = 0;

  for (size_t t = 0; t < n_steps; t++) {
    const int d0 = (int)soft[2 * t] * 2 - 7;
    const int d1 = (int)soft[2 * t + 1] * 2 - 7;
    for (int s = 0; s < NI_V_STATES; s++) npm[s] = NEG;
    uint8_t* pv = &prev[(t + 1) * NI_V_STATES];
    for (int s = 0; s < NI_V_STATES; s++) {
      const int32_t base = pm[s];
      if (base <= NEG / 2) continue;
      for (int b = 0; b < 2; b++) {
        uint8_t ob = T.outbits[s][b];
        int e0 = (ob >> 1) & 1, e1 = ob & 1;
        int32_t cand = base + (e0 ? d0 : -d0) + (e1 ? d1 : -d1);
        int ns = T.next_state[s][b];
        if (cand > npm[ns]) {
          npm[ns] = cand;
          pv[ns] = (uint8_t)s;
        }
      }
    }
    for (int s = 0; s < NI_V_STATES; s++) pm[s] = npm[s];
  }

  int s = 0;  // encoder flushed to the all-zero state
  for (size_t t = n_steps; t-- > 0;) {
    int b = s & 1;  // input bit that produced state s (LSB of the successor state)
    if (t < n_info) info_bits[t] = (uint8_t)b;
    s = prev[(t + 1) * NI_V_STATES + s];
  }
  return (int)n_info;
}

int ni_conv_decode_hard(const uint8_t* hard, size_t n_coded, uint8_t* info_bits) {
  std::vector<uint8_t> s(n_coded);
  for (size_t i = 0; i < n_coded; i++) s[i] = hard[i] ? 7 : 0;
  return ni_conv_decode_soft(&s[0], n_coded, info_bits);
}

void ni_interleave_init(Interleave* il, int rows, int cols) {
  il->ok = false;
  il->rows = il->cols = 0;
  il->perm.clear();
  if (rows < 1 || cols < 1) return;
  il->rows = rows;
  il->cols = cols;
  size_t n = (size_t)rows * (size_t)cols;
  il->perm.assign(n, 0);
  size_t idx = 0;
  for (int c = 0; c < cols && idx < n; c++)
    for (int r = 0; r < rows && idx < n; r++) il->perm[idx++] = (uint32_t)((size_t)r * cols + c);
  il->ok = true;
}

void ni_interleave(const Interleave* il, const uint8_t* in, size_t n_in, uint8_t* out) {
  size_t n = ni_interleave_size(*il);
  for (size_t i = 0; i < n; i++) {
    uint32_t src = il->perm[i];
    out[i] = ((size_t)src < n_in) ? in[src] : (uint8_t)0;
  }
}
void ni_deinterleave(const Interleave* il, const uint8_t* in, uint8_t* out) {
  size_t n = ni_interleave_size(*il);
  for (size_t i = 0; i < n; i++) out[il->perm[i]] = in[i];
}

}  // namespace ni
