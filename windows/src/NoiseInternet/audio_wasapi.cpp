// audio_wasapi.cpp — WASAPI shared-mode render + capture, and device enumeration.
// See audio_wasapi.h for the contract. NEVER COMPILED: no Windows toolchain was available where
// this project was written; every API call below was written against the documented interface and
// needs a real build + run on a machine with a speaker and a microphone.
#include "ni/audio_wasapi.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>   // PROPVAR / PropVariantClear (MSVC does not pull this in via the others)

#include <new>
#include <string>
#include <vector>

namespace ni {
namespace {

thread_local bool t_com_ok = false;

std::string utf8(const wchar_t* w) {
  if (!w) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  s.resize(s.size() - 1);  // drop the terminator WideCharToMultiByte wrote
  return s;
}

// ComInitializeEx is per thread. If the thread already has an apartment (possibly with a different
// model, which is the case when the caller ran WasapiEngine::thread_init() as MTA first), the call
// returns RPC_E_CHANGED_MODE: the thread is usable, we just must not uninitialize it on the way out.
struct ComGuard {
  ComGuard() {
    hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ok_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    mine_ = SUCCEEDED(hr_);
  }
  ~ComGuard() {
    if (mine_) CoUninitialize();
  }
  bool ok() const { return ok_; }

 private:
  HRESULT hr_ = S_OK;
  bool ok_ = false, mine_ = false;
};

// "Stereo Mix", "What U Hear", "Loopback": a device that plays the machine's own output back.
// Never acceptable as a microphone for an acoustic test, so it is flagged, and the CLI refuses it.
bool looks_like_loopback(const std::string& name) {
  static const char* kMarks[] = {"stereo mix", "what u hear", "mix (realtek", "loopback",
                                 "virtual audio", "cable output", "sndfx", "vb-audio"};
  std::string low = name;
  for (char& c : low) c = static_cast<char>(tolower(c));
  for (const char* m : kMarks)
    if (low.find(m) != std::string::npos) return true;
  return false;
}

std::string friendly_name(IMMDevice* dev) {
  IPropertyStore* ps = nullptr;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps)) || !ps) return std::string();
  PROPVAR pv;
  PropVariantInit(&pv);
  std::string out;
  if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
    out = utf8(pv.pwszVal);
  PropVariantClear(&pv);
  ps->Release();
  return out;
}

int enumerate_role(IMMDeviceEnumerator* en, EDataFlow flow, std::vector<AudioDeviceInfo>* out) {
  IMMDeviceCollection* coll = nullptr;
  HRESULT hr = en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll);
  if (FAILED(hr) || !coll) return 0;
  UINT count = 0;
  coll->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* dev = nullptr;
    if (FAILED(coll->Item(i, &dev)) || !dev) continue;
    AudioDeviceInfo d;
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
      d.id = utf8(id);
      CoTaskMemFree(id);
    }
    d.name = friendly_name(dev);
    if (d.name.empty()) d.name = d.id;
    d.is_capture = (flow == eCapture);
    d.api = "WASAPI-shared";
    d.channels = 2;
    d.bits = 32;
    d.nominal_rate = 48000.0;
    d.loopback = looks_like_loopback(d.name);
    // The mix format tells us the rate and channel count the endpoint will actually run at, which
    // is what the pump must match; asking for a different rate is refused by open().
    IAudioClient* ac = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&ac))) && ac) {
      WAVEFORMATEX* wf = nullptr;
      if (SUCCEEDED(ac->GetMixFormat(&wf)) && wf) {
        d.nominal_rate = (double)wf->nSamplesPerSec;
        d.channels = wf->nChannels > 0 ? (int)wf->nChannels : 1;
        d.bits = wf->wBitsPerSample ? (int)wf->wBitsPerSample : 32;
        UINT per = 0, buf = 0;
        if (SUCCEEDED(ac->GetBufferSize(&buf))) d.min_frames = (int)buf;
        if (SUCCEEDED(ac->GetPeriodicity(&per))) {
          // periodicity is in 100 ns units; convert to frames so the pump can size a period
          double frames = (per / 10000000.0) * d.nominal_rate;
          if (frames > 1.0 && d.min_frames == 0) d.min_frames = (int)frames;
        }
        CoTaskMemFree(wf);
      }
      ac->Release();
    }
    dev->Release();
    out->push_back(d);
  }
  coll->Release();
  return (int)out->size();
}

}  // namespace

int ni_wasapi_enumerate(std::vector<AudioDeviceInfo>* out, std::string* why_not) {
  if (!out) return NI_ERR_PARAM;
  out->clear();
  ComGuard com;
  if (!com.ok()) {
    if (why_not) *why_not = "CoInitializeEx failed";
    return NI_ERR_IO;
  }
  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
  if (FAILED(hr) || !en) {
    if (why_not) *why_not = "no MMDeviceEnumerator (audio service not running?)";
    return NI_ERR_NOAUDIO;
  }
  size_t before = out->size();
  enumerate_role(en, eRender, out);
  enumerate_role(en, eCapture, out);
  en->Release();
  if (out->size() == before) {
    if (why_not) *why_not = "WASAPI reported no active endpoints";
    return NI_ERR_NOAUDIO;
  }
  return (int)(out->size() - before);
}

struct WasapiEngine::Impl {
  struct Side {
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    IAudioCaptureClient* cc = nullptr;
    HANDLE ev = nullptr;
    WAVEFORMATEXTENSIBLE fmt;
    int ch = 1;
    int buf = 0;
  };
  Side render, capture;
  ~Impl() { stop(); }
  void stop() {
    if (render.ac) render.ac->Stop();
    if (capture.ac) capture.ac->Stop();
    if (render.rc) { render.rc->Release(); render.rc = nullptr; }
    if (capture.cc) { capture.cc->Release(); capture.cc = nullptr; }
    if (render.ac) { render.ac->Release(); render.ac = nullptr; }
    if (capture.ac) { capture.ac->Release(); capture.ac = nullptr; }
    if (render.dev) { render.dev->Release(); render.dev = nullptr; }
    if (capture.dev) { capture.dev->Release(); capture.dev = nullptr; }
    if (render.ev) { CloseHandle(render.ev); render.ev = nullptr; }
    if (capture.ev) { CloseHandle(capture.ev); capture.ev = nullptr; }
  }
};

namespace {

// Pick the device: explicit endpoint id, else the role default.
IMMDevice* pick(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& id) {
  IMMDevice* dev = nullptr;
  if (!id.empty()) {
    int w = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wid((size_t)w);
    MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, &wid[0], w);
    if (SUCCEEDED(en->GetDevice(&wid[0], &dev)) && dev) return dev;
  }
  if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev)) && dev) return dev;
  return nullptr;
}

// Float, mono-interleaved-at-the-device, whatever rate the endpoint reports. We ask WASAPI for the
// IEEE-float variant of the mix format at the requested rate; if the endpoint refuses that rate we
// bail rather than pretending a resampler exists.
int open_side(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& id, double want_fs,
              WasapiEngine::Impl::Side& s, std::string* err, double* fs_out) {
  s.dev = pick(en, flow, id);
  if (!s.dev) {
    *err = flow == eRender ? "no render endpoint selected" : "no capture endpoint selected";
    return NI_ERR_NOAUDIO;
  }
  HRESULT hr = s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&s.ac));
  if (FAILED(hr) || !s.ac) {
    *err = "IAudioClient activate failed";
    return NI_ERR_IO;
  }
  WAVEFORMATEX* mix = nullptr;
  if (FAILED(s.ac->GetMixFormat(&mix)) || !mix) {
    *err = "GetMixFormat failed";
    return NI_ERR_IO;
  }
  bool is_float = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                  reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  WAVEFORMATEXTENSIBLE f;
  ZeroMemory(&f, sizeof(f));
  f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  f.Format.nChannels = mix->nChannels ? mix->nChannels : 2;
  f.Format.nSamplesPerSec = want_fs > 0 ? (DWORD)lround(want_fs) : mix->nSamplesPerSec;
  f.Format.wBitsPerSample = 32;
  f.Format.nBlockAlign = (WORD)(f.Format.nChannels * 4);
  f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
  f.Format.cbSize = 22;
  f.Samples.wValidBitsPerSample = 32;
  f.dwChannelMask = f.Format.nChannels == 1 ? SPEAKER_MONO : SPEAKER_STEREO;
  f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  WAVEFORMATEX* closest = nullptr;
  hr = s.ac->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, is_float ? (WAVEFORMATEX*)&f : (WAVEFORMATEX*)&f,
                              &closest);
  if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT || (FAILED(hr) && hr != S_FALSE)) {
    DWORD got = closest ? closest->nSamplesPerSec : mix->nSamplesPerSec;
    char buf[160];
    _snprintf(buf, sizeof(buf),
              "endpoint will not do 32-bit float at %lu Hz (it wants %lu Hz, %u ch, %u bits); "
              "set the node sample rate to the device rate instead of resampling",
              (unsigned long)f.Format.nSamplesPerSec, (unsigned long)got,
              closest ? closest->nChannels : mix->nChannels,
              closest ? closest->wBitsPerSample : mix->wBitsPerSample);
    *err = buf;
    if (closest) CoTaskMemFree(closest);
    CoTaskMemFree(mix);
    return NI_ERR_NOAUDIO;
  }
  if (closest) CoTaskMemFree(closest);
  CoTaskMemFree(mix);

  UINT buf_frames = 0, per = 0;
  s.ac->GetBufferSize(&buf_frames);
  s.ac->GetPeriodicity(&per);
  if (buf_frames < 8) buf_frames = 480;
  s.buf = (int)buf_frames;
  s.ch = (int)f.Format.nChannels;
  s.fmt = f;
  if (fs_out) *fs_out = (double)f.Format.nSamplesPerSec;

  s.ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  UINT32 init_flags = flow == eCapture ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
  hr = s.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, init_flags, buf_frames, 0, (WAVEFORMATEX*)&s.fmt,
                        flow == eCapture ? s.ev : nullptr);
  if (FAILED(hr)) {
    char buf[128];
    _snprintf(buf, sizeof(buf), "IAudioClient::Initialize failed (0x%08lx)", (unsigned long)hr);
    *err = buf;
    return NI_ERR_IO;
  }
  if (flow == eCapture) s.ac->SetEventHandle(s.ev);
  if (flow == eRender) {
    hr = s.ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&s.rc));
  } else {
    hr = s.ac->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&s.cc));
  }
  if (FAILED(hr) || (flow == eRender ? !s.rc : !s.cc)) {
    *err = "GetService for the endpoint client failed";
    return NI_ERR_IO;
  }
  if (FAILED(s.ac->Start())) {
    *err = "IAudioClient::Start failed";
    return NI_ERR_IO;
  }
  return NI_OK;
}

}  // namespace

WasapiEngine::WasapiEngine() : p_(new (std::nothrow) Impl()) {}

WasapiEngine::~WasapiEngine() {
  if (p_) {
    p_->stop();
    delete p_;
  }
}

// Pump threads must bracket their work with these: WASAPI pointers are apartment-affine, and
// forgetting CoInitializeEx on a worker thread shows up as obscure 0x8004... failures deep inside
// GetBuffer, so the engine refuses to open when the calling thread is not marked ready.
int WasapiEngine::thread_init() {
  if (t_com_ok) return NI_OK;  // already initialized here; a second call returns S_FALSE anyway
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return NI_ERR_IO;
  t_com_ok = true;
  return NI_OK;
}

void WasapiEngine::thread_shutdown() {
  if (t_com_ok) {
    CoUninitialize();
    t_com_ok = false;
  }
}

bool WasapiEngine::com_ready() { return t_com_ok; }

int WasapiEngine::open(const std::string& render_id, const std::string& capture_id, double want_fs) {
  if (!p_) return NI_ERR_NOMEM;
  p_->stop();
  // WASAPI interfaces are apartment-affine: the thread that opens the streams, and any pump thread
  // that later uses them, must each have called WasapiEngine::thread_init() first. open() does not
  // initialize or uninitialize COM for you, because an apartment torn down while a stream is alive
  // invalidates the very pointers the audio threads are still holding.
  if (!com_ready()) {
    err_ = "COM is not initialized on this thread; call WasapiEngine::thread_init() first";
    return NI_ERR_IO;
  }
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) || !en) {
    err_ = "no MMDeviceEnumerator";
    return NI_ERR_NOAUDIO;
  }
  double fs = want_fs;
  int rc = open_side(en, eRender, render_id, want_fs, p_->render, &err_, &fs);
  if (rc == NI_OK) rc = open_side(en, eCapture, capture_id, fs, p_->capture, &err_, &fs);
  en->Release();
  if (rc != NI_OK) {
    p_->stop();
    return rc;
  }
  if (p_->capture.ch != p_->render.ch) {
    // Both sides mix down/up to mono in the pump, so a mismatch is legal but worth surfacing.
    err_ = "note: capture and render endpoints have different channel counts";
  }
  fs_ = fs;
  render_ch_ = p_->render.ch;
  capture_ch_ = p_->capture.ch;
  render_buf_ = p_->render.buf;
  capture_buf_ = p_->capture.buf;
  return NI_OK;
}

void WasapiEngine::close() {
  if (p_) p_->stop();
  fs_ = render_ch_ = capture_ch_ = render_buf_ = capture_buf_ = 0;
}

int WasapiEngine::render(const float* mono, int frames, int timeout_ms) {
  if (!p_ || !p_->render.rc || frames <= 0) return 0;
  Impl::Side& s = p_->render;
  int written = 0;
  DWORD t0 = GetTickCount();
  while (written < frames) {
    UINT padding = 0, free_frames = 0;
    if (FAILED(s.ac->GetCurrentPadding(&padding))) {
      err_ = "GetCurrentPadding failed";
      return written ? written : NI_ERR_IO;
    }
    free_frames = (UINT)(s.buf > (int)padding ? s.buf - padding : 0);
    if (free_frames == 0) {
      if (timeout_ms >= 0 && (int)(GetTickCount() - t0) > timeout_ms) return written;  // backpressure
      Sleep(2);
      continue;
    }
    UINT want = (UINT)(frames - written);
    if (want > free_frames) want = free_frames;
    float* data = nullptr;
    HRESULT hr = s.rc->GetBuffer(want, reinterpret_cast<BYTE**>(&data));
    if (FAILED(hr) || !data) {
      err_ = "IAudioRenderClient::GetBuffer failed";
      return written ? written : NI_ERR_IO;
    }
    if (s.ch == 1) {
      memcpy(data, mono + written, sizeof(float) * want);
    } else {
      // mono -> N channels (WASAPI shared mode gives the device mix's channel count)
      for (UINT i = 0; i < want; ++i)
        for (int c = 0; c < s.ch; ++c) data[i * s.ch + c] = mono[written + i];
    }
    s.rc->ReleaseBuffer(want, 0);
    written += (int)want;
  }
  return written;
}

int WasapiEngine::capture(float* mono, int frames) {
  if (!p_ || !p_->capture.cc || frames <= 0) return 0;
  Impl::Side& s = p_->capture;
  UINT got = 0;
  HRESULT hr = s.cc->GetNextPacketSize(&got);
  if (FAILED(hr)) {
    err_ = "GetNextPacketSize failed";
    return NI_ERR_IO;
  }
  if (got == 0) return 0;
  float* data = nullptr;
  UINT n = 0;
  DWORD flags = 0;
  hr = s.cc->GetBuffer(reinterpret_cast<BYTE**>(&data), &n, &flags, nullptr, nullptr);
  if (FAILED(hr) || !data) {
    err_ = "IAudioCaptureClient::GetBuffer failed";
    return NI_ERR_IO;
  }
  capture_silent_ = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
  int limit = (int)n < frames ? (int)n : frames;
  if (capture_silent_) {
    for (int i = 0; i < limit; ++i) mono[i] = 0.f;
  } else if (s.ch == 1) {
    memcpy(mono, data, sizeof(float) * limit);
  } else {
    for (int i = 0; i < limit; ++i) {
      float acc = 0.f;
      for (int c = 0; c < s.ch; ++c) acc += data[i * s.ch + c];
      mono[i] = acc / (float)s.ch;  // downmix; the modem works on one channel
    }
  }
  s.cc->ReleaseBuffer(n);
  return limit;
}

std::string WasapiEngine::describe() const {
  char buf[224];
  _snprintf(buf, sizeof(buf),
            "WASAPI shared: render %d ch, capture %d ch, %.0f Hz, render buffer %d frames, "
            "capture buffer %d frames",
            render_ch_, capture_ch_, fs_, render_buf_, capture_buf_);
  return std::string(buf);
}

}  // namespace ni
