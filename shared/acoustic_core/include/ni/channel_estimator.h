// channel_estimator.h — per-subcarrier SNR tracking and adaptive modulation selection,
// plus the calibration state machine (prompt 5.3.4 "adaptive", prompt 6).
//
// The estimator is fed one vector of per-bin |H[k]|^2 and noise power per burst. It keeps an
// EWMA per bin, derives a band SNR, and proposes the fastest profile whose predicted frame
// success rate stays above a target. Selection is hysteretic (needs N consistent samples to
// move up, 1 to move down) so the link cannot oscillate.
#ifndef NI_CHANNEL_ESTIMATOR_H
#define NI_CHANNEL_ESTIMATOR_H
#include <string>
#include <vector>
#include "ni/common.h"

namespace ni {

struct BinQuality {
  double h2 = 0.0;     // |H|^2 average
  double snr_db = 0.0;
  int samples = 0;
};

class ChannelEstimator {
 public:
  explicit ChannelEstimator(double ewma_alpha = 0.30, int max_bins = 128);
  void reset();
  int n_bins() const { return n_bins_; }
  void observe(const float* h_re, const float* h_im, double noise_var, int n);
  void observe_hard_failure();  // a burst that never locked: push the whole band down
  double band_snr_db() const;   // SNR over bins with usable |H|
  double band_gain_db() const;  // mean 20log10|H| of used bins (channel attenuation)
  const std::vector<BinQuality>& bins() const { return bins_; }
  int bad_bins(double snr_floor_db) const;
  double coherence() const;     // 0..1, consistency of recent |H| estimates
  int samples() const { return samples_; }

 private:
  double alpha_;
  int max_bins_;
  int n_bins_ = 0;
  int samples_ = 0;
  int fails_ = 0;
  std::vector<BinQuality> bins_;
  double last_band_snr_ = -60.0;
};

// ------------------------------------------------------------------ adaptive controller
class RateAdaptation {
 public:
  struct Decision {
    int profile_id = 0;
    double predicted_snr_db = 0.0;
    double predicted_frame_success = 0.0;
    bool changed = false;
    std::string reason;
  };
  explicit RateAdaptation(int start_profile = 2, double target_success = 0.95);
  // Feed measured results; returns the profile that should be used next.
  Decision update(const ChannelEstimator& est, double frame_success, double snr_db,
                  int active_profile);
  int held() const { return held_; }
  int upvotes() const { return up_; }
  const std::vector<int>& history() const { return hist_; }

 private:
  int held_ = 0;
  int up_ = 0;
  double target_ = 0.95;
  std::vector<int> hist_;
};

// ------------------------------------------------------------------ calibration
// The CALIBRATE button runs this. It never starts a tunnel (prompt 6 rule).
class Calibrator {
 public:
  struct BandResult {
    int band_id = 0;
    int profile_id = 0;
    std::string name;
    double snr_db = 0.0;
    double sig_dbfs = -120.0, noise_dbfs = -120.0;
    double frame_success = 0.0;
    double raw_ber = 0.0, fec_ber = 0.0;
    double payload_bps = 0.0;
    bool clipped = false;
    bool usable = false;
    std::string note;
  };
  struct Report {
    bool hardware_ok = false;
    bool input_ok = false, output_ok = false;
    bool permission_ok = false;
    double sample_rate = 0.0;
    int device_frames_min = 0;
    double noise_floor_dbfs = -120.0;
    double max_output_gain = 1.0;
    std::vector<BandResult> bands;
    int best_profile = -1;
    int suggested_mtu = 0;
    std::string summary;
    std::string to_json() const;
  };

  // Which profiles should be exercised at all, given what the device can reproduce.
  static std::vector<int> candidate_profiles(bool ultrasonic_supported);
  static void evaluate(const std::vector<BandResult>& measured, Report* out);
};

}  // namespace ni
#endif
