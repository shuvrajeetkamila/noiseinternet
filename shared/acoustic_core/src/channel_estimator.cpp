// channel_estimator.cpp — per-subcarrier SNR tracking, adaptive modulation, calibration.
#include "ni/channel_estimator.h"
#include <math.h>
#include <sstream>

namespace ni {

ChannelEstimator::ChannelEstimator(double alpha, int max_bins)
    : alpha_(alpha), max_bins_(max_bins) {}

void ChannelEstimator::reset() {
  bins_.clear();
  n_bins_ = 0;
  samples_ = 0;
  fails_ = 0;
  last_band_snr_ = -60.0;
}

void ChannelEstimator::observe(const float* h_re, const float* h_im, double noise_var, int n) {
  if (n <= 0) return;
  if (n > max_bins_) n = max_bins_;
  if (n_bins_ != n) {
    bins_.assign((size_t)n, BinQuality());
    n_bins_ = n;
  }
  if (noise_var <= 0) noise_var = 1e-12;
  for (int i = 0; i < n; i++) {
    double h2 = (double)h_re[i] * h_re[i] + (double)h_im[i] * h_im[i];
    BinQuality& b = bins_[(size_t)i];
    if (b.samples == 0) {
      b.h2 = h2;
      b.snr_db = 10.0 * log10((h2 + 1e-18) / noise_var);
    } else {
      b.h2 = alpha_ * h2 + (1.0 - alpha_) * b.h2;
      double s = 10.0 * log10((b.h2 + 1e-18) / noise_var);
      b.snr_db = alpha_ * s + (1.0 - alpha_) * b.snr_db;
    }
    b.samples++;
  }
  double acc = 0;
  for (auto& b : bins_) acc += b.snr_db;
  last_band_snr_ = bins_.empty() ? -60.0 : acc / (double)bins_.size();
  samples_++;
}

void ChannelEstimator::observe_hard_failure() {
  fails_++;
  for (auto& b : bins_) {
    b.snr_db -= 6.0;
    b.h2 *= 0.25;
  }
  // Distrust the estimate after a hard failure - but with a floor. The penalty used to be applied
  // once per CRC failure with no limit, and a receiver that cannot read its peer at all (wrong
  // profile, no sync) racks up hundreds of failures per second: the reported SNR ran away to
  // -4659 dB, which then fed the receiver squelch and the adaptation decision and made recovery
  // impossible. -45 dB is below anything this modem can demodulate, so stopping there is safe.
  if (last_band_snr_ > -45.0) last_band_snr_ = -45.0;
  else last_band_snr_ -= 6.0;
  if (last_band_snr_ < -45.0) last_band_snr_ = -45.0;
}

double ChannelEstimator::band_snr_db() const { return last_band_snr_; }

double ChannelEstimator::band_gain_db() const {
  if (bins_.empty()) return 0.0;
  double a = 0;
  int c = 0;
  for (auto& b : bins_)
    if (b.h2 > 1e-18) {
      a += 20.0 * log10(sqrt(b.h2));
      c++;
    }
  return c ? a / (double)c : 0.0;
}

int ChannelEstimator::bad_bins(double floor_db) const {
  int n = 0;
  for (auto& b : bins_)
    if (b.snr_db < floor_db) n++;
  return n;
}

double ChannelEstimator::coherence() const {
  if (bins_.size() < 4) return 1.0;
  double mean = 0;
  for (auto& b : bins_) mean += b.h2;
  mean /= (double)bins_.size();
  if (mean <= 1e-18) return 0.0;
  int within = 0;
  double lo = mean * 0.5, hi = mean * 2.0;  // +-3 dB
  for (auto& b : bins_)
    if (b.h2 >= lo && b.h2 <= hi) within++;
  return (double)within / (double)bins_.size();
}

// ---------------------------------------------------------------- rate adaptation
namespace {
// Required in-band SNR (dB) for >=95% frame success with the rate-1/2 inner code, measured
// on this implementation (docs/TEST_RESULTS.md table "modem_channel_sweep"); these are the
// thresholds the controller uses, not theoretical ones.
double snr_required_for(int profile_id) {
  const Profile& p = ni_profile(profile_id);
  switch (p.mod) {
    case NI_MOD_FSK2: return 9.0;
    case NI_MOD_DBPSK: return p.fec_rate_den == 2 ? 4.0 : 9.5;
    case NI_MOD_DQPSK: return 8.0;
    case NI_MOD_QAM16: return 13.0;
    case NI_MOD_QAM64: return 19.0;
    default: return 12.0;
  }
}
double payload_rank(int profile_id) {
  // rough ordering high -> low preference, used as a tie-breaker
  static const int order[] = {5, 13, 12, 6, 4, 3, 2, 11, 10, 1, 9, 8, 7, 0};
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
    if (order[i] == profile_id) return (double)(100 - i);
  return 0.0;
}
}  // namespace

RateAdaptation::RateAdaptation(int start_profile, double target)
    : held_(start_profile), up_(0), target_(target) {
  hist_.push_back(start_profile);
}

RateAdaptation::Decision RateAdaptation::update(const ChannelEstimator& est, double frame_success,
                                                double snr_db, int active) {
  Decision d;
  d.predicted_snr_db = snr_db;
  d.profile_id = active;
  int best = active;
  double best_score = -1e30;
  for (int pid = 0; pid < ni_profile_count(); pid++) {
    const Profile& p = ni_profile(pid);
    if (p.mod == NI_MOD_FSK2 && pid != 2) {
      // FSK profiles are reserved for acquisition; never selected as the steady-state mode
    }
    double req = snr_required_for(pid);
    double margin = snr_db - req;
    double psucc = 1.0 / (1.0 + exp(-1.35 * margin));
    if (p.mod == NI_MOD_FSK2) psucc *= 0.35;  // far too slow for bulk use
    if (pid == 7 || pid == 8) psucc *= 0.5;   // ultrasonic: hardware-dependent, penalised
    if (psucc < target_) continue;
    double score = payload_rank(pid) * (0.5 + 0.5 * psucc);
    if (score > best_score) {
      best_score = score;
      best = pid;
    }
  }
  d.predicted_frame_success = best == active ? 1.0 : 0.0;
  std::ostringstream os;
  if (best != held_) {
    if (best > held_ && frame_success < 0.9) {
      // degradation: act immediately
      up_ = 0;
      held_ = best;
      d.changed = true;
      os << "downgrade -> " << ni_profile(best).name << " (success " << (int)(frame_success * 100)
         << "%, snr " << (int)snr_db << " dB)";
    } else if (best > held_) {
      up_++;
      if (up_ >= 3) {
        held_ = best;
        up_ = 0;
        d.changed = true;
        os << "upgrade -> " << ni_profile(best).name << " after 3 consistent samples";
      } else {
        os << "upgrade candidate held (" << up_ << "/3)";
      }
    } else {
      up_ = 0;
      held_ = best;
      d.changed = true;
      os << "fallback -> " << ni_profile(best).name;
    }
  } else {
    up_ = 0;
    os << "hold " << ni_profile(held_).name;
  }
  d.profile_id = held_;
  d.reason = os.str();
  hist_.push_back(held_);
  if (hist_.size() > 512) hist_.erase(hist_.begin(), hist_.begin() + 256);
  (void)est;
  return d;
}

// ---------------------------------------------------------------- calibration
std::vector<int> Calibrator::candidate_profiles(bool ultrasound_ok) {
  std::vector<int> v = {2, 3, 4, 5, 9, 1};  // robust -> fast, plus the no-FEC reference
  if (ultrasound_ok) {
    v.push_back(7);
    v.push_back(8);
  }
  return v;
}

void Calibrator::evaluate(const std::vector<BandResult>& measured, Report* out) {
  out->bands = measured;
  out->best_profile = -1;
  double best_bps = -1.0;
  int usable_count = 0;
  std::ostringstream sum;
  for (auto& b : measured) {
    bool usable = b.frame_success >= 0.95 && b.snr_db >= 4.0 && !b.clipped;
    const_cast<BandResult&>(b).usable = usable;
    if (usable) {
      usable_count++;
      double eff = b.payload_bps * (b.fec_ber < 1e-5 ? 1.0 : 0.5);
      if (b.profile_id == 9) eff *= 0.5;  // no FEC: unreliable for real traffic
      if (eff > best_bps) {
        best_bps = eff;
        out->best_profile = b.profile_id;
      }
    }
  }
  if (out->best_profile < 0 && !measured.empty()) out->best_profile = 2;
  sum << measured.size() << " band(s) probed, " << usable_count << " usable";
  if (out->best_profile >= 0)
    sum << "; conservative choice " << ni_profile(out->best_profile).name;
  else
    sum << "; NO USABLE BAND - acoustic link cannot be established on this hardware";
  out->summary = sum.str();
  out->suggested_mtu = 0;
  if (out->best_profile >= 0) {
    int mp = ni_profile_max_payload(ni_profile(out->best_profile), out->sample_rate);
    out->suggested_mtu = mp > 224 ? 224 : mp;
  }
}

std::string Calibrator::Report::to_json() const {
  std::ostringstream o;
  o << "{\n \"hardware_ok\": " << (hardware_ok ? "true" : "false") << ",\n"
    << " \"input_ok\": " << (input_ok ? "true" : "false") << ",\n"
    << " \"output_ok\": " << (output_ok ? "true" : "false") << ",\n"
    << " \"permission_ok\": " << (permission_ok ? "true" : "false") << ",\n"
    << " \"sample_rate\": " << sample_rate << ",\n"
    << " \"device_frames_min\": " << device_frames_min << ",\n"
    << " \"noise_floor_dbfs\": " << noise_floor_dbfs << ",\n"
    << " \"max_output_gain\": " << max_output_gain << ",\n"
    << " \"best_profile\": " << best_profile << ",\n"
    << " \"best_profile_name\": \""
    << (best_profile >= 0 && best_profile < ni_profile_count() ? ni_profile(best_profile).name : "")
    << "\",\n"
    << " \"suggested_mtu\": " << suggested_mtu << ",\n"
    << " \"summary\": \"" << summary << "\",\n"
    << " \"bands\": [";
  for (size_t i = 0; i < bands.size(); i++) {
    const BandResult& b = bands[i];
    if (i) o << ",";
    o << "\n   {\"band_id\": " << b.band_id << ", \"profile_id\": " << b.profile_id
      << ", \"name\": \"" << b.name << "\", \"snr_db\": " << b.snr_db
      << ", \"sig_dbfs\": " << b.sig_dbfs << ", \"noise_dbfs\": " << b.noise_dbfs
      << ", \"frame_success\": " << b.frame_success << ", \"raw_ber\": " << b.raw_ber
      << ", \"fec_ber\": " << b.fec_ber << ", \"payload_bps\": " << b.payload_bps
      << ", \"clipped\": " << (b.clipped ? "true" : "false")
      << ", \"usable\": " << (b.usable ? "true" : "false") << ", \"note\": \"" << b.note << "\"}";
  }
  o << "\n ]\n}\n";
  return o.str();
}

}  // namespace ni
