// audio_wasapi.cpp — WASAPI shared-mode render + capture, and device enumeration.
// See audio_wasapi.h for the contract. Compiled by CI with MSVC 19.51 on every push; it has still never
// been run against a sound card, because no build host here has one. The unit tests do not reach it -
// `ni audit` and the app's device lists are the only callers - which is why the crash described next went
// unnoticed until CI ran the window.
#include "ni/audio_wasapi.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>   // IAudioEndpointVolume: the Windows volume of each endpoint (round 6)
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>   // PropVariantClear
#include <mmreg.h>     // SPEAKER_* channel-mask constants

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
  // PROPVAR is a C++-only RAII helper in propidl.h; the plain variant is what PropVariantClear takes and
  // it needs no extra header. It must be zeroed first: the old code declared it uninitialised and called
  // PropVariantClear() on it unconditionally, so a failing GetValue() meant Clear() interpreting a stack
  // garbage vt - if that garbage looked like VT_LPWSTR or VT_BSTR it frees an arbitrary pointer, and the
  // process dies with 0xC0000005 in code that never looked wrong. Zeroing makes Clear() a documented no-op
  // for VT_EMPTY, which is why it is now unconditionally safe.
  PROPVARIANT pv;
  ZeroMemory(&pv, sizeof(pv));
  std::string out;
  HRESULT hr = ps->GetValue(PKEY_Device_FriendlyName, &pv);
  if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal) out = utf8(pv.pwszVal);
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
        UINT buf = 0;
        REFERENCE_TIME hpn = 0, hsn = 0;
        if (SUCCEEDED(ac->GetBufferSize(&buf))) d.min_frames = (int)buf;
        // IAudioClient has no GetPeriodicity(); that is IAudioClient2. The device period pair is
        // what the shared-mode engine actually runs on, and it is already in 100 ns units.
        if (SUCCEEDED(ac->GetDevicePeriod(&hpn, &hsn))) {
          REFERENCE_TIME per = flow == eRender ? hpn : hsn;
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
    IAudioEndpointVolume* vol = nullptr;   // shared-mode volume/mute control for this endpoint
    // The endpoint that was actually opened, by its Windows friendly name. The status panel printed the
    // channel counts and buffer sizes but never WHICH device was in use, so a machine with several
    // microphones could not tell whether Windows picked the one the user plugged in (defect I).
    std::string name;
  };
  Side render, capture;
  // Set by open(): the render volume the engine found, and whether it changed it (restored by close()).
  bool vol_changed = false;
  double vol_before_pct = -1.0;
  ~Impl() { stop(); }
  void stop() {
    if (render.ac) render.ac->Stop();
    if (capture.ac) capture.ac->Stop();
    if (render.rc) { render.rc->Release(); render.rc = nullptr; }
    if (capture.cc) { capture.cc->Release(); capture.cc = nullptr; }
    if (render.ac) { render.ac->Release(); render.ac = nullptr; }
    if (capture.ac) { capture.ac->Release(); capture.ac = nullptr; }
    if (render.vol) { render.vol->Release(); render.vol = nullptr; }
    if (capture.vol) { capture.vol->Release(); capture.vol = nullptr; }
    if (render.dev) { render.dev->Release(); render.dev = nullptr; }
    if (capture.dev) { capture.dev->Release(); capture.dev = nullptr; }
    if (render.ev) { CloseHandle(render.ev); render.ev = nullptr; }
    if (capture.ev) { CloseHandle(capture.ev); capture.ev = nullptr; }
  }
};

// Defined after Impl, because Impl is what it reads: the first version sat above the struct and MSVC
// rejected it with C2027 (use of undefined type) - found by CI, not by reading.
int WasapiEngine::queued_frames() const {
  if (!p_ || !p_->render.ac) return -1;
  UINT32 pad = 0;
  if (FAILED(p_->render.ac->GetCurrentPadding(&pad))) return -1;
  return (int)pad;
}

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
  // Named here, where the endpoint has just been chosen, and never guessed later.
  s.name = friendly_name(s.dev);
  if (s.name.empty()) s.name = id.empty() ? "(system default)" : id;
  // Volume, if this endpoint offers it. Not fatal when it does not: the level is a diagnostic, and a
  // machine that will not report it must not lose its audio over it.
  {
    IAudioEndpointVolume* vol = nullptr;
    HRESULT vhr = s.dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&vol));
    if (SUCCEEDED(vhr) && vol) s.vol = vol;
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
  f.dwChannelMask = f.Format.nChannels == 1 ? SPEAKER_FRONT_CENTER
                                            : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
  f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  WAVEFORMATEX* closest = nullptr;
  (void)is_float;  // the float mix format is what shared mode gives you; if the endpoint disagrees it
                   // answers S_FALSE/unsupported and `closest` tells us what it wants (handled below).
  hr = s.ac->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&f, &closest);
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

  UINT buf_frames = 0;
  REFERENCE_TIME hpn = 0, hsn = 0;
  s.ac->GetBufferSize(&buf_frames);
  s.ac->GetDevicePeriod(&hpn, &hsn);
  if (buf_frames < 8) buf_frames = 480;
  s.buf = (int)buf_frames;
  s.ch = (int)f.Format.nChannels;
  s.fmt = f;
  if (fs_out) *fs_out = (double)f.Format.nSamplesPerSec;

  s.ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  UINT32 init_flags = flow == eCapture ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
  // Initialize's 4th/5th duration arguments are 100 ns periods, not frame counts, and its last
  // argument is an audio-session GUID - the capture event belongs in SetEventHandle (below), not
  // here. Passing frames/handles was a wrong-unit bug that MSVC's prototype caught for the handle.
  REFERENCE_TIME rate = (REFERENCE_TIME)f.Format.nSamplesPerSec;
  REFERENCE_TIME hns_buf = rate ? (REFERENCE_TIME)(10000000.0 * buf_frames / rate) : 300000;
  REFERENCE_TIME hns_per = flow == eCapture ? hsn : hpn;
  hr = s.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, init_flags, hns_buf, hns_per,
                        (WAVEFORMATEX*)&s.fmt, nullptr);
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

  // Self-tuning, once, and reversible. A WASAPI shared-mode stream plays at the endpoint's own volume, so
  // a speaker slider left at 20 % is a modem nobody in the room can hear - and on the two field runs the
  // PC's own recording sat 7 dB above the room, which is what a nearly-inaudible speaker looks like in a
  // level meter. If the render volume is below half, it is raised for the session and put back on Stop
  // (see close()): loud enough to be heard, and never left changed behind the user's back.
  if (p_->render.vol) {
    float sc = 0.0f;
    BOOL mute = FALSE;
    if (SUCCEEDED(p_->render.vol->GetMasterVolumeLevelScalar(&sc)) &&
        SUCCEEDED(p_->render.vol->GetMute(&mute))) {
      const double pct = 100.0 * (double)sc;
      p_->vol_before_pct = pct;
      if (pct < 50.0 || mute) {
        if (mute) p_->render.vol->SetMute(FALSE, nullptr);
        if (SUCCEEDED(p_->render.vol->SetMasterVolumeLevelScalar(0.85f, nullptr)))
          p_->vol_changed = true;
      }
    }
  }
  return NI_OK;
}

void WasapiEngine::close() {
  // Put the speaker volume back where it was found, before the endpoint's interfaces are released.
  if (p_ && p_->vol_changed && p_->render.vol && p_->vol_before_pct >= 0.0) {
    p_->render.vol->SetMasterVolumeLevelScalar((float)(p_->vol_before_pct / 100.0), nullptr);
    p_->vol_changed = false;
  }
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
  int out = 0;
  // Whatever a previous call could not fit stays here instead of being thrown away. The old version
  // read exactly ONE device packet per call and released the whole packet even when the caller's
  // buffer was smaller, so every sample past the end of that buffer was dropped - silently, and in the
  // middle of the bursts the peer is trying to decode. (With a 2048-frame caller buffer and the usual
  // 480-960 frame packets it rarely triggered; on a device that hands over larger packets it would
  // corrupt every burst.) Now: drain the hold-over first, then keep reading packets while there is
  // room, keeping the remainder for the next call.
  while (out < frames && (int)cap_hold_.size() > cap_hold_off_) {
    mono[out++] = cap_hold_[(size_t)cap_hold_off_++];
  }
  if (cap_hold_off_ >= (int)cap_hold_.size()) {
    cap_hold_.clear();
    cap_hold_off_ = 0;
  }
  for (;;) {
    UINT got = 0;
    HRESULT hr = s.cc->GetNextPacketSize(&got);
    if (FAILED(hr)) {
      err_ = "GetNextPacketSize failed";
      return out ? out : NI_ERR_IO;
    }
    if (got == 0) return out;
    float* data = nullptr;
    UINT n = 0;
    DWORD flags = 0;
    hr = s.cc->GetBuffer(reinterpret_cast<BYTE**>(&data), &n, &flags, nullptr, nullptr);
    if (FAILED(hr) || !data) {
      err_ = "IAudioCaptureClient::GetBuffer failed";
      return out ? out : NI_ERR_IO;
    }
    capture_silent_ = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    bool any = false;
    for (UINT i = 0; i < n; ++i) {
      float v = 0.f;
      if (!capture_silent_) {
        if (s.ch == 1) {
          v = data[i];
        } else {
          float acc = 0.f;
          for (int c = 0; c < s.ch; ++c) acc += data[i * (UINT)s.ch + (UINT)c];
          v = acc / (float)s.ch;   // downmix; the modem works on one channel
        }
      }
      if (out < frames) {
        mono[out++] = v;
      } else {
        cap_hold_.push_back(v);    // keep it for the next call - never drop a sample
        any = true;
      }
    }
    s.cc->ReleaseBuffer(n);
    if (!any && out >= frames) return out;
  }
}

bool WasapiEngine::read_volumes(double* render_pct, bool* render_muted, double* capture_pct,
                                bool* capture_muted) const {
  if (!p_) return false;
  bool any = false;
  if (p_->render.vol) {
    float sc = 0.0f;
    BOOL m = FALSE;
    if (SUCCEEDED(p_->render.vol->GetMasterVolumeLevelScalar(&sc)) &&
        SUCCEEDED(p_->render.vol->GetMute(&m))) {
      if (render_pct) *render_pct = 100.0 * (double)sc;
      if (render_muted) *render_muted = (m != FALSE);
      any = true;
    }
  }
  if (p_->capture.vol) {
    float sc = 0.0f;
    BOOL m = FALSE;
    if (SUCCEEDED(p_->capture.vol->GetMasterVolumeLevelScalar(&sc)) &&
        SUCCEEDED(p_->capture.vol->GetMute(&m))) {
      if (capture_pct) *capture_pct = 100.0 * (double)sc;
      if (capture_muted) *capture_muted = (m != FALSE);
      any = true;
    }
  }
  return any;
}

bool WasapiEngine::set_render_volume_pct(double pct) {
  if (!p_ || !p_->render.vol) return false;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;
  return SUCCEEDED(p_->render.vol->SetMasterVolumeLevelScalar((float)(pct / 100.0), nullptr));
}

double WasapiEngine::render_volume_before() const {
  return (p_ && p_->vol_changed) ? p_->vol_before_pct : -1.0;
}

static std::string volumes_text(const WasapiEngine::Impl& im) {
  // One sentence per endpoint: percentage, and the mute flag spelled out. Silence has two causes - a
  // slider at zero and a muted endpoint - and they look identical in a level meter.
  char buf[256];
  double rp = -1, cp = -1;
  bool rm = false, cm = false;
  bool any = false;
  if (im.render.vol) {
    float sc = 0.0f;
    BOOL m = FALSE;
    if (SUCCEEDED(im.render.vol->GetMasterVolumeLevelScalar(&sc)) &&
        SUCCEEDED(im.render.vol->GetMute(&m))) {
      rp = 100.0 * (double)sc;
      rm = (m != FALSE);
      any = true;
    }
  }
  if (im.capture.vol) {
    float sc = 0.0f;
    BOOL m = FALSE;
    if (SUCCEEDED(im.capture.vol->GetMasterVolumeLevelScalar(&sc)) &&
        SUCCEEDED(im.capture.vol->GetMute(&m))) {
      cp = 100.0 * (double)sc;
      cm = (m != FALSE);
      any = true;
    }
  }
  if (!any) return "volume: this endpoint does not report one";
  _snprintf(buf, sizeof(buf),
            "volume: speaker %.0f%%%s, microphone %.0f%%%s", rp,
            rm ? " (MUTED - unmute it, or nothing can be heard)" : "", cp,
            cm ? " (MUTED - Windows is dropping every sample)" : "");
  return std::string(buf);
}

std::string WasapiEngine::describe() const {
  // Names first: "which speaker, which microphone" is the question a field test has to answer, and it was
  // the one thing this line did not say (defect I).
  char buf[512];
  _snprintf(buf, sizeof(buf),
            "WASAPI shared: render \"%s\" (%d ch, buffer %d frames), capture \"%s\" (%d ch, buffer %d "
            "frames), %.0f Hz%s",
            p_ && !p_->render.name.empty() ? p_->render.name.c_str() : "(not open)", render_ch_, render_buf_,
            p_ && !p_->capture.name.empty() ? p_->capture.name.c_str() : "(not open)", capture_ch_,
            capture_buf_, fs_, capture_silent_ ? " - THE CAPTURE ENDPOINT IS SILENT (muted or removed)" : "");
  std::string out(buf);
  if (p_) {
    out += ", ";
    out += volumes_text(*p_);
    if (p_->vol_changed && p_->vol_before_pct >= 0.0) {
      char b2[96];
      _snprintf(b2, sizeof(b2), " [raised from %.0f%% for this session; put back on Stop]",
                p_->vol_before_pct);
      out += b2;
    }
  }
  return out;
}

}  // namespace ni
