// synchronization.h — burst acquisition: matched-filter preamble detection, coarse CFO,
// and reference-symbol re-lock for long bursts.
//
// Design: the preamble is an OFDM symbol whose used bins carry a fixed, seeded PN-BPSK
// sequence, so it is automatically band-limited to the profile (a wideband ZC chirp would
// put energy in bands the loudspeaker cannot reproduce, which is exactly how cheap
// acoustic modems fail on laptops). Detection is a time-domain normalised cross-correlation
// with the known template; two identical sync symbols give (a) a presence test and (b) a
// coarse carrier offset from their relative phase.
#ifndef NI_SYNCHRONIZATION_H
#define NI_SYNCHRONIZATION_H
#include <stdint.h>
#include <vector>
#include "ni/common.h"

namespace ni {

struct Fft;

struct SyncResult {
  size_t pos = 0;        // index of first sample of sync symbol S0
  double peak = 0.0;     // normalised correlation magnitude [0,1]
  double cfo_hz = 0.0;   // signed carrier frequency offset estimate
  double corr1_re = 0, corr1_im = 0;
  double sig_rms = 0.0;  // measured receive level at the burst (for RSS/SNR reporting)
  bool found = false;
};

struct SyncConfig {
  double thresh = 0.55;  // min normalised correlation to accept
  int symbol_len = NI_N_FFT;
  int cp = NI_CP;
  double sample_rate = 48000.0;
  int search_stride = 1;  // 1 = sample-accurate; larger = cheaper
  bool autocorr_only = false;    // skip the template score entirely (tests of the detector)
  bool require_template = false; // demote the score when the template match is weak
  double template_thresh = 0.35; // only read when require_template is set
};

class Synchronizer {
 public:
  Synchronizer() = default;
  // template must be the exact waveform the transmitter uses for one sync symbol
  void init(const SyncConfig& cfg, const float* tmpl, int tmpl_len);
  const SyncConfig& config() const { return cfg_; }
  // Searches x[0..n - needed) for the 2-symbol preamble. n is the number of usable samples.
  SyncResult detect(const float* x, size_t n, int max_skip = 0) const;
  // Fine local search: returns refined start (max |corr| within +-win of the coarse peak).
  size_t refine(const float* x, size_t n, size_t coarse, int win) const;
  int needed_samples() const { return 2 * (cfg_.symbol_len + cfg_.cp); }
  const std::vector<float>& template_wave() const { return tmpl_; }

 private:
  SyncConfig cfg_{};
  std::vector<float> tmpl_;
  double tmpl_energy_ = 0.0;
};

}  // namespace ni
#endif
