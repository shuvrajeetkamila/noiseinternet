// modem.h — the acoustic PHY: byte PDU <-> PCM waveform, both directions.
//
// burst = [S0][S1][R][ D... ] where S0==S1 are the sync symbols, R a known all-ones
// reference symbol (per-bin channel + phase reference), then the data symbols. Reference
// symbols are re-inserted every `ref_interval` data symbols so long robust-mode bursts can
// re-lock and track slow channel drift.
//
// FEC chain (transmit order):
//   pdu_len(16b) | MAC frame  ->  scramble  ->  rate-1/2 K=7 conv encode (+6 flush bits)
//   ->  block interleave (rows = data symbols)  ->  symbol mapping  ->  OFDM
#ifndef NI_MODEM_H
#define NI_MODEM_H
#include <string>
#include <vector>
#include "ni/common.h"
#include "ni/error_correction.h"
#include "ni/fft.h"
#include "ni/frame_codec.h"
#include "ni/synchronization.h"

namespace ni {

struct ModemConfig {
  double sample_rate = 48000.0;
  int profile_id = 2;             // DBPSK-MID (robust default)
  uint16_t session_id = 0;
  double tx_amplitude = 0.85;     // peak of the rendered burst, full-scale fraction
  double sync_thresh = 0.55;
  int max_payload = NI_MAX_PAYLOAD;  // further clamped by the profile's airtime bound
  int debug_soft = 0;                // when non-zero, RxResult carries hard/soft bit dumps
  std::string label;              // free text, appears in reports
};

struct PhyStats {
  double snr_db = 0.0;
  double rssi_dbfs = -120.0;
  double noise_dbfs = -120.0;
  double cfo_hz = 0.0;
  double sync_peak = 0.0;
  double evec = 0.0;          // error vector magnitude, % of average symbol power
  int bins_used = 0;
  int bins_bad = 0;           // bins whose |H| or SNR was unusable
  int symbols = 0;
  double duration_ms = 0.0;
  double noise_var = 0.0;      // per-bin noise power (drives LLR scaling and SNR)
  double crc_fail_rate = 0.0;  // filled in by the session from its counters
  double raw_ber = 0.0;
  double decoded_ber = 0.0;
  int fec_corrections = 0;
  bool clipped = false;
};

// Result of one receive attempt.
struct RxResult {
  int status = NI_OK;
  std::vector<uint8_t> pdu;  // the decoded MAC frame (no pdu_len prefix)
  BurstPlan plan;
  PhyStats stats;
  Frame frame;
  size_t consumed = 0;  // samples used from the input buffer
  size_t start_pos = 0;  // where the burst began inside the supplied capture window
  double cfo = 0.0;     // measured carrier offset of this burst, Hz
  // Debug taps so the benchmark can report BOTH raw BER (before FEC) and corrected BER:
  std::vector<double> desc_llr;    // the 24 accumulated descriptor LLRs (diagnostics/UI)
  double sync_h2 = 0.0;            // mean |H|^2 of the sync-derived channel estimate
  std::vector<uint8_t> hard_coded;  // hard decision of every coded bit, interleaved order
  std::vector<uint8_t> info_bits;   // decoded information bits after descrambling (the PDU bits),
                              // filled even when the frame check fails, so BER is always measurable
  std::vector<float> h_re, h_im;    // per-bin channel estimate of the last symbol
};

class Modem {
 public:
  explicit Modem(const ModemConfig& cfg);
  ~Modem();
  bool ok() const { return ok_; }
  const std::string& error() const { return err_; }

  const ModemConfig& config() const { return cfg_; }
  const Profile& profile() const { return *profile_; }
  const CarrierMap& map() const { return map_; }
  double sample_rate() const { return cfg_.sample_rate; }
  int max_payload() const { return max_payload_; }
  int cp() const { return cp_; }  // this profile's cyclic-prefix length in samples

  // ---- transmit side ---------------------------------------------------------
  // Renders a burst carrying pdu[0..n). pdu already contains the 2-byte pdu_len prefix
  // produced by ni_phy_wrap(). Waveform is mono float, full scale +-1, written to *out;
  // returns NI_OK on success or a negative NI_ERR_* (out->size() is the sample count).
  int tx_render(const uint8_t* pdu, size_t n, std::vector<float>* out) const;
  int tx_bits_for(size_t pdu_bytes) const { return (2 + (int)pdu_bytes) * 8; }
  BurstPlan plan(size_t payload_bytes) const;
  // Convenience: render a burst from a MAC frame body (adds pdu_len).
  int tx_render_frame(const uint8_t* frame, size_t n, std::vector<float>* out) const;
  const std::vector<float>& sync_waveform() const { return sync_; }  // one S0 (no CP)

  // ---- receive side ---------------------------------------------------------
  // Look for a burst anywhere in x[0..n). Returns NI_ERR_NOFRAME when there is nothing.
  int rx_detect(const float* x, size_t n, SyncResult* out) const;
  // Full demodulation of a buffer that starts at a detected sync position.
  int rx_decode(const float* x, size_t n, size_t start, RxResult* out) const;
  // Detect-and-decode convenience for one-shot files.
  int rx_capture(const float* x, size_t n, RxResult* out) const;
  int burst_samples_for(size_t payload_bytes) const;

  // ---- symbol mapping (exposed for the test vectors / Python cross-check) ----
  // map_bits_to_symbols: n_data symbols of map.n_data units, each unit `bits` bits.
  // The returned array holds constellation indices in [0, 2^bits) per unit per symbol,
  // except FSK where the index is the bin offset (0 or 1) inside the pair.
  int map_bits_to_symbols(const uint8_t* coded, size_t n_coded, std::vector<int>* idx) const;
  int max_symbols() const { return profile_->max_burst_symbols; }

 private:
  void build_sync();

  ModemConfig cfg_;
  const Profile* profile_ = nullptr;
  CarrierMap map_{};
  Fft fft_{};
  bool ok_ = false;
  std::string err_;
  int max_payload_ = 0;
  int cp_ = NI_CP;
  std::vector<float> sync_;       // N samples, no CP
  std::vector<float> sync_cp_;    // N+CP with taper (what the transmitter actually emits)
  std::vector<int8_t> pn_;        // per-bin BPSK pattern for the sync symbol
  std::vector<float> norm_;       // per-bin constellation scaling
  double unit_energy_ = 1.0;
};

// Phase rotation between the two identical sync symbols, which are L = nfft + cp apart. The
// estimate is reported for the UI and for the calibrator; it is deliberately NOT used to rotate
// the burst, because the impairment this pipeline actually has is a sample-clock ratio (a time
// scale, corrected by choosing matching device rates) rather than a mixer offset. Over a burst
// of at most a few hundred milliseconds the two differ by well under a degree of rotation.
double ni_cfo_estimate(const struct Fft& f, const float* x, size_t n, size_t pos, int k0,
                       int k1, double fs, int cp = NI_CP);

// PHY helpers: 2-byte big-endian length prefix, and the reverse with validation.
int ni_phy_wrap(const uint8_t* frame, size_t n, std::vector<uint8_t>* pdu);
int ni_phy_unwrap(const uint8_t* pdu, size_t n, const uint8_t** frame, size_t* frame_len);

// Peak/RMS helpers shared by the audio backends and the calibration code.
double ni_peak(const float* x, size_t n);
double ni_rms(const float* x, size_t n);
int ni_clip_count(const float* x, size_t n, double threshold = 0.999);
void ni_scale(float* x, size_t n, double g);

}  // namespace ni
#endif
