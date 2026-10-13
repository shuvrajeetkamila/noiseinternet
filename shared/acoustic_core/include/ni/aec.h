// aec.h — normalised LMS acoustic echo cancellation for the full-duplex experiment, plus
// self-interference estimation and ERLE measurement.
#ifndef NI_AEC_H
#define NI_AEC_H
#include <stddef.h>
#include <stdint.h>
#include <vector>
namespace ni {

class NlmsAec {
 public:
  NlmsAec(int taps = 1024, double step = 0.03, double leakage = 1e-4);
  void reset();
  int taps() const { return taps_; }
  // ref = the signal we are playing, ech = what the microphone captured.
  // out = residual after subtraction (still contains the peer's signal).
  void process(const float* ref, size_t n, const float* ech, float* out);
  // Delay of the best-aligned echo path, in samples (searched at init/refit).
  void set_delay(int d) { delay_ = d; }
  int delay() const { return delay_; }
  double erle_db() const { return erle_; }
  double convergence_ms() const { return conv_ms_; }
  uint64_t samples_processed() const { return n_; }
  // Feed known-silence to estimate the residual echo power (ERLE measure).
  void measure_erle(const float* ref, const float* ech, size_t n);
  double misalignment_db() const;

 private:
  int taps_, delay_ = 0;
  double step_, leakage_;
  std::vector<float> w_, x_, buf_;
  int pos_ = 0;
  double erle_ = 0.0, conv_ms_ = 0.0;
  uint64_t n_ = 0;
  double echo_pow_ = 0.0, resid_pow_ = 0.0;
};

// Self-interference estimate: how much of our own TX leaks into our RX (dB). Measured by
// correlating the known playback against the capture while the peer is silent.
double ni_self_interference_db(const float* played, size_t n, const float* captured);

}  // namespace ni
#endif
