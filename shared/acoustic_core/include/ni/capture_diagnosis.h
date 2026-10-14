// capture_diagnosis.h — "what did the microphone actually record?", answered in words.
//
// Why this exists: the second and third field reports both ended in the same place - the engine
// counted bursts but decoded nothing - and nothing in the product could say *why*. Every possible
// cause (clipping, a level that collapses, a device that resamples, Windows' echo cancellation
// eating the signal, plain silence) looks identical from a status panel and needs a different fix.
//
// This is deliberately measurement-only: it reports what the buffer contains and what the modem can
// do with it, and it never guesses at a cause it cannot see. The one thing it can prove is the
// strongest signal there is - "this recording decodes" - because that removes the microphone, the
// speaker and the room from the list of suspects in one line.
#ifndef NI_CAPTURE_DIAGNOSIS_H
#define NI_CAPTURE_DIAGNOSIS_H
#include <stddef.h>
#include <string>
namespace ni {

struct CaptureDiagnosis {
  // levels, all in dBFS, measured over the buffer as a whole and over its loud parts
  double peak_dbfs = -120.0;
  double rms_dbfs = -120.0;
  double noise_dbfs = -120.0;    // the quiet 10 % of 20 ms frames: what the microphone hears between bursts
  double burst_dbfs = -120.0;    // the loud 10 %: what it hears during one
  double span_db = 0.0;          // burst_dbfs - noise_dbfs
  double clip_pct = 0.0;         // share of samples pinned near full scale
  double duty_pct = 0.0;         // share of frames that look like signal rather than room
  // what the modem makes of it
  int frames_decoded = 0;        // bursts in this buffer that decoded at this profile
  double decoded_snr_db = -120.0;
  bool decodes = false;
  // is the sound where the profile says it should be? measured from the -10 dB edges of the
  // in-band spectrum, compared with the profile's own band
  double band_low_hz = 0.0, band_high_hz = 0.0;
  double shift_ppm = 0.0;        // + = the recorded sound sits higher than the profile's band
  std::string verdict;           // one sentence for the person holding the device
  std::string to_json() const;
};

// x[0..n) is mono float PCM at fs. profile_id selects the band and the demodulator.
int ni_diagnose_capture(const float* x, size_t n, double fs, int profile_id, CaptureDiagnosis* out);

}  // namespace ni
#endif
