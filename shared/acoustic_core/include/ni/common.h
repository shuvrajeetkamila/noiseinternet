// NOISE INTERNET — shared acoustic core
// common.h : types, constants, protocol limits, modulation profile table.
//
// This header is the single source of truth for the wire format on every platform
// (Android via JNI, Windows by direct link, browsers via WASM, and the Python
// reference implementation validates itself against vectors emitted from here).
#ifndef NI_COMMON_H
#define NI_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string>

// NI_PI is not standard C++: it is hidden behind _USE_MATH_DEFINES on MSVC and is absent in strict
// mode. Every use in this tree goes through NI_PI so no source file has to care which compiler it is
// fed, which is the whole point of a shared core.
#define NI_PI 3.14159265358979323846

// Which build of the engine is running, stamped in by the build system (CMake -DNI_BUILD_STAMP=...).
//
// This exists because a user reported "the fix did not work" and neither of us could tell whether the app
// on the phone was the new one: successive CI builds produced APKs that could not be installed over each
// other (a fresh debug key every run - see android/app/build.gradle.kts) and nothing on the screen said
// which build it was. Every front end now prints this string, the app prints its own stamp next to it, and
// a mismatch is visible at a glance instead of being guessed at.
#ifndef NI_BUILD_STAMP
#define NI_BUILD_STAMP "local build (unstamped)"
#endif

#if defined(_WIN32)
#define NI_STRCASECMP _stricmp
#else
#include <strings.h>
#define NI_STRCASECMP strcasecmp
#endif

namespace ni {

// ---------------------------------------------------------------- protocol constants
enum : int {
  NI_PROTO_VERSION = 1,          // low nibble of version_type is reserved, version is the high nibble
  NI_HEADER_BYTES = 16,          // MAC frame header (see frame_codec.h for the layout)
  NI_CRC32_BYTES = 4,            // MAC frame trailer
  NI_PDU_LEN_BYTES = 2,          // PHY-level big-endian length prefix
  NI_MAX_PAYLOAD = 240,          // hard bound, prompt 5.4 / 8 (bounded packet sizes)
  NI_MAX_FRAME = NI_HEADER_BYTES + NI_MAX_PAYLOAD + NI_CRC32_BYTES,       // 260
  NI_MAX_PDU = NI_PDU_LEN_BYTES + NI_MAX_FRAME,                            // 262
  NI_MAX_PDU_BITS = NI_MAX_PDU * 8,
  NI_MAX_CODED_BITS = (NI_MAX_PDU_BITS + 8) * 2 + 12,  // scrambler+fec headroom
  NI_MAX_BURST_SAMPLES = 96000,  // 2 s at 48 kHz: absolute airtime ceiling
  NI_N_FFT = 256,
  NI_CP = 64,
  NI_SYNC_SYMBOLS = 2,
  NI_HDR_BITS = 24,           // burst descriptor: pdu_bytes(11)|mod_id(4)|res(1)|crc8(8)
  NI_HDR_SYMBOLS = 2,         // descriptor is repeated on 2 symbols and soft-combined
  NI_MAX_BURST_PDU = 1100,    // concatenated frames per burst, PHY bound
  NI_MAX_DATA_BIN = NI_N_FFT / 2 - 1,
  NI_REF_INTERVAL = 24,  // reference symbol every N data symbols (protocol constant)
  NI_V_TAIL_BITS = 6,  // convolutional-code flush bits (kept here so profiles can plan)
};

// Header flag bits (byte 14 of the MAC header).
enum : uint8_t {
  NI_FLAG_ACK_VALID = 1u << 0,
  NI_FLAG_NACK = 1u << 1,
  NI_FLAG_FRAG_MORE = 1u << 2,
  NI_FLAG_ENCRYPTED = 1u << 3,
  NI_FLAG_CTRL = 1u << 4,
  NI_FLAG_DUP_WIN = 1u << 5,
  // Set only on the final key-confirmation frame of the pairing handshake. It cannot reuse
  // NI_FLAG_ACK_VALID (every acknowledged frame has that one set), which is what made both
  // endpoints reject each other's pairing frames.
  NI_FLAG_PAIR_CONFIRM = 1u << 6,
};

// Frame types, 4 bits.
enum FrameType : uint8_t {
  NI_FT_DATA = 0,
  NI_FT_ACK = 1,
  NI_FT_NACK = 2,
  NI_FT_PING = 3,
  NI_FT_PONG = 4,
  NI_FT_CAL_START = 5,
  NI_FT_CAL_PROBE = 6,
  NI_FT_CAL_REPORT = 7,
  NI_FT_PAIR_CHALLENGE = 8,
  NI_FT_PAIR_RESPONSE = 9,
  NI_FT_NETINFO = 10,
  NI_FT_CTRL = 11,
  NI_FT_RESYNC = 12,
  NI_FT_KEEPALIVE = 13,
  NI_FT_ERROR = 14,
  NI_FT_NOP = 15,
  NI_FT_COUNT = 16
};

// Link states. Each one is only entered by an actual measured event (prompt 6).
enum LinkState : int {
  NI_ST_IDLE = 0,
  NI_ST_HARDWARE_OK = 1,   // audio devices opened, real mix format recorded
  NI_ST_CALIBRATED = 2,   // a calibration run produced a channel table
  NI_ST_SIGNAL_OK = 3,    // preamble energy/matched-filter detection happened
  NI_ST_FRAME_OK = 4,     // at least one CRC-valid frame decoded
  NI_ST_PEER_OK = 5,      // pairing challenge/response succeeded
  NI_ST_SESSION_OK = 6,   // session established, ARQ running
  NI_ST_TUNNEL_OK = 7,    // local proxy accepted + served a client request
  NI_ST_INTERNET_OK = 8,  // giver's real external fetch verified end-to-end
  NI_ST_ERROR = 9,
};
const char* ni_state_name(int s);

enum Role : uint8_t { NI_ROLE_GIVER = 0, NI_ROLE_RECEIVER = 1 };
const char* ni_role_name(int r);

// ---------------------------------------------------------------- error codes
enum : int {
  NI_OK = 0,
  NI_ERR_PARAM = -1,
  NI_ERR_SHORT = -2,        // truncated buffer
  NI_ERR_VERSION = -3,      // unknown protocol version
  NI_ERR_CRC = -4,          // integrity check failed
  NI_ERR_LEN = -5,          // declared length out of range
  NI_ERR_TYPE = -6,         // unknown frame type
  NI_ERR_NOPEER = -7,       // no authenticated peer
  NI_ERR_TIMEOUT = -8,
  NI_ERR_NOMEM = -9,
  NI_ERR_CRYPTO = -10,      // AEAD / MAC failure
  NI_ERR_STATE = -11,       // operation illegal in this state
  NI_ERR_UNSUPPORTED = -12,
  NI_ERR_OVERFLOW = -13,    // would exceed a configured bound
  NI_ERR_DECODE = -14,      // FEC/symbol decode failed
  NI_ERR_REPLAY = -15,      // sequence outside replay window
  NI_ERR_RATELIMIT = -16,   // too many malformed frames / attempts
  NI_ERR_IO = -17,
  NI_ERR_NOAUDIO = -18,     // device absent or permission denied
  NI_ERR_CLIP = -19,        // input clipping detected during calibration
  NI_ERR_NOFRAME = -20,     // nothing detected in the capture window
  NI_ERR_BITSLIP = -21,     // sync found but bit stream unusable
};
const char* ni_strerror(int code);
inline bool ni_ok(int code) { return code == NI_OK; }

// ---------------------------------------------------------------- modulation profiles
enum Modulation : uint8_t {
  NI_MOD_FSK2 = 0,   // wideband binary FSK over adjacent tone pairs (robust baseline)
  NI_MOD_DBPSK = 1,  // differential BPSK per subcarrier
  NI_MOD_DQPSK = 2,  // differential QPSK per subcarrier (Gray)
  NI_MOD_QAM16 = 3,  // coherent 16-QAM, scattered pilots
  NI_MOD_QAM64 = 4,  // coherent 64-QAM, scattered pilots (experimental)
  NI_MOD_COUNT = 5
};
const char* ni_mod_name(int m);
int ni_mod_bits_per_carrier(int m);
// Number of *used* OFDM bins consumed by one information bit (2 for FSK, 1 otherwise).
int ni_mod_bins_per_unit(int m);

struct Profile {
  uint8_t id;
  const char* name;
  Modulation mod;
  double f_low;               // Hz, inclusive lower band edge
  double f_high;              // Hz, inclusive upper band edge
  uint8_t fec_rate_den;       // 1 = no FEC, 2 = rate 1/2 (K=7, 133/171 octal)
  uint8_t pilot_stride;       // 0 = no scattered pilots (needed by coherent QAM)
  int16_t max_burst_symbols;  // airtime bound for a single burst
  uint8_t cp;                 // cyclic-prefix samples used by every symbol of this profile
  double tx_linear_gain;      // scale applied to the synthesised waveform (<=1)
  const char* note;
};

int ni_profile_count();
// Every symbol in a burst uses the profile's own prefix length. The prefix therefore belongs to
// the profile definition (and so to the frame format), not to a per-session parameter: the
// receiver must know it before it can read anything, and all it knows at that moment is the
// profile id carried by the burst descriptor. 0 means NI_CP.
inline int ni_profile_cp(const Profile& p) { return p.cp > 0 ? (int)p.cp : NI_CP; }
const Profile& ni_profile(int id);           // asserts range
int ni_profile_lookup(const char* name);     // -1 when absent

// bits per OFDM symbol for a profile at a given sample rate (0 if band too narrow)
struct CarrierMap {
  int nfft;
  int k0, k1;        // first/last used bin (inclusive)
  int n_bins;        // used bins in [k0,k1]
  int bins_per_unit; // 2 for FSK, else 1
  int n_pilot;       // scattered pilot bins
  int n_data;        // modulation units available for information
  int bits_per_symbol;
  int pilot_bins[64];
  int mod_bins[NI_MAX_DATA_BIN + 1];  // bins carrying data (already unit-expanded)
};
int ni_build_carrier_map(const Profile& p, double fs, int nfft, CarrierMap* out);

// The burst descriptor is NI_HDR_BITS bits spread over the profile's modulation units, one
// symbol at a time. A narrow profile (FSK-VOICE has 10 units) cannot carry 24 bits in two
// symbols, so it needs three - and the receiver derives the same count from its own carrier
// map, which is why this must be a pure function of n_data and never a negotiated field: the
// descriptor is what tells the receiver the profile, so the profile cannot depend on it.
inline int ni_hdr_symbols_for(int n_data) {
  if (n_data < 1) n_data = 1;
  int h = (NI_HDR_BITS + n_data - 1) / n_data;
  return h < NI_HDR_SYMBOLS ? NI_HDR_SYMBOLS : h;
}
// slot -> descriptor bit. The stride 7 is coprime with 24, so consecutive subcarriers carry
// bits 7 apart: a single deep notch (real rooms produce notches about one subcarrier wide)
// costs one soft sample per bit instead of a run of adjacent bits. Coverage is guaranteed
// because slot = 0..23 already hits every bit exactly once.
inline int ni_hdr_bit_for(int slot) { return (7 * slot) % NI_HDR_BITS; }

// Airtime / capacity bookkeeping. Fills a small struct used by both the modem and the
// reporting tools, so no number in a report is hand-written.
struct BurstPlan {
  int symbols_data;        // OFDM symbols carrying the FEC stream
  int symbols_hdr;         // burst descriptor symbols (>= NI_HDR_SYMBOLS, see ni_hdr_symbols_for)
  int symbols_total;       // + sync symbols
  int samples;             // total burst length in samples (with CPs)
  double duration_ms;
  int info_bits;
  int coded_bits;
  int payload_bytes;       // payload of the MAC frame this plan was computed for
  double raw_bitrate;      // coded bits / s
  double info_bitrate;     // information bits / s (before MAC/PHY overhead)
  double payload_bitrate;  // payload bytes / s (the honest "useful throughput" ceiling)
  double sync_ms;
  bool fits;               // false when the payload cannot be sent in this profile
};
int ni_plan_burst(const Profile& p, const CarrierMap& m, double fs, int payload_bytes,
                  BurstPlan* out, int ref_interval = 24);

int ni_profile_max_payload(const Profile& p, double fs);

// ---------------------------------------------------------------- small utils
inline size_t ni_min(size_t a, size_t b) { return a < b ? a : b; }
inline size_t ni_max(size_t a, size_t b) { return a > b ? a : b; }

uint16_t ni_rd16(const uint8_t* p);
std::string ni_json_escape(const std::string& s);
uint32_t ni_rd32(const uint8_t* p);
void ni_wr16(uint8_t* p, uint16_t v);
void ni_wr32(uint8_t* p, uint32_t v);

// 32-bit splitmix-style PRNG: identical on every platform (no <random> portability risk),
// used for reproducible tests, simulations and payload generation.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed = 0x9e3779b97f4a7c15ull) : s(seed ? seed : 1) {}
  uint32_t next();
  double uniform();  // [0,1)
  double normal();   // mean 0, variance 1
  void bytes(void* dst, size_t n);
};

}  // namespace ni
#endif  // NI_COMMON_H
