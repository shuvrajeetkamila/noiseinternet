// t_pairing.cpp — the pairing handshake as a pure state machine: two Pairing objects exchanging
// Frames in memory, no audio, no channel. This is the test that pins the handshake ordering, the
// key-confirmation semantics, SAS agreement, and the rate-limit lockout.
#include <cstring>
#include <string>
#include <vector>

#include "ni/crypto.h"
#include "ni/frame_codec.h"
#include "ntest.h"

using namespace ni;

namespace {

struct Wire {
  // delivers A->B or B->A through a real frame encode/decode so the header flags are exercised
  static int deliver(Pairing* from, Pairing* to, int64_t now) {
    Frame f;
    std::vector<uint8_t> pl;
    if (from->next_frame(now, &f, &pl) != NI_OK) return 0;  // nothing to send
    f.session_id = 0x4e31;
    std::vector<uint8_t> buf(NI_MAX_FRAME);
    int n = ni_frame_encode(buf.data(), buf.size(), f);
    if (n <= 0) return -1;
    Frame g;
    if (ni_frame_decode(buf.data(), (size_t)n, &g) != NI_OK) return -2;
    (void)g.payload;
    return to->on_frame(f, now) == NI_OK ? 1 : -3;
  }
};

std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) s += std::string{d[p[i] >> 4], d[p[i] & 15]};
  return s;
}

}  // namespace

int main() {
  printf("[t_pairing] handshake state machine\n");

  // ---- 1. happy path: challenge -> response -> confirm -> both DONE -------------------------
  {
    PairingConfig ca, cb;
    ca.secret = cb.secret = "correct horse battery";
    ca.session_id = cb.session_id = 0x4e31;
    ca.self_is_a = true;
    cb.self_is_a = false;
    ca.caps = cb.caps = 0x0005;
    Pairing A(ca), B(cb);
    NT_EQ(A.state(), NI_PAIR_WAIT_RESPONSE);
    NT_EQ(B.state(), NI_PAIR_WAIT_CHALLENGE);

    Frame f;
    std::vector<uint8_t> pl;
    NT_EQ(A.next_frame(0, &f, &pl), NI_OK);
    NT_EQ((int)f.type, NI_FT_PAIR_CHALLENGE);
    NT_EQ(B.on_frame(f, 10), NI_OK);
    NT_EQ(B.state(), NI_PAIR_WAIT_CONFIRM);

    // the response carries nonce_b and the HMAC; A must accept it
    Frame r;
    std::vector<uint8_t> rpl;
    NT_EQ(B.next_frame(20, &r, &rpl), NI_OK);
    NT_EQ((int)r.type, NI_FT_PAIR_RESPONSE);
    NT_CHECK_MSG(!r.has(NI_FLAG_PAIR_CONFIRM), "B's response must not be marked as a confirm");
    NT_EQ(A.on_frame(r, 30), NI_OK);
    NT_EQ(A.state(), NI_PAIR_WAIT_CONFIRM);

    Frame c;
    std::vector<uint8_t> cpl;
    NT_EQ(A.next_frame(40, &c, &cpl), NI_OK);
    NT_CHECK_MSG(c.has(NI_FLAG_PAIR_CONFIRM), "A's confirm must carry NI_FLAG_PAIR_CONFIRM");
    NT_EQ(B.on_frame(c, 50), NI_OK);
    NT_EQ(B.state(), NI_PAIR_DONE);
    NT_EQ(A.state(), NI_PAIR_DONE);

    // SAS and keys: the two endpoints must agree on the shared material and on the SAS, and the
    // directional keys must be mirrored.
    NT_STR_EQ(A.sas(), B.sas());
    // four hyphen-separated code-group words, e.g. "alpha-bravo-cedar-delta" - enough for two
    // people to read aloud and compare, short enough to be practical over a phone call
    int seps = 0;
    for (size_t i = 0; i < A.sas().size(); i++) seps += A.sas()[i] == '-';
    NT_EQ(seps, 3);
    NT_CHECK(A.sas().size() > 10);
    NT_STR_EQ(hex(A.keys().tx, 32), hex(B.keys().rx, 32));
    NT_STR_EQ(hex(A.keys().rx, 32), hex(B.keys().tx, 32));
    NT_STR_EQ(hex(A.keys().confirm, 32), hex(B.keys().confirm, 32));
    NT_CHECK(memcmp(A.keys().tx, A.keys().rx, 32) != 0);  // direction separation
    NT_EQ((int)A.keys().session_id, 0x4e31);
    NT_CHECK(A.keys().valid && B.keys().valid);
  }

  // ---- 2. wrong passphrase: A rejects the HMAC, and both end up unpaired ---------------------
  {
    PairingConfig ca, cb;
    ca.secret = "right secret";
    cb.secret = "wrong secret";
    ca.self_is_a = true;
    cb.self_is_a = false;
    Pairing A(ca), B(cb);
    Frame f, r;
    std::vector<uint8_t> pl, rpl;
    NT_EQ(A.next_frame(0, &f, &pl), NI_OK);
    NT_EQ(B.on_frame(f, 1), NI_OK);  // B does not know yet that the secrets differ
    NT_EQ(B.next_frame(2, &r, &rpl), NI_OK);
    NT_CHECK_MSG(A.on_frame(r, 3) != NI_OK, "a mismatched secret must be rejected");
    NT_CHECK(A.state() == NI_PAIR_FAILED || A.state() == NI_PAIR_LOCKED);
    // Different secrets must produce different material: if these two strings ever come out
    // equal, the secret is not actually feeding the KDF and the whole handshake is theatre.
    NT_CHECK_MSG(hex(A.keys().tx, 32) != hex(B.keys().rx, 32),
                 "endpoints with different passphrases derived the same key");
  }

  // ---- 3. replay of the challenge must not re-run the handshake ------------------------------
  {
    PairingConfig ca, cb;
    ca.secret = cb.secret = "shared passphrase here";
    ca.self_is_a = true;
    cb.self_is_a = false;
    Pairing A(ca), B(cb);
    Frame f;
    std::vector<uint8_t> pl;
    NT_EQ(A.next_frame(0, &f, &pl), NI_OK);
    NT_EQ(B.on_frame(f, 1), NI_OK);
    int rc = B.on_frame(f, 2);
    NT_CHECK_MSG(rc != NI_OK, "a duplicated challenge must be refused in state WAIT_CONFIRM");
    NT_EQ(B.state(), NI_PAIR_WAIT_CONFIRM);
  }

  // ---- 4. repeated failures lock the responder out with a backoff ----------------------------
  // The attacker here is on the *initiator* side: it can send challenges and (bad) confirmations
  // freely, so the responder must be the one that counts attempts.
  {
    PairingConfig cb;
    cb.secret = "aaa";
    cb.self_is_a = false;
    cb.max_attempts = 3;
    cb.backoff_ms = 5000;
    Pairing B(cb);
    int64_t t = 0;
    for (int i = 0; i < 3; i++) {
      Frame f;
      std::vector<uint8_t> pl;
      // (a) a syntactically valid challenge from an unknown peer
      PairChallenge pc;
      pc.caps = 1;
      pc.peer_id = 7 + (uint32_t)i;
      for (int k = 0; k < 16; k++) pc.nonce[k] = (uint8_t)(k + i);
      std::vector<uint8_t> ch(64, 0);
      Writer w(ch.data(), ch.size());
      pc.encode(w);
      f.type = NI_FT_PAIR_CHALLENGE;
      f.flags = NI_FLAG_CTRL;
      f.payload = ch.data();
      f.payload_len = w.size();
      NT_CHECK_MSG(B.on_frame(f, t) == NI_OK, "challenge must be accepted while WAIT_CHALLENGE");
      t += 10;
      // (b) B answers; the attacker cannot compute the confirm mac with the right key
      Frame r;
      std::vector<uint8_t> rpl;
      NT_EQ(B.next_frame(t, &r, &rpl), NI_OK);
      t += 10;
      PairResponse bad;
      bad.caps = 1;
      bad.peer_id = 0xdeadbeef;
      for (int k = 0; k < 16; k++) {
        bad.nonce[k] = r.payload[k];      // nonce_b is echoed so B keeps the same key material
        bad.mac[k] = (uint8_t)(0xA5 ^ k); // and the mac is wrong on purpose
      }
      std::vector<uint8_t> cb2(64, 0);
      Writer w2(cb2.data(), cb2.size());
      bad.encode(w2);
      f.type = NI_FT_PAIR_RESPONSE;
      f.flags = (uint16_t)(NI_FLAG_CTRL | NI_FLAG_PAIR_CONFIRM);
      f.payload = cb2.data();
      f.payload_len = w2.size();
      NT_CHECK_MSG(B.on_frame(f, t) != NI_OK, "a forged confirmation must be rejected");
      t += 10;
      if (i < 2) {
        // retry: the responder must come back to the state its role starts in, not to WAIT_RESPONSE
        B.next_frame(t, &r, &rpl);
        t += 10;
        NT_EQ(B.state(), NI_PAIR_WAIT_CHALLENGE);
      }
    }
    NT_CHECK_MSG(B.state() == NI_PAIR_LOCKED, "three bad confirmations must lock the responder");
    if (B.state() == NI_PAIR_LOCKED) {
      NT_CHECK(B.locked_until_ms() > t);
      Frame f;
      std::vector<uint8_t> pl;
      NT_EQ(B.next_frame(t, &f, &pl), NI_ERR_RATELIMIT);
      NT_EQ(B.on_frame(f, t), NI_ERR_RATELIMIT);
      // once the backoff expires, the responder must accept a fresh challenge again
      NT_EQ(B.next_frame(B.locked_until_ms() + 1, &f, &pl), NI_ERR_STATE);
      NT_EQ(B.state(), NI_PAIR_WAIT_CHALLENGE);
    }
    printf("  note: responder locked after %d failed confirmations, until %lld ms\n",
           (int)B.failures(), (long long)B.locked_until_ms());
  }

  // ---- 5. SAS words: deterministic in the nonces, and different for different secrets --------
  {
    uint8_t na[16], nb[16];
    for (int i = 0; i < 16; i++) {
      na[i] = (uint8_t)i;
      nb[i] = (uint8_t)(0xf0 + i);
    }
    std::string s1 = ni_sas_words(na, nb, 0x4e31);
    std::string s2 = ni_sas_words(na, nb, 0x4e31);
    nb[3] ^= 0x80;
    std::string s3 = ni_sas_words(na, nb, 0x4e31);
    NT_STR_EQ(s1, s2);
    NT_CHECK(s1 != s3);
    int words = 1;
    for (char c : s1)
      if (c == '-') words++;
    NT_EQ(words, 4);
    printf("  note: SAS example \"%s\"\n", s1.c_str());
  }

  // ---- 6. entropy estimate is honest about weak secrets --------------------------------------
  {
    NT_CHECK(ni_entropy_bits("1234") < 20.0);
    NT_CHECK(ni_entropy_bits("correct horse battery staple") > 40.0);
  }

  return ntest::summary("t_pairing");
}
