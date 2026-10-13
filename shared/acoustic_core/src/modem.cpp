// modem.cpp — the acoustic PHY. Byte PDU -> PCM burst, PCM burst -> byte PDU.
//
// Read alongside docs/PROTOCOL_SPECIFICATION.md; tests/t_protocol_vectors regenerates the
// published vectors from this code and fails if they diverge.
//
// Burst layout (all symbols are N=256 useful + 64 cyclic prefix, Hann-tapered):
//   [S0][S1]  band-limited PN-OFDM sync pair  -> time/fine sync + presence + AGC settle
//   [H0][H1]  burst descriptor, DBPSK, soft-combined -> pdu_bytes(11) mod_id(4) rsv(1) crc8(8)
//   [R]       all used bins = +1             -> per-bin channel + differential phase start
//   [D...]    FEC-interleaved data symbols, a fresh [R] every NI_REF_INTERVAL symbols
#include "ni/modem.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include "ni/bits.h"
#include "ni/crc.h"

namespace ni {
namespace {

struct Point {
  float re, im;
};
struct Constellation {
  int bits = 1;
  std::vector<Point> pts;
};

Constellation make_constellation(int mod) {
  Constellation c;
  switch (mod) {
    case NI_MOD_DQPSK: {
      c.bits = 2;
      c.pts.resize(4);
      for (int v = 0; v < 4; v++) {
        uint32_t g = ni_gray_enc((uint32_t)v);
        double a = 0.5 * M_PI * (double)g;
        c.pts[v] = {(float)cos(a), (float)sin(a)};
      }
      break;
    }
    case NI_MOD_QAM16:
    case NI_MOD_QAM64: {
      int per = (mod == NI_MOD_QAM16) ? 2 : 3;
      c.bits = 2 * per;
      int levels = 1 << per;
      double norm = 1.0 / sqrt((double)(levels * levels - 1) * 2.0 / 3.0);
      c.pts.resize((size_t)1 << c.bits);
      for (size_t v = 0; v < c.pts.size(); v++) {
        int ib = (int)((v >> per) & (uint32_t)(levels - 1));
        int qb = (int)(v & (uint32_t)(levels - 1));
        double ri = 2.0 * ib - (levels - 1), rq = 2.0 * qb - (levels - 1);
        c.pts[v] = {(float)(ri * norm), (float)(rq * norm)};
      }
      break;
    }
    case NI_MOD_DBPSK:
    case NI_MOD_FSK2:
    default: {
      c.bits = 1;
      c.pts = {{1.f, 0.f}, {-1.f, 0.f}};
      break;
    }
  }
  return c;
}

inline int units_of(const CarrierMap& m) { return m.n_data / m.bins_per_unit; }

}  // namespace

// ---------------------------------------------------------------- construction
Modem::Modem(const ModemConfig& cfg) : cfg_(cfg) {
  profile_ = &ni_profile(cfg.profile_id);
  cp_ = ni_profile_cp(*profile_);
  err_.clear();
  if (cfg_.sample_rate < 16000.0 || cfg_.sample_rate > 192000.0) {
    err_ = "sample rate out of the supported range (16 kHz .. 192 kHz)";
    return;
  }
  if (ni_build_carrier_map(*profile_, cfg_.sample_rate, NI_N_FFT, &map_) != NI_OK) {
    err_ = std::string("profile ") + profile_->name + " has no usable subcarriers at " +
           std::to_string((int)cfg_.sample_rate) +
           " Hz (band too narrow for this device / sample rate)";
    return;
  }
  if (!ni_fft_init(&fft_, NI_N_FFT)) {
    err_ = "fft init failed";
    return;
  }
  max_payload_ = ni_profile_max_payload(*profile_, cfg_.sample_rate);
  if (cfg_.max_payload > 0 && cfg_.max_payload < max_payload_) max_payload_ = cfg_.max_payload;
  if (max_payload_ < 32) {
    err_ = std::string("profile ") + profile_->name +
           " can only carry " + std::to_string(max_payload_) +
           " payload bytes at this sample rate - too small to be useful";
    max_payload_ = 0;
    return;
  }
  build_sync();
  ok_ = true;
}

void Modem::build_sync() {
  const int nb = map_.n_bins;
  std::vector<float> re(nb), im(nb);
  Rng rng(0x5151ACBEull);  // fixed seed: both ends derive the same PN without negotiating
  pn_.assign((size_t)nb, 0);
  for (int i = 0; i < nb; i++) {
    int8_t v = (int8_t)((rng.next() & 1) ? 1 : -1);
    pn_[i] = v;
    re[i] = (float)v;
    im[i] = 0.f;
  }
  sync_.assign(NI_N_FFT, 0.f);
  ni_synthesize_real(&fft_, &re[0], &im[0], map_.k0, map_.k1, &sync_[0]);
  // Deliberately NOT normalised to unit RMS. Every symbol in a burst is built the same way -
  // unit-magnitude constellation points on the used bins - so the preamble carries the same
  // average power as the payload it protects. Scaling the sync up to "1.0 RMS" (an earlier
  // revision) put it 21 dB above the data symbols, which then forces the whole burst's
  // peak normalisation to be spent on the preamble: the payload ends up near the 16-bit
  // quantisation floor, the loudspeaker limiter clips the preamble, and an OS AGC that locks
  // onto the loud part ducks everything after it. Keeping the preamble at the same level as
  // the data is also what makes `ni calibrate`'s clipping warning meaningful.
  double r = ni_rms(&sync_[0], NI_N_FFT);
  (void)r;
  sync_cp_.assign(NI_N_FFT + cp_, 0.f);
  ni_add_cp_tapered(&sync_[0], cp_, &sync_cp_[0], 0.0f);
}

Modem::~Modem() { ni_fft_free(&fft_); }

BurstPlan Modem::plan(size_t payload_bytes) const {
  BurstPlan p;
  ni_plan_burst(*profile_, map_, cfg_.sample_rate, (int)payload_bytes, &p, NI_REF_INTERVAL);
  return p;
}

int Modem::burst_samples_for(size_t payload_bytes) const {
  BurstPlan p;
  ni_plan_burst(*profile_, map_, cfg_.sample_rate, (int)payload_bytes, &p, NI_REF_INTERVAL);
  return p.samples;
}

// ---------------------------------------------------------------- symbol mapping
int Modem::map_bits_to_symbols(const uint8_t* coded, size_t n_coded, std::vector<int>* out) const {
  Constellation c = make_constellation(profile_->mod);
  int nu = units_of(map_);
  int bps = c.bits * nu;
  size_t nsym = (n_coded + (size_t)bps - 1) / (size_t)bps;
  out->clear();
  out->reserve(nsym * (size_t)nu);
  for (size_t s = 0; s < nsym; s++) {
    for (int u = 0; u < nu; u++) {
      int v = 0;
      for (int b = 0; b < c.bits; b++) {
        size_t idx = s * (size_t)bps + (size_t)u * (size_t)c.bits + (size_t)b;
        int bit = idx < n_coded ? (coded[idx] & 1) : 0;
        v = (v << 1) | bit;
      }
      out->push_back(v);
    }
  }
  return (int)nsym;
}

// ---------------------------------------------------------------- TX
int Modem::tx_render(const uint8_t* pdu, size_t n, std::vector<float>* out) const {
  if (!ok_) return NI_ERR_STATE;
  if (!pdu || !out) return NI_ERR_PARAM;
  if (n < NI_HEADER_BYTES + NI_CRC32_BYTES || n > NI_MAX_BURST_PDU) return NI_ERR_LEN;

  const int bps = map_.bits_per_symbol;
  const int n_units = units_of(map_);
  Constellation cons = make_constellation(profile_->mod);
  const bool fec = profile_->fec_rate_den == 2;

  size_t info_bits = n * 8;
  std::vector<uint8_t> bits(info_bits);
  ni_bits_from_bytes(pdu, n, bits.data());
  std::vector<uint8_t> scr(info_bits);
  ni_scramble(bits.data(), info_bits, scr.data());
  std::vector<uint8_t> coded;
  if (fec) {
    coded.resize(ni_conv_out_bits(info_bits));
    ni_conv_encode(scr.data(), info_bits, coded.data());
  } else {
    coded = scr;
  }
  int n_data = (int)((coded.size() + (size_t)bps - 1) / (size_t)bps);
  if (n_data > profile_->max_burst_symbols) return NI_ERR_OVERFLOW;
  coded.resize((size_t)n_data * (size_t)bps, 0);

  Interleave il;
  ni_interleave_init(&il, n_data, bps);
  std::vector<uint8_t> inter((size_t)n_data * (size_t)bps, 0);
  ni_interleave(&il, coded.data(), coded.size(), inter.data());

  // burst descriptor
  // 16-bit burst descriptor: pdu_bytes(11) | mod_id(4) | reserved(1). A 1400 B PDU would
  // not fit in 11 bits, and NI_MAX_BURST_PDU is 1100, so the field is wide enough by design.
  uint32_t dv = (((uint32_t)n & 0x7FFu) << 5) | (((uint32_t)profile_->id & 0xFu) << 1);
  if (n > 0x7FFu) return NI_ERR_OVERFLOW;
  uint8_t dbuf[2] = {(uint8_t)(dv >> 8), (uint8_t)dv};
  uint8_t hdrbits[NI_HDR_BITS];
  for (int i = 0; i < 16; i++) hdrbits[i] = (uint8_t)((dbuf[i / 8] >> (7 - (i % 8))) & 1);
  uint8_t hc = ni_crc8(dbuf, 2);
  for (int i = 0; i < 8; i++) hdrbits[16 + i] = (uint8_t)((hc >> (7 - i)) & 1);

  const int nb = map_.n_bins;
  const int n_hdr = ni_hdr_symbols_for(map_.n_data);
  std::vector<float> sre((size_t)nb), sim((size_t)nb), sym(NI_N_FFT);
  std::vector<std::vector<float>> wave;
  auto clear_spec = [&]() {
    for (int i = 0; i < nb; i++) {
      sre[i] = 0.f;
      sim[i] = 0.f;
    }
  };
  auto put = [&](int krel, float re, float im) {
    sre[krel] = re;
    sim[krel] = im;
  };
  auto emit = [&](const float* s) {
    std::vector<float> with_cp((size_t)NI_N_FFT + cp_);
    ni_add_cp_tapered(s, cp_, &with_cp[0], 0.0f);
    wave.push_back(std::move(with_cp));
  };

  for (int i = 0; i < NI_SYNC_SYMBOLS; i++) emit(&sync_[0]);

  std::vector<float> pre(nb, 1.f), pre_i(nb, 0.f);
  for (int h = 0; h < n_hdr; h++) {
    clear_spec();
    for (int pi = 0; pi < map_.n_pilot; pi++) {
      int k = map_.pilot_bins[pi] - map_.k0;
      put(k, (float)pn_[k], 0.f);
    }
    // Absolute BPSK, NOT differential: the receiver demaps the descriptor coherently against
    // the channel estimate taken from the sync symbols, and a differential encoding here would
    // make symbol h>0 the product of two *different* descriptor bits (the slots advance per
    // symbol), which the coherent receiver would read as noise.
    for (int j = 0; j < map_.n_data; j++) {
      int krel = map_.mod_bins[j] - map_.k0;
      float sgn = hdrbits[ni_hdr_bit_for(h * map_.n_data + j)] ? -1.f : 1.f;
      put(krel, sgn, 0.f);
    }
    ni_synthesize_real(&fft_, &sre[0], &sim[0], map_.k0, map_.k1, &sym[0]);
    emit(&sym[0]);
  }
  clear_spec();
  for (int k = 0; k < nb; k++) put(k, 1.f, 0.f);
  ni_synthesize_real(&fft_, &sre[0], &sim[0], map_.k0, map_.k1, &sym[0]);
  emit(&sym[0]);

  std::fill(pre.begin(), pre.end(), 1.f);
  std::fill(pre_i.begin(), pre_i.end(), 0.f);
  const bool differential = profile_->mod == NI_MOD_DBPSK || profile_->mod == NI_MOD_DQPSK;
  int bitpos = 0;
  for (int s = 0; s < n_data; s++) {
    clear_spec();
    for (int pi = 0; pi < map_.n_pilot; pi++) {
      int k = map_.pilot_bins[pi] - map_.k0;
      put(k, (float)pn_[k], 0.f);
    }
    for (int u = 0; u < n_units; u++) {
      int v = 0;
      for (int b = 0; b < cons.bits; b++, bitpos++)
        v = (v << 1) | (bitpos < (int)inter.size() ? (inter[bitpos] & 1) : 0);
      int krel = map_.mod_bins[u * map_.bins_per_unit] - map_.k0;
      if (profile_->mod == NI_MOD_FSK2) {
        int krel1 = map_.mod_bins[u * map_.bins_per_unit + 1] - map_.k0;
        put(krel, v ? 0.f : 1.f, 0.f);
        put(krel1, v ? 1.f : 0.f, 0.f);
        continue;
      }
      const Point& p = cons.pts[(size_t)v];
      if (differential) {
        float re = pre[krel] * p.re - pre_i[krel] * p.im;
        float im = pre[krel] * p.im + pre_i[krel] * p.re;
        put(krel, re, im);
        pre[krel] = re;
        pre_i[krel] = im;
      } else {
        put(krel, p.re, p.im);
      }
    }
    ni_synthesize_real(&fft_, &sre[0], &sim[0], map_.k0, map_.k1, &sym[0]);
    emit(&sym[0]);
    if ((s + 1) % NI_REF_INTERVAL == 0 && s + 1 < n_data) {
      clear_spec();
      for (int k = 0; k < nb; k++) put(k, 1.f, 0.f);
      ni_synthesize_real(&fft_, &sre[0], &sim[0], map_.k0, map_.k1, &sym[0]);
      emit(&sym[0]);
      std::fill(pre.begin(), pre.end(), 1.f);
      std::fill(pre_i.begin(), pre_i.end(), 0.f);
    }
  }

  size_t total = 0;
  for (auto& w : wave) total += w.size();
  std::vector<float>& o = *out;
  o.assign(total + cp_, 0.f);
  size_t off = 0;
  for (auto& w : wave) {
    memcpy(&o[off], w.data(), sizeof(float) * w.size());
    off += w.size();
  }
  double pk = ni_peak(&o[0], total);
  double g = (pk > 1e-9) ? (cfg_.tx_amplitude * profile_->tx_linear_gain / pk) : 1.0;
  ni_scale(&o[0], total, g);
  for (int i = 0; i < cp_; i++) o[i] *= (float)((double)i / (double)cp_);
  for (int i = 0; i < cp_; i++)
    o[total + i] = (float)((double)o[total - 1] * (1.0 - (double)i / (double)cp_));
  return NI_OK;  // length is out->size(); every other result is a negative NI_ERR_*
}

int Modem::tx_render_frame(const uint8_t* frame, size_t n, std::vector<float>* out) const {
  std::vector<uint8_t> pdu;
  int rc = ni_phy_wrap(frame, n, &pdu);
  if (rc != NI_OK) return rc;
  return tx_render(&pdu[0], pdu.size(), out);
}

// ---------------------------------------------------------------- RX
int Modem::rx_detect(const float* x, size_t n, SyncResult* out) const {
  SyncConfig sc;
  sc.sample_rate = cfg_.sample_rate;
  sc.symbol_len = NI_N_FFT;
  sc.cp = cp_;
  sc.thresh = cfg_.sync_thresh;
  Synchronizer s;
  s.init(sc, &sync_[0], NI_N_FFT);
  *out = s.detect(x, n);
  if (out->found) out->pos = s.refine(x, n, out->pos, 4);
  return out->found ? NI_OK : NI_ERR_NOFRAME;
}

int Modem::rx_decode(const float* x, size_t n, size_t start, RxResult* out) const {
  if (!ok_) return NI_ERR_STATE;
  if (!x || !out) return NI_ERR_PARAM;
  out->pdu.clear();
  const int N = NI_N_FFT, CP = cp_, L = N + CP;
  const int nb = map_.n_bins;
  const int n_units = units_of(map_);
  const int bps = map_.bits_per_symbol;
  const bool fec = profile_->fec_rate_den == 2;
  const int n_hdr = ni_hdr_symbols_for(map_.n_data);
  if ((n - start) / (size_t)L < (size_t)(NI_SYNC_SYMBOLS + n_hdr + 1)) return NI_ERR_SHORT;

  std::vector<float> yre(nb), yim(nb), tmp(N);
  // `p` is the index of the FIRST SAMPLE OF THE USEFUL SYMBOL (the CP that precedes it is
  // the transmitter's business: it exists exactly so the receiver can ignore it). The
  // detector's SyncResult::pos is on the same convention, so every offset below is
  // pos + slot * L with L = N + CP.
  auto symbol_at = [&](size_t p) -> int {
    if (p + N > n) return NI_ERR_SHORT;
    memcpy(&tmp[0], x + p, sizeof(float) * (size_t)N);
    ni_analyse_real(&fft_, &tmp[0], &yre[0], &yim[0], map_.k0, map_.k1);
    return NI_OK;
  };

  // ---- 1. burst descriptor ----------------------------------------------------
  // The descriptor is demapped COHERENTLY against a channel estimate taken from the two sync
  // symbols, not differentially against a hard-decided reference. The reason is measured, not
  // theoretical: the acquisition loop locks within the cyclic prefix, and any residual timing
  // offset tau is a per-bin phase ramp exp(j 2 pi k tau / N). Differentially decoding against a
  // real +-1 reference throws that ramp away (Re(r) = |r| cos(phi)), so at tau = 3 samples the
  // high descriptor bits sit at cos(100 deg) - sign-flipping coin flips through our own device
  // model. The sync estimate already contains the ramp, so dividing by it removes it. The two
  // identical sync copies are averaged for 3 dB, and |H|^2 is used as the per-bin confidence
  // weight so a faded bin cannot outvote a healthy one.
  std::vector<double> acc(NI_HDR_BITS, 0.0);
  std::vector<double> hdr_r(nb, 0.0), hdr_i(nb, 0.0), hdr2(nb, 0.0);
  for (int si = 0; si < (int)NI_SYNC_SYMBOLS; si++) {
    if (symbol_at(start + (size_t)si * L) != NI_OK) return NI_ERR_SHORT;
    for (int k = 0; k < nb; k++) {
      double g = pn_[k] != 0 ? 1.0 / (double)pn_[k] : 1.0;
      hdr_r[k] += yre[k] * g;
      hdr_i[k] += yim[k] * g;
    }
  }
  double syn_scale = 1.0 / (double)(NI_SYNC_SYMBOLS > 0 ? NI_SYNC_SYMBOLS : 1);
  double sync_mean_h2 = 1e-12;
  for (int k = 0; k < nb; k++) {
    hdr_r[k] *= syn_scale;
    hdr_i[k] *= syn_scale;
    hdr2[k] = hdr_r[k] * hdr_r[k] + hdr_i[k] * hdr_i[k];
    sync_mean_h2 += hdr2[k];
  }
  sync_mean_h2 /= (double)(nb > 0 ? nb : 1);
  for (int h = 0; h < n_hdr; h++) {
    if (symbol_at(start + (size_t)(NI_SYNC_SYMBOLS + h) * L) != NI_OK) return NI_ERR_SHORT;
    for (int i = 0; i < map_.n_data; i++) {
      int krel = map_.mod_bins[i] - map_.k0;
      const int hbit = ni_hdr_bit_for(h * map_.n_data + i);
      double hr = hdr_r[krel], hi = hdr_i[krel];
      double den = hr * hr + hi * hi;
      if (den < 1e-12) den = 1e-12;
      // zero-forcing: z = y * conj(H) / |H|^2, ideal z = +-1. The transmitter maps bit 1 to
      // -1, so the log-likelihood of "1" is -Re(z); getting this sign backwards inverts every
      // descriptor bit and shows up as a CRC failure with strong magnitudes.
      double zr = ((double)yre[krel] * hr + (double)yim[krel] * hi) / den;
      double conf = den / sync_mean_h2;
      if (conf > 8.0) conf = 8.0;  // a suspiciously hot bin is not trusted proportionally
      double llr = -2.0 * zr * conf;
      if (llr > 30) llr = 30;
      if (llr < -30) llr = -30;
      acc[hbit] += llr;
    }
  }
  out->desc_llr.assign(acc.begin(), acc.end());
  out->sync_h2 = sync_mean_h2;
  uint8_t dbuf[2] = {0, 0};
  for (int i = 0; i < 16; i++) {
    dbuf[i / 8] = (uint8_t)(dbuf[i / 8] << 1);
    dbuf[i / 8] |= (uint8_t)(acc[i] >= 0 ? 1 : 0);
  }
  uint8_t hc = 0;
  for (int i = 0; i < 8; i++) hc = (uint8_t)((hc << 1) | (acc[16 + i] >= 0 ? 1 : 0));
  if (ni_crc8(dbuf, 2) != hc) return NI_ERR_BITSLIP;
  uint32_t dv = ((uint32_t)dbuf[0] << 8) | dbuf[1];
  size_t pdu_bytes = dv >> 5;
  int hdr_mod = (int)((dv >> 1) & 0xF);
  (void)hdr_mod;  // reported by the caller through RxResult::plan; used for diagnostics
  if (pdu_bytes < NI_HEADER_BYTES + NI_CRC32_BYTES || pdu_bytes > NI_MAX_BURST_PDU)
    return NI_ERR_LEN;

  size_t info_bits = pdu_bytes * 8;
  size_t coded_bits = fec ? ni_conv_out_bits(info_bits) : info_bits;
  int n_data = (int)((coded_bits + (size_t)bps - 1) / (size_t)bps);
  if (n_data < 1 || n_data > profile_->max_burst_symbols) return NI_ERR_LEN;
  int n_refs = (n_data - 1) / NI_REF_INTERVAL;
  int n_slots = NI_SYNC_SYMBOLS + n_hdr + 1 + n_data + n_refs;
  if ((size_t)n_slots * L + start > n) return NI_ERR_SHORT;

  out->consumed = (size_t)n_slots * L;
  out->start_pos = start;
  ni_plan_burst(*profile_, map_, cfg_.sample_rate,
                (int)pdu_bytes - (int)(NI_PDU_LEN_BYTES + NI_HEADER_BYTES + NI_CRC32_BYTES),
                &out->plan, NI_REF_INTERVAL);
  out->plan.symbols_data = n_data;
  out->plan.symbols_total = n_slots;
  out->plan.samples = (size_t)n_slots * L;
  out->plan.duration_ms = 1000.0 * (double)out->plan.samples / cfg_.sample_rate;
  out->plan.info_bits = (int)info_bits;
  out->plan.coded_bits = (int)coded_bits;
  out->plan.symbols_hdr = n_hdr;

  PhyStats& st = out->stats;
  st = PhyStats();
  st.bins_used = map_.n_data;
  st.symbols = n_data;
  st.duration_ms = out->plan.duration_ms;
  st.clipped = ni_clip_count(x + start, out->consumed, 0.9995) > 3;
  st.rssi_dbfs = 20.0 * log10(ni_rms(x + start, out->consumed) + 1e-12);

  // ---- 2. noise floor from out-of-band bins ----------------------------------
  double noise_var = 0.0;
  int noise_n = 0;
  {
    std::vector<int> ob;
    for (int k = 1; k < map_.k0; k++) ob.push_back(k);
    for (int k = map_.k1 + 1; k < N / 2; k++) ob.push_back(k);
    if (!ob.empty()) {
      int stride = std::max(1, n_slots / 8);
      for (int si = 0; si < n_slots; si += stride) {
        size_t p = start + (size_t)si * L;  // body position, same convention as symbol_at
        if (p + N > n) continue;
        memcpy(&tmp[0], x + p, sizeof(float) * (size_t)N);
        std::vector<float> full(2 * (size_t)N, 0.f);
        for (int m = 0; m < N; m++) full[2 * m] = tmp[m];
        ni_fft_forward(&fft_, &full[0]);
        for (int k : ob) {
          double re = full[2 * k], im = full[2 * k + 1];
          noise_var += re * re + im * im;
          noise_n++;
        }
      }
      noise_var /= (double)noise_n;
    }
    if (noise_var < 1e-14) noise_var = 1e-14;
  }
  st.noise_dbfs = 10.0 * log10(noise_var / (double)N + 1e-18);
  st.noise_var = noise_var;

  // ---- 3. reference symbol -> per-bin channel estimate -----------------------
  std::vector<double> h_re(nb, 1.0), h_im(nb, 0.0), h2(nb, 1.0);
  if (symbol_at(start + (size_t)(NI_SYNC_SYMBOLS + n_hdr) * L) != NI_OK)
    return NI_ERR_SHORT;
  for (int k = 0; k < nb; k++) {
    h_re[k] = yre[k];
    h_im[k] = yim[k];
    h2[k] = h_re[k] * h_re[k] + h_im[k] * h_im[k];
  }
  double mean_h2 = 0.0;
  for (int i = 0; i < map_.n_data; i++) mean_h2 += h2[map_.mod_bins[i] - map_.k0];
  mean_h2 /= (double)std::max(1, map_.n_data);
  st.snr_db = 10.0 * log10((mean_h2 + 1e-18) / (noise_var + 1e-18));

  // ---- 4. data symbols -> soft/hard bits ------------------------------------
  Constellation cons = make_constellation(profile_->mod);
  const int cbits = cons.bits;
  const bool coherent = profile_->mod == NI_MOD_QAM16 || profile_->mod == NI_MOD_QAM64;
  std::vector<double> prev_re(nb, 1.0), prev_im(nb, 0.0);
  int total_inter = n_data * bps;
  std::vector<uint8_t> inter_soft((size_t)total_inter, 4), inter_hard((size_t)total_inter, 0);
  double evm_acc = 0.0, ref_pow = 0.0;
  int evm_n = 0, bad = 0;
  int slot = NI_SYNC_SYMBOLS + n_hdr + 1;

  for (int s = 0; s < n_data; s++) {
    size_t p = start + (size_t)slot * L;
    if (symbol_at(p) != NI_OK) return NI_ERR_SHORT;
    slot++;
    // scattered pilots -> channel update, linear interpolation across the band
    if (map_.n_pilot >= 2) {
      double pr_re[64], pr_im[64], pr_k[64];
      int np = map_.n_pilot;
      for (int pi = 0; pi < np; pi++) {
        int k = map_.pilot_bins[pi] - map_.k0;
        double g = pn_[k] != 0 ? 1.0 / (double)pn_[k] : 1.0;
        pr_re[pi] = yre[k] * g;
        pr_im[pi] = yim[k] * g;
        pr_k[pi] = map_.pilot_bins[pi];
      }
      for (int k = 0; k < nb; k++) {
        double bin = map_.k0 + k;
        int lo = 0;
        while (lo + 1 < np && pr_k[lo + 1] < bin) lo++;
        int hi = std::min(np - 1, lo + 1);
        double span = pr_k[hi] - pr_k[lo];
        double t = span > 0 ? (bin - pr_k[lo]) / span : 0.0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        double er = pr_re[lo] * (1 - t) + pr_re[hi] * t;
        double ei = pr_im[lo] * (1 - t) + pr_im[hi] * t;
        h_re[k] = 0.6 * er + 0.4 * h_re[k];
        h_im[k] = 0.6 * ei + 0.4 * h_im[k];
        h2[k] = h_re[k] * h_re[k] + h_im[k] * h_im[k];
      }
    }
    for (int u = 0; u < n_units; u++) {
      int krel = map_.mod_bins[u * map_.bins_per_unit] - map_.k0;
      int base = s * bps + u * cbits;
      if (h2[krel] < 0.02 * mean_h2) {  // deep fade / unusable subcarrier: say "don't know"
        bad++;
        for (int b = 0; b < cbits; b++)
          if (base + b < total_inter) {
            inter_soft[base + b] = 4;
            inter_hard[base + b] = 0;
          }
        continue;
      }
      double scale = h2[krel] / (2.0 * noise_var);
      if (profile_->mod == NI_MOD_FSK2) {
        int krel1 = map_.mod_bins[u * map_.bins_per_unit + 1] - map_.k0;
        double e0 = (double)yre[krel] * yre[krel] + (double)yim[krel] * yim[krel];
        double e1 = (double)yre[krel1] * yre[krel1] + (double)yim[krel1] * yim[krel1];
        double llr = (e1 - e0) / (noise_var + 1e-12) * 0.5;
        if (llr > 30) llr = 30;
        if (llr < -30) llr = -30;
        if (base < total_inter) {
          inter_soft[base] = ni_soft_from_llr(llr);
          inter_hard[base] = (uint8_t)(llr >= 0 ? 1 : 0);
        }
        continue;
      }
      Point r;
      if (coherent) {
        double den = h2[krel] + 1e-12;
        r.re = (float)((yre[krel] * h_re[krel] + yim[krel] * h_im[krel]) / den);
        r.im = (float)((yim[krel] * h_re[krel] - yre[krel] * h_im[krel]) / den);
      } else {
        double pr = prev_re[krel], pi = prev_im[krel];
        double den = pr * pr + pi * pi + 1e-12;
        r.re = (float)((yre[krel] * pr + yim[krel] * pi) / den);
        r.im = (float)((yim[krel] * pr - yre[krel] * pi) / den);
        prev_re[krel] = yre[krel];
        prev_im[krel] = yim[krel];
        scale *= 0.5;
      }
      double best0[6], best1[6];
      for (int b = 0; b < cbits; b++) {
        best0[b] = 1e30;
        best1[b] = 1e30;
      }
      double nearest = 1e30;
      for (size_t v = 0; v < cons.pts.size(); v++) {
        double dr = (double)r.re - cons.pts[v].re, di = (double)r.im - cons.pts[v].im;
        double m = dr * dr + di * di;
        if (m < nearest) nearest = m;
        for (int b = 0; b < cbits; b++) {
          int bit = (int)((v >> (cbits - 1 - b)) & 1);
          if (bit) {
            if (m < best1[b]) best1[b] = m;
          } else if (m < best0[b])
            best0[b] = m;
        }
      }
      evm_acc += nearest;
      ref_pow += (double)r.re * r.re + (double)r.im * r.im;
      evm_n++;
      for (int b = 0; b < cbits; b++) {
        double llr = 0.5 * scale * (best0[b] - best1[b]);
        if (llr > 40) llr = 40;
        if (llr < -40) llr = -40;
        int idx = base + b;
        if (idx < total_inter) {
          inter_soft[idx] = ni_soft_from_llr(llr);
          inter_hard[idx] = (uint8_t)(llr >= 0 ? 1 : 0);
        }
      }
    }
    if ((s + 1) % NI_REF_INTERVAL == 0 && s + 1 < n_data) {
      if (symbol_at(start + (size_t)slot * L) != NI_OK) return NI_ERR_SHORT;
      slot++;
      for (int k = 0; k < nb; k++) {
        h_re[k] = yre[k];
        h_im[k] = yim[k];
        h2[k] = h_re[k] * h_re[k] + h_im[k] * h_im[k];
      }
      std::fill(prev_re.begin(), prev_re.end(), 1.0);
      std::fill(prev_im.begin(), prev_im.end(), 0.0);
    }
  }
  st.bins_bad = bad;
  if (evm_n > 0 && ref_pow > 0)
    st.evec = 100.0 * sqrt(evm_acc / (double)evm_n / (ref_pow / (double)evm_n));

  // ---- 5. deinterleave -> FEC -> descramble -> PDU ---------------------------
  std::vector<uint8_t> soft_coded((size_t)total_inter, 4), hard_coded((size_t)total_inter, 0);
  Interleave il;
  ni_interleave_init(&il, n_data, bps);
  ni_deinterleave(&il, &inter_soft[0], &soft_coded[0]);
  ni_deinterleave(&il, &inter_hard[0], &hard_coded[0]);
  out->hard_coded.assign(hard_coded.begin(), hard_coded.begin() + (long)coded_bits);

  std::vector<uint8_t> info_bits_buf(info_bits, 0);
  if (fec) {
    int n_info = ni_conv_decode_soft(&soft_coded[0], coded_bits, &info_bits_buf[0]);
    if (n_info != (int)info_bits) return NI_ERR_DECODE;
  } else {
    // No FEC configured (the RAW-* profiles): the information bits are the hard decisions
    // themselves. Taking them from the soft metric with ">= 4" would report every punctured
    // (unknown) bin as a 1, which is how the raw-BER column of the report came out worse than
    // the channel actually was.
    for (size_t i = 0; i < info_bits; i++) info_bits_buf[i] = hard_coded[i];
  }
  std::vector<uint8_t> descrambled(info_bits);
  ni_descramble(&info_bits_buf[0], info_bits, &descrambled[0]);
  std::vector<uint8_t> bytes(info_bits / 8);
  ni_bytes_from_bits(&descrambled[0], info_bits, &bytes[0]);
  // Published before the length checks: a burst that fails the PDU sanity tests is still the
  // most informative sample a benchmark can have (raw and post-FEC bit error rates are
  // computed from these), and swallowing them is how a PHY test ends up reporting 0% BER for
  // a total failure.
  out->info_bits = descrambled;
  if (bytes.size() != pdu_bytes) return NI_ERR_LEN;
  const uint8_t* framep = &bytes[0] + NI_PDU_LEN_BYTES;
  size_t flen = pdu_bytes - NI_PDU_LEN_BYTES;
  if ((size_t)ni_rd16(&bytes[0]) != flen) return NI_ERR_LEN;

  out->pdu.assign(framep, framep + flen);
  Frame f;
  int rc = ni_frame_decode(out->pdu.data(), out->pdu.size(), &f);
  out->status = rc;
  out->frame = f;
  if (out->h_re.empty()) {
    out->h_re.resize(nb);
    out->h_im.resize(nb);
  }
  for (int k = 0; k < nb; k++) {
    out->h_re[k] = (float)h_re[k];
    out->h_im[k] = (float)h_im[k];
  }
  return NI_OK;
}

// Coarse carrier offset from the phase rotation between the two identical sync
// symbols: each in-band bin picks up 2*pi*df*T between them, T = (N+CP)/fs.
double ni_cfo_estimate(const Fft& f, const float* x, size_t n, size_t pos, int k0, int k1,
                       double fs, int cp) {
  int nbins = k1 - k0 + 1;
  if (nbins < 2) return 0.0;
  size_t L = NI_N_FFT + (size_t)(cp > 0 ? cp : NI_CP);
  if (pos + L + NI_N_FFT > n) return 0.0;
  std::vector<float> r0(nbins), i0(nbins), r1(nbins), i1(nbins), t(NI_N_FFT);
  ni_strip_cp(x + pos, cp > 0 ? cp : NI_CP, &t[0]);
  ni_analyse_real(&f, &t[0], &r0[0], &i0[0], k0, k1);
  ni_strip_cp(x + pos + L, cp > 0 ? cp : NI_CP, &t[0]);
  ni_analyse_real(&f, &t[0], &r1[0], &i1[0], k0, k1);
  double sr = 0, si = 0;
  for (int i = 0; i < nbins; i++) {
    sr += (double)r1[i] * r0[i] + (double)i1[i] * i0[i];
    si += (double)i1[i] * r0[i] - (double)r1[i] * i0[i];
  }
  double T = (double)L / fs;
  return atan2(si, sr) / (2.0 * M_PI * T);
}

int Modem::rx_capture(const float* x, size_t n, RxResult* out) const {
  SyncResult sr;
  int rc = rx_detect(x, n, &sr);
  if (rc != NI_OK) {
    out->status = rc;
    return rc;
  }
  out->cfo = ni_cfo_estimate(fft_, x, n, sr.pos, map_.k0, map_.k1, cfg_.sample_rate, cp_);
  // Report the burst position on EVERY path, including the "burst still arriving" NI_ERR_SHORT
  // return inside rx_decode (which bails out before it gets to set it). The link layer front-trims
  // its capture window to this index, which is what turns the arrival-wait loop from a rescan of
  // the whole 1.2 s window per callback into a decode that starts where the burst starts.
  out->start_pos = sr.pos;
  rc = rx_decode(x, n, sr.pos, out);
  out->stats.sync_peak = sr.peak;
  out->stats.cfo_hz = out->cfo;
  return rc;
}

}  // namespace ni
