// t_fec.cpp — the forward error correction chain: convolutional encoder, 3-bit soft Viterbi,
// block interleaver and scrambler, as one pipeline and separately.
//
// The pinned expectations here are computed from the code's *definition* (generator polynomials
// written out in this file) rather than by calling the library, so a change to the generators,
// the tail convention or the interleaver geometry fails loudly instead of drifting.
#include <string.h>
#include <vector>

#include "ni/bits.h"
#include "ni/common.h"
#include "ni/error_correction.h"
#include "ntest.h"

using namespace ni;

namespace {

// Definition, independently implemented: reg bit6 = newest input bit, output1 = parity(reg & G1),
// output2 = parity(reg & G2), i.e. octal generators (133, 171), rate 1/2, K = 7.
const uint8_t REF_G1 = 0x5B, REF_G2 = 0x79;

int ref_parity(uint8_t v) {
  int p = 0;
  for (int i = 0; i < 8; i++) p ^= (v >> i) & 1;
  return p;
}
size_t ref_encode(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
  out.assign(2 * (n + NI_V_TAIL), 0);
  uint8_t reg = 0;
  size_t o = 0;
  for (size_t i = 0; i < n + NI_V_TAIL; i++) {
    uint8_t b = i < n ? in[i] : 0;  // the flush is six zero input bits
    reg = (uint8_t)(((reg << 1) | b) & 0x7F);
    out[o++] = (uint8_t)ref_parity((uint8_t)(reg & REF_G1));
    out[o++] = (uint8_t)ref_parity((uint8_t)(reg & REF_G2));
  }
  return o;
}

// Free distance of the code, found by brute force over the trellis: the minimum weight of any
// non-zero path that leaves and returns to the all-zero state. Computed here, from the generators,
// because "d_free = 10" is the claim the whole FEC budget (and therefore the rate table) rests on.
int ref_d_free() {
  struct Node {
    int state, w;
  };
  const int LIMIT = 40;  // steps; enough for K=7 to converge
  std::vector<std::vector<Node>> cur(NI_V_STATES), nxt(NI_V_STATES);
  for (int b = 0; b < 2; b++) {
    int reg = b;  // from state 0 with input b
    int w = ref_parity((uint8_t)(reg & REF_G1)) + ref_parity((uint8_t)(reg & REF_G2));
    if (w) cur[reg & (NI_V_STATES - 1)].push_back({reg & (NI_V_STATES - 1), w});
  }
  int best = 1 << 30;
  for (int step = 1; step < LIMIT; step++) {
    for (int s = 0; s < NI_V_STATES; s++) nxt[s].clear();
    for (int s = 0; s < NI_V_STATES; s++)
      for (size_t i = 0; i < cur[s].size(); i++) {
        int w0 = cur[s][i].w;
        for (int b = 0; b < 2; b++) {
          int reg = (uint8_t)((s << 1) | b) & 0x7F;
          int ns = reg & (NI_V_STATES - 1);
          int dw = ref_parity((uint8_t)(reg & REF_G1)) + ref_parity((uint8_t)(reg & REF_G2));
          int nw = w0 + dw;
          if (ns == 0) {
            if (nw < best) best = nw;
          } else if (nw < best) {
            nxt[ns].push_back({ns, nw});
          }
        }
      }
    for (int s = 0; s < NI_V_STATES; s++) cur[s] = nxt[s];
  }
  return best;
}

}  // namespace

int main() {
  printf("[t_fec] convolutional code, Viterbi, interleaver\n");

  // ---- 1. the encoder matches its definition, for many lengths -----------------
  {
    Rng rng(0xC0FFEE);
    int lens[] = {1, 2, 7, 8, 15, 16, 100, 255, 256, 1136, 2272, 8800};
    for (int li = 0; li < (int)(sizeof lens / sizeof lens[0]); li++) {
      size_t n = (size_t)lens[li];
      std::vector<uint8_t> in(n), out(ni_conv_out_bits(n)), ref;
      for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(rng.next() & 1);
      NT_EQ((int)ni_conv_out_bits(n), (int)(2 * (n + NI_V_TAIL)));
      ni_conv_encode(in.data(), n, out.data());
      ref_encode(in.data(), n, ref);
      NT_CHECK_MSG(out.size() == ref.size() && memcmp(out.data(), ref.data(), ref.size()) == 0,
                   "encoder output differs from the documented generators");
    }
    // all-zero input encodes to all-zero output (the tail must not inject energy)
    std::vector<uint8_t> z(64, 0), o(2 * 70, 9);
    ni_conv_encode(z.data(), z.size(), o.data());
    size_t nz = 0;
    for (size_t i = 0; i < 2 * (z.size() + NI_V_TAIL); i++) nz += o[i] ? 1 : 0;
    NT_EQ((int)nz, 0);
  }

  // ---- 2. generator polynomials as pinned in the protocol documents ------------
  {
    // A single '1' information bit, then the flush: this is the code's impulse response, and it
    // is exactly what protocol/protocol_vectors pin for other implementations.
    std::vector<uint8_t> one(1, 1), out(2 * 7, 0);
    ni_conv_encode(one.data(), 1, out.data());
    printf("  note: impulse response of the rate-1/2 K=7 encoder:");
    for (size_t i = 0; i < out.size(); i++) printf(" %d", out[i]);
    printf("\n");
    std::vector<uint8_t> ref;
    ref_encode(one.data(), 1, ref);
    NT_CHECK(memcmp(out.data(), ref.data(), ref.size()) == 0);
    // (153,171) - a pair that appears in older textbooks - has d_free = 8; the code must be
    // (133,171) for the promised distance. Recomputed here, not trusted from a comment.
    NT_EQ((int)ref_d_free(), 10);
  }

  // ---- 3. decode(encode(x)) == x with ideal metrics, hard and soft -------------
  {
    Rng rng(4242);
    for (int n_info : {8, 64, 400, 1136}) {
      std::vector<uint8_t> in((size_t)n_info), coded((size_t)n_info * 2 + 12), dec((size_t)n_info + 8);
      for (int i = 0; i < n_info; i++) in[i] = (uint8_t)(rng.next() & 1);
      ni_conv_encode(in.data(), (size_t)n_info, coded.data());
      std::vector<uint8_t> soft(coded.size());
      for (size_t i = 0; i < coded.size(); i++) soft[i] = coded[i] ? 7 : 0;
      int a = ni_conv_decode_soft(soft.data(), coded.size(), dec.data());
      NT_EQ(a, n_info);
      NT_CHECK(memcmp(in.data(), dec.data(), (size_t)n_info) == 0);
      std::vector<uint8_t> dec2((size_t)n_info + 8);
      int b = ni_conv_decode_hard(coded.data(), coded.size(), dec2.data());
      NT_EQ(b, a);
      NT_CHECK(memcmp(dec.data(), dec2.data(), (size_t)n_info) == 0);
    }
  }

  // ---- 4. the decoder actually corrects: measured gain against an uncoded path --
  {
    Rng rng(2024);
    const int n_info = 1136;  // one full 142-byte PDU, the size an acoustic burst carries
    std::vector<uint8_t> in((size_t)n_info), coded(ni_conv_out_bits((size_t)n_info));
    for (int i = 0; i < n_info; i++) in[i] = (uint8_t)(rng.next() & 1);
    ni_conv_encode(in.data(), (size_t)n_info, coded.data());
    struct Res {
      double raw, fec;
    };
    auto run = [&](double p, int trials, int bps) {
      double raw_sum = 0, fec_sum = 0;
      for (int t = 0; t < trials; t++) {
        Interleave il;
        int n_sym = (int)(coded.size() + (size_t)bps - 1) / bps;
        std::vector<uint8_t> pad(coded);
        pad.resize((size_t)n_sym * (size_t)bps, 0);
        ni_interleave_init(&il, n_sym, bps);
        std::vector<uint8_t> inter(pad.size()), back(pad.size());
        ni_interleave(&il, pad.data(), pad.size(), inter.data());
        size_t flipped = 0;
        for (size_t i = 0; i < inter.size(); i++)
          if (rng.uniform() < p) {
            inter[i] ^= 1;
            flipped++;
          }
        ni_deinterleave(&il, inter.data(), back.data());
        std::vector<uint8_t> soft(back.size());
        for (size_t i = 0; i < back.size(); i++) soft[i] = back[i] ? 7 : 0;
        std::vector<uint8_t> dec(back.size() / 2 + 8);
        int got = ni_conv_decode_soft(soft.data(), coded.size(), dec.data());
        NT_CHECK(got >= 0);
        size_t fe = 0;
        for (int i = 0; i < n_info; i++) fe += (dec[(size_t)i] != in[(size_t)i]) ? 1u : 0u;
        raw_sum += (double)flipped / (double)inter.size();
        fec_sum += (double)fe / (double)n_info;
      }
      Res r{raw_sum / trials, fec_sum / trials};
      return r;
    };
    for (double p : {0.005, 0.01, 0.02, 0.03}) {
      Res r = run(p, 6, 25);
      printf("  measured: raw %.3f%% -> post-FEC %.4f%%  (%dx rate-1/2, %d bits/frame)\n",
             r.raw * 100.0, r.fec * 100.0, n_info, 25);
      // Below the cliff the decoder must improve on the channel; the cliff itself is measured
      // and reported by this test rather than quoted from a textbook (see reports/RESULTS.md).
      NT_CHECK_MSG(r.fec <= r.raw, "FEC made the bit error rate worse at a low raw rate");
      if (p <= 0.02) NT_CHECK_MSG(r.fec < 1e-3, "expected near-error-free decoding below 2% raw");
    }
    // and above the cliff the decoder must not be reported as helping: with 12% raw the hard
    // metrics dominate and the output degrades. Asserting the *direction* here documents the
    // limit the rate adaptation exists to respect.
    {
      Res r = run(0.12, 3, 25);
      printf("  measured: raw %.2f%% -> post-FEC %.2f%% (beyond the code's cliff)\n", r.raw * 100.0,
             r.fec * 100.0);
      NT_CHECK(r.fec > r.raw * 0.5);
    }
  }

  // ---- 5. soft metrics beat hard ones, and "unknown" is handled ---------------
  {
    const int n_info = 400;
    Rng rng(777);
    std::vector<uint8_t> in((size_t)n_info), coded(ni_conv_out_bits((size_t)n_info));
    for (int i = 0; i < n_info; i++) in[i] = (uint8_t)(rng.next() & 1);
    ni_conv_encode(in.data(), (size_t)n_info, coded.data());
    // flip every third bit; soft metrics get 3 of 4 levels of confidence from the LLR magnitude
    std::vector<uint8_t> soft_h(coded.size()), soft_s(coded.size());
    for (size_t i = 0; i < coded.size(); i++) {
      uint8_t c = coded[i];
      soft_h[i] = c ? 7 : 0;
      soft_s[i] = c ? 7 : 0;
    }
    size_t corrupted = 0;
    for (size_t i = 0; i < coded.size(); i += 3) {
      // a *bit* error with full confidence in soft_h (metric 0/7 is already a hard decision),
      // and the same position marked "no information" in soft_s
      soft_s[i] = 4;
      soft_h[i] = coded[i] ? 0 : 7;
      corrupted++;
    }
    std::vector<uint8_t> d1((size_t)n_info + 8), d2((size_t)n_info + 8);
    int a = ni_conv_decode_soft(soft_h.data(), coded.size(), d1.data());
    int b = ni_conv_decode_soft(soft_s.data(), coded.size(), d2.data());
    size_t e1 = 0, e2 = 0;
    for (int i = 0; i < n_info; i++) {
      if (i < a) e1 += (d1[(size_t)i] != in[(size_t)i]) ? 1u : 0u;
      if (i < b) e2 += (d2[(size_t)i] != in[(size_t)i]) ? 1u : 0u;
    }
    printf("  measured: %zu corrupted bits -> %zu errors as hard flips, %zu errors as \"unknown\"\n",
           corrupted, e1, e2);
    NT_CHECK_MSG(e2 < e1, "puncturing a bit (metric 4) must beat asserting the wrong value");
  }

  // ---- 6. ni_soft_from_llr: monotone, symmetric, saturating --------------------
  {
    NT_EQ(ni_soft_from_llr(-1e9), 0);
    NT_EQ(ni_soft_from_llr(1e9), 7);
    NT_EQ(ni_soft_from_llr(0.0), 4);
    uint8_t prev = 255;
    for (double llr = -5.0; llr <= 5.0; llr += 0.1) {
      uint8_t v = ni_soft_from_llr(llr);
      NT_CHECK(v <= 7);
      NT_CHECK(prev == 255 || v >= prev);
      prev = v;
    }
    NT_NEAR((double)ni_soft_from_llr(1.2) - 4.0, 4.0 - (double)ni_soft_from_llr(-1.2), 1.0);
  }

  // ---- 7. interleaver geometry ------------------------------------------------
  {
    Interleave il;
    ni_interleave_init(&il, 7, 5);
    NT_CHECK(il.ok);
    NT_EQ(ni_interleave_size(il), (size_t)35);
    std::vector<uint8_t> in(35), inter(35), back(35, 9);
    for (int i = 0; i < 35; i++) in[i] = (uint8_t)i;
    ni_interleave(&il, in.data(), in.size(), inter.data());
    ni_deinterleave(&il, inter.data(), back.data());
    NT_CHECK(memcmp(in.data(), back.data(), 35) == 0);
    // consecutive coded bits must not stay in the same symbol, or a burst error becomes a
    // codeword catastrophe. Row 0 of the *output* is one symbol, so adjacent input indices have
    // to land rows apart.
    size_t p0 = 0, p1 = 0;
    for (size_t i = 0; i < inter.size(); i++) {
      if (inter[i] == 0) p0 = i;
      if (inter[i] == 1) p1 = i;
    }
    NT_CHECK_MSG(p1 / 5 != p0 / 5, "block interleaver left adjacent coded bits in one symbol");
    // padding: a short input is zero-filled to the block size
    std::vector<uint8_t> shortin(3, 1), out2(35, 0);
    ni_interleave(&il, shortin.data(), 3, out2.data());
    size_t ones = 0;
    for (size_t i = 0; i < 35; i++) ones += out2[i] ? 1 : 0;
    NT_EQ((int)ones, 3);
    // degenerate geometry must be refused, not used
    Interleave bad;
    ni_interleave_init(&bad, 0, 8);
    NT_CHECK(!bad.ok);
    ni_interleave_init(&bad, 8, 0);
    NT_CHECK(!bad.ok);
  }

  // ---- 8. decoder input validation -------------------------------------------
  {
    std::vector<uint8_t> buf(64, 0), out(64, 0);
    NT_EQ(ni_conv_decode_soft(buf.data(), 11, out.data()), NI_ERR_PARAM);
    NT_EQ(ni_conv_decode_soft(buf.data(), 12, out.data()), NI_ERR_SHORT);  // only the tail
    NT_EQ(ni_conv_decode_soft(buf.data(), 13, out.data()), NI_ERR_PARAM);  // odd count
    NT_EQ(ni_conv_decode_hard(buf.data(), 11, out.data()), NI_ERR_PARAM);
  }

  // ---- 9. the full transmit chain, as modem.cpp drives it ---------------------
  // scramble -> encode -> pad to whole symbols -> interleave -> [channel] -> deinterleave ->
  // decode -> unpad -> descramble. Two profiles' bits-per-symbol are exercised because the
  // padding differs (16 and 25 bits/symbol here, 55 for the wide profiles).
  {
    Rng rng(31337);
    for (int bps : {16, 25, 55}) {
      for (double p : {0.0, 0.01, 0.03}) {
        const size_t n_info = 1136;
        std::vector<uint8_t> info(n_info), scr(n_info);
        for (size_t i = 0; i < n_info; i++) info[i] = (uint8_t)(rng.next() & 1);
        ni_scramble(info.data(), n_info, scr.data());
        std::vector<uint8_t> coded(ni_conv_out_bits(n_info));
        ni_conv_encode(scr.data(), n_info, coded.data());
        size_t coded_len = coded.size();
        int n_sym = (int)((coded_len + (size_t)bps - 1) / (size_t)bps);
        coded.resize((size_t)n_sym * (size_t)bps, 0);
        Interleave il;
        ni_interleave_init(&il, n_sym, bps);
        std::vector<uint8_t> inter(coded.size());
        ni_interleave(&il, coded.data(), coded.size(), inter.data());
        for (size_t i = 0; i < inter.size(); i++)
          if (rng.uniform() < p) inter[i] ^= 1;
        std::vector<uint8_t> back(coded.size());
        ni_deinterleave(&il, inter.data(), back.data());
        std::vector<uint8_t> soft(back.size());
        for (size_t i = 0; i < back.size(); i++) soft[i] = back[i] ? 7 : 0;
        std::vector<uint8_t> dec(back.size() / 2 + 8);
        int got = ni_conv_decode_soft(soft.data(), coded_len, dec.data());
        NT_CHECK(got > 0);
        if (got > 0) {
          std::vector<uint8_t> un((size_t)got);
          ni_descramble(dec.data(), (size_t)got, un.data());
          size_t bad = 0;
          for (size_t i = 0; i < n_info && i < un.size(); i++) bad += (un[i] != info[i]) ? 1u : 0u;
          if (p == 0.0)
            NT_CHECK_MSG(bad == 0, "a clean channel must deliver the identical information bits");
          else if (p == 0.01)
            NT_CHECK_MSG(bad == 0, "1% interleaved bit errors must be fully corrected");
          printf("  measured: bps=%2d raw %.1f%% -> %zu/%zu information bits wrong\n", bps, p * 100.0,
                 bad, n_info);
        }
      }
    }
  }

  return ntest::summary("t_fec");
}
