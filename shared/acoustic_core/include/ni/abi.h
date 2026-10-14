// abi.h — flat C ABI. This is what Android's JNI bridge and (optionally) a .NET P/Invoke
// layer or a WASM/JS harness call. Keeping the FFI surface this small is what makes the
// "one core, many shells" claim in ADR-001 D1 true rather than aspirational.
#ifndef NI_ABI_H
#define NI_ABI_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NiNode NiNode;

// JSON-in/JSON-out config so the mobile/desktop shells never need to mirror C++ structs.
// Keys: name, role("giver"|"receiver"), profile(int), fs, tx_amplitude, duplex(0|1),
//       rx_profile, session_id, pairing_secret, proxy{port,bind,token,enable},
//       sim_channel(bool), snr_db, distance_m
NiNode* ni_abi_create(const char* config_json, char* err, int err_cap);
int ni_abi_destroy(NiNode* n);
int ni_abi_start(NiNode* n);
int ni_abi_stop(NiNode* n);
int ni_abi_step(NiNode* n, int64_t now_ms);
// How many frames the platform's audio device still has to play. The shell that owns the sound card
// is the only thing that can measure this (WASAPI GetCurrentPadding, AudioTrack head vs written), and
// turn-taking is timed against played sound, so a shell that never calls this leaves the link
// assuming its bursts are on the air the moment the samples leave the library.
int ni_abi_set_tx_backlog(NiNode* n, unsigned frames);
// microphone -> engine
int ni_abi_feed_pcm(NiNode* n, const float* pcm, int n_frames);
// engine -> speaker; returns frames copied, 0 when nothing to play
int ni_abi_pull_pcm(NiNode* n, float* out, int cap);
int ni_abi_pull_pcm_needed(NiNode* n);
// caller must release with ni_abi_free
// UI actions that need a live peer. Each writes a JSON document into out (cap bytes) and
// returns a NI_* status; the JSON carries the measured detail (elapsed ms, hashes, per-band
// results), so the shells only ever display what was actually observed.
int ni_abi_calibrate(NiNode* n, int64_t now_ms, char* out_json, int cap);
int ni_abi_test_link(NiNode* n, int64_t now_ms, int payload_bytes, char* out_json, int cap);
int ni_abi_test_internet(NiNode* n, int64_t now_ms, const char* url, char* out_json, int cap);
void ni_abi_free(char* p);
char* ni_abi_status_json(NiNode* n);
char* ni_abi_report_json(NiNode* n);
int ni_abi_send_bytes(NiNode* n, int stream, int type, const uint8_t* data, int len,
                      int flags);
// Stateless PHY helpers, used by the Android app's "decode a recording" feature and by the
// tools: render a burst carrying `text` with `profile` at `fs`; returns samples written or
// negative error.
int ni_abi_render(int profile, double fs, double amplitude, uint16_t session_id,
                  const uint8_t* payload, int payload_len, float* out, int out_cap);
// Decode captured samples; writes recovered payload, returns byte count or negative error.
int ni_abi_decode(int profile, double fs, uint16_t session_id, const float* pcm, int n,
                  uint8_t* out, int out_cap, double* snr_out, double* peak_out);
const char* ni_abi_version(void);
const char* ni_abi_profiles_json(void);
int ni_abi_audio_enumerate(char* out_json, int cap);  // NI_ERR_UNSUPPORTED without hardware

#ifdef __cplusplus
}
#endif
#endif
