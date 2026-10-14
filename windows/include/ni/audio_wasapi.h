// audio_wasapi.h — Windows audio backend for the NOISE INTERNET core.
//
// This file is the *only* place the project talks to a sound card directly. The shared core
// (shared/acoustic_core) stays platform-free: it receives float PCM and hands back float PCM, and
// `ni::audio_io.h` documents that contract. Compiling the core with -DNI_WITH_WASAPI makes
// `ni_audio_enumerate()` forward here; without that define the core reports "no audio
// enumeration on this platform build" and the UI must show NOT TESTED instead of a device list.
//
// STATUS: compiled by CI with MSVC (this is where five API misuses were found and fixed: PROPVARIANT,
// GetDevicePeriod, the Initialize session-GUID argument, SPEAKER_* masks, buffer-period units). It has
// never been run against audio hardware, because no build host here has any - so treat it as "compiles and
// is unit-tested", not as "a driver proven on a sound card".
#ifndef NI_AUDIO_WASAPI_H
#define NI_AUDIO_WASAPI_H
#include <string>
#include <vector>
#include "ni/audio_io.h"

namespace ni {

// Filled by `ni_audio_enumerate()` when the core is built with NI_WITH_WASAPI.
int ni_wasapi_enumerate(std::vector<AudioDeviceInfo>* out, std::string* why_not);

// One open stream pair: a render endpoint for the speaker and a capture endpoint for the
// microphone, both in shared mode, both at the device's own float format. Two separate
// IAudioClient objects (rather than one duplex client) because WASAPI duplex needs a
// synchronization event that shared mode does not give you, and because the phone side is
// full-duplex only as an experiment - half duplex just leaves one side idle.
class WasapiEngine {
 public:
  WasapiEngine();
  ~WasapiEngine();
  WasapiEngine(const WasapiEngine&) = delete;
  WasapiEngine& operator=(const WasapiEngine&) = delete;

  // Public because the translation unit that implements the engine opens each side through
  // `WasapiEngine::Impl::Side&`; keeping it private made that a compile error (found by MSVC).
  struct Impl;

  // COM apartment bookkeeping. Call thread_init() on every thread that touches an open engine
  // (the opening thread, the render pump, the capture pump) and thread_shutdown() on the way out.
  static int thread_init();
  static void thread_shutdown();
  static bool com_ready();

  // `render_id` / `capture_id` are endpoint ids from ni_wasapi_enumerate(); empty string selects
  // the device role default. `want_fs` is the rate the engine was asked for; if the device will not
  // do float at that rate, open() returns NI_ERR_NOAUDIO and sets last_error() with the rate the
  // device actually wants, so the caller can reconfigure the node instead of resampling here.
  int open(const std::string& render_id, const std::string& capture_id, double want_fs);
  void close();

  // Frames actually written / read. Both are non-blocking up to `timeout_ms` for render backpressure.
  int render(const float* mono, int frames, int timeout_ms);
  int capture(float* mono, int frames);

  // Frames the render endpoint has accepted but not yet played (IAudioClient::GetCurrentPadding).
  // The half-duplex scheduler times its next burst from when the previous one finished *playing*, so
  // this is the number that keeps an endpoint from talking over its peer's reply. Returns -1 when no
  // render stream is open, which the callers report as "no backlog" rather than inventing one.
  int queued_frames() const;

  double sample_rate() const { return fs_; }
  int render_channels() const { return render_ch_; }
  int capture_channels() const { return capture_ch_; }
  // Hardware buffer size in frames, per endpoint - used to size the pump period.
  int render_buffer_frames() const { return render_buf_; }
  int capture_buffer_frames() const { return capture_buf_; }
  const std::string& last_error() const { return err_; }
  // True when the capture endpoint reported AUDCLNT_BUFFERFLAGS_SILENT on the last read, i.e. the
  // microphone is there but muted/disconnected. Reported to the UI rather than papered over.
  bool capture_silent() const { return capture_silent_; }
  std::string describe() const;

 private:
  Impl* p_ = nullptr;
  double fs_ = 0.0;
  int render_ch_ = 0, capture_ch_ = 0, render_buf_ = 0, capture_buf_ = 0;
  bool capture_silent_ = false;
  // Samples read from the capture endpoint that did not fit in the caller's buffer last time.
  // Nothing is dropped: the remainder is handed over at the start of the next capture() call.
  // `off_` is the read cursor into it, so no element-moving on the hot path.
  std::vector<float> cap_hold_;
  int cap_hold_off_ = 0;
  std::string err_;
};

}  // namespace ni
#endif
