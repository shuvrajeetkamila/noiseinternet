// crypto.cpp — SHA-256 / HMAC / HKDF / ChaCha20 / Poly1305 / AEAD + pairing + replay window.
// Written to FIPS 180-4, RFC 2104, RFC 5869 and RFC 8439; tests/crypto.cpp checks every one of
// them against the published test vectors, so "we wrote our own crypto" here means "we wrote
// standard crypto and proved it against the RFCs", not "we invented a cipher".
#include "ni/crypto.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>
namespace ni {

namespace {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint32_t le32(const uint8_t* p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
inline void wr32be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
inline void wr32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline void wr64le(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
// The 64 round constants, i.e. the first 32 bits of the fractional part of the cube root of each
// of the first 64 primes. t_crypto recomputes this table from that definition, because a single
// mistyped word silently breaks every digest - as one here once did (index 38 read 0x81c1979b,
// which is not a SHA-256 constant at all), and that one word made HMAC, HKDF and therefore every
// session key non-standard while all the round-trip tests kept passing.
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

void sha256_block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

void ni_sha256_init(Sha256Ctx* c) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; i++) c->h[i] = h0[i];
  c->len = 0;
  c->nbuf = 0;
}
void ni_sha256_update(Sha256Ctx* c, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  c->len += n;
  while (n) {
    size_t take = 64 - c->nbuf;
    if (take > n) take = n;
    memcpy(c->buf + c->nbuf, p, take);
    c->nbuf += take;
    p += take;
    n -= take;
    if (c->nbuf == 64) {
      sha256_block(c->h, c->buf);
      c->nbuf = 0;
    }
  }
}
void ni_sha256_final(Sha256Ctx* c, uint8_t out[32]) {
  uint64_t bits = c->len * 8ull;
  uint8_t pad = 0x80;
  ni_sha256_update(c, &pad, 1);
  uint8_t z = 0;
  while (c->nbuf != 56) ni_sha256_update(c, &z, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
  c->len -= 8;  // the length field is not counted in the message length
  ni_sha256_update(c, lenb, 8);
  for (int i = 0; i < 8; i++) wr32be(out + 4 * i, c->h[i]);
}
void ni_sha256(const void* data, size_t n, uint8_t out[32]) {
  Sha256Ctx c;
  ni_sha256_init(&c);
  ni_sha256_update(&c, data, n);
  ni_sha256_final(&c, out);
}
std::string ni_hex(const void* data, size_t n) {
  static const char* d = "0123456789abcdef";
  const uint8_t* p = (const uint8_t*)data;
  std::string s;
  s.resize(n * 2);
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}

void ni_hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen,
                   uint8_t out[32]) {
  uint8_t k[64] = {0}, ipad[64], opad[64], tmp[32];
  if (klen > 64) {
    ni_sha256(key, klen, k);
  } else {
    memcpy(k, key, klen);
  }
  for (int i = 0; i < 64; i++) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5c);
  }
  Sha256Ctx c;
  ni_sha256_init(&c);
  ni_sha256_update(&c, ipad, 64);
  ni_sha256_update(&c, msg, mlen);
  ni_sha256_final(&c, tmp);
  ni_sha256_init(&c);
  ni_sha256_update(&c, opad, 64);
  ni_sha256_update(&c, tmp, 32);
  ni_sha256_final(&c, out);
}

int ni_hkdf_sha256(const uint8_t* ikm, size_t ilen, const uint8_t* salt, size_t slen,
                   const uint8_t* info, size_t inlen, uint8_t* okm, size_t olen) {
  if (olen > 255 * 32) return NI_ERR_PARAM;
  uint8_t prk[32], t[32];
  uint8_t zs[32] = {0};
  if (!salt || !slen) {
    salt = zs;
    slen = 32;
  }
  ni_hmac_sha256(salt, slen, ikm, ilen, prk);
  size_t done = 0;
  int n = 0;
  t[0] = 0;
  while (done < olen) {
    n++;
    uint8_t cbuf[1 + 32 + 1 + 1024];
    size_t o = 0;
    if (n > 1) {
      memcpy(cbuf, t, 32);
      o = 32;
    }
    memcpy(cbuf + o, info, inlen);
    o += inlen;
    cbuf[o++] = (uint8_t)n;
    uint8_t mac[32];
    ni_hmac_sha256(prk, 32, cbuf, o, mac);
    size_t take = olen - done;
    if (take > 32) take = 32;
    memcpy(okm + done, mac, take);
    memcpy(t, mac, 32);
    done += take;
  }
  return NI_OK;
}

// ---------------------------------------------------------------- ChaCha20
namespace {
#define QR(a, b, c, d)                                     \
  a += b; d ^= a; d = (d << 16) | (d >> 16);               \
  c += d; b ^= c; b = (b << 12) | (b >> 20);               \
  a += b; d ^= a; d = (d << 8) | (d >> 24);                \
  c += d; b ^= c; b = (b << 7) | (b >> 25);
void chacha_block(uint32_t out[16], const uint32_t in[16]) {
  uint32_t x[16];
  memcpy(x, in, 64);
  for (int i = 0; i < 10; i++) {
    QR(x[0], x[4], x[8], x[12]);
    QR(x[1], x[5], x[9], x[13]);
    QR(x[2], x[6], x[10], x[14]);
    QR(x[3], x[7], x[11], x[15]);
    QR(x[0], x[5], x[10], x[15]);
    QR(x[1], x[6], x[11], x[12]);
    QR(x[2], x[7], x[8], x[13]);
    QR(x[3], x[4], x[9], x[14]);
  }
  for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}
#undef QR
}  // namespace

void ni_chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                 const uint8_t* in, size_t n, uint8_t* out) {
  uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
  st[12] = counter;
  for (int i = 0; i < 3; i++) st[13 + i] = le32(nonce + 4 * i);
  uint8_t ks[64];
  for (size_t off = 0; off < n; off += 64) {
    chacha_block((uint32_t*)ks, st);
    st[12]++;
    size_t take = n - off < 64 ? n - off : 64;
    for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
  }
}

// ---------------------------------------------------------------- Poly1305 (base 2^26)
void ni_poly1305(const uint8_t key[32], const uint8_t* m, size_t bytes, uint8_t out[16]) {
  uint32_t t, r0, r1, r2, r3, r4, s1, s2, s3, s4;
  uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
  uint64_t d0, d1, d2, d3, d4, c;
  t = le32(key + 0); r0 = t & 0x3ffffff;
  t = le32(key + 3); r1 = (t >> 2) & 0x3ffff03;
  t = le32(key + 6); r2 = (t >> 4) & 0x3ffc0ff;
  t = le32(key + 9); r3 = (t >> 6) & 0x3f03fff;
  t = le32(key + 12); r4 = (t >> 8) & 0x00fffff;
  s1 = r1 * 5; s2 = r2 * 5; s3 = r3 * 5; s4 = r4 * 5;
  while (bytes > 0) {
    uint8_t blk[16];
    size_t nb = bytes < 16 ? bytes : 16;
    memset(blk, 0, 16);
    memcpy(blk, m, nb);
    // the padding bit: a 0x01 byte right after the data (or the implicit bit 128 for a
    // full block) -- this is what makes Poly1305 immune to truncation/reordering.
    if (nb < 16) blk[nb] = 1;
    uint32_t pad = (nb == 16) ? 0x1000000u : 0u;
    t = le32(blk + 0); h0 += t & 0x3ffffff;
    t = le32(blk + 3); h1 += (t >> 2) & 0x3ffffff;
    t = le32(blk + 6); h2 += (t >> 4) & 0x3ffffff;
    t = le32(blk + 9); h3 += (t >> 6) & 0x3ffffff;
    t = le32(blk + 12);
    h4 += (t >> 8) | pad;
    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 +
         (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 +
         (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 +
         (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 +
         (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 +
         (uint64_t)h4 * r0;
    h0 = (uint32_t)d0 & 0x3ffffff;
    c = d0 >> 26;
    d1 += c; h1 = (uint32_t)d1 & 0x3ffffff;
    c = d1 >> 26;
    d2 += c; h2 = (uint32_t)d2 & 0x3ffffff;
    c = d2 >> 26;
    d3 += c; h3 = (uint32_t)d3 & 0x3ffffff;
    c = d3 >> 26;
    d4 += c; h4 = (uint32_t)d4 & 0x3ffffff;
    c = d4 >> 26;
    h0 += (uint32_t)c * 5;
    h1 += h0 >> 26; h0 &= 0x3ffffff;
    m += nb;
    bytes -= nb;
  }
  // fully carry h
  c = h1 >> 26; h1 &= 0x3ffffff; h2 += (uint32_t)c;
  c = h2 >> 26; h2 &= 0x3ffffff; h3 += (uint32_t)c;
  c = h3 >> 26; h3 &= 0x3ffffff; h4 += (uint32_t)c;
  c = h4 >> 26; h4 &= 0x3ffffff; h0 += (uint32_t)c * 5;
  c = h0 >> 26; h0 &= 0x3ffffff; h1 += (uint32_t)c;
  // compute h + -p
  uint32_t g0 = h0 + 5, c0 = g0 >> 26; g0 &= 0x3ffffff;
  uint32_t g1 = h1 + c0, c1 = g1 >> 26; g1 &= 0x3ffffff;
  uint32_t g2 = h2 + c1, c2 = g2 >> 26; g2 &= 0x3ffffff;
  uint32_t g3 = h3 + c2, c3 = g3 >> 26; g3 &= 0x3ffffff;
  uint32_t g4 = h4 + c3 - (1u << 26);
  // select h if h < p, or h + -p if h >= p
  uint32_t mask = (g4 >> ((sizeof(uint32_t) * 8) - 1)) - 1;  // 0xffffffff if no borrow
  g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
  // h = h % (2^128)
  h0 = (h0 | (h1 << 26)) & 0xffffffffu;
  h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
  h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
  h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;
  // mac = (h + pad) % (2^128)
  uint64_t f;
  f = (uint64_t)h0 + le32(key + 16); h0 = (uint32_t)f;
  f = (uint64_t)h1 + le32(key + 20) + (f >> 32); h1 = (uint32_t)f;
  f = (uint64_t)h2 + le32(key + 24) + (f >> 32); h2 = (uint32_t)f;
  f = (uint64_t)h3 + le32(key + 28) + (f >> 32); h3 = (uint32_t)f;
  wr32le(out + 0, h0);
  wr32le(out + 4, h1);
  wr32le(out + 8, h2);
  wr32le(out + 12, h3);
}

// ---------------------------------------------------------------- AEAD
int ni_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                 const uint8_t* pt, size_t plen, uint8_t* ct, uint8_t tag[16]) {
  uint8_t block0[64] = {0}, k[32] = {0};
  ni_chacha20(key, nonce, 0, block0, 32, k);
  ni_chacha20(key, nonce, 1, pt, plen, ct);
  std::vector<uint8_t> macin;
  macin.reserve(alen + 16 + plen + 16 + 16);
  macin.insert(macin.end(), aad, aad + alen);
  while (macin.size() % 16) macin.push_back(0);
  size_t off = macin.size();
  macin.resize(off + plen);
  memcpy(&macin[off], ct, plen);
  while (macin.size() % 16) macin.push_back(0);
  uint8_t lb[16];
  memset(lb, 0, 16);
  wr64le(lb, (uint64_t)alen);
  wr64le(lb + 8, (uint64_t)plen);
  macin.insert(macin.end(), lb, lb + 16);
  ni_poly1305(k, &macin[0], macin.size(), tag);
  return NI_OK;
}
int ni_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                 const uint8_t* ct, size_t clen, uint8_t* pt, const uint8_t tag[16]) {
  uint8_t block0[64] = {0}, k[32] = {0}, calc[16];
  ni_chacha20(key, nonce, 0, block0, 32, k);
  std::vector<uint8_t> macin;
  macin.reserve(alen + 16 + clen + 16 + 16);
  macin.insert(macin.end(), aad, aad + alen);
  while (macin.size() % 16) macin.push_back(0);
  macin.insert(macin.end(), ct, ct + clen);
  while (macin.size() % 16) macin.push_back(0);
  uint8_t lb[16];
  memset(lb, 0, 16);
  wr64le(lb, (uint64_t)alen);
  wr64le(lb + 8, (uint64_t)clen);
  macin.insert(macin.end(), lb, lb + 16);
  ni_poly1305(k, &macin[0], macin.size(), calc);
  if (ni_ct_eq(calc, tag, 16) != 1) return NI_ERR_CRYPTO;
  ni_chacha20(key, nonce, 1, ct, clen, pt);
  return NI_OK;
}

int ni_ct_eq(const void* a, const void* b, size_t n) {
  const uint8_t* x = (const uint8_t*)a;
  const uint8_t* y = (const uint8_t*)b;
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
  return d == 0 ? 1 : 0;
}

double ni_entropy_bits(const std::string& s) {
  int cnt[256] = {0};
  for (unsigned char c : s) cnt[c]++;
  double h = 0;
  size_t n = s.size();
  if (!n) return 0;
  for (int i = 0; i < 256; i++)
    if (cnt[i]) {
      double p = (double)cnt[i] / (double)n;
      h -= p * log2(p);
    }
  return h * (double)n;
}

void ni_make_nonce(const uint8_t sid_be[4], uint64_t ctr, uint8_t out[12]) {
  memcpy(out, sid_be, 4);
  for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
}

int ni_derive_keys(const std::string& secret, const uint8_t nonce_a[16], const uint8_t nonce_b[16],
                   uint16_t session_id, bool self_is_a, SessionKeys* out) {
  if (secret.empty()) return NI_ERR_PARAM;
  uint8_t salt[34];
  memcpy(salt, nonce_a, 16);
  memcpy(salt + 16, nonce_b, 16);
  salt[32] = (uint8_t)(session_id >> 8);
  salt[33] = (uint8_t)session_id;
  const char* info = "noise-internet/v1 session keys";
  uint8_t okm[96];
  int rc = ni_hkdf_sha256((const uint8_t*)secret.data(), secret.size(), salt, sizeof salt,
                          (const uint8_t*)info, strlen(info), okm, sizeof okm);
  if (rc != NI_OK) return rc;
  if (self_is_a) {
    memcpy(out->tx, okm, 32);
    memcpy(out->rx, okm + 32, 32);
  } else {
    memcpy(out->tx, okm + 32, 32);
    memcpy(out->rx, okm, 32);
  }
  memcpy(out->confirm, okm + 64, 32);
  out->session_id = session_id ? session_id : (uint16_t)(((uint16_t)okm[64] << 8 | okm[65]) | 1);
  out->sid_be[0] = 0;
  out->sid_be[1] = 0;
  out->sid_be[2] = (uint8_t)(out->session_id >> 8);
  out->sid_be[3] = (uint8_t)out->session_id;
  out->valid = true;
  return NI_OK;
}

std::string ni_sas_words(const uint8_t nonce_a[16], const uint8_t nonce_b[16],
                         uint16_t session_id) {
  static const char* words[] = {
      "alpha", "bravo", "cedar", "delta", "ember", "forty", "globe", "hazel",
      "ivory", "joker", "kappa", "lunar", "mango", "noble", "oscar", "pilot",
      "quartz", "robin", "solar", "tiger", "ultra", "vivid", "walnut", "xylem",
      "yacht", "zephyr", "amber", "birch", "coral", "dune", "eagle", "fjord",
      "grain", "harbor", "island", "jasper", "kite", "lotus", "meadow", "nectar",
      "onyx", "pepper", "quill", "ridge", "sand", "thistle", "umber", "vale",
      "willow", "xenon", "yield", "zenith", "atlas", "basil", "clay", "dew",
      "elm", "fern", "gale", "heath", "iris", "juniper", "koala", "laurel"};
  // The alphabet must hold exactly 64 entries, because six bits of the digest index it. It once
  // had 62, and the last two slots were null pointers, so any pairing whose SAS happened to draw
  // index 62 or 63 died in strlen(NULL). Deriving the size keeps the assert honest.
  static_assert(sizeof(words) / sizeof(words[0]) == 64, "SAS alphabet must have 64 words");
  uint8_t buf[34];
  memcpy(buf, nonce_a, 16);
  memcpy(buf + 16, nonce_b, 16);
  buf[32] = (uint8_t)(session_id >> 8);
  buf[33] = (uint8_t)session_id;
  uint8_t h[32];
  ni_sha256(buf, sizeof buf, h);
  std::string s;
  for (int i = 0; i < 4; i++) {
    if (i) s += "-";
    s += words[((h[2 * i] << 8) | h[2 * i + 1]) & 63];
  }
  return s;
}

// ---------------------------------------------------------------- replay window
ReplayWindow::ReplayWindow(uint32_t span) : high_(0), span_(span < 32 ? 32 : span) {
  win_.assign(span_, 0);
}
void ReplayWindow::reset() {
  high_ = 0;
  started_ = false;
  memset(&win_[0], 0, span_);
}
int ReplayWindow::accept(uint64_t ctr) {
  if (ctr > UINT64_MAX / 2) return NI_ERR_PARAM;
  if (!started_) {
    started_ = true;
    high_ = ctr;
    memset(&win_[0], 0, span_);
    win_[0] = 1;
    return NI_OK;
  }
  if (ctr > high_) {
    uint64_t sh = ctr - high_;
    if (sh >= span_) {
      memset(&win_[0], 0, span_);
    } else {
      memmove(&win_[sh], &win_[0], (size_t)(span_ - sh));
      // The shift moves every recorded marker up by `sh`; the sh slots it vacates at the bottom
      // now describe counters between high_-sh+1 and high_-1, which have NOT been seen. Leaving
      // them as they were made the window reject valid out-of-order frames - exactly the frames
      // the selective-retransmit ARQ delivers - so they must be cleared.
      memset(&win_[0], 0, (size_t)sh);
    }
    high_ = ctr;
    win_[0] = 1;
    return NI_OK;
  }
  uint64_t back = high_ - ctr;
  if (back >= span_) return NI_ERR_REPLAY;  // older than the window: refuse
  if (win_[back]) return NI_ERR_REPLAY;  // duplicate
  win_[back] = 1;
  return NI_OK;
}

// ---------------------------------------------------------------- pairing
static const char* kStateNames[] = {"idle",  "wait-challenge", "wait-response",
                                    "wait-confirm", "paired",   "failed", "locked"};

Pairing::Pairing(const PairingConfig& cfg) : cfg_(cfg) {
  state_ = cfg_.self_is_a ? NI_PAIR_WAIT_RESPONSE : NI_PAIR_WAIT_CHALLENGE;
  // Nonces come from a process-local counter mixed with the pairing secret hash. This is
  // deliberately NOT wall-clock based so the automated tests stay deterministic while two
  // Pairing objects in one process still get different nonces.
  static uint64_t seq = 0x517cc1ull;
  uint8_t sh[32];
  ni_sha256(cfg_.secret.data(), cfg_.secret.size(), sh);
  uint64_t seed = (seq++) * 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 8; i++) seed ^= (uint64_t)sh[i] << (8 * i);
  Rng r(seed);
  r.bytes(nonce_a_, 16);
  caps_ = cfg_.caps;
}

const char* Pairing::state_name() const {
  if (state_ < 0 || state_ > NI_PAIR_LOCKED) return "?";
  return kStateNames[state_];
}

int Pairing::next_frame(int64_t now_ms, Frame* out, std::vector<uint8_t>* payload) {
  if (now_ms < lock_until_) return NI_ERR_RATELIMIT;
  *out = Frame();
  out->session_id = cfg_.session_id;
  out->mod_id = 0;
  out->seq = 0;
  payload->assign(64, 0);
  switch (state_) {
    case NI_PAIR_WAIT_CHALLENGE:
      return NI_ERR_STATE;  // peer must go first
    case NI_PAIR_WAIT_RESPONSE: {
      PairChallenge pc;
      pc.caps = cfg_.caps;
      pc.peer_id = ((uint32_t)nonce_a_[0] << 24) | ((uint32_t)nonce_a_[1] << 16) |
                   ((uint32_t)nonce_a_[2] << 8) | nonce_a_[3];
      memcpy(pc.nonce, nonce_a_, 16);
      Writer w(payload->data(), payload->size());
      pc.encode(w);
      if (w.error()) return w.error();
      payload->resize(w.size());
      // the Frame carries a pointer into the caller's payload vector: set it here so that no
      // caller can forget and hand a length-0 frame to the peer's on_frame().
      out->payload = payload->data();
      out->payload_len = (uint16_t)payload->size();
      out->type = NI_FT_PAIR_CHALLENGE;
      out->flags = NI_FLAG_CTRL;
      return NI_OK;
    }
    case NI_PAIR_WAIT_CONFIRM: {
      // Both endpoints speak from this state, with the same payload type and different roles:
      //   B (responder) answers the challenge with nonce_b + HMAC -> "PAIR_RESPONSE".
      //   A (initiator) proves it derived the same keys -> same shape plus NI_FLAG_PAIR_CONFIRM.
      // Only one frame is ever emitted per endpoint from here (confirm_sent_), and B stays in
      // WAIT_CONFIRM afterwards because it must still hear A's confirmation before it is DONE.
      if (confirm_sent_) return NI_ERR_STATE;
      uint8_t buf[64];
      memcpy(buf, nonce_a_, 16);
      memcpy(buf + 16, nonce_b_, 16);
      uint8_t mac[32];
      ni_hmac_sha256(keys_.confirm, 32, buf, 32, mac);
      PairResponse pr;
      pr.caps = caps_;
      pr.peer_id = ((uint32_t)nonce_b_[0] << 24) | ((uint32_t)nonce_b_[1] << 16) |
                   ((uint32_t)nonce_b_[2] << 8) | nonce_b_[3];
      memcpy(pr.nonce, nonce_b_, 16);
      memcpy(pr.mac, mac, 16);
      Writer w(payload->data(), payload->size());
      pr.encode(w);
      if (w.error()) return w.error();
      payload->resize(w.size());
      out->payload = payload->data();
      out->payload_len = (uint16_t)payload->size();
      out->type = NI_FT_PAIR_RESPONSE;
      out->flags = cfg_.self_is_a ? (uint16_t)(NI_FLAG_CTRL | NI_FLAG_PAIR_CONFIRM)
                                  : (uint16_t)NI_FLAG_CTRL;
      confirm_sent_ = true;
      if (cfg_.self_is_a) state_ = NI_PAIR_DONE;  // A is proved-key once it has confirmed
      return NI_OK;
    }
    case NI_PAIR_DONE:
      return NI_ERR_STATE;
    case NI_PAIR_FAILED:
      // "the user pressed retry": each side must return to the state its role starts in, else a
      // responder that had one bad passphrase would turn into an initiator and the handshake
      // could never be restarted from either side.
      state_ = cfg_.self_is_a ? NI_PAIR_WAIT_RESPONSE : NI_PAIR_WAIT_CHALLENGE;
      return NI_ERR_STATE;
    case NI_PAIR_LOCKED:
      if (now_ms >= lock_until_) {
        state_ = cfg_.self_is_a ? NI_PAIR_WAIT_RESPONSE : NI_PAIR_WAIT_CHALLENGE;
        return NI_ERR_STATE;
      }
      return NI_ERR_RATELIMIT;
    default:
      return NI_ERR_STATE;
  }
}

int Pairing::on_frame(const Frame& f, int64_t now_ms) {
  if (now_ms < lock_until_) return NI_ERR_RATELIMIT;
  if (f.type == NI_FT_PAIR_CHALLENGE && state_ == NI_PAIR_WAIT_CHALLENGE) {
    PairChallenge pc;
    Reader r(f.payload, f.payload_len);
    if (pc.decode(r) != NI_OK) return NI_ERR_TYPE;
    memcpy(nonce_a_, pc.nonce, 16);
    caps_ = pc.caps;
    peer_id_ = pc.peer_id;
    // a fresh attempt must clear the "already answered" latch, otherwise a responder that has
    // replied once could never reply again, no matter how many times the user retried
    confirm_sent_ = false;
    got_confirm_ = false;
    uint8_t ah[32];
    ni_sha256(nonce_a_, 16, ah);
    uint64_t seed2 = 0;
    for (int i = 0; i < 8; i++) seed2 ^= (uint64_t)ah[i] << (8 * i);
    Rng r2(seed2);
    r2.bytes(nonce_b_, 16);
    if (ni_derive_keys(cfg_.secret, nonce_a_, nonce_b_, cfg_.session_id, false, &keys_) != NI_OK)
      return NI_ERR_CRYPTO;
    sas_ = ni_sas_words(nonce_a_, nonce_b_, keys_.session_id);
    state_ = NI_PAIR_WAIT_CONFIRM;
    return NI_OK;
  }
  if (f.type == NI_FT_PAIR_RESPONSE) {
    PairResponse pr;
    Reader r(f.payload, f.payload_len);
    if (pr.decode(r) != NI_OK) return NI_ERR_TYPE;
    bool is_confirm = f.has(NI_FLAG_PAIR_CONFIRM);
    if (state_ == NI_PAIR_WAIT_RESPONSE && !is_confirm) {
      memcpy(nonce_b_, pr.nonce, 16);
      caps_ = pr.caps;
      peer_id_ = pr.peer_id;
      if (ni_derive_keys(cfg_.secret, nonce_a_, nonce_b_, cfg_.session_id, true, &keys_) != NI_OK)
        return NI_ERR_CRYPTO;
      uint8_t buf[64];
      memcpy(buf, nonce_a_, 16);
      memcpy(buf + 16, nonce_b_, 16);
      uint8_t mac[32];
      ni_hmac_sha256(keys_.confirm, 32, buf, 32, mac);
      if (ni_ct_eq(mac, pr.mac, 16) != 1) {
        if (++failures_ >= (uint64_t)cfg_.max_attempts) {
          state_ = NI_PAIR_LOCKED;
          lock_until_ = now_ms + cfg_.backoff_ms * (int64_t)(failures_ + 1);
          return NI_ERR_RATELIMIT;
        }
        state_ = NI_PAIR_FAILED;
        return NI_ERR_CRYPTO;
      }
      sas_ = ni_sas_words(nonce_a_, nonce_b_, keys_.session_id);
      state_ = NI_PAIR_WAIT_CONFIRM;  // must now send CONFIRM
      return NI_OK;
    }
    if (state_ == NI_PAIR_WAIT_CONFIRM && is_confirm) {
      uint8_t buf[64];
      memcpy(buf, nonce_a_, 16);
      memcpy(buf + 16, nonce_b_, 16);
      uint8_t mac[32];
      ni_hmac_sha256(keys_.confirm, 32, buf, 32, mac);
      if (ni_ct_eq(mac, pr.mac, 16) != 1) {
        // The responder is the party that faces a guessing attacker, so its confirm check must
        // feed the same attempt counter and backoff as the initiator's; without it an attacker
        // could try confirmations at B forever.
        if (++failures_ >= (uint64_t)cfg_.max_attempts) {
          state_ = NI_PAIR_LOCKED;
          lock_until_ = now_ms + cfg_.backoff_ms * (int64_t)(failures_ + 1);
          return NI_ERR_RATELIMIT;
        }
        state_ = NI_PAIR_FAILED;
        return NI_ERR_CRYPTO;
      }
      got_confirm_ = true;
      state_ = NI_PAIR_DONE;
      return NI_OK;
    }
    return NI_ERR_STATE;
  }
  return NI_ERR_TYPE;
}

}  // namespace ni
