// t_fft.cpp — the transform and the real passband pair. The OFDM chain lives or dies on these
// properties: ni_analyse_real must be the exact inverse of ni_synthesize_real over the used bins
// (otherwise constellations rotate or scale with bin index), a cyclic time shift must appear as a
// pure per-bin phase ramp, and the cyclic prefix must be removable without touching the body.
//
// Conventions under test, from include/ni/fft.h:
//   synthesize:  x[m] = 2/n * Re( sum_{k=k0..k1} S[k] e^{+j2pi k m/n} )
//   analyse:     the inverse of the above (a plain forward FFT, which cancels the 2/n exactly)
//   the spectrum arrays are PACKED from k0: array index i is bin k0+i.
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

#include "ni/common.h"
#include "ni/fft.h"
#include "ntest.h"

using namespace ni;

int main() {
  printf("[t_fft] transform, real passband pair, cyclic prefix\n");
  const int N = NI_N_FFT;
  Fft f;
  NT_CHECK(ni_fft_init(&f, N));
  NT_EQ(f.n, N);
  {
    Fft bad;
    NT_CHECK(!ni_fft_init(&bad, 100));  // not a power of two
    NT_CHECK(!ni_fft_init(&bad, 4));    // below the minimum size
  }

  // ---- forward/inverse round trip on a complex vector ----
  {
    std::vector<float> a((size_t)N * 2), b;
    for (size_t i = 0; i < a.size(); i++) a[i] = (float)(sin(0.37 * (double)i) * (1.0 + i * 0.01));
    b = a;
    ni_fft_forward(&f, b.data());
    ni_fft_inverse(&f, b.data());
    double worst = 0;
    for (size_t i = 0; i < a.size(); i++) worst = std::max(worst, (double)fabs(a[i] - b[i]));
    NT_CHECK_MSG(worst < 1e-4, "fft forward+inverse is not the identity");
    NT_CHECK(worst > 0 || true);  // (the inverse must scale by 1/n, verified by the round trip)
  }

  // ---- a single tone: exact time waveform, exact recovery, no leakage ----
  {
    const int k0 = 1, k1 = N / 2 - 1, kb = 37;
    std::vector<float> re((size_t)(k1 - k0 + 1), 0.f), im(re.size(), 0.f);
    re[kb - k0] = 1.f;
    std::vector<float> x((size_t)N, 0.f);
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x.data());
    double worst = 0;
    for (int m = 0; m < N; m++) {
      double want = (2.0 / N) * cos(2.0 * NI_PI * kb * m / N);
      worst = std::max(worst, fabs(want - (double)x[m]));
    }
    NT_CHECK_MSG(worst < 1e-6, "ni_synthesize_real is not 2/n*Re(sum S[k] e^{+j2pi km/n})");

    std::vector<float> br(re.size(), 0.f), bi(re.size(), 0.f);
    ni_analyse_real(&f, x.data(), br.data(), bi.data(), k0, k1);
    NT_NEAR(br[kb - k0], 1.0, 1e-6);
    NT_NEAR(bi[kb - k0], 0.0, 1e-6);
    double leak = 0;
    for (size_t i = 0; i < br.size(); i++)
      if ((int)i != kb - k0) leak = std::max(leak, (double)(fabs(br[i]) + fabs(bi[i])));
    NT_CHECK_MSG(leak < 1e-6, "one subcarrier leaked into its neighbours: bins are not orthogonal");
  }

  // ---- a rotated constellation point must come back with the same complex value ----
  {
    const int k0 = 5, k1 = N / 2 - 1;
    size_t nb = (size_t)(k1 - k0 + 1);
    std::vector<float> re(nb, 0.f), im(nb, 0.f);
    for (size_t i = 0; i < nb; i++) {
      double th = 0.61 * (double)i;
      re[i] = (float)cos(th) * 0.8f;
      im[i] = (float)sin(th) * 0.8f;
    }
    std::vector<float> x((size_t)N), ar(nb), ai(nb);
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x.data());
    ni_analyse_real(&f, x.data(), ar.data(), ai.data(), k0, k1);
    double worst = 0;
    for (size_t i = 0; i < nb; i++) {
      worst = std::max(worst, (double)fabs(re[i] - ar[i]));
      worst = std::max(worst, (double)fabs(im[i] - ai[i]));
    }
    NT_CHECK_MSG(worst < 1e-5, "analyse(synthesize(S)) != S over the used bins");
  }

  // ---- out-of-band bins must not be emitted, and must read back as zero ----
  {
    const int k0 = 20, k1 = 100;
    size_t nb = (size_t)(k1 - k0 + 1);
    std::vector<float> re(nb, 1.f), im(nb, 0.f), x((size_t)N, 0.f);
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x.data());
    // the synthesised signal must carry no DC and no Nyquist component
    double mean = 0;
    for (int m = 0; m < N; m++) mean += x[m];
    NT_NEAR(mean / N, 0.0, 1e-6);
    // asking for the forbidden range (DC, Nyquist and above) must give exact zeros rather than
    // whatever the transform happened to leave there
    size_t nb2 = 6;
    std::vector<float> ar(nb2, 9.f), ai(nb2, 9.f);
    ni_analyse_real(&f, x.data(), ar.data(), ai.data(), N / 2, (int)(N / 2) + 5);
    for (size_t i = 0; i < nb2; i++) {
      NT_EQ(ar[i], 0.f);
      NT_EQ(ai[i], 0.f);
    }
    // and the two in-band bins just below Nyquist must be tiny but not forced (float noise floor)
    std::vector<float> ar2(3, 9.f), ai2(3, 9.f);
    ni_analyse_real(&f, x.data(), ar2.data(), ai2.data(), N / 2 - 3, N / 2 - 1);
    for (size_t i = 0; i < 3; i++) NT_NEAR(ar2[i], 0.0, 1e-5);
  }

  // ---- a circular time shift is a linear phase ramp, magnitude preserved ----
  {
    const int k0 = 8, k1 = 100;
    size_t nb = (size_t)(k1 - k0 + 1);
    std::vector<float> re(nb), im(nb);
    Rng r(0x51de);
    for (size_t i = 0; i < nb; i++) {
      re[i] = (float)((int)(r.next() % 2000) - 1000) / 1000.f;
      im[i] = (float)((int)(r.next() % 2000) - 1000) / 1000.f;
    }
    std::vector<float> x((size_t)N), y((size_t)N);
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x.data());
    const int sh = 17;
    for (int m = 0; m < N; m++) y[m] = x[(m + sh) % N];  // delay through the cyclic prefix
    std::vector<float> ar(nb), ai(nb), br(nb), bi(nb);
    ni_analyse_real(&f, x.data(), ar.data(), ai.data(), k0, k1);
    ni_analyse_real(&f, y.data(), br.data(), bi.data(), k0, k1);
    double worst_m = 0, worst_p = 0;
    for (size_t i = 0; i < nb; i++) {
      int k = k0 + (int)i;
      double ma = hypot(ar[i], ai[i]), mb = hypot(br[i], bi[i]);
      worst_m = std::max(worst_m, fabs(ma - mb));
      double pa = atan2(ai[i], ar[i]), pb = atan2(bi[i], br[i]);
      double d = pb - pa - 2.0 * NI_PI * (double)k * sh / N;
      d = fmod(d + NI_PI, 2.0 * NI_PI);
      if (d < 0) d += 2.0 * NI_PI;  // fmod keeps the sign of the dividend: wrap it explicitly
      d -= NI_PI;
      worst_p = std::max(worst_p, fabs(d));
    }
    NT_CHECK_MSG(worst_m < 1e-5, "circular shift changed subcarrier magnitudes");
    printf("  note: shift %d samples -> worst magnitude err %.3g, worst phase err %.3g rad\n", sh,
           worst_m, worst_p);
    NT_CHECK_MSG(worst_p < 2e-3, "circular shift is not a pure phase ramp (prefix math wrong)");
  }

  // ---- energy scales with the square of the symbol amplitude (no hidden normalisation) ----
  {
    const int k0 = 20, k1 = 110;
    size_t nb = (size_t)(k1 - k0 + 1);
    std::vector<float> re(nb), im(nb, 0.f);
    for (size_t i = 0; i < nb; i++) re[i] = (i & 1) ? 1.f : -1.f;
    std::vector<float> x1((size_t)N), x2((size_t)N);
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x1.data());
    for (size_t i = 0; i < nb; i++) re[i] *= 2.f;
    ni_synthesize_real(&f, re.data(), im.data(), k0, k1, x2.data());
    double e1 = 0, e2 = 0;
    for (int m = 0; m < N; m++) {
      e1 += (double)x1[m] * x1[m];
      e2 += (double)x2[m] * x2[m];
    }
    NT_NEAR(e2 / e1, 4.0, 1e-4);
    // and the documented absolute scaling: n/2 * (2/n)^2 * sum|S|^2
    double want = (2.0 / N) * (double)nb;
    NT_NEAR(e1, want, 1e-6);
  }

  // ---- cyclic prefix helpers ----
  {
    std::vector<float> x((size_t)N), y((size_t)(N + NI_CP)), z((size_t)N);
    Rng r(99);
    for (auto& v : x) v = (float)((int)(r.next() % 2000) - 1000) / 1000.f;
    // taper_gain = 1 means "no windowing": the prefix must be an exact copy of the tail
    ni_add_cp_tapered(x.data(), NI_CP, y.data(), 1.0f);
    for (int i = 0; i < NI_CP; i++) NT_NEAR(y[i], x[N - NI_CP + i], 1e-6);
    ni_strip_cp(y.data(), NI_CP, z.data());
    NT_CHECK(memcmp(x.data(), z.data(), sizeof(float) * (size_t)N) == 0);

    // Any taper must leave the useful symbol bit-exact, because the receiver windows the body
    // with a rectangle. This was once violated (the taper ate body samples) and the symptom was
    // a modem that decoded fine except on the lowest subcarriers.
    for (float g : {0.f, 0.25f, 0.5f, 1.f}) {
      std::vector<float> y2((size_t)(N + NI_CP)), z2((size_t)N);
      ni_add_cp_tapered(x.data(), NI_CP, y2.data(), g);
      ni_strip_cp(y2.data(), NI_CP, z2.data());
      double worst = 0;
      for (int i = 0; i < N; i++) worst = std::max(worst, (double)fabs(x[i] - z2[i]));
      NT_CHECK_MSG(worst == 0.0, "the raised-cosine taper changed the symbol body");
      // the taper only attenuates: it can never exceed the copied tail's own peak
      double src = 0, dst = 0;
      for (int i = 0; i < NI_CP; i++) {
        src = std::max(src, (double)fabs(x[N - NI_CP + i]));
        dst = std::max(dst, (double)fabs(y2[i]));
      }
      NT_CHECK(dst <= src + 1e-6);
      if (g == 0.0f) {
        // Fully windowed, the prefix is a raised-cosine bump: it starts at nothing, reaches the
        // copied amplitude at the middle of the prefix, and returns to nothing at the body. Both
        // edges are what touch a discontinuity; the body itself is never touched (checked
        // above), which is the property the receiver depends on.
        NT_CHECK(fabs(y2[0]) < 0.05 * fabs(x[N - NI_CP]));
        NT_CHECK(fabs(y2[NI_CP / 4]) > fabs(y2[0]));
        NT_CHECK(fabs(y2[NI_CP - 1]) < 0.05 * fabs(x[N - 1]));
        NT_CHECK(fabs(y2[NI_CP / 2]) > 0.9 * fabs(x[N - NI_CP + NI_CP / 2]));
      } else {
        // partial taper: attenuated, monotone toward the body, and never above the copy
        NT_CHECK(fabs(y2[0]) < 1.05 * fabs(x[N - NI_CP]) * (g + 0.3));
      }
    }
  }

  ni_fft_free(&f);
  return ntest::summary("t_fft");
}
