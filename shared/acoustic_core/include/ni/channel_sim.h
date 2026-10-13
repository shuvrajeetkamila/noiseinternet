// channel_sim.h — physical acoustic channel model: loudspeaker and microphone responses,
// room multipath, air absorption, background noise, OS-style AGC/noise gate, clipping,
// 16-bit quantisation and sample-rate offset between the two sound cards.
//
// This exists because the build sandbox has no transducers (see ADR-001). It is a model, and
// every report produced through it is labelled SIMULATED CHANNEL. It is deliberately hostile
// in the ways real devices are hostile: a phone loudspeaker is a high-passed,
// resonant, current-limited box whose response collapses above ~15 kHz, and the OS audio
// stack applies AGC that will happily mute a low-level modem burst.
#ifndef NI_CHANNEL_SIM_H
#define NI_CHANNEL_SIM_H
#include <cstdint>
#include <string>
#include <vector>
#include "ni/common.h"

namespace ni {

enum DeviceModel : int {
  NI_DEV_IDEAL = 0,      // flat, infinite bandwidth: used for PHY unit tests
  NI_DEV_LAPTOP_SPK = 1,  // 2nd-order HP ~400 Hz, resonant bump 1.5 kHz, -12 dB/oct >14 kHz
  NI_DEV_PHONE_SPK = 2,   // HP ~600 Hz, strong cut below 1 kHz, sharp rolloff > 13 kHz
  NI_DEV_LAPTOP_MIC = 3,  // HP ~100 Hz, gentle HF rolloff
  NI_DEV_PHONE_MIC = 4,   // HP ~120 Hz + OS noise gate emulation
  NI_DEV_SMART_SPEAKER = 5,  // aggressive DSP: limiter + 15 kHz cut
  NI_DEV_MODEL_COUNT = 6
};
const char* ni_device_name(int m);
void ni_device_defaults(int model, double* hp_hz, double* reson_hz, double* reson_db,
                        double* lp_hz, double* noise_gate_dbfs);

struct Biquad {
  double b0, b1, b2, a1, a2;
  double z1 = 0, z2 = 0;
  void reset() { z1 = z2 = 0; }
};
// 2nd-order Butterworth designs via bilinear transform (Q = 0.7071).
Biquad ni_biquad_highpass(double f0, double fs);
Biquad ni_biquad_lowpass(double f0, double fs);
Biquad ni_biquad_peaking(double f0, double fs, double gain_db, double q);
inline void ni_biquad_run(Biquad* b, const float* x, size_t n, float* y) {
  for (size_t i = 0; i < n; i++) {
    double in = x[i];
    double out = b->b0 * in + b->z1;
    b->z1 = b->b1 * in - b->a1 * out + b->z2;
    b->z2 = b->b2 * in - b->a2 * out;
    y[i] = (float)out;
  }
}

struct ChannelConfig {
  double fs = 48000.0;
  int speaker = NI_DEV_LAPTOP_SPK;
  int microphone = NI_DEV_LAPTOP_MIC;
  double distance_m = 0.5;
  double snr_db = 25.0;       // in-band signal-to-noise target
  double noise_rms = -1;      // >=0 overrides snr_db (absolute full-scale rms)
  double sro_ppm = 0.0;       // receiver clock offset, parts per million
  bool multipath = true;
  int n_reflections = 2;
  double refl_delay_ms[4] = {1.1, 2.6, 0.0, 0.0};
  double refl_db[4] = {-4.5, -8.0, 0.0, 0.0};
  bool agc = true;            // OS-style AGC + noise gate
  bool agc_fast = false;      // pathological 20 ms loop: breaks OFDM on purpose, for tests
  bool clipping = true;       // power amplifier limit
  double tx_level_db = 0.0;   // user's output volume, dB relative to full scale
  int quantize_bits = 16;
  bool air_absorption = true;
  uint64_t seed = 1234;
  std::string label;
};

class AcousticChannel {
 public:
  explicit AcousticChannel(const ChannelConfig& cfg);
  // Processes a block of transmitted audio. The channel is a *streaming* model: every filter,
  // envelope and noise generator keeps its state between calls, so the result does not depend on
  // where the caller's audio-callback boundaries happen to fall. `out` can be slightly shorter or
  // longer than `n` because of the sample-rate-offset resampler.
  void process(const float* in, size_t n, std::vector<float>* out);
  // Clear all streaming state (new session / new test). Without this the previous run's filter
  // and AGC state would leak into the next.
  void reset();
  double path_loss_db() const { return loss_db_; }
  std::string describe() const;
  const ChannelConfig& config() const { return cfg_; }
  // Noise-only helper for calibration tests.
  void noise_only(size_t n, std::vector<float>* out);

 private:
  void apply_device(const float* in, size_t n, std::vector<float>* out, bool mic_side);
  ChannelConfig cfg_;
  double loss_db_ = 0.0;
  std::vector<Biquad> spk_, mic_;
  double hf_slope_db_at_20k_ = 0.0;
  double gate_dbfs_ = -70.0;
  // ---- streaming state (see process()) ----
  std::vector<float> mp_tail_;   // pre-multipath samples before the current block
  std::vector<float> hist3_;     // interpolator history for the SRO resampler
  double absorb_acc_ = 0.0;      // one-pole HF absorption memory
  double agc_gain_ = 1.0;        // AGC loop gain, persists across blocks
  double agc_env_ = 1e-9;        // AGC envelope
  double sig_level_ = 1e-9;      // long-term signal RMS: what the SNR target is referred to
  double sro_pos_ = 0.0;         // continuous read position through the resampled stream
  double pn_b0_ = 0.0, pn_b1_ = 0.0, pn_b2_ = 0.0;  // pink-noise generator state
  uint64_t pn_s_ = 0;            // noise RNG state; 0 means "not initialised yet"
  double pn_gauss();
  void noise_stream(size_t n, std::vector<float>* out);
};

// Convenience: full path render->channel->decode, used by every PHY benchmark. Returns
// the same status codes as Modem::rx_capture and fills the BER counters.
struct PhyTestResult {
  bool decoded = false;
  bool payload_match = false;
  double raw_ber = 0.0, fec_ber = 0.0;
  double snr_db = 0.0, rssi_dbfs = -120.0;
  double burst_ms = 0.0, payload_bps = 0.0;
  double evec_pct = 0.0;
  int cfo_est_hz = 0;
  double noise_dbfs = -120.0;
  int bins_bad = 0;        // subcarriers the receiver declared unusable and soft-punctured
  bool clipped = false;    // saturation seen in the capture window
  int status = 0;          // the rx_capture code, kept even when this helper returns OK
  int tx_bits = 0, rx_bits = 0;  // coded bits expected / recovered (0 rx_bits = no decode)
  std::string error;
};
int ni_phy_test(const class Modem& tx_modem, const class Modem& rx_modem,
                const uint8_t* payload, size_t n, const ChannelConfig& ch,
                PhyTestResult* out);

}  // namespace ni
#endif
