// wav.cpp — minimal RIFF WAVE reader/writer (16-bit PCM and 32-bit float, mono or stereo).
// Used for: offline rendering for real-hardware tests, the file-backed loopback tests, and
// saving recordings produced on device for later analysis.
#include "ni/audio_io.h"
#include <stdio.h>
#include <string.h>

namespace ni {

namespace {
struct ChunkHdr {
  char id[4];
  uint32_t size;
};
}  // namespace

int ni_wav_write_f32(const std::string& path, const float* x, size_t n, double fs, int bits,
                     int channels) {
  if (n == 0 || path.empty()) return NI_ERR_PARAM;
  if (bits != 16 && bits != 32) return NI_ERR_UNSUPPORTED;
  if (channels < 1) channels = 1;
  size_t frames = n / (size_t)channels;
  size_t bpf = (size_t)bits / 8 * (size_t)channels;
  uint32_t data_bytes = (uint32_t)(frames * bpf);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return NI_ERR_IO;
  uint32_t byte_rate = (uint32_t)(fs * (double)bpf);
  char id[4] = {'R', 'I', 'F', 'F'};
  fwrite(id, 1, 4, f);
  uint32_t riff_size = 36 + data_bytes;
  fwrite(&riff_size, 4, 1, f);
  char w[4] = {'W', 'A', 'V', 'E'};
  fwrite(w, 1, 4, f);
  char fmt[4] = {'f', 'm', 't', ' '};
  fwrite(fmt, 1, 4, f);
  uint32_t fmt_size = 16;
  fwrite(&fmt_size, 4, 1, f);
  uint16_t fmt_tag = (bits == 32) ? 3 : 1;
  fwrite(&fmt_tag, 2, 1, f);
  uint16_t ch = (uint16_t)channels;
  fwrite(&ch, 2, 1, f);
  uint32_t rate = (uint32_t)(fs + 0.5);
  fwrite(&rate, 4, 1, f);
  fwrite(&byte_rate, 4, 1, f);
  uint16_t align = (uint16_t)bpf;
  fwrite(&align, 2, 1, f);
  uint16_t bps = (uint16_t)bits;
  fwrite(&bps, 2, 1, f);
  char dat[4] = {'d', 'a', 't', 'a'};
  fwrite(dat, 1, 4, f);
  fwrite(&data_bytes, 4, 1, f);
  if (bits == 16) {
    std::vector<int16_t> buf(frames * (size_t)channels);
    for (size_t i = 0; i < buf.size(); i++) {
      double v = x[i];
      if (v > 1.0) v = 1.0;
      if (v < -1.0) v = -1.0;
      buf[i] = (int16_t)(v < 0 ? v * 32768.0 : v * 32767.0);
    }
    fwrite(&buf[0], 2, buf.size(), f);
  } else {
    fwrite(x, 4, frames * (size_t)channels, f);
  }
  fclose(f);
  return (int)frames;
}

int ni_wav_read_f32(const std::string& path, std::vector<float>* out, double* fs, int* channels,
                    int* bits) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return NI_ERR_IO;
  char riff[4] = {0}, wave[4] = {0};
  uint32_t riff_size = 0;
  if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) || fread(&riff_size, 4, 1, f) != 1 ||
      fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4)) {
    fclose(f);
    return NI_ERR_SHORT;  // not a RIFF/WAVE file (or truncated)
  }
  int ch = 1, bps = 16;
  uint32_t rate = 48000;
  uint16_t tag = 1;
  bool have_fmt = false;
  long data_off = -1;
  uint32_t data_len = 0;
  long pos = 12;
  fseek(f, 0, SEEK_END);
  long flen = ftell(f);
  fseek(f, 12, SEEK_SET);
  while (pos + 8 <= flen) {
    ChunkHdr h;
    if (fread(&h, 1, 8, f) != 8) break;
    pos += 8;
    long body = (long)h.size;
    if (pos + body > flen) body = flen - pos;
    if (!memcmp(h.id, "fmt ", 4) && body >= 16) {
      uint16_t t16;
      fread(&t16, 2, 1, f);
      tag = t16;
      fread(&t16, 2, 1, f);
      ch = t16;
      fread(&rate, 4, 1, f);
      fseek(f, 4, SEEK_CUR);  // AvgBytesPerSec - derived, not needed
      fseek(f, 2, SEEK_CUR);  // wBlockAlign   - derived, not needed
      fread(&t16, 2, 1, f);
      bps = t16;              // wBitsPerSample is the field we actually want
      fseek(f, (long)(body - 16), SEEK_CUR);
      have_fmt = true;
    } else if (!memcmp(h.id, "data", 4)) {
      data_off = pos;
      data_len = (uint32_t)body;
      break;
    } else {
      fseek(f, body + (body & 1), SEEK_CUR);
    }
    pos += (long)(h.size + (h.size & 1));
  }
  if (!have_fmt || data_off < 0) {
    fclose(f);
    return NI_ERR_SHORT;
  }
  if (tag != 1 && tag != 3) {
    fclose(f);
    return NI_ERR_UNSUPPORTED;
  }
  if (ch < 1 || ch > 8) {
    fclose(f);
    return NI_ERR_UNSUPPORTED;
  }
  int bytes_per_sample = bps / 8;
  if (bytes_per_sample < 1) {
    fclose(f);
    return NI_ERR_UNSUPPORTED;  // zero or sub-byte sample depth is not something we decode
  }
  size_t frames = data_len / ((size_t)bytes_per_sample * (size_t)ch);
  if (frames == 0) {
    fclose(f);
    return NI_ERR_SHORT;
  }
  fseek(f, data_off, SEEK_SET);
  out->assign(frames * (size_t)ch, 0.f);
  if (tag == 3 && bps == 32) {
    if (fread(&(*out)[0], 4, out->size(), f) != out->size()) {
      fclose(f);
      return NI_ERR_SHORT;
    }
  } else if (bps == 16) {
    std::vector<int16_t> s(out->size());
    if (fread(&s[0], 2, s.size(), f) != s.size()) {
      fclose(f);
      return NI_ERR_SHORT;
    }
    for (size_t i = 0; i < s.size(); i++)
      (*out)[i] = (float)((double)s[i] / (s[i] < 0 ? 32768.0 : 32767.0));
  } else if (bps == 8) {
    std::vector<uint8_t> s(out->size());
    if (fread(&s[0], 1, s.size(), f) != s.size()) {
      fclose(f);
      return NI_ERR_SHORT;
    }
    for (size_t i = 0; i < s.size(); i++) (*out)[i] = (float)((s[i] - 128) / 128.0);
  } else if (bps == 24) {
    std::vector<uint8_t> s(out->size() * 3);
    if (fread(&s[0], 1, s.size(), f) != s.size()) {
      fclose(f);
      return NI_ERR_SHORT;
    }
    for (size_t i = 0; i < out->size(); i++) {
      int32_t v = (int32_t)(s[3 * i] | (s[3 * i + 1] << 8) | (s[3 * i + 2] << 16));
      if (v & 0x800000) v |= (int32_t)0xFF000000;
      (*out)[i] = (float)(v / 8388608.0);
    }
  } else {
    fclose(f);
    return NI_ERR_UNSUPPORTED;
  }
  fclose(f);
  if (ch > 1) {  // downmix to mono: the modem is single channel by design
    std::vector<float> m(frames, 0.f);
    for (size_t i = 0; i < frames; i++) {
      double a = 0;
      for (int c = 0; c < ch; c++) a += (*out)[i * (size_t)ch + (size_t)c];
      m[i] = (float)(a / (double)ch);
    }
    *out = m;
  }
  if (fs) *fs = (double)rate;
  if (channels) *channels = ch;
  if (bits) *bits = bps;
  return (int)frames;
}

std::string ni_wav_path_of(const std::string& p) { return p; }

int ni_audio_enumerate(std::vector<AudioDeviceInfo>*, std::string* why) {
#if defined(NI_WITH_WASAPI)
  return ni_wasapi_enumerate(out, why);
#else
  if (why)
    *why =
        "no audio enumeration on this platform build; WASAPI backend is compiled only into the "
        "Windows executable, and Android enumerates in Kotlin";
  (void)why;
  return NI_ERR_UNSUPPORTED;
#endif
}

bool ni_audio_backend_available() {
#if defined(NI_WITH_WASAPI)
  return true;
#else
  return false;
#endif
}

}  // namespace ni
