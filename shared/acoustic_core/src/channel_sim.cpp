// channel_sim.cpp — the simulated loudspeaker -> room -> microphone path.
//
// Processing order (matters, and is documented because it decides the results):
//   tx level -> speaker filter (HP, resonance, HF rolloff) -> amplifier clipping
//   -> free-field spreading + air absorption -> multipath taps -> microphone filter
//   -> OS AGC + noise gate -> additive noise -> sample-rate offset -> 16-bit quantisation
//
// The filters are biquad cascades designed by bilinear transform, i.e. what a real device
// measurement would be fitted with. The response figures are representative of small laptop
// and phone speakers, taken from published device-response summaries; they are a MODEL and
// every number derived from them is labelled simulated in the reports.
#include "ni/channel_sim.h"
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include "ni/bits.h"
#include "ni/common.h"
#include "ni/error_correction.h"
#include "ni/modem.h"

namespace ni {

const char* ni_device_name(int m) {
  switch (m) {
    case NI_DEV_IDEAL: return "ideal";
    case NI_DEV_LAPTOP_SPK: return "laptop-speaker";
    case NI_DEV_PHONE_SPK: return "phone-speaker";
    case NI_DEV_LAPTOP_MIC: return "laptop-mic";
    case NI_DEV_PHONE_MIC: return "phone-mic";
    case NI_DEV_SMART_SPEAKER: return "smart-speaker-dsp";
    default: return "unknown";
  }
}

void ni_device_defaults(int model, double* hp, double* res_hz, double* res_db, double* lp,
                        double* gate) {
  switch (model) {
    case NI_DEV_LAPTOP_SPK:
      *hp = 420; *res_hz = 1500; *res_db = 3.0; *lp = 16500; *gate = -78; break;
    case NI_DEV_PHONE_SPK:
      *hp = 620; *res_hz = 2200; *res_db = 5.0; *lp = 13000; *gate = -72; break;
    case NI_DEV_LAPTOP_MIC:
      *hp = 95; *res_hz = 0; *res_db = 0; *lp = 19000; *gate = -85; break;
    case NI_DEV_PHONE_MIC:
      *hp = 120; *res_hz = 0; *res_db = 0; *lp = 17500; *gate = -62; break;
    case NI_DEV_SMART_SPEAKER:
      *hp = 550; *res_hz = 1100; *res_db = 4.0; *lp = 15000; *gate = -55; break;
    case NI_DEV_IDEAL:
    default:
      *hp = 20; *res_hz = 0; *res_db = 0; *lp = 0 /*none*/; *gate = -140; break;
  }
}

Biquad ni_biquad_highpass(double f0, double fs) {
  Biquad b{};
  double w0 = 2.0 * M_PI * f0 / fs;
  if (w0 > M_PI - 1e-6) w0 = M_PI - 1e-6;
  double cw = cos(w0), sw = sin(w0), q = 0.7071068, al = sw / (2.0 * q);
  double a0 = 1.0 + al;
  b.b0 = ((1.0 + cw) * 0.5) / a0;
  b.b1 = (-(1.0 + cw)) / a0;
  b.b2 = ((1.0 + cw) * 0.5) / a0;
  b.a1 = (-2.0 * cw) / a0;
  b.a2 = (1.0 - al) / a0;
  return b;
}
Biquad ni_biquad_lowpass(double f0, double fs) {
  Biquad b{};
  double w0 = 2.0 * M_PI * f0 / fs;
  if (w0 > M_PI - 1e-6) w0 = M_PI - 1e-6;
  double cw = cos(w0), sw = sin(w0), q = 0.7071068, al = sw / (2.0 * q);
  double a0 = 1.0 + al;
  b.b0 = ((1.0 - cw) * 0.5) / a0;
  b.b1 = (1.0 - cw) / a0;
  b.b2 = ((1.0 - cw) * 0.5) / a0;
  b.a1 = (-2.0 * cw) / a0;
  b.a2 = (1.0 - al) / a0;
  return b;
}
Biquad ni_biquad_peaking(double f0, double fs, double gain_db, double q) {
  // RBJ cookbook peaking EQ. The two finite zeros must straddle f0 (b2 = 1 - al*A), otherwise
  // the section is a bare resonator: measured with the previous b2 = b0 form, a "+3 dB bump at
  // 1.5 kHz" filter was actually adding +16 dB at 1 kHz, which then drove the amplifier limiter
  // and destroyed the burst.
  Biquad b{};
  double A = pow(10.0, gain_db / 40.0);
  double w0 = 2.0 * M_PI * f0 / fs;
  if (w0 > M_PI - 1e-6) w0 = M_PI - 1e-6;
  double cw = cos(w0), sw = sin(w0), al = sw / (2.0 * q);
  double a0 = 1.0 + al / A;
  b.b0 = (1.0 + al * A) / a0;
  b.b1 = (-2.0 * cw) / a0;
  b.b2 = (1.0 - al * A) / a0;
  b.a1 = (-2.0 * cw) / a0;
  b.a2 = (1.0 - al / A) / a0;
  return b;
}

AcousticChannel::AcousticChannel(const ChannelConfig& cfg) : cfg_(cfg) {
  double hp, rh, rd, lp, gate;
  ni_device_defaults(cfg.speaker, &hp, &rh, &rd, &lp, &gate);
  spk_.push_back(ni_biquad_highpass(hp, cfg.fs));
  if (rd != 0.0) spk_.push_back(ni_biquad_peaking(rh, cfg.fs, rd, 1.6));
  if (lp > 0) {
    // 2nd-order lowpass twice -> ~-12 dB/oct above the corner, i.e. a real rolloff
    spk_.push_back(ni_biquad_lowpass(lp, cfg.fs));
    spk_.push_back(ni_biquad_lowpass(lp, cfg.fs));
  }
  ni_device_defaults(cfg.microphone, &hp, &rh, &rd, &lp, &gate);
  gate_dbfs_ = gate;
  mic_.push_back(ni_biquad_highpass(hp, cfg.fs));
  if (lp > 0) mic_.push_back(ni_biquad_lowpass(lp, cfg.fs));
  // Spherical spreading referenced to 0.2 m, as a NEGATIVE gain in dB (loss_db_ is applied as
  // 10^(loss/20), so a positive value here would be a free 8 dB amplification - that bug made
  // every simulated burst louder than it was transmitted and clipped the limiter). Near-field
  // distances below the reference are capped rather than amplified without bound.
  double d = cfg.distance_m < 0.05 ? 0.05 : cfg.distance_m;
  loss_db_ = -20.0 * log10(d / 0.2);
  if (loss_db_ > 0.0) loss_db_ = 0.0;
  if (cfg.air_absorption) loss_db_ -= 0.012 * d * 6.0;  // ~0.12 dB/m at 6 kHz, scaled
  reset();  // zero the streaming state (see process())
}

void AcousticChannel::apply_device(const float*, size_t, std::vector<float>*, bool) {}

void AcousticChannel::noise_only(size_t n, std::vector<float>* out) {
  Rng rng(cfg_.seed ^ 0x1234abcdu);
  out->assign(n, 0.f);
  double b0 = 0, b1 = 0, b2 = 0;
  for (size_t i = 0; i < n; i++) {  // Voss-McCartney-lite: 1-pole filtered white = pink-ish
    double w = rng.normal();
    b0 = 0.997 * b0 + w * 0.031;
    b1 = 0.963 * b1 + w * 0.124;
    b2 = 0.575 * b1 + w * 0.168;
    (*out)[i] = (float)((b0 + b1 + b2) * 0.35);
  }
  double r = ni_rms(&(*out)[0], n);
  if (r > 0) ni_scale(&(*out)[0], n, 1.0 / r);
}

double AcousticChannel::pn_gauss() {
  // xorshift64* + Box-Muller, with the generator state kept in the object so the noise is a
  // continuous process across audio blocks. (Re-seeding per block, as this file used to do, made
  // the "background noise" an identical repeating pattern in every block, which is not noise at
  // all: a receiver can lock onto it and it also made the measured SNR depend on block size.)
  pn_s_ ^= pn_s_ >> 12; pn_s_ ^= pn_s_ << 25; pn_s_ ^= pn_s_ >> 27;
  double u1 = ((double)(pn_s_ * 0x2545F4914F6CDD1Dull) + 1.0) / 1.8446744073709552e19;
  pn_s_ ^= pn_s_ >> 12; pn_s_ ^= pn_s_ << 25; pn_s_ ^= pn_s_ >> 27;
  double u2 = ((double)(pn_s_ * 0x2545F4914F6CDD1Dull) + 1.0) / 1.8446744073709552e19;
  if (u1 < 1e-12) u1 = 1e-12;
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

void AcousticChannel::noise_stream(size_t n, std::vector<float>* out) {
  // Pink-ish noise (three cascaded one-poles) driven by the persistent generator above, so the
  // spectrum continues smoothly from one block into the next.
  out->assign(n, 0.f);
  for (size_t i = 0; i < n; i++) {
    double w = pn_gauss();
    pn_b0_ = 0.997 * pn_b0_ + w * 0.031;
    pn_b1_ = 0.963 * pn_b1_ + w * 0.124;
    pn_b2_ = 0.575 * pn_b1_ + w * 0.168;
    (*out)[i] = (float)(pn_b0_ + pn_b1_ + pn_b2_);
  }
  double r = ni_rms(&(*out)[0], n);
  if (r > 1e-9) ni_scale(&(*out)[0], n, 1.0 / r);  // unit RMS, like noise_only()
}

void AcousticChannel::reset() {
  for (auto& bq : spk_) bq.reset();
  for (auto& bq : mic_) bq.reset();
  mp_tail_.clear();
  hist3_.clear();
  absorb_acc_ = 0.0;
  agc_gain_ = 1.0;
  agc_env_ = 1e-9;
  sig_level_ = 1e-9;
  sro_pos_ = 0.0;
  pn_b0_ = pn_b1_ = pn_b2_ = 0.0;
  pn_s_ = cfg_.seed ? (uint64_t)cfg_.seed * 0x9E3779B97F4A7C15ull : 0x9E3779B97F4A7C15ull;
  if (pn_s_ == 0) pn_s_ = 0x1234567u;
}

void AcousticChannel::process(const float* in, size_t n, std::vector<float>* out) {
  if (pn_s_ == 0) reset();
  std::vector<float> a(in, in + n), b(n);
  double txg = pow(10.0, cfg_.tx_level_db / 20.0);
  for (size_t i = 0; i < n; i++) a[i] = (float)(a[i] * txg);

  // Loudspeaker response. Deliberately NOT reset per block: the ring/box resonances and the
  // high-pass corner are continuous in time, and re-zeroing them at a callback boundary stamps a
  // transient into the first ~ms of every block.
  for (auto& bq : spk_) {
    ni_biquad_run(&bq, &a[0], n, &b[0]);
    a.swap(b);
  }
  if (cfg_.clipping) {  // amplifier / limiter soft clip
    for (size_t i = 0; i < n; i++) {
      double v = a[i];
      if (v > 0.98) v = 0.98 + tanh(v - 0.98) * 0.02;
      if (v < -0.98) v = -0.98 - tanh(-v - 0.98) * 0.02;
      a[i] = (float)v;
    }
  }
  double g = pow(10.0, loss_db_ / 20.0);
  for (size_t i = 0; i < n; i++) a[i] = (float)(a[i] * g);

  if (cfg_.multipath) {
    int dmax = 0;
    for (int r = 0; r < cfg_.n_reflections && r < 4; r++) {
      if (cfg_.refl_db[r] == 0.0 && cfg_.refl_delay_ms[r] == 0.0) continue;
      int dd = (int)(cfg_.refl_delay_ms[r] * 1e-3 * cfg_.fs);
      if (dd > dmax) dmax = dd;
    }
    if (dmax > 0) {
      // A reflection may arrive from audio that was played before this block started, so keep the
      // last `dmax` samples of the dry signal as history. Without it the first `dmax` samples of
      // every block lose their echoes, which is a discontinuity in the middle of a burst.
      std::vector<float> ext;
      ext.reserve(mp_tail_.size() + n);
      ext.insert(ext.end(), mp_tail_.begin(), mp_tail_.end());
      ext.insert(ext.end(), a.begin(), a.end());
      size_t off = mp_tail_.size();
      std::vector<float> y(n, 0.f);
      for (size_t i = 0; i < n; i++) y[i] += ext[off + i];
      for (int r = 0; r < cfg_.n_reflections && r < 4; r++) {
        if (cfg_.refl_db[r] == 0.0 && cfg_.refl_delay_ms[r] == 0.0) continue;
        size_t dd = (size_t)(cfg_.refl_delay_ms[r] * 1e-3 * cfg_.fs);
        double gg = pow(10.0, cfg_.refl_db[r] / 20.0);
        for (size_t i = 0; i < n; i++) {
          size_t idx = off + i;
          if (idx >= dd) y[i] += (float)(ext[idx - dd] * gg);
        }
      }
      mp_tail_.assign(a.end() - (size_t)dmax, a.end());
      a.swap(y);
    }
  }
  if (cfg_.air_absorption && cfg_.distance_m > 0.5) {
    // distance-dependent HF absorption as a one-pole lowpass, state kept across blocks
    double fc = 16000.0 / (1.0 + 0.35 * (cfg_.distance_m - 0.5));
    if (fc < 2500) fc = 2500;
    if (fc > 0.45 * cfg_.fs) fc = 0.45 * cfg_.fs;
    double k = exp(-2.0 * M_PI * fc / cfg_.fs);
    double acc = absorb_acc_;
    for (size_t i = 0; i < n; i++) {
      acc = k * acc + (1.0 - k) * a[i];
      a[i] = (float)(acc / (1.0 - k * k + 1e-9) * (1.0 - k));
    }
    absorb_acc_ = acc;
  }
  for (auto& bq : mic_) {
    ni_biquad_run(&bq, &a[0], n, &b[0]);
    a.swap(b);
  }

  // ---- background noise ----------------------------------------------------
  // Added HERE, i.e. acoustically in front of the microphone, so the OS AGC below amplifies signal
  // and noise together and cannot change the in-band SNR. It used to be added after the AGC and
  // scaled by the RMS of the current block, which (a) made `--snr` a lie (a block that was mostly
  // silence got almost no noise, a loud block got a lot) and (b) left the noise level swinging at
  // block boundaries in the middle of a burst.
  double blk = ni_rms(&a[0], n);
  double alpha = 1.0 - exp(-(double)n / (48000.0 * 1.0));  // ~1 s time constant
  sig_level_ += alpha * (blk - sig_level_);
  if (sig_level_ < 1e-9) sig_level_ = 1e-9;
  std::vector<float> nz;
  noise_stream(n, &nz);
  double nrms = cfg_.noise_rms >= 0.0 ? cfg_.noise_rms
                                      : sig_level_ * pow(10.0, -cfg_.snr_db / 20.0);
  if (nrms > 0) {
    for (size_t i = 0; i < n; i++) a[i] = (float)(a[i] + nz[i] * nrms);
  }

  // ---- OS AGC + noise gate -------------------------------------------------
  // A realistic OS automatic gain control: slow attack, slower release, and a noise gate that
  // slams down when the input is quiet. This is the single most common reason an acoustic modem
  // "works on the bench and dies in the field", so it is on by default in the model. The loop is
  // continuous across blocks (agc_gain_/agc_env_ are members): the gain the burst sees must not
  // depend on where the harness cut the stream.
  //
  // The time constants matter more than the target level: with a ~20 ms loop the gain moved 60 %
  // *inside one OFDM symbol*, which is not a channel an OFDM receiver can equalise - every
  // subcarrier rotates relative to its neighbours and orthogonality is gone. Real AGCs are 100 ms
  // or slower, so a burst sees a near-constant gain, and what kills modems in the field is the
  // gate, modelled below. Set agc_fast (or --fast-agc on the CLI) to reproduce the pathological
  // case; `ni selftest --fast-agc` fails for exactly that reason.
  if (cfg_.agc) {
    double gate_lin = pow(10.0, gate_dbfs_ / 20.0);
    double gain = agc_gain_;
    double target = 0.12;
    double a_tau = cfg_.agc_fast ? 0.999 : 0.995;   // per-sample pole (0.995 -> 200 ms at 48 kHz)
    double r_tau = cfg_.agc_fast ? 0.9998 : 0.9992;  // release ~ 600 ms
    double env = agc_env_ > blk ? agc_env_ : blk;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; i++) {
      double x = a[i];
      double aa = fabs(x);
      env = aa > env ? (1.0 - (1.0 - a_tau) * 4.0) * env + (1.0 - a_tau) * 4.0 * aa
                     : r_tau * env + (1.0 - r_tau) * aa;
      if (env < 1e-9) env = 1e-9;
      double want = env > 1e-6 ? target / env : 1.0;
      if (want > 18.0) want = 18.0;
      if (want < 0.05) want = 0.05;
      double tau = want > gain ? a_tau : r_tau;
      gain = tau * gain + (1.0 - tau) * want;
      double g2 = gain;
      if (env < gate_lin) g2 *= 0.1;  // gate slams down on quiet input
      y[i] = (float)(x * g2);
    }
    agc_gain_ = gain;
    agc_env_ = env;
    a.swap(y);
  }

  // sample-rate offset between the two sound cards. Continuous across blocks: the read position is
  // carried in sro_pos_ and the interpolator's left taps come from hist3_, otherwise every block
  // boundary would be a resampling discontinuity.
  if (fabs(cfg_.sro_ppm) > 0.01) {
    double ratio = 1.0 + cfg_.sro_ppm * 1e-6;
    std::vector<float> ext;
    ext.insert(ext.end(), hist3_.begin(), hist3_.end());
    ext.insert(ext.end(), a.begin(), a.end());
    size_t h = hist3_.size();
    double span = (double)(h + n) / ratio - sro_pos_;
    size_t m = span > 0.0 ? (size_t)span : 0;
    std::vector<float> y(m);
    for (size_t i = 0; i < m; i++) {
      double src = sro_pos_ + (double)i * ratio;
      size_t i0 = (size_t)src;
      double fr = src - (double)i0;
      float v0 = i0 > 0 ? ext[i0 - 1] : 0.f, v1 = ext[std::min(i0, ext.size() - 1)],
            v2 = ext[std::min(i0 + 1, ext.size() - 1)], v3 = ext[std::min(i0 + 2, ext.size() - 1)];
      // Catmull-Rom
      double c0 = v1;
      double c1 = 0.5 * (v2 - v0);
      double c2 = v0 - 2.5 * v1 + 2.0 * v2 - 0.5 * v3;
      double c3 = 0.5 * (v3 - v0) + 1.5 * (v1 - v2);
      y[i] = (float)((c3 * fr + c2) * fr * fr + c1 * fr + c0);
    }
    sro_pos_ += (double)m * ratio - (double)(h + n);
    if (sro_pos_ < 0.0) sro_pos_ = 0.0;
    hist3_.assign(a.end() - std::min<size_t>(3, a.size()), a.end());
    a.swap(y);
  }

  if (cfg_.quantize_bits == 16) {
    for (size_t i = 0; i < a.size(); i++) {
      double v = a[i];
      if (v > 1.0) v = 1.0;
      if (v < -1.0) v = -1.0;
      int q = (int)lround(v * 32767.0);
      a[i] = (float)(q / 32767.0);
    }
  }
  *out = a;
}

std::string AcousticChannel::describe() const {
  char buf[400];
  snprintf(buf, sizeof buf,
           "sim-channel[%s->%s, %.2fm, snr %.1fdB, sro %.0fppm, multipath %d, agc %s, tx %.0fdB]",
           ni_device_name(cfg_.speaker), ni_device_name(cfg_.microphone), cfg_.distance_m,
           cfg_.snr_db, cfg_.sro_ppm, cfg_.multipath ? 1 : 0, cfg_.agc ? (cfg_.agc_fast ? "fast(broken)" : "on") : "off",
           cfg_.tx_level_db);
  return buf;
}

// ------------------------------------------------------------------ PHY test helper
int ni_phy_test(const Modem& tx, const Modem& rx, const uint8_t* payload, size_t n,
                const ChannelConfig& ch, PhyTestResult* out) {
  out->decoded = false;
  out->payload_match = false;
  out->raw_ber = 0.0;
  out->fec_ber = 0.0;
  out->error.clear();
  // The harness wraps the payload in a real MAC frame, exactly as the session layer does, so a
  // pass means "a frame survived acoustic transmission bit-exactly" and not merely "some bytes
  // came back". Building the frame here (rather than accepting one) keeps every caller -
  // selftest, bench, calibration, the Android and Windows self-tests - on the same path.
  Frame src;
  src.type = NI_FT_DATA;
  src.mod_id = (uint8_t)tx.profile().id;
  src.session_id = 0x2A2A;  // NI_SESSION_TEST
  src.seq = 1;
  src.ack = 0;
  src.flags = 0;
  src.payload = payload;
  src.payload_len = (uint16_t)n;
  std::vector<uint8_t> frame(NI_MAX_FRAME);
  int fl = ni_frame_encode(&frame[0], frame.size(), src);
  if (fl <= 0) {
    out->error = std::string("frame encode: ") + ni_strerror(fl);
    return fl;
  }
  std::vector<uint8_t> pdu;
  int rc = ni_phy_wrap(&frame[0], (size_t)fl, &pdu);
  if (rc != NI_OK) {
    out->error = ni_strerror(rc);
    return rc;
  }
  std::vector<float> wave;
  rc = tx.tx_render(pdu.data(), pdu.size(), &wave);
  if (rc != NI_OK) {
    out->error = std::string("render failed: ") + ni_strerror(rc);
    return rc;
  }
  AcousticChannel chan(ch);
  std::vector<float> rec;
  // pad so that resampling/filter latency cannot cut the burst
  std::vector<float> padded(wave.size() + 2048, 0.f);
  memcpy(&padded[1024], &wave[0], sizeof(float) * wave.size());
  chan.process(&padded[0], padded.size(), &rec);
  RxResult r;
  rc = rx.rx_capture(&rec[0], rec.size(), &r);
  out->snr_db = r.stats.snr_db;
  out->rssi_dbfs = r.stats.rssi_dbfs;
  out->noise_dbfs = r.stats.noise_dbfs;
  out->burst_ms = r.stats.duration_ms;
  // Useful payload rate over the burst, which is what the calibration's profile ranking consumes
  // (ChannelEstimator multiplies payload_bps by a coding penalty). It was never assigned, so every
  // band scored zero and the "usable" threshold rejected even a profile that decoded two frames in
  // three - the ranking silently ignored throughput.
  out->payload_bps = out->burst_ms > 0.0 ? 8.0 * (double)n / (out->burst_ms / 1000.0) : 0.0;
  out->evec_pct = r.stats.evec;
  out->cfo_est_hz = (int)lround(r.cfo);
  out->bins_bad = r.stats.bins_bad;
  out->clipped = r.stats.clipped;
  out->status = rc;

  // ---- bit error rates, measured whether or not the frame validated ----------------
  size_t info_bits = pdu.size() * 8;
  std::vector<uint8_t> bits(info_bits), scr(info_bits);
  ni_bits_from_bytes(pdu.data(), pdu.size(), bits.data());
  ni_scramble(bits.data(), info_bits, scr.data());
  const Profile& p = tx.profile();
  size_t coded_bits = p.fec_rate_den == 2 ? ni_conv_out_bits(info_bits) : info_bits;
  std::vector<uint8_t> coded(coded_bits, 0);
  if (p.fec_rate_den == 2) ni_conv_encode(scr.data(), info_bits, coded.data());
  else coded = scr;
  if (!r.hard_coded.empty()) {
    size_t cn = std::min(coded.size(), r.hard_coded.size());
    size_t e = 0;
    for (size_t i = 0; i < cn; i++) e += ((r.hard_coded[i] ^ coded[i]) & 1) ? 1u : 0u;
    out->raw_ber = cn ? (double)e / (double)cn : 0.0;
  }
  if (!r.info_bits.empty()) {
    size_t cn = std::min(info_bits, r.info_bits.size());
    size_t e = 0;
    for (size_t i = 0; i < cn; i++) e += ((r.info_bits[i] ^ bits[i]) & 1) ? 1u : 0u;
    out->fec_ber = cn ? (double)e / (double)cn : 0.0;
  }
  if (rc != NI_OK) {
    out->error = std::string("rx: ") + ni_strerror(rc);
    return rc;
  }
  out->decoded = true;
  // compare the recovered frame payload with the transmitted payload
  Frame f;
  if (ni_frame_decode(r.pdu.data(), r.pdu.size(), &f) != NI_OK) {
    out->error = "frame decode failed after PHY";
    return NI_ERR_CRC;
  }
  if ((size_t)f.payload_len != n || (n && memcmp(f.payload, payload, n) != 0)) {
    out->error = "payload mismatch";
    return NI_ERR_CRC;
  }
  out->payload_match = true;
  out->tx_bits = (int)(coded_bits ? coded_bits : 0);
  out->rx_bits = (int)r.hard_coded.size();
  BurstPlan bp;
  CarrierMap cm;
  ni_build_carrier_map(tx.profile(), ch.fs > 0 ? ch.fs : tx.sample_rate(), NI_N_FFT, &cm);
  ni_plan_burst(tx.profile(), cm, ch.fs > 0 ? ch.fs : tx.sample_rate(), (int)n, &bp);
  out->burst_ms = bp.duration_ms;
  out->payload_bps = bp.duration_ms > 0.0 ? 8.0 * (double)n / (bp.duration_ms / 1000.0) : 0.0;
  return NI_OK;
}


}  // namespace ni
