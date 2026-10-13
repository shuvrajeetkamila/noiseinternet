// audio_io.h — audio backends. The engine only ever sees float PCM + a sample rate, so the
// identical code path runs on: WAV files (offline/portable), the UDP PCM pipe (two-process
// tests), WASAPI (Windows), and the JNI-fed AudioRecord/AudioTrack path (Android).
//
// No backend here fakes a device: when nothing is available, open() returns NI_ERR_NOAUDIO
// and the UI must show "hardware NOT TESTED" rather than a green light.
#ifndef NI_AUDIO_IO_H
#define NI_AUDIO_IO_H
#include <string>
#include <vector>
#include "ni/common.h"

namespace ni {

struct AudioDeviceInfo {
  std::string id;
  std::string name;
  bool is_capture = false;   // true = microphone
  double nominal_rate = 48000.0;
  int channels = 1;
  int bits = 32;
  int min_frames = 0;
  bool loopback = false;     // a virtual/loopback device: never acceptable for real tests
  std::string api;           // "WASAPI-shared", "WASAPI-exclusive", "UDP-PCM", "WAV", ...
};

// ---- WAV I/O (always available; used by tests, the render/decode tooling and CI) ----
// Both WAV calls use a "count or negative error" convention: they return the number of
// frames read/written, or a negative NI_ERR_* (check with rc < 0, not rc != NI_OK).
int ni_wav_read_f32(const std::string& path, std::vector<float>* out, double* fs,
                    int* channels = nullptr, int* bits = nullptr);
int ni_wav_write_f32(const std::string& path, const float* x, size_t n, double fs,
                     int bits = 16, int channels = 1);
std::string ni_wav_path_of(const std::string& p);

// ---- device enumeration, implemented per platform -----------------------------------
// On Linux/CI this returns an empty list (there is no audio subsystem here) and sets
// *why_not when possible.
int ni_audio_enumerate(std::vector<AudioDeviceInfo>* out, std::string* why_not);
bool ni_audio_backend_available();  // false in this sandbox -> hardware gates = NOT TESTED

}  // namespace ni
#endif
