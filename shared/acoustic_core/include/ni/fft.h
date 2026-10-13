// fft.h — radix-2 complex FFT/IFFT with cached twiddle tables, and the real passband
// helpers used by the OFDM PHY.
//
// Determinism note (matters for a cross-platform protocol): the twiddle tables are built
// with sin/cos from the platform libm, so the *waveform* of a burst is only bit-exact
// within one build. That is harmless: the constellation symbol indices — which are the
// actual wire definition — are integer-exact and are what the protocol test vectors pin.
// Receivers use their own tables; a 1e-6 twiddle difference is 120 dB below the noise floor.
#ifndef NI_FFT_H
#define NI_FFT_H
#include <stddef.h>
#include <stdint.h>
namespace ni {

struct Fft {
  int n;
  float* tw_re;  // n/2 entries
  float* tw_im;
  uint32_t* rev;  // bit-reversal permutation, n entries
  float* scratch;  // 2*n
  bool ok;
};
bool ni_fft_init(Fft* f, int n);  // n must be a power of two, >=8
void ni_fft_free(Fft* f);

// in-place, interleaved re/im of length n
void ni_fft_forward(const Fft* f, float* x);
void ni_fft_inverse(const Fft* f, float* x);  // includes 1/n scaling

// ---- real passband OFDM helpers ----------------------------------------------
// Spectrum is defined on bins 1..n/2-1 (Hermitian symmetry implied, bins 0 and n/2 are
// forced to zero because a real passband signal carries no DC information).
//
// ni_synthesize: S (complex pairs for bins k0..k1) -> real time samples of length n
//                x[m] = 2/n * Re( sum_{k} S[k] e^{+j 2 pi k m / n} )
// ni_analyse:    inverse of ni_synthesize for the same bin range (returns 1/sqrt scaling
//                such that a unit-magnitude constellation point is recovered as magnitude 1).
void ni_synthesize_real(const Fft* f, const float* spec_re, const float* spec_im, int k0,
                        int k1, float* out /*n*/);
void ni_analyse_real(const Fft* f, const float* in /*n*/, float* spec_re, float* spec_im,
                     int k0, int k1);

// Cyclic-prefix helpers with a raised-cosine taper confined to the prefix itself: it
// removes the head/tail discontinuity (hence most of the out-of-band splatter that provokes
// laptop AGC) while leaving the useful symbol untouched, so the receiver can take a plain
// rectangular window and keep orthogonality.
void ni_add_cp_tapered(const float* sym /*n*/, int cp, float* out /*n+cp*/, float taper_gain);
void ni_strip_cp(const float* sym_with_cp, int cp, float* out /*n*/);

}  // namespace ni
#endif
