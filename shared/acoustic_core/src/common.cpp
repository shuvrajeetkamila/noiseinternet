// common.cpp — profile table, carrier map, burst planning, misc helpers.
#include "ni/common.h"
#include <math.h>
#include <stdio.h>
#include <cstring>

namespace ni {

// Every profile declares its band in HERTZ, never in bin indices, so the numerics
// recompute correctly at 44.1 kHz and at 48 kHz without a second table (prompt 5.1).
static const Profile kProfiles[] = {
    // id  name              modulation      f_low    f_high   fec pilot maxsym cp  gain  note
    {0,  "FSK-VOICE",       NI_MOD_FSK2,   400.0,  2800.0, 2, 0, 200,  64, 0.90,
     "ultra-robust baseline (voice band), tiny rate; handshake only"},
    {1,  "FSK-MID",         NI_MOD_FSK2,  1000.0,  6000.0, 2, 0, 200,  64, 0.90,
     "wideband FSK: no phase sensitivity, tolerates Doppler/agc"},
    {2,  "DBPSK-MID",       NI_MOD_DBPSK, 1000.0,  6000.0, 2, 0, 240,  64, 0.90,
     "robust default: differential BPSK, no carrier recovery needed"},
    {3,  "DBPSK-WIDE",      NI_MOD_DBPSK, 1500.0, 12000.0, 2, 0, 160,  64, 0.90,
     "wider DBPSK, same robustness"},
    {4,  "DQPSK-WIDE",      NI_MOD_DQPSK, 1500.0, 12000.0, 2, 0,  96,  64, 0.90,
     "2x rate of DBPSK-WIDE, still differential"},
    {5,  "QAM16-WIDE",      NI_MOD_QAM16, 1500.0, 13000.0, 2, 6,  48,  64, 0.85,
     "coherent 16-QAM: needs good SNR and a short echo spread"},
    {6,  "QAM64-WIDE",      NI_MOD_QAM64, 1500.0, 13000.0, 2, 4,  40,  64, 0.80,
     "experimental 64-QAM, usually rejected by the adaptive controller"},
    {7,  "DBPSK-ULTRA",     NI_MOD_DBPSK,16000.0, 20000.0, 2, 0, 240,  64, 0.90,
     "near-ultrasonic: expected to fail on most phone/laptop speakers"},
    {8,  "QAM16-ULTRA",     NI_MOD_QAM16,15500.0, 19500.0, 2, 6,  48,  64, 0.90,
     "near-ultrasonic coherent"},
    {9,  "RAW-DBPSK-MID",   NI_MOD_DBPSK, 1000.0,  6000.0, 1, 0, 240,  64, 0.90,
     "identical to DBPSK-MID without FEC - used to report raw vs corrected BER"},
    {10, "FD-LOW-DBPSK",    NI_MOD_DBPSK,  700.0,  3600.0, 2, 0, 120,  64, 0.90,
     "full-duplex A->B direction band"},
    {11, "FD-HIGH-DBPSK",   NI_MOD_DBPSK, 7000.0, 13000.0, 2, 0, 120,  64, 0.90,
     "full-duplex B->A direction band"},
    {12, "FD-LOW-QAM16",    NI_MOD_QAM16,  700.0,  3600.0, 2, 6,  40,  64, 0.85,
     "full-duplex high rate A->B"},
    {13, "FD-HIGH-QAM16",   NI_MOD_QAM16, 7000.0, 13000.0, 2, 6,  40,  64, 0.85,
     "full-duplex high rate B->A"},
    // Echo-rated profiles. A 2.6 ms reflection is twice the 1.33 ms prefix of the standard
    // profiles, and inter-carrier interference from an over-run echo is not something an OFDM
    // equaliser can undo - it destroys the orthogonality the equaliser assumes. Widening the
    // prefix is the fix, and the cost is explicit in the rate: cp 192 costs 36% of the airtime.
    // The calibration step measures the room's delay spread and these are what it should pick
    // when the spread exceeds the prefix (see docs/RESULTS.md, Gate B).
    {14, "DBPSK-MID-ECHO",  NI_MOD_DBPSK, 1000.0,  6000.0, 2, 0, 200, 192, 0.90,
     "4 ms prefix for echoic rooms: decodes where DBPSK-MID cannot, at 36% less throughput"},
    {15, "DQPSK-WIDE-ECHO", NI_MOD_DQPSK, 1500.0, 12000.0, 2, 6, 120, 128, 0.85,
     "2.7 ms prefix wideband DQPSK for echoic rooms"},
};


int ni_profile_count() { return (int)(sizeof(kProfiles) / sizeof(kProfiles[0])); }

const Profile& ni_profile(int id) {
  if (id < 0 || id >= ni_profile_count()) id = 2;
  return kProfiles[id];
}

int ni_profile_lookup(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < ni_profile_count(); i++)
    if (NI_STRCASECMP(kProfiles[i].name, name) == 0) return i;
  return -1;
}

const char* ni_mod_name(int m) {
  switch (m) {
    case NI_MOD_FSK2: return "FSK2";
    case NI_MOD_DBPSK: return "DBPSK";
    case NI_MOD_DQPSK: return "DQPSK";
    case NI_MOD_QAM16: return "16QAM";
    case NI_MOD_QAM64: return "64QAM";
    default: return "?";
  }
}

int ni_mod_bits_per_carrier(int m) {
  switch (m) {
    case NI_MOD_QAM64: return 6;
    case NI_MOD_QAM16: return 4;
    case NI_MOD_DQPSK: return 2;
    case NI_MOD_DBPSK:
    case NI_MOD_FSK2:
    default:
      return 1;
  }
}
int ni_mod_bins_per_unit(int m) { return m == NI_MOD_FSK2 ? 2 : 1; }

const char* ni_state_name(int s) {
  switch (s) {
    case NI_ST_IDLE: return "IDLE";
    case NI_ST_HARDWARE_OK: return "HARDWARE";
    case NI_ST_CALIBRATED: return "CALIBRATED";
    case NI_ST_SIGNAL_OK: return "SIGNAL";
    case NI_ST_FRAME_OK: return "FRAMES";
    case NI_ST_PEER_OK: return "PEER";
    case NI_ST_SESSION_OK: return "SESSION";
    case NI_ST_TUNNEL_OK: return "TUNNEL";
    case NI_ST_INTERNET_OK: return "INTERNET";
    case NI_ST_ERROR: return "ERROR";
    default: return "?";
  }
}
const char* ni_role_name(int r) { return r == NI_ROLE_GIVER ? "GIVER" : "RECEIVER"; }

const char* ni_strerror(int c) {
  switch (c) {
    case NI_OK: return "ok";
    case NI_ERR_PARAM: return "invalid parameter";
    case NI_ERR_SHORT: return "buffer too short";
    case NI_ERR_VERSION: return "protocol version mismatch";
    case NI_ERR_CRC: return "integrity check failed";
    case NI_ERR_LEN: return "declared length out of range";
    case NI_ERR_TYPE: return "unknown frame type";
    case NI_ERR_NOPEER: return "no authenticated peer";
    case NI_ERR_TIMEOUT: return "timed out";
    case NI_ERR_NOMEM: return "out of memory";
    case NI_ERR_CRYPTO: return "authentication/decryption failed";
    case NI_ERR_STATE: return "not allowed in this state";
    case NI_ERR_UNSUPPORTED: return "unsupported by this build or device";
    case NI_ERR_OVERFLOW: return "value would exceed a configured bound";
    case NI_ERR_DECODE: return "channel decode failed";
    case NI_ERR_REPLAY: return "replay detected";
    case NI_ERR_RATELIMIT: return "rate limited";
    case NI_ERR_IO: return "i/o error";
    case NI_ERR_NOAUDIO: return "audio device unavailable or not permitted";
    case NI_ERR_CLIP: return "input clipping";
    case NI_ERR_NOFRAME: return "no burst detected in capture";
    case NI_ERR_BITSLIP: return "synced but bit stream unusable";
    default: return "unknown error";
  }
}

uint16_t ni_rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t ni_rd32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void ni_wr16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
void ni_wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

uint32_t Rng::next() {
  // splitmix64
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z = z ^ (z >> 31);
  return (uint32_t)(z >> 32);
}
double Rng::uniform() { return (double)(next() >> 8) * (1.0 / 16777216.0); }
double Rng::normal() {
  double u1 = uniform();
  if (u1 < 1e-12) u1 = 1e-12;
  double u2 = uniform();
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
void Rng::bytes(void* dst, size_t n) {
  uint8_t* p = (uint8_t*)dst;
  for (size_t i = 0; i < n; i++) {
    uint32_t v = next();
    p[i] = (uint8_t)(v >> ((i & 3) * 8));
  }
}

// -------------------------------------------------------------- carrier map
int ni_build_carrier_map(const Profile& p, double fs, int nfft, CarrierMap* out) {
  memset(out, 0, sizeof(*out));
  if (nfft < 64 || (nfft & (nfft - 1))) return NI_ERR_PARAM;
  double spacing = fs / (double)nfft;
  int k0 = (int)ceil(p.f_low / spacing);
  int k1 = (int)floor(p.f_high / spacing);
  if (k0 < 1) k0 = 1;
  if (k1 > nfft / 2 - 1) k1 = nfft / 2 - 1;
  out->nfft = nfft;
  out->k0 = k0;
  out->k1 = k1;
  out->n_bins = k1 - k0 + 1;
  if (out->n_bins < 6) return NI_ERR_UNSUPPORTED;  // band too narrow to be useful
  out->bins_per_unit = ni_mod_bins_per_unit(p.mod);

  // Pilots: band edges always, plus a comb for coherent modes.
  int np = 0;
  out->pilot_bins[np++] = k0;
  out->pilot_bins[np++] = k1;
  if (p.pilot_stride >= 3) {
    for (int k = k0 + p.pilot_stride; k < k1; k += p.pilot_stride) {
      if (np >= 62) break;
      out->pilot_bins[np++] = k;
    }
  }
  // dedupe + sort
  for (int i = 0; i < np; i++)
    for (int j = i + 1; j < np; j++)
      if (out->pilot_bins[j] < out->pilot_bins[i]) {
        int t = out->pilot_bins[i];
        out->pilot_bins[i] = out->pilot_bins[j];
        out->pilot_bins[j] = t;
      }
  int uniq = 0;
  for (int i = 0; i < np; i++)
    if (i == 0 || out->pilot_bins[i] != out->pilot_bins[i - 1]) out->pilot_bins[uniq++] = out->pilot_bins[i];
  np = uniq;
  out->n_pilot = np;

  int nd = 0;
  for (int k = k0; k <= k1; k++) {
    bool is_pilot = false;
    for (int i = 0; i < np; i++)
      if (out->pilot_bins[i] == k) { is_pilot = true; break; }
    if (is_pilot) continue;
    if (nd >= NI_MAX_DATA_BIN) break;
    out->mod_bins[nd++] = k;
  }
  int usable_units = nd / out->bins_per_unit;
  if (usable_units < 2) return NI_ERR_UNSUPPORTED;
  out->n_data = usable_units * out->bins_per_unit;
  out->bits_per_symbol = usable_units * ni_mod_bits_per_carrier(p.mod);
  if (out->bits_per_symbol < 4) return NI_ERR_UNSUPPORTED;
  return NI_OK;
}

int ni_profile_max_payload(const Profile& p, double fs) {
  CarrierMap m;
  if (ni_build_carrier_map(p, fs, NI_N_FFT, &m) != NI_OK) return 0;
  int coded_avail = p.max_burst_symbols * m.bits_per_symbol;
  int info_bits = (p.fec_rate_den == 2) ? (coded_avail / 2 - NI_V_TAIL_BITS) : coded_avail;
  if (info_bits < 0) return 0;
  int pdu = info_bits / 8;
  int payload = pdu - (NI_PDU_LEN_BYTES + NI_HEADER_BYTES + NI_CRC32_BYTES);
  if (payload < 0) payload = 0;
  if (payload > NI_MAX_PAYLOAD) payload = NI_MAX_PAYLOAD;
  return payload;
}

int ni_plan_burst(const Profile& p, const CarrierMap& m, double fs, int payload_bytes,
                  BurstPlan* out, int ref_interval) {
  memset(out, 0, sizeof(*out));
  out->payload_bytes = payload_bytes;
  size_t pdu = (size_t)NI_PDU_LEN_BYTES + NI_HEADER_BYTES + (size_t)payload_bytes +
               NI_CRC32_BYTES;
  out->info_bits = (int)(pdu * 8);
  int fec = p.fec_rate_den == 2 ? 2 : 1;
  out->coded_bits = fec == 2 ? 2 * (out->info_bits + NI_V_TAIL_BITS) : out->info_bits;
  int sym = (out->coded_bits + m.bits_per_symbol - 1) / m.bits_per_symbol;
  out->symbols_data = sym;
  // The transmitter emits a mid-burst reference after every ref_interval data symbols
  // *provided another data symbol follows*, so the count is (sym-1)/interval, not sym/interval.
  int refs = ref_interval > 0 && sym > 0 ? ((sym - 1) / ref_interval) : 0;
  out->symbols_hdr = ni_hdr_symbols_for(m.n_data);
  out->symbols_total = sym + refs + NI_SYNC_SYMBOLS + out->symbols_hdr + 1;  // +1 = leading ref
  int per = NI_N_FFT + ni_profile_cp(p);
  out->samples = out->symbols_total * per;
  out->duration_ms = 1000.0 * (double)out->samples / fs;
  out->sync_ms =
      1000.0 * (double)(NI_SYNC_SYMBOLS + out->symbols_hdr + 1) * per / fs;
  out->raw_bitrate = out->duration_ms > 0 ? (double)out->coded_bits / (out->duration_ms / 1000.0) : 0;
  out->info_bitrate = out->duration_ms > 0 ? (double)out->info_bits / (out->duration_ms / 1000.0) : 0;
  out->payload_bitrate =
      out->duration_ms > 0 ? 8.0 * (double)payload_bytes / (out->duration_ms / 1000.0) : 0;
  out->fits = (sym <= p.max_burst_symbols) && m.bits_per_symbol > 0 && payload_bytes >= 0 &&
              (size_t)payload_bytes <= NI_MAX_PAYLOAD;
  return NI_OK;
}

}  // namespace ni
