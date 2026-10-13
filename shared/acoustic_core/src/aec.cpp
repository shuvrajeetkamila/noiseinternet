// aec.cpp — NLMS echo canceller for the full-duplex experiment.
#include "ni/aec.h"
#include <math.h>
#include <string.h>

namespace ni {

NlmsAec::NlmsAec(int taps, double step, double leakage)
    : taps_(taps < 8 ? 8 : taps), step_(step), leakage_(leakage) {
  w_.assign((size_t)taps_ + 1, 0.f);
  x_.assign((size_t)taps_ * 4, 0.f);
  buf_.assign((size_t)taps_ + 1, 0.f);
  pos_ = 0;
}

void NlmsAec::reset() {
  memset(&w_[0], 0, sizeof(float) * w_.size());
  memset(&x_[0], 0, sizeof(float) * x_.size());
  pos_ = 0;
  echo_pow_ = resid_pow_ = 0.0;
  n_ = 0;
}

void NlmsAec::process(const float* ref, size_t n, const float* ech, float* out) {
  const int T = taps_;
  const int XN = (int)x_.size();
  for (size_t i = 0; i < n; i++) {
    size_t head = ((size_t)pos_ + (size_t)T) % (size_t)XN;
    x_[head] = ref ? ref[i] : 0.f;
    double y = 0.0, ex = 0.0;
    for (int k = 0; k < T; k++) {
      double xk = x_[(head - (size_t)k + (size_t)XN * 2) % (size_t)XN];
      y += w_[(size_t)k] * xk;
      ex += xk * xk;
    }
    double e = (double)ech[i] - y;
    out[i] = (float)e;
    echo_pow_ = 0.9999 * echo_pow_ + 0.0001 * (double)ech[i] * ech[i];
    resid_pow_ = 0.9999 * resid_pow_ + 0.0001 * e * e;
    if (ex > 1e-9) {
      double mu = step_ / (ex / (double)T + 1e-6);
      if (mu > 0.6) mu = 0.6;
      for (int k = 0; k < T; k++) {
        double xk = x_[(head - (size_t)k + (size_t)XN * 2) % (size_t)XN];
        w_[(size_t)k] =
            (float)((double)w_[(size_t)k] * (1.0 - leakage_) + 2.0 * mu * e * xk / (double)T);
      }
    }
    pos_ = (pos_ + 1) % XN;
    n_++;
  }
  if (echo_pow_ > 1e-18) erle_ = 10.0 * log10(echo_pow_ / (resid_pow_ + 1e-18));
}

void NlmsAec::measure_erle(const float* ref, const float* ech, size_t n) {
  std::vector<float> out(n);
  double ep = 0, rp = 0;
  for (size_t i = 0; i < n; i++) {
    float before = ech[i];
    process(&ref[i], 1, &ech[i], &out[i]);
    ep += (double)before * before;
    rp += (double)out[i] * out[i];
  }
  if (rp > 1e-20 && ep > 1e-20) erle_ = 10.0 * log10(ep / rp);
  conv_ms_ = (double)n / 48000.0 * 1000.0;
}

double NlmsAec::misalignment_db() const {
  double s = 0;
  for (size_t i = 0; i < w_.size(); i++) s += (double)w_[i] * w_[i];
  return s > 1e-18 ? 10.0 * log10(s) : -99.0;
}

double ni_self_interference_db(const float* played, size_t n, const float* captured) {
  if (!n) return -999.0;
  double best = -1e30;
  int lag_search = (int)(0.02 * 48000.0);  // up to 20 ms of path delay
  for (int lag = -lag_search; lag <= lag_search; lag++) {
    double num = 0, d1 = 0, d2 = 0;
    for (size_t i = 0; i + (size_t)abs(lag) < n; i++) {
      double a = played[i];
      double b = captured[lag >= 0 ? i + (size_t)lag : i];
      num += a * b;
      d1 += a * a;
      d2 += b * b;
    }
    if (d1 > 1e-12 && d2 > 1e-12) {
      double c = num / sqrt(d1 * d2);
      if (c > best) best = c;
    }
  }
  if (best < 1e-6) best = 1e-6;
  return 20.0 * log10(best);
}

}  // namespace ni
