// fft.cpp — iterative radix-2 FFT with cached tables.
#include "ni/fft.h"
#include "ni/common.h"
#include <math.h>
#include <stdlib.h>
#include <cstring>

namespace ni {

bool ni_fft_init(Fft* f, int n) {
  memset(f, 0, sizeof(*f));
  if (n < 8 || (n & (n - 1))) return false;
  f->n = n;
  f->tw_re = (float*)malloc(sizeof(float) * (size_t)(n / 2));
  f->tw_im = (float*)malloc(sizeof(float) * (size_t)(n / 2));
  f->rev = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)n);
  f->scratch = (float*)malloc(sizeof(float) * 2 * (size_t)n);
  if (!f->tw_re || !f->tw_im || !f->rev || !f->scratch) {
    ni_fft_free(f);
    return false;
  }
  for (int k = 0; k < n / 2; k++) {
    double a = -2.0 * M_PI * (double)k / (double)n;
    f->tw_re[k] = (float)cos(a);
    f->tw_im[k] = (float)sin(a);
  }
  int lg = 0;
  while ((1 << lg) < n) lg++;
  for (int i = 0; i < n; i++) {
    uint32_t r = 0;
    for (int b = 0; b < lg; b++)
      if (i & (1 << b)) r |= 1u << (lg - 1 - b);
    f->rev[i] = r;
  }
  f->ok = true;
  return true;
}

void ni_fft_free(Fft* f) {
  free(f->tw_re);
  free(f->tw_im);
  free(f->rev);
  free(f->scratch);
  memset(f, 0, sizeof(*f));
}

void ni_fft_forward(const Fft* f, float* x) {
  const int n = f->n;
  for (int i = 0; i < n; i++) {
    uint32_t j = f->rev[i];
    if (j > (uint32_t)i) {
      float tr = x[2 * i], ti = x[2 * i + 1];
      x[2 * i] = x[2 * (int)j];
      x[2 * i + 1] = x[2 * (int)j + 1];
      x[2 * (int)j] = tr;
      x[2 * (int)j + 1] = ti;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1, step = n / len;
    for (int i = 0; i < n; i += len) {
      for (int j = 0; j < half; j++) {
        float wr = f->tw_re[j * step], wi = f->tw_im[j * step];
        float* a = &x[2 * (i + j)];
        float* b = &x[2 * (i + j + half)];
        float br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;
        b[0] = a[0] - br;
        b[1] = a[1] - bi;
        a[0] += br;
        a[1] += bi;
      }
    }
  }
}

void ni_fft_inverse(const Fft* f, float* x) {
  const int n = f->n;
  for (int i = 0; i < n; i++) x[2 * i + 1] = -x[2 * i + 1];
  ni_fft_forward(f, x);
  double s = 1.0 / (double)n;
  for (int i = 0; i < n; i++) {
    x[2 * i] = (float)(x[2 * i] * s);
    x[2 * i + 1] = (float)(-x[2 * i + 1] * s);
  }
}

void ni_synthesize_real(const Fft* f, const float* spec_re, const float* spec_im, int k0,
                        int k1, float* out) {
  const int n = f->n;
  float* x = f->scratch;
  memset(x, 0, sizeof(float) * 2 * (size_t)n);
  for (int k = k0; k <= k1; k++) {
    if (k <= 0 || k >= n / 2) continue;
    int i = k - k0;
    x[2 * k] = spec_re[i];
    x[2 * k + 1] = spec_im[i];
    x[2 * (n - k)] = spec_re[i];
    x[2 * (n - k) + 1] = -spec_im[i];
  }
  ni_fft_inverse(f, x);
  for (int m = 0; m < n; m++) out[m] = x[2 * m];
}

void ni_analyse_real(const Fft* f, const float* in, float* spec_re, float* spec_im, int k0,
                     int k1) {
  const int n = f->n;
  float* x = f->scratch;
  for (int m = 0; m < n; m++) {
    x[2 * m] = in[m];
    x[2 * m + 1] = 0.f;
  }
  ni_fft_forward(f, x);
  for (int k = k0; k <= k1; k++) {
    int i = k - k0;
    if (k <= 0 || k >= n / 2) {
      spec_re[i] = 0.f;
      spec_im[i] = 0.f;
      continue;
    }
    spec_re[i] = x[2 * k];
    spec_im[i] = x[2 * k + 1];
  }
}

void ni_add_cp_tapered(const float* sym, int cp, float* out, float taper_gain) {
  // Cyclic prefix (last `cp` samples copied in front) followed by a raised-cosine
  // window over cp/2 samples at both ends -- windowed OFDM. This is what keeps
  // out-of-band splatter low enough that laptop/phone AGC and equalisers do not
  // react to the burst, which is a real failure mode of naive OFDM audio modems.
  const int n = NI_N_FFT;
  memcpy(out, sym + (n - cp), sizeof(float) * (size_t)cp);
  memcpy(out + cp, sym, sizeof(float) * (size_t)n);
  int w = cp / 2;
  if (w < 1) w = 1;
  if (taper_gain < 0.0) taper_gain = 0.0;
  if (taper_gain > 1.0) taper_gain = 1.0;
  // The window lives entirely INSIDE the prefix: fading the copied tail up toward the
  // symbol body removes the discontinuity without touching a single sample of the useful
  // part. That matters because the receiver takes a rectangular window over the body
  // (ni_strip_cp) and only there does the subcarrier orthogonality survive: tapering into
  // the body (the "smoothed OFDM" variant) would need a matched window at the receiver and
  // silently destroys the differential decisions on the first bins.
  for (int i = 0; i < w; i++) {
    double a = 0.5 * (1.0 - cos(M_PI * (double)(i + 1) / (double)(w + 1)));
    float g = (float)(taper_gain + (1.0 - taper_gain) * a);
    out[i] *= g;             // fade in from the previous symbol's tail
    out[cp - 1 - i] *= g;    // fade up into the body
  }
  (void)n;
}

void ni_strip_cp(const float* sym_with_cp, int cp, float* out) {
  const int n = NI_N_FFT;
  memcpy(out, sym_with_cp + cp, sizeof(float) * (size_t)n);
}

}  // namespace ni
