#include "ni/capture_diagnosis.h"
#include "ni/common.h"
#include "ni/fft.h"
#include "ni/modem.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace ni {
namespace {

double dbfs(double lin) { return 20.0 * log10(lin > 1e-12 ? lin : 1e-12); }

// One 20 ms frame's RMS, in dBFS.
double frame_dbfs(const float* x, size_t n) {
  double e = 0.0;
  for (size_t i = 0; i < n; ++i) e += (double)x[i] * (double)x[i];
  return dbfs(n ? std::sqrt(e / (double)n) : 0.0);
}

}  // namespace

int ni_diagnose_capture(const float* x, size_t n, double fs, int profile_id, CaptureDiagnosis* out) {
  if (!x || !out || n < 64 || fs <= 0) return NI_ERR_PARAM;
  const Profile& prof = ni_profile(profile_id);
  *out = CaptureDiagnosis();

  // ---- levels ------------------------------------------------------------------
  double peak = 0.0;
  size_t clipped = 0;
  for (size_t i = 0; i < n; ++i) {
    double a = std::fabs((double)x[i]);
    if (a > peak) peak = a;
    if (a >= 0.999) clipped++;
  }
  out->peak_dbfs = dbfs(peak);
  out->rms_dbfs = frame_dbfs(x, n);
  out->clip_pct = 100.0 * (double)clipped / (double)n;

  size_t frame = (size_t)(fs * 0.02);          // 20 ms
  if (frame < 32) frame = 32;
  std::vector<double> frames;
  for (size_t off = 0; off + frame <= n; off += frame) frames.push_back(frame_dbfs(x + off, frame));
  if (frames.size() >= 8) {
    std::vector<double> sorted(frames);
    std::sort(sorted.begin(), sorted.end());
    size_t lo = sorted.size() / 10, hi = sorted.size() - 1 - sorted.size() / 10;
    out->noise_dbfs = sorted[lo];
    out->burst_dbfs = sorted[hi];
    out->span_db = out->burst_dbfs - out->noise_dbfs;
    // "duty" is the share of frames that stand at least 6 dB above the quiet part. For the modem's
    // half-duplex bursts this lands near the real airtime fraction; for steady noise it lands near
    // zero, and for a constant tone (or a clipped mess) near 100.
    double thresh = out->noise_dbfs + 6.0;
    size_t loud = 0;
    for (double f : frames) if (f > thresh) loud++;
    out->duty_pct = 100.0 * (double)loud / (double)frames.size();
  } else {
    out->noise_dbfs = out->burst_dbfs = out->rms_dbfs;
    out->span_db = 0.0;
  }

  // ---- can the modem read it? --------------------------------------------------
  {
    ModemConfig mc;
    mc.sample_rate = fs;
    mc.profile_id = profile_id;
    Modem m(mc);
    if (m.ok()) {
      RxResult r;
      int rc = m.rx_capture(x, n, &r);
      if (rc == NI_OK) {
        out->frames_decoded = 1;
        out->decodes = true;
        out->decoded_snr_db = r.stats.snr_db;
      }
    }
  }

  // ---- is the sound where the profile says it should be? ------------------------
  // The burst occupies the profile's band. A device that resamples shifts it by exactly its rate
  // ratio, and a resampled burst is perfectly audible and completely undecodable - so the band edges
  // are compared with the profile's own, from an averaged spectrum of the loud frames.
  {
    const int N = NI_N_FFT;
    Fft f;
    if (ni_fft_init(&f, N)) {
      std::vector<float> acc((size_t)N, 0.f), win((size_t)N);
      for (int i = 0; i < N; ++i)
        // NI_PI, not M_PI: M_PI is a GNU extension that MSVC does not define without _USE_MATH_DEFINES,
        // which is exactly the sort of difference that turns into a red Windows job (it did).
        win[(size_t)i] = (float)(0.5 - 0.5 * cos(2.0 * NI_PI * i / (N - 1)));   // Hann
      int used = 0;
      // Which frames to average: the loudest ones, but never more than a quarter of the buffer, so a
      // recording that is *all* burst (a rendered file, a loopback capture) is still measured instead of
      // being skipped for having no quiet frames to compare against.
      std::vector<std::pair<double, size_t>> ranked;
      for (size_t off = 0; off + (size_t)N <= n; off += (size_t)N / 2)
        ranked.push_back(std::make_pair(frame_dbfs(x + off, (size_t)N), off));
      std::sort(ranked.begin(), ranked.end(),
                [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) {
                  return a.first > b.first;
                });
      size_t take = std::min<size_t>(40, std::max<size_t>(1, ranked.size() / 4));
      // ...but a frame that is at the noise floor is not signal, however loud the rest is: require it to
      // stand above the room by 3 dB, and if nothing does, say so by leaving the band empty.
      // Two conditions, because a recording may or may not have quiet parts in it: at least 3 dB above
      // the room when there is a room, and never more than 12 dB below this recording's own loudest
      // frame. (A file that is nothing but a burst has no room to measure, and requiring one would make
      // the band unknown for exactly the recording someone would be looking at.)
      // The floor to measure *against*. A recording with quiet parts has a room to compare with; one
      // that is nothing but a burst (a rendered file, a loopback capture) does not, and pretending its
      // own 10th percentile is a room would make the band unknown for exactly the recording someone
      // would be looking at.
      double floor_db = ranked.empty() ? -120.0
                                       : (out->span_db >= 6.0 ? out->noise_dbfs
                                                              : ranked[0].first - 20.0);
      double hi_thresh = std::max(floor_db + 3.0, ranked.empty() ? -120.0 : ranked[0].first - 12.0);
      for (size_t k = 0; k < take && used < 40; ++k) {
        if (ranked[k].first < hi_thresh) break;
        size_t off = ranked[k].second;
        std::vector<float> re((size_t)N);
        for (int i = 0; i < N; ++i) re[(size_t)i] = x[off + (size_t)i] * win[(size_t)i];
        // Interleaved re/im, so the real sample goes at 2*i. (Writing them contiguously is the
        // classic way to get a spectrum that looks like noise and still passes a round-trip test:
        // the imaginary parts end up holding the next sample's real value.)
        std::vector<float> buf((size_t)N * 2, 0.f);
        for (int i = 0; i < N; ++i) buf[(size_t)(2 * i)] = re[(size_t)i];
        ni_fft_forward(&f, &buf[0]);
        for (int i = 0; i < N; ++i) {
          double p = (double)buf[(size_t)(2 * i)] * buf[(size_t)(2 * i)] +
                     (double)buf[(size_t)(2 * i + 1)] * buf[(size_t)(2 * i + 1)];
          acc[(size_t)i] += (float)p;
        }
        used++;
      }
      ni_fft_free(&f);
      if (used > 0) {
        double bin_hz = fs / (double)N;
        double pk = 0.0;
        int pk_bin = (int)(prof.f_low / bin_hz);
        for (int i = 1; i < N / 2; ++i)
          if (acc[(size_t)i] > pk) { pk = acc[(size_t)i]; pk_bin = i; }
        const double rel = 0.316;   // -10 dB in power
        double thr = pk * rel;
        int lo = pk_bin, hi = pk_bin;
        while (lo > 1 && acc[(size_t)(lo - 1)] > thr) lo--;
        while (hi < N / 2 - 1 && acc[(size_t)(hi + 1)] > thr) hi++;
        out->band_low_hz = lo * bin_hz;
        out->band_high_hz = hi * bin_hz;
        double mid_meas = 0.5 * (out->band_low_hz + out->band_high_hz);
        double mid_prof = 0.5 * (prof.f_low + prof.f_high);
        if (mid_prof > 0 && mid_meas > 0) out->shift_ppm = 1e6 * (mid_meas / mid_prof - 1.0);
      }
    }
  }

  // ---- the sentence -------------------------------------------------------------
  char b[512];
  if (out->rms_dbfs <= -119.0 && out->peak_dbfs <= -100.0) {
    out->verdict =
        "NOTHING AT ALL: the recording is digital silence - the microphone delivered no samples "
        "(a refused format, a held microphone, or a muted/disabled input), not a quiet room.";
  } else if (out->decodes) {
    snprintf(b, sizeof b,
             "THIS RECORDING DECODES: the modem reads a burst out of it at %s (%.1f dB SNR). The "
             "microphone, the speaker and the way the sound travelled are therefore good, and the "
             "problem is elsewhere - most often the other device, or the two ends not agreeing on "
             "profile/session/passphrase.",
             prof.name, out->decoded_snr_db);
    out->verdict = b;
  } else if (out->peak_dbfs < -60.0) {
    snprintf(b, sizeof b,
             "FAR TOO QUIET: the loudest thing in this recording is %.0f dBFS, near the noise floor "
             "of the input. Move the devices closer, raise the sending volume, and check that the "
             "microphone selected is the one facing the speaker. (Room %.0f dBFS, loudest %.0f dBFS.)",
             out->peak_dbfs, out->noise_dbfs, out->burst_dbfs);
    out->verdict = b;
  } else if (out->clip_pct > 0.5) {
    snprintf(b, sizeof b,
             "CLIPPED: %.1f %% of the samples in this recording are pinned at full scale. A burst that "
             "clips is a burst whose bits are gone - lower the speaker volume, or move the devices "
             "further apart, and repeat. (Loudest %.0f dBFS.)",
             out->clip_pct, out->peak_dbfs);
    out->verdict = b;
  } else if (std::fabs(out->shift_ppm) > 1500.0 && out->band_low_hz > 0.0 &&
             (out->band_high_hz - out->band_low_hz) >=
                 0.4 * ((double)prof.f_high - (double)prof.f_low)) {
    snprintf(b, sizeof b,
             "THE SOUND IS SHIFTED: the burst sits at %.0f-%.0f Hz where %s expects %.0f-%.0f Hz - a "
             "ratio of %.2f %%. That is a device resampling the audio (a 44.1 kHz device asked for "
             "48 kHz, or a resampler in the path), and a resampled burst is audible but never "
             "decodable. Set both devices to 48000 Hz, or use a device whose own rate is 48000 Hz.",
             out->band_low_hz, out->band_high_hz, prof.name, prof.f_low, prof.f_high,
             100.0 * (1.0 + out->shift_ppm / 1e6));
    out->verdict = b;
  } else if (out->span_db < 6.0) {
    snprintf(b, sizeof b,
             "NO SEPARATION: this recording is %.0f dB loud throughout with no quiet parts, so there "
             "is no burst in it to decode - either nothing was transmitted during it, or the input is "
             "saturated with something else. (Loudest %.0f dBFS, room %.0f dBFS.)",
             out->span_db, out->burst_dbfs, out->noise_dbfs);
    out->verdict = b;
  } else {
    snprintf(b, sizeof b,
             "HEARD BUT NOT READABLE, and nothing obvious in the levels: the recording has real "
             "structure (%.0f dB above the room, %.0f %% of the time, band %.0f-%.0f Hz) and the "
             "modem still cannot read a burst out of it. The remaining causes are the ones that "
             "change the shape of the sound rather than its level: the operating system's own "
             "microphone processing (Windows 'audio enhancements', echo cancellation or noise "
             "suppression subtract exactly the kind of signal this is), a microphone array that "
             "beam-forms towards speech, or a device that resamples with a filter that smears the "
             "symbols. Try turning every microphone enhancement off, then repeat.",
             out->span_db, out->duty_pct, out->band_low_hz, out->band_high_hz);
    out->verdict = b;
  }
  return NI_OK;
}

std::string CaptureDiagnosis::to_json() const {
  std::ostringstream o;
  o << "{\"peak_dbfs\": " << peak_dbfs << ", \"rms_dbfs\": " << rms_dbfs
    << ", \"noise_dbfs\": " << noise_dbfs << ", \"burst_dbfs\": " << burst_dbfs
    << ", \"span_db\": " << span_db << ", \"clip_pct\": " << clip_pct
    << ", \"duty_pct\": " << duty_pct << ", \"frames_decoded\": " << frames_decoded
    << ", \"decoded_snr_db\": " << decoded_snr_db << ", \"decodes\": "
    << (decodes ? "true" : "false") << ", \"band_low_hz\": " << band_low_hz
    << ", \"band_high_hz\": " << band_high_hz << ", \"shift_ppm\": " << shift_ppm
    << ", \"verdict\": \"" << verdict << "\"}";
  return o.str();
}

}  // namespace ni
