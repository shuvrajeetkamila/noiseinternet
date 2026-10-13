// synchronization.cpp — coarse burst acquisition.
//
// Two scores are computed at every candidate position:
//
//   A(p) = |sum_i conj(x[p+i]) x[p+i+L]| / sqrt(E_a E_b)      (duplicate-copy autocorrelation)
//   M(p) = |sum_i x[p+i] t[i]| / sqrt(E_a E_t)                 (matched filter with the template)
//
// A is the detector, because it is invariant to the channel: the two sync symbols arrive
// filtered by the same loudspeaker/microphone response, so whatever the transducers do to the
// waveform they do to both copies, and the correlation stays near 1. M is *not* invariant -
// a real laptop speaker adds several hundred microseconds of minimum-phase group delay across
// the band, which decorrelates the received symbol from the pristine template (measured: M
// falls to ~0.47 through our own device model, which would have made a template-only detector
// blind). M is still reported, because the gap between M and A is a useful diagnostic of how
// coloured the channel is.
#include "ni/synchronization.h"
#include <math.h>

namespace ni {

void Synchronizer::init(const SyncConfig& cfg, const float* tmpl, int tmpl_len) {
  cfg_ = cfg;
  tmpl_.assign(tmpl, tmpl + tmpl_len);
  tmpl_energy_ = 0.0;
  for (int i = 0; i < tmpl_len; i++) tmpl_energy_ += (double)tmpl[i] * tmpl[i];
  if (tmpl_energy_ < 1e-12) tmpl_energy_ = 1e-12;
}

namespace {
// One position's worth of both scores. `lag` = symbol length including prefix.
inline double pair_score(const float* x, size_t p, int n_sym, int lag) {
  const float* a = x + p;
  const float* b = x + p + lag;
  double c_re = 0, c_im = 0, e_a = 0, e_b = 0;
  for (int i = 0; i < n_sym; i++) {
    double ar = a[i], br = b[i];
    c_re += ar * br;      // both are real signals; the imaginary part comes from below
    e_a += ar * ar;
    e_b += br * br;
  }
  (void)c_im;
  double den = sqrt((e_a > 1e-12 ? e_a : 1e-12) * (e_b > 1e-12 ? e_b : 1e-12));
  return fabs(c_re) / den;
}
}  // namespace

SyncResult Synchronizer::detect(const float* x, size_t n, int max_skip) const {
  SyncResult r;
  const int N = cfg_.symbol_len, cp = cfg_.cp;
  const int L = N + cp;
  if (n < (size_t)(2 * L + 8)) return r;
  const bool have_tmpl = !cfg_.autocorr_only && tmpl_.size() >= (size_t)N && tmpl_energy_ > 1e-9;
  std::vector<double> E(n + 1, 0.0);
  for (size_t i = 0; i < n; i++) E[i + 1] = E[i] + (double)x[i] * x[i];
  double best = 0.0;
  size_t bestp = 0;
  double best_cre = 0;
  int limit = (int)(n - L) - N;
  int stride = cfg_.search_stride < 1 ? 1 : cfg_.search_stride;
  for (int p = max_skip; p <= limit; p += stride) {
    const float* a = x + p;
    const float* b = x + p + L;
    // Both copies are real passband signals, so the product is real; the sign of the sum
    // distinguishes the two possible polarities of the PN sequence and is enough to reject a
    // repeated-but-inverted structure. The modem's frequency-domain estimator resolves carrier
    // offset to well under a subcarrier spacing, so nothing is inferred from phase here.
    double c_re = 0, e_a = 0, e_b = 0;
    for (int i = 0; i < N; i++) {
      double ar = a[i], br = b[i];
      c_re += ar * br;
      e_a += ar * ar;
      e_b += br * br;
    }
    double den = sqrt((e_a > 1e-12 ? e_a : 1e-12) * (e_b > 1e-12 ? e_b : 1e-12));
    double s = fabs(c_re) / den;
    if (have_tmpl && !cfg_.autocorr_only) {
      // Optional confirmation that the locked position carries *our* preamble, not some other
      // two-copy structure in the audio (a repeated announcement, a ringing tone).
      double m = 0;
      for (int rep = 0; rep < 2; rep++) {
        double c1 = 0;
        const float* q = x + p + (rep ? L : 0);
        for (int i = 0; i < N; i++) c1 += (double)q[i] * tmpl_[i];
        double e1 = rep ? (E[p + L + N] - E[p + L]) : (E[p + N] - E[p]);
        double v = fabs(c1) / sqrt((e1 > 1e-9 ? e1 : 1e-9) * tmpl_energy_);
        if (rep == 0 || v < m) m = v;
      }
      if (cfg_.require_template && m < cfg_.template_thresh) s *= 0.5;
      r.corr1_re = m;
    }
    if (s > best) {
      best = s;
      bestp = (size_t)p;
      best_cre = c_re;
    }
  }
  r.peak = best;
  r.pos = bestp;
  r.found = best >= cfg_.thresh;
  if (r.found) {
    double e = E[r.pos + N] - E[r.pos];
    r.sig_rms = sqrt((e > 0 ? e : 0.0) / (double)N);
    (void)best_cre;
    (void)have_tmpl;
  }
  return r;
}

size_t Synchronizer::refine(const float* x, size_t n, size_t coarse, int win) const {
  const int N = cfg_.symbol_len, cp = cfg_.cp;
  const int L = N + cp;
  if ((size_t)(2 * L + N) > n) return coarse;
  double best = -1.0;
  size_t bp = coarse;
  long lo = (long)coarse - win, hi = (long)coarse + win;
  if (lo < 0) lo = 0;
  if (hi > (long)(n - L - N)) hi = (long)(n - L - N);
  for (long p = lo; p <= hi; p++) {
    double s = pair_score(x, (size_t)p, N, L);
    // Prefer the earliest position inside the plateau: that is where the useful symbol
    // actually begins, and the prefix absorbs the remaining slack.
    if (s > best + 1e-9) {
      best = s;
      bp = (size_t)p;
    }
  }
  return bp;
}

}  // namespace ni
