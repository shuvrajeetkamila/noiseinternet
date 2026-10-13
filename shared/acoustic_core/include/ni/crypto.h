// crypto.h — standard primitives, vendored because no package manager is reachable in the
// build sandbox. Every implementation here is validated against its published test vectors
// (tests/crypto_tests): SHA-256 = FIPS 180-4 examples, HMAC = RFC 4231, HKDF = RFC 5869,
// ChaCha20/Poly1305/AEAD = RFC 8439. CMake option NI_USE_LIBSODIUM swaps in libsodium with
// the same API for production builds.
//
// Nothing here is a custom cipher. The only bespoke part is the pairing state machine, which
// is a documented 3-message challenge/response over HKDF-derived keys (see SECURITY_MODEL.md),
// and it carries an explicit warning that a 6-digit code is ~20 bits of entropy.
#ifndef NI_CRYPTO_H
#define NI_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "ni/common.h"
#include "ni/frame_codec.h"
namespace ni {

// ---------------------------------------------------------------- SHA-256
struct Sha256Ctx {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t nbuf;
};
void ni_sha256_init(Sha256Ctx* c);
void ni_sha256_update(Sha256Ctx* c, const void* data, size_t n);
void ni_sha256_final(Sha256Ctx* c, uint8_t out[32]);
void ni_sha256(const void* data, size_t n, uint8_t out[32]);
std::string ni_hex(const void* data, size_t n);

// ---------------------------------------------------------------- HMAC / HKDF
void ni_hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen,
                    uint8_t out[32]);
// returns NI_OK; okk must be >= 1
int ni_hkdf_sha256(const uint8_t* ikm, size_t ilen, const uint8_t* salt, size_t slen,
                   const uint8_t* info, size_t inlen, uint8_t* okm, size_t olen);

// ---------------------------------------------------------------- ChaCha20 / Poly1305
void ni_chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                 const uint8_t* in, size_t n, uint8_t* out);
void ni_poly1305(const uint8_t key[32], const uint8_t* msg, size_t n, uint8_t tag[16]);
int ni_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                 size_t alen, const uint8_t* pt, size_t plen, uint8_t* ct /*plen*/,
                 uint8_t tag[16]);
int ni_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                 size_t alen, const uint8_t* ct, size_t clen, uint8_t* pt /*clen*/,
                 const uint8_t tag[16]);

// ---------------------------------------------------------------- constant-time / buffers
int ni_ct_eq(const void* a, const void* b, size_t n);  // 1 equal, 0 different
double ni_entropy_bits(const std::string& secret);      // Shannon estimate, for UI warnings

// ---------------------------------------------------------------- session keys
enum : size_t { NI_KEY_BYTES = 32, NI_TAG_BYTES = 16 };
struct SessionKeys {
  uint8_t tx[NI_KEY_BYTES];  // this endpoint -> peer
  uint8_t rx[NI_KEY_BYTES];  // peer -> this endpoint
  uint8_t confirm[NI_KEY_BYTES];
  uint8_t sid_be[4];
  uint16_t session_id = 0;
  bool valid = false;
};

// Derives both-direction keys from the pairing secret and the two nonces.
// `self_is_a` selects which derived key becomes tx, so both endpoints agree.
int ni_derive_keys(const std::string& pairing_secret, const uint8_t nonce_a[16],
                   const uint8_t nonce_b[16], uint16_t session_id, bool self_is_a,
                   SessionKeys* out);

// 4-word short authenticated string shown to the user for out-of-band confirmation.
std::string ni_sas_words(const uint8_t nonce_a[16], const uint8_t nonce_b[16],
                         uint16_t session_id);

// ---------------------------------------------------------------- AEAD nonce + replay
// nonce = session(4) || 64-bit monotonic counter (8). Counter never repeats while the key
// lives; a new pairing produces new keys, so re-pairing is safe.
void ni_make_nonce(const uint8_t sid_be[4], uint64_t ctr, uint8_t out[12]);

class ReplayWindow {
 public:
  explicit ReplayWindow(uint32_t span = 512);
  // Returns NI_OK the first time a counter is seen, NI_ERR_REPLAY for duplicates or for
  // counters older than the window.
  int accept(uint64_t ctr);
  uint64_t highest() const { return high_; }
  void reset();

 private:
  uint64_t high_;
  uint32_t span_;
  std::vector<uint8_t> win_;  // one byte per counter position inside the window
  bool started_ = false;
};

// ---------------------------------------------------------------- pairing state machine
enum : int {
  NI_PAIR_IDLE = 0,
  NI_PAIR_WAIT_CHALLENGE = 1,
  NI_PAIR_WAIT_RESPONSE = 2,
  NI_PAIR_WAIT_CONFIRM = 3,
  NI_PAIR_DONE = 4,
  NI_PAIR_FAILED = 5,
  NI_PAIR_LOCKED = 6,   // too many failures -> rate limited
};

struct PairingConfig {
  std::string secret;      // passphrase or 6-digit code (see docs/SECURITY_MODEL.md)
  uint16_t session_id = 0;
  uint16_t caps = 0;       // bitmask of supported profile ids
  bool self_is_a = true;   // giver = A
  int max_attempts = 5;
  int backoff_ms = 4000;
};

class Pairing {
 public:
  explicit Pairing(const PairingConfig& cfg);
  int state() const { return state_; }
  const char* state_name() const;
  const SessionKeys& keys() const { return keys_; }
  std::string sas() const { return sas_; }
  uint64_t failures() const { return failures_; }
  int64_t locked_until_ms() const { return lock_until_; }

  // Produces the next handshake frame this side must send, or NI_ERR_STATE if it is the
  // peer's turn. Frames are PAIR_CHALLENGE / PAIR_RESPONSE / (CONFIRM = PAIR_RESPONSE w/
  // second mac).
  int next_frame(int64_t now_ms, Frame* out, std::vector<uint8_t>* payload);
  // Consumes a peer handshake frame.
  int on_frame(const Frame& f, int64_t now_ms);
  // Confirmation mac sent after keys are derived (protects against key confirmation attacks).
  bool confirm_ready() const { return state_ == NI_PAIR_DONE && !confirm_sent_; }

 private:
  PairingConfig cfg_;
  int state_ = NI_PAIR_IDLE;
  uint8_t nonce_a_[16] = {0}, nonce_b_[16] = {0};
  uint32_t peer_id_ = 0;
  uint16_t caps_ = 0;
  SessionKeys keys_{};
  std::string sas_;
  uint64_t failures_ = 0;
  int64_t lock_until_ = 0;
  bool confirm_sent_ = false;
  bool got_confirm_ = false;
};

}  // namespace ni
#endif
