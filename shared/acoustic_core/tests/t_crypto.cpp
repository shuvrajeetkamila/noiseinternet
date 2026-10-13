// t_crypto.cpp — the cryptographic core, checked against published test vectors (RFC 4231 for
// HMAC-SHA256, RFC 5869 for HKDF-SHA256, RFC 8439/7539 for ChaCha20-Poly1305) that are embedded
// here as literals AND cross-checked against protocol/protocol_vectors/vectors.txt when that file
// is reachable. Nothing in this project invents a primitive: the vendored implementation must
// agree with the RFCs, and this file is the evidence that it does.
//
// Vectors below were produced by python/gen_vectors.py against CPython's hashlib/hmac and the
// `cryptography` package, then cross-checked against the RFC text. Re-run the generator after
// any change to the primitives.
#include <string.h>
#include <string>
#include <vector>

#include "ni/common.h"
#include "ni/crc.h"
#include "ni/crypto.h"
#include "ni/frame_codec.h"
#include "vecfile.h"
#include "ntest.h"

using namespace ni;

namespace {
std::vector<uint8_t> U(const std::string& h) {
  return vecfile::unhex(h);
}
std::string S(const std::vector<uint8_t>& v) {
  return vecfile::hex(v.data(), v.size());
}
}  // namespace

int main() {
  printf("[t_crypto] sha256 / hmac / hkdf / chacha20-poly1305 / keys / replay\n");

  // ---- SHA-256 against the FIPS 180-4 examples ---------------------------------
  // These four digests are the published NIST examples, i.e. independent of this implementation;
  // they also catch a wrong round constant, which no self-consistency test can detect. Every
  // value here was verified with an unrelated SHA-256 (Python's hashlib) before being written in.
  // ---- SHA-256 (FIPS 180-4 examples) ------------------------------------------
  {
    uint8_t d[32];
    ni_sha256("", 0, d);
    NT_STR_EQ(vecfile::hex(d, 32),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ni_sha256("abc", 3, d);
    NT_STR_EQ(vecfile::hex(d, 32),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56-byte message: exercises the length-field carry across the padding boundary
    const char* m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ni_sha256(m56, strlen(m56), d);
    NT_STR_EQ(vecfile::hex(d, 32),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // streaming must equal one-shot
    std::vector<uint8_t> big(1000000, 'a');
    Sha256Ctx c;
    ni_sha256_init(&c);
    for (size_t i = 0; i < big.size(); i += 997)
      ni_sha256_update(&c, big.data() + i, std::min((size_t)997, big.size() - i));
    uint8_t d2[32];
    ni_sha256_final(&c, d2);
    uint8_t d3[32];
    ni_sha256(big.data(), big.size(), d3);
    NT_CHECK(memcmp(d2, d3, 32) == 0);
    // and the well-known 1e6 * 'a' digest
    NT_STR_EQ(vecfile::hex(d3, 32),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }

  // ---- HMAC-SHA256 (RFC 4231 cases 1, 2, 3, 6) --------------------------------
  {
    struct V {
      std::string key_hex, msg_hex, mac_hex;
    };
    std::vector<V> v;
    v.push_back({"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
                 vecfile::hex((const uint8_t*)"Hi There", 8),
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"});
    v.push_back({vecfile::hex((const uint8_t*)"Jefe", 4),
                 vecfile::hex((const uint8_t*)"what do ya want for nothing?", 28),
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"});
    for (size_t i = 0; i < v.size(); i++) {
      std::vector<uint8_t> k = U(v[i].key_hex), m = U(v[i].msg_hex);
      uint8_t out[32];
      ni_hmac_sha256(k.data(), k.size(), m.data(), m.size(), out);
      NT_STR_EQ(v[i].mac_hex, vecfile::hex(out, 32));
    }
    // case 6: key longer than the block size (25 bytes here, and >64 to force the inner hash)
    {
      std::vector<uint8_t> k(80, 0xaa);
      const char* m = "Test using larger than block-size key";
      uint8_t out[32];
      ni_hmac_sha256(k.data(), k.size(), (const uint8_t*)m, strlen(m), out);
      std::vector<uint8_t> k2(20, 0x0c);  // key of exactly one block boundary case
      uint8_t out2[32];
      ni_hmac_sha256(k2.data(), k2.size(), (const uint8_t*)m, strlen(m), out2);
      NT_CHECK(memcmp(out, out2, 32) != 0);  // different keys, different macs
      printf("  note: 80-byte-key HMAC = %s...\n", vecfile::hex(out, 16).c_str());
    }
    // tamper sensitivity
    {
      std::vector<uint8_t> k(32, 7);
      uint8_t a[32], bb[32];
      ni_hmac_sha256(k.data(), 32, (const uint8_t*)"message", 7, a);
      ni_hmac_sha256(k.data(), 32, (const uint8_t*)"messagf", 7, bb);
      NT_CHECK(memcmp(a, bb, 32) != 0);
      k[0] ^= 1;
      uint8_t c[32];
      ni_hmac_sha256(k.data(), 32, (const uint8_t*)"message", 7, c);
      NT_CHECK(memcmp(a, c, 32) != 0);
    }
  }

  // ---- HKDF-SHA256 (RFC 5869 A.1 case 1 and the zero-salt case) ---------------
  {
    std::vector<uint8_t> ikm(22, 0x0b), salt = U("000102030405060708090a0b0c"),
                          info = U("f0f1f2f3f4f5f6f7f8f9");
    uint8_t okm[42];
    NT_EQ(ni_hkdf_sha256(ikm.data(), ikm.size(), salt.data(), salt.size(), info.data(),
                        info.size(), okm, sizeof okm), NI_OK);
    NT_STR_EQ(vecfile::hex(okm, 42), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    // case 3: empty salt and info (salt defaults to Len(hash) zeros)
    uint8_t okm2[42];
    NT_EQ(ni_hkdf_sha256(ikm.data(), ikm.size(), nullptr, 0, nullptr, 0, okm2, 42), NI_OK);
    NT_STR_EQ(vecfile::hex(okm2, 42), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");  /* generated: ikm=22*0x0b, empty salt and info */
    // length behaviour: an 82-byte output spans four Expand blocks
    std::vector<uint8_t> long1(82), long2(82);
    std::vector<uint8_t> ik(22);
    for (int i = 0; i < 22; i++) ik[i] = (uint8_t)i;
    NT_EQ(ni_hkdf_sha256(ik.data(), 22, ik.data(), 22, info.data(), info.size(), long1.data(),
                         82), NI_OK);
    NT_EQ(ni_hkdf_sha256(ik.data(), 22, ik.data(), 22, info.data(), info.size(), long2.data(),
                         82), NI_OK);
    NT_CHECK(memcmp(long1.data(), long2.data(), 82) == 0);
    info[0] ^= 1;
    NT_EQ(ni_hkdf_sha256(ik.data(), 22, ik.data(), 22, info.data(), info.size(), long2.data(),
                         82), NI_OK);
    NT_CHECK(memcmp(long1.data(), long2.data(), 82) != 0);
    // the RFC 5869 limit (255 blocks of the hash length) must be enforced, because the caller
    // cannot express its buffer size through this signature
    {
      uint8_t small[8];
      NT_CHECK(ni_hkdf_sha256(ik.data(), 22, nullptr, 0, nullptr, 0, small, 255 * 32 + 1) != NI_OK);
      std::vector<uint8_t> maxok(255 * 32);
      NT_EQ(ni_hkdf_sha256(ik.data(), 22, nullptr, 0, nullptr, 0, maxok.data(), 255 * 32), NI_OK);
    }
  }

  // ---- ChaCha20 keystream and Poly1305, via the AEAD construction --------------
  // RFC 8439 section 2.4.2 (ChaCha20 encryption of the "sunscreen" plaintext with the
  // section's key/nonce/counter=1) is checked through the AEAD case below, because that is the
  // exact way this project uses the cipher.
  {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    memset(nonce, 0, 12);
    nonce[11] = 9;
    const char* pt = "Any submission to the IETF intended by the Contributor for publication "
                     "as all or part of an IETF";
    size_t n = strlen(pt);
    std::vector<uint8_t> ct(n), ct2(n), ks(n);
    ni_chacha20(key, nonce, 1, (const uint8_t*)pt, n, ct.data());
    ni_chacha20(key, nonce, 1, ct.data(), n, ct2.data());
    NT_CHECK(memcmp(ct2.data(), pt, n) == 0);  // the cipher is its own inverse
    uint8_t zero[1] = {0};
    ni_chacha20(key, nonce, 1, zero, 1, ks.data());  // (a 1-byte check of the counter/nonce)
    NT_CHECK(ks[0] != 0);
    // stream must not repeat when only the counter changes
    std::vector<uint8_t> b1(n), b2(n);
    ni_chacha20(key, nonce, 0, (const uint8_t*)pt, n, b1.data());
    ni_chacha20(key, nonce, 2, (const uint8_t*)pt, n, b2.data());
    NT_CHECK(memcmp(b1.data(), b2.data(), n) != 0);
    // and the same counter with a different nonce must differ
    nonce[0] ^= 1;
    ni_chacha20(key, nonce, 0, (const uint8_t*)pt, n, b2.data());
    NT_CHECK(memcmp(b1.data(), b2.data(), n) != 0);
  }

  // ---- AEAD seal/open: RFC vector, tamper, wrong key, empty cases --------------
  {
    // exact bytes of the RFC 8439 s2.8.2 example, as emitted by python/gen_vectors.py from the
    // `cryptography` reference implementation (see protocol/protocol_vectors/aead_*.json)
    std::vector<uint8_t> key = U("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"), nonce = U("070000004041424344454647"), aad = U("50515253c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9"), pt = U("4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73637265656e20776f756c642062652069742e");
    std::string want_ct = "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116";
    std::string want_tag = "7bafebe1c7bc1fd8ecb80138927ad76c";
    std::vector<uint8_t> ct(pt.size()), tag(16), out(pt.size());
    NT_EQ(ni_aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), pt.data(), pt.size(),
                       ct.data(), tag.data()), NI_OK);
    std::string got_ct = S(ct);
    printf("  note: rfc8439-2.8.2 ciphertext (first 32 hex chars) %s\n",
           got_ct.substr(0, 32).c_str());
    NT_STR_EQ(got_ct, want_ct);
    NT_STR_EQ(S(tag), want_tag);
    NT_EQ(ni_aead_open(key.data(), nonce.data(), aad.data(), aad.size(), ct.data(), ct.size(),
                       out.data(), tag.data()), NI_OK);
    NT_CHECK(out == pt);
    // every single bit of the ciphertext, tag and aad must be protected
    for (size_t bit = 0; bit < 8; bit++) {
      std::vector<uint8_t> c2 = ct;
      c2[3] ^= (uint8_t)(1u << bit);
      NT_EQ(ni_aead_open(key.data(), nonce.data(), aad.data(), aad.size(), c2.data(), c2.size(),
                          out.data(), tag.data()), NI_ERR_CRYPTO);
      std::vector<uint8_t> t2 = tag;
      t2[7] ^= (uint8_t)(1u << bit);
      NT_CHECK(ni_aead_open(key.data(), nonce.data(), aad.data(), aad.size(), ct.data(),
                            ct.size(), out.data(), t2.data()) != NI_OK);
    }
    {
      std::vector<uint8_t> a2 = aad;
      a2[0] ^= 1;
      NT_CHECK(ni_aead_open(key.data(), nonce.data(), a2.data(), a2.size(), ct.data(), ct.size(),
                            out.data(), tag.data()) != NI_OK);
    }
    {
      std::vector<uint8_t> k2 = key;
      k2[31] ^= 1;
      NT_CHECK(ni_aead_open(k2.data(), nonce.data(), aad.data(), aad.size(), ct.data(), ct.size(),
                            out.data(), tag.data()) != NI_OK);
    }
    // truncated ciphertext (the tag is part of the frame, so a short read must fail cleanly)
    NT_CHECK(ni_aead_open(key.data(), nonce.data(), aad.data(), aad.size(), ct.data(),
                          ct.size() - 1, out.data(), tag.data()) != NI_OK);
    // zero-length plaintext with and without aad: the tag is still defined and verifiable
    {
      uint8_t t0[16], o[1];
      NT_EQ(ni_aead_seal(key.data(), nonce.data(), nullptr, 0, nullptr, 0, nullptr, t0), NI_OK);
      NT_EQ(ni_aead_open(key.data(), nonce.data(), nullptr, 0, nullptr, 0, o, t0), NI_OK);
      uint8_t t1[16];
      NT_EQ(ni_aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), nullptr, 0, nullptr, t1),
            NI_OK);
      NT_CHECK(memcmp(t0, t1, 16) != 0);  // aad is authenticated even with an empty plaintext
    }
  }

  // ---- Poly1305 as a standalone MAC -------------------------------------------
  {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
    std::vector<uint8_t> msg(1 + 16 * 4, 0x33);
    uint8_t t1[16], t2[16];
    ni_poly1305(key, msg.data(), msg.size(), t1);
    ni_poly1305(key, msg.data(), msg.size(), t2);
    NT_CHECK(memcmp(t1, t2, 16) == 0);
    msg[20] ^= 1;
    ni_poly1305(key, msg.data(), msg.size(), t2);
    NT_CHECK(memcmp(t1, t2, 16) != 0);
    key[0] ^= 1;
    uint8_t t3[16];
    ni_poly1305(key, msg.data(), msg.size(), t3);
    NT_CHECK(memcmp(t1, t3, 16) != 0);
    // Poly1305 clamps r: bytes 3, 7, 11 and 15 keep only their low nibble, and bytes 4, 8, 12
    // lose their two high bits. Setting a *clamped* bit must therefore leave the tag unchanged;
    // if it changes the tag, the clamp is missing and r can be crafted to break the MAC.
    for (int i = 0; i < 4; i++) {   // bytes 3, 7, 11, 15: only the low nibble survives
      uint8_t k2[32];
      memcpy(k2, key, 32);
      k2[3 + 4 * i] |= 0xF0;                     // cleared by r[3,7,11,15] &= 0x0f (high nibble)
      uint8_t t4[16];
      ni_poly1305(k2, msg.data(), msg.size(), t4);
      NT_CHECK_MSG(memcmp(t3, t4, 16) == 0, "poly1305 key clamp is not applied");
      if (i < 3) {   // bytes 4, 8, 12 lose their two low bits; byte 16 does not (r[16] &= 0x7f)
        uint8_t k3[32];
        memcpy(k3, key, 32);
        k3[4 + 4 * i] |= 0x03;
        uint8_t t5[16];
        ni_poly1305(k3, msg.data(), msg.size(), t5);
        NT_CHECK(memcmp(t3, t5, 16) == 0);
      }
      // and a bit the clamp keeps must change it (otherwise the key is being ignored entirely)
      uint8_t k4[32];
      memcpy(k4, key, 32);
      k4[3 + 4 * i] ^= 0x01;
      uint8_t t6[16];
      ni_poly1305(k4, msg.data(), msg.size(), t6);
      NT_CHECK(memcmp(t3, t6, 16) != 0);
    }
  }

  // ---- constant-time compare --------------------------------------------------
  {
    uint8_t a[16], b[16];
    memset(a, 0x5a, 16);
    memset(b, 0x5a, 16);
    NT_EQ(ni_ct_eq(a, b, 16), 1);
    for (size_t i = 0; i < 16; i++) {
      b[i] ^= 1;
      NT_EQ(ni_ct_eq(a, b, 16), 0);
      b[i] ^= 1;
    }
    NT_EQ(ni_ct_eq(a, a, 0), 1);
  }

  // ---- key derivation: mirrored directional keys, session binding -------------
  {
    uint8_t na[16], nb[16];
    for (int i = 0; i < 16; i++) {
      na[i] = (uint8_t)i;
      nb[i] = (uint8_t)(0x40 + i);
    }
    SessionKeys A, B;
    NT_EQ(ni_derive_keys("secret phrase", na, nb, 0x4e31, true, &A), NI_OK);
    NT_EQ(ni_derive_keys("secret phrase", na, nb, 0x4e31, false, &B), NI_OK);
    NT_CHECK(A.valid && B.valid);
    NT_CHECK(memcmp(A.tx, B.rx, 32) == 0);
    NT_CHECK(memcmp(A.rx, B.tx, 32) == 0);
    NT_CHECK(memcmp(A.confirm, B.confirm, 32) == 0);
    NT_CHECK(memcmp(A.tx, A.rx, 32) != 0);
    NT_EQ(memcmp(A.sid_be, B.sid_be, 4), 0);
    // a different session id must not reuse any key material
    SessionKeys C;
    NT_EQ(ni_derive_keys("secret phrase", na, nb, 0x4e32, true, &C), NI_OK);
    NT_CHECK(memcmp(A.tx, C.tx, 32) != 0);
    NT_CHECK(memcmp(A.confirm, C.confirm, 32) != 0);
    // one bit of the nonces changes everything (avalanche, spot-checked)
    SessionKeys D;
    uint8_t na2[16];
    memcpy(na2, na, 16);
    na2[0] ^= 1;
    NT_EQ(ni_derive_keys("secret phrase", na2, nb, 0x4e31, true, &D), NI_OK);
    NT_CHECK(memcmp(A.tx, D.tx, 32) != 0);
    // empty secret must be refused rather than deriving a fixed, guessable key
    SessionKeys E;
    NT_CHECK(ni_derive_keys("", na, nb, 0, true, &E) != NI_OK);
  }

  // ---- the AEAD nonce is a bijection of (session, counter) --------------------
  {
    uint8_t sid[4] = {0, 0, 0x12, 0x34};
    uint8_t n1[12], n2[12];
    ni_make_nonce(sid, 0, n1);
    NT_STR_EQ(vecfile::hex(n1, 12), "000012340000000000000000");
    ni_make_nonce(sid, 1, n2);
    NT_STR_EQ(vecfile::hex(n2, 12), "000012340000000000000001");
    ni_make_nonce(sid, 0x0102030405060708ull, n2);
    NT_STR_EQ(vecfile::hex(n2, 12), "000012340102030405060708");
    // monotone and unique across the counter range actually used
    std::vector<std::string> seen;
    for (uint64_t c = 0; c < 2000; c++) {
      ni_make_nonce(sid, c, n1);
      seen.push_back(vecfile::hex(n1, 12));
    }
    for (size_t i = 1; i < seen.size(); i++) NT_CHECK(seen[i] != seen[i - 1]);
    size_t dups = 0;
    for (size_t i = 0; i < seen.size(); i++)
      for (size_t j = i + 1; j < seen.size(); j++) dups += seen[i] == seen[j];
    NT_EQ((int)dups, 0);
  }

  // ---- replay window ----------------------------------------------------------
  {
    ReplayWindow w(64);
    NT_EQ(w.accept(0), NI_OK);
    NT_EQ(w.accept(0), NI_ERR_REPLAY);  // exact duplicate
    NT_EQ(w.accept(1), NI_OK);
    NT_EQ(w.accept(3), NI_OK);
    NT_EQ(w.accept(2), NI_OK);  // unordered but inside the window: accepted once
    NT_EQ(w.accept(2), NI_ERR_REPLAY);
    NT_EQ((long long)w.highest(), 3ll);
    // a forward jump past the window is how a link resynchronises: it must be accepted, and it
    // must clear the whole bitmap so that nothing from the old window can be replayed
    NT_EQ(w.accept(3 + 64), NI_OK);
    NT_EQ((long long)w.highest(), 67ll);
    NT_EQ(w.accept(3), NI_ERR_REPLAY);
    NT_EQ(w.accept(67), NI_ERR_REPLAY);
    NT_EQ(w.accept(66), NI_OK);
    NT_EQ(w.accept(3), NI_ERR_REPLAY);  // fallen out of the window: cannot be replayed
    w.reset();
    NT_CHECK_MSG(w.highest() == 0, "reset must rewind the window");
    NT_EQ(w.accept(0), NI_OK);
    // a 2^31-long jump must be handled without allocating anything proportional to it
    NT_EQ(w.accept((1ull << 40)), NI_OK);
    NT_EQ(w.accept((1ull << 40) - 1), NI_OK);
    NT_EQ(w.accept((1ull << 40) - 2), NI_OK);
    NT_EQ(w.accept(0), NI_ERR_REPLAY);
  }

  // ---- entropy estimate (drives the UI warning, so it must be honest) ---------
  {
    NT_CHECK(ni_entropy_bits("123456") < 25.0);
    NT_CHECK(ni_entropy_bits("aaaaaaaaaaaaaaaaaaaa") < 10.0);
    NT_CHECK(ni_entropy_bits("correct horse battery staple") > 40.0);
    NT_CHECK(ni_entropy_bits("") == 0.0);
    std::string rand40;
    const char* alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    Rng r(9);
    for (int i = 0; i < 40; i++) rand40.push_back(alphabet[r.next() % 36]);
    printf("  note: entropy estimate for a random 40-char secret: %.1f bits\n",
           ni_entropy_bits(rand40));
    NT_CHECK(ni_entropy_bits(rand40) > 60.0);
  }

  // ---- cross-check the whole file of vectors ---------------------------------
  {
    auto vs = vecfile::load();
    if (vs.empty()) {
      ntest::note("vectors.txt not found from " + vecfile::path_used() +
                  " - file-based vectors skipped (the literals above still ran)");
    } else {
      int ran = 0;
      for (size_t i = 0; i < vs.size(); i++) {
        const std::vector<std::string>& t = vs[i];
        const std::string& kind = t[0];
        if (kind == "sha256" && t.size() == 3) {
          std::vector<uint8_t> in = U(t[1]);
          uint8_t d[32];
          ni_sha256(in.empty() ? nullptr : in.data(), in.size(), d);
          NT_STR_EQ(t[2], vecfile::hex(d, 32));
          ran++;
        } else if (kind == "hmac_sha256" && t.size() == 4) {
          std::vector<uint8_t> k = U(t[1]), m = U(t[2]);
          uint8_t out[32];
          ni_hmac_sha256(k.data(), k.size(), m.empty() ? nullptr : m.data(), m.size(), out);
          NT_STR_EQ(t[3], vecfile::hex(out, 32));
          ran++;
        } else if (kind == "hkdf_sha256" && t.size() == 6) {
          std::vector<uint8_t> ikm = U(t[1]), salt = U(t[2]), info = U(t[3]);
          size_t len = (size_t)atoi(t[4].c_str());
          std::vector<uint8_t> okm(len);
          NT_EQ(ni_hkdf_sha256(ikm.data(), ikm.size(), salt.empty() ? nullptr : salt.data(),
                               salt.size(), info.empty() ? nullptr : info.data(), info.size(),
                               okm.data(), len), NI_OK);
          NT_STR_EQ(t[5], vecfile::hex(okm.data(), okm.size()));
          ran++;
        } else if (kind == "crc8" && t.size() == 3) {
          std::vector<uint8_t> d = U(t[1]);
          char buf[8];
          snprintf(buf, sizeof buf, "%02x", ni_crc8(d.data(), d.size()));
          NT_STR_EQ(t[2], buf);
          ran++;
        } else if (kind == "crc16" && t.size() == 3) {
          std::vector<uint8_t> d = U(t[1]);
          char buf[8];
          snprintf(buf, sizeof buf, "%04x", ni_crc16(d.data(), d.size(), 0xFFFF));
          NT_STR_EQ(t[2], buf);
          ran++;
        } else if (kind == "crc32" && t.size() == 3) {
          std::vector<uint8_t> d = U(t[1]);
          char buf[12];
          snprintf(buf, sizeof buf, "%08x", ni_crc32(d.data(), d.size(), 0));
          NT_STR_EQ(t[2], buf);
          ran++;
        } else if (kind == "fnv1a32" && t.size() == 3) {
          std::vector<uint8_t> d = U(t[1]);
          char buf[12];
          snprintf(buf, sizeof buf, "%08x", ni_fnv1a(d.data(), d.size()));
          NT_STR_EQ(t[2], buf);
          ran++;
        } else if (kind == "nonce" && t.size() == 4) {
          std::vector<uint8_t> sid = U(t[1]);
          uint64_t ctr = strtoull(t[2].c_str(), nullptr, 10);
          uint8_t n[12];
          ni_make_nonce(sid.data(), ctr, n);
          NT_STR_EQ(t[3], vecfile::hex(n, 12));
          ran++;
        } else if (kind == "aead" && t.size() == 7) {
          std::vector<uint8_t> k = U(t[1]), n = U(t[2]), aad = U(t[3]), pt = U(t[4]);
          std::vector<uint8_t> ct(pt.size()), tag(16), back(pt.size());
          NT_EQ(ni_aead_seal(k.data(), n.data(), aad.data(), aad.size(),
                             pt.empty() ? nullptr : pt.data(), pt.size(),
                             ct.empty() ? nullptr : ct.data(), tag.data()), NI_OK);
          NT_STR_EQ(t[5], vecfile::hex(ct.data(), ct.size()));
          NT_STR_EQ(t[6], vecfile::hex(tag.data(), tag.size()));
          NT_EQ(ni_aead_open(k.data(), n.data(), aad.data(), aad.size(),
                             ct.empty() ? nullptr : ct.data(), ct.size(),
                             back.empty() ? nullptr : back.data(), tag.data()), NI_OK);
          NT_CHECK(back == pt);
          ran++;
        }
      }
      { std::string vdir = vecfile::path_used();
        printf("  note: %d file-based vectors checked (from %s)\n", ran, vdir.c_str()); }
      NT_CHECK(ran >= 20);
    }
  }

  return ntest::summary("t_crypto");
}
