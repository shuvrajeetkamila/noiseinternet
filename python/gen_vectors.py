#!/usr/bin/env python3
"""gen_vectors.py — regenerate protocol/protocol_vectors/*.json from reference implementations.

The C++ core is checked against these files by tests/t_vectors.cpp and tests/t_crypto.cpp, and
the Android (Kotlin) and Windows shells are expected to do the same, so a vector is only as good
as the *independent* implementation that produced it. Sources used here:

  hashlib/hmac   -> SHA-256, HMAC-SHA256, and HKDF-SHA256 built from the RFC 5869 construction
  cryptography   -> ChaCha20-Poly1305 (RFC 8439/7539 AEAD construction)
  fixed tables   -> CRC-16/CCITT-FALSE, CRC-32/IEEE, CRC-8 (poly 0x07, init 0xFF), FNV-1a, all
                    computed by the bitwise definitions in this file, not by the C++ code

Run:  python3 python/gen_vectors.py [output-dir]
"""
import hashlib
import hmac
import json
import os
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), os.pardir,
                                                          "protocol", "protocol_vectors")
OUT = os.path.abspath(OUT)


def b(x):
    return x.hex() if isinstance(x, (bytes, bytearray)) else "%02x" % x


# ---------------------------------------------------------------- hashes and MACs
def sha256(x):
    return hashlib.sha256(x).digest()


def hmac_sha256(k, m):
    return hmac.new(k, m, hashlib.sha256).digest()


def hkdf_sha256(ikm, salt, info, length):
    """RFC 5869 construction, written out here on purpose: it is the definition the C++ must
    match, and re-deriving it in the generator catches a silent change on either side."""
    prk = hmac_sha256(salt, ikm)
    okm = b""
    t = b""
    counter = 1
    while len(okm) < length:
        t = hmac_sha256(prk, t + info + bytes([counter]))
        okm += t
        counter += 1
    return okm[:length]


# ---------------------------------------------------------------- CRCs by definition
def crc8(p):  # poly 0x07, init 0xFF, no reflection  (the burst-descriptor and frame header crc)
    c = 0xFF
    for x in p:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16_ccitt_false(p):  # poly 0x1021, init 0xFFFF, no reflection, no xorout
    c = 0xFFFF
    for x in p:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def crc32_ieee(p):  # reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF
    c = 0xFFFFFFFF
    for x in p:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    return c ^ 0xFFFFFFFF


def fnv1a32(p):
    h = 0x811C9DC5
    for x in p:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------- scrambler (phy side)
def scramble(bits, state=0x1AAA, mask=0x7FFF):
    """1/(1 + x^14 + x^15), self-synchronising, register fed with the OUTPUT bits.
    Bit i of the register: 0 = most recent. Taps are the 14th and 15th most recent outputs."""
    out = []
    r = state & mask
    for x in bits:
        fb = ((r >> 13) ^ (r >> 14)) & 1
        o = (x ^ fb) & 1
        out.append(o)
        r = ((r << 1) | o) & mask
    return out


def descramble(bits, state=0x1AAA, mask=0x7FFF):
    out = []
    r = state & mask
    for y in bits:
        fb = ((r >> 13) ^ (r >> 14)) & 1
        out.append((y ^ fb) & 1)
        r = ((r << 1) | y) & mask
    return out


# ---------------------------------------------------------------- convolutional code
G1, G2 = 0x5B, 0x79  # octal 133, 171 -> rate 1/2, K=7, d_free=10


def parity(v):
    p = 0
    while v:
        p ^= v & 1
        v >>= 1
    return p


def conv_encode(bits):
    reg = 0
    out = []
    for x in list(bits) + [0] * 6:
        reg = ((reg << 1) | x) & 0x7F
        out.append(parity(reg & G1))
        out.append(parity(reg & G2))
    return out


def d_free():
    """Minimum nonzero-path weight by breadth-first search over the difference trellis."""
    INF = 1 << 30
    best = INF
    cur = {}
    for b in (1,):
        reg = b
        w = parity(reg & G1) + parity(reg & G2)
        ns = reg & 63
        if ns == 0:
            best = min(best, w)
        else:
            cur[ns] = min(cur.get(ns, INF), w)
    for _ in range(40):
        nxt = {}
        for s, w in cur.items():
            for i in (0, 1):
                reg = ((s << 1) | i) & 0x7F
                nw = w + parity(reg & G1) + parity(reg & G2)
                if nw >= best:
                    continue
                ns = reg & 63
                if ns == 0:
                    best = min(best, nw)
                else:
                    if nxt.get(ns, INF) > nw:
                        nxt[ns] = nw
        cur = nxt
        if not cur:
            break
    return best


def gray(v):
    return v ^ (v >> 1)


# ---------------------------------------------------------------- vectors
def write(name, obj):
    path = os.path.join(OUT, name)
    with open(path, "w") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True,
                  default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o))
        fh.write("\n")
    print("wrote", os.path.relpath(path, os.path.dirname(OUT)))


def main():
    os.makedirs(OUT, exist_ok=True)
    flat = []
    check = b"123456789"
    # (flat collects the line-oriented vectors written to vectors.txt at the end of main)

    write("hashes.json", {
        "_comment": "generated by python/gen_vectors.py; see that file for the independent "
                    "implementation of every value here",
        "sha256": [{"input_hex": "", "sha256_hex": b(sha256(b""))},
                   {"input_hex": b"abc".hex(), "sha256_hex": b(sha256(b"abc"))},
                   {"input_hex": check.hex(), "sha256_hex": b(sha256(check))}],
        "hmac_sha256": [  # also appended to `flat` below
            {"key_hex": "%02x" % 11 * 20, "msg_hex": b"Hi There".hex(),
             "mac_hex": b(hmac_sha256(b"\x0b" * 20, b"Hi There"))},
            {"key_hex": b"Jefe", "msg_hex": b"what do ya want for nothing?".hex(),
             "mac_hex": b(hmac_sha256(b"Jefe", b"what do ya want for nothing?"))},
            {"key_hex": "%02x" % 0xAA * 20, "msg_hex": (b"\xdd" * 50).hex(),
             "mac_hex": b(hmac_sha256(b"\xaa" * 20, b"\xdd" * 50))},
            {"key_hex": "\x01" * 25, "msg_hex": b"Test Using Larger Than Block-Size Key - Hash "
                                                 b"Key First".hex(),
             "mac_hex": b(hmac_sha256(b"\x01" * 25,
                                      b"Test Using Larger Than Block-Size Key - Hash Key First"))},
        ],
        # RFC 5869 test cases 1 and 2, plus a long-output case (multi-block expand)
        "hkdf_sha256": [  # also appended to `flat` below
            {"ikm_hex": "%02x" % 11 * 22, "salt_hex": bytes(range(13)).hex(),
             "info_hex": bytes(range(0xF0, 0xFA)).hex(), "length": 42,
             "okm_hex": b(hkdf_sha256(b"\x0b" * 22, bytes(range(13)), bytes(range(0xF0, 0xFA)),
                                      42))},
            {"ikm_hex": bytes(range(22)).hex(), "salt_hex": bytes(range(22)).hex(),
             "info_hex": bytes(range(10)).hex(), "length": 82,
             "okm_hex": b(hkdf_sha256(bytes(range(22)), bytes(range(22)), bytes(range(10)), 82))},
            {"ikm_hex": "%02x" % 11 * 22, "salt_hex": "", "info_hex": "", "length": 42,
             "okm_hex": b(hkdf_sha256(b"\x0b" * 22, b"", b"", 42))},
        ],
    })

    for k, m in [(b"\x0b" * 20, b"Hi There"), (b"Jefe", b"what do ya want for nothing?"),
                 (b"\xaa" * 20, b"\xdd" * 50), (b"\x01" * 25,
                  b"Test Using Larger Than Block-Size Key - Hash Key First"),
                 (bytes(range(32)), bytes(range(64)))]:
        flat.append(("hmac_sha256", [b(k), b(m), b(hmac_sha256(k, m))]))
    for ikm, salt, info, ln in [(b"\x0b" * 22, bytes(range(13)), bytes(range(0xF0, 0xFA)), 42),
                                (bytes(range(22)), bytes(range(22)), bytes(range(10)), 82),
                                (b"\x0b" * 22, b"", b"", 42),
                                (bytes(range(40)), bytes(range(8)), b"noise-internet/v1", 96)]:
        flat.append(("hkdf_sha256", [b(ikm), b(salt), b(info), ln,
                                     b(hkdf_sha256(ikm, salt, info, ln))]))

    write("checksums.json", {
        "_comment": "CRC/FNV check values for the ASCII string 123456789 and for the empty "
                    "string, computed by the bitwise definitions in python/gen_vectors.py",
        "crc8_poly07_initff": {"123456789": "0x%02x" % crc8(check), "empty": "0x%02x" % crc8(b"")},
        "crc16_ccitt_false": {"123456789": "0x%04x" % crc16_ccitt_false(check),
                       "empty": "0x%04x" % crc16_ccitt_false(b"")},
        "crc32_ieee": {"123456789": "0x%08x" % crc32_ieee(check),
                    "empty": "0x%08x" % crc32_ieee(b"")},
        "fnv1a32": {"123456789": "0x%08x" % fnv1a32(check), "empty": "0x%08x" % fnv1a32(b""),
                "a": "0x%08x" % fnv1a32(b"a")},
    })
    for data in [b"", b"a", check, bytes(range(64))]:
        h = b(data)
        flat.append(("crc8", [h, "%02x" % crc8(data)]))
        flat.append(("crc16", [h, "%04x" % crc16_ccitt_false(data)]))
        flat.append(("crc32", [h, "%08x" % crc32_ieee(data)]))
        flat.append(("fnv1a32", [h, "%08x" % fnv1a32(data)]))
        flat.append(("sha256", [h, b(sha256(data))]))

    # scrambler + convolutional code, over a deterministic bit stream
    bits = []
    s = 0x12345678
    for _ in range(1024):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        bits.append(s & 1)
    scr = scramble(bits)
    write("phy_coding.json", {
        "_comment": "bit order is MSB-first within every byte; the scrambler register is 15 bits "
                    "wide, initialised to 0x1AAA, and fed with the output (transmitted) bits",
        "scrambler": {"poly": "1/(1+x^14+x^15)", "init": "0x1aaa", "mask": "0x7fff",
                      "input_bits_hex": b"".join(struct.pack(">B", v) for v in bits).hex(),
                      "scrambled_bits_hex": b"".join(struct.pack(">B", v) for v in scr).hex(),
                      "round_trip_ok": descramble(scr) == bits},
        "convolutional": {"rate": "1/2", "K": 7, "g1_octal": "133", "g2_octal": "171",
                          "g1": "0x%02x" % G1, "g2": "0x%02x" % G2, "tail_bits": 6,
                          "d_free": d_free(),
                          "impulse_response": conv_encode([1]),
                          "coded_bits_hex": b"".join(
                              struct.pack(">B", v) for v in conv_encode(bits[:400])).hex()},
        "gray": [{"bits": i, "gray": gray(i)} for i in range(8)],
    })
    flat.append(("scramble", [pack_bits(bits), pack_bits(scr)]))
    flat.append(("cencode", [pack_bits(bits[:400]), pack_bits(conv_encode(bits[:400]))]))

    # AEAD: the RFC 8439 section 2.8.2 vector, then random cases through a reference library
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    rfc_key = bytes.fromhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")
    rfc_nonce = bytes.fromhex("070000004041424344454647")
    rfc_aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9")
    rfc_pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
              b"the future, sunscreen would be it.")
    aead = ChaCha20Poly1305(rfc_key)
    rfc_ct = aead.encrypt(rfc_nonce, rfc_pt, rfc_aad)
    cases = [{"name": "rfc8439-2.8.2", "key_hex": b(rfc_key), "nonce_hex": b(rfc_nonce),
              "aad_hex": b(rfc_aad), "pt_hex": b(rfc_pt),
              "ct_hex": b(rfc_ct[:-16]), "tag_hex": b(rfc_ct[-16:])}]
    import random
    rnd = random.Random(20240607)
    for i in range(12):
        k = bytes(rnd.randrange(256) for _ in range(32))
        n = bytes(rnd.randrange(256) for _ in range(12))
        al = rnd.randrange(0, 40)
        pl = rnd.randrange(0, 220)
        aad = bytes(rnd.randrange(256) for _ in range(al))
        pt = bytes(rnd.randrange(256) for _ in range(pl))
        ct = ChaCha20Poly1305(k).encrypt(n, pt, aad)
        cases.append({"name": "random-%02d" % i, "key_hex": b(k), "nonce_hex": b(n),
                      "aad_hex": b(aad), "pt_hex": b(pt), "ct_hex": b(ct[:-16]),
                      "tag_hex": b(ct[-16:])})
    for c in cases:
        flat.append(("aead", [c["key_hex"], c["nonce_hex"], c["aad_hex"], c["pt_hex"],
                             c["ct_hex"], c["tag_hex"]]))
    write("aead_chacha20_poly1305.json", {
        "_comment": "ChaCha20-Poly1305 as used by NOISE INTERNET: 12-byte nonce, tag over "
                    "aad||ct||len-blocks, ciphertext from counter 1. Generated with the "
                    "cryptography package (Rust backend); the first case is RFC 8439 s2.8.2.",
        "cases": cases,
    })

    # AEAD nonce construction (crypto.cpp ni_make_nonce): 4-byte session id then the 64-bit
    # counter, both big endian. It is pinned because nonce reuse under one key is the one
    # mistake that breaks a stream cipher completely.
    write("nonce_construction.json", {  # noqa: E303
        "_comment": "96-bit AEAD nonce = sid_be(4) || counter_be(8). The counter is the sequence "
                    "number carried on the wire, and it is never rewound while a key is live.",
        "cases": [{"sid_be_hex": b(struct.pack(">I", sid)), "counter": ctr,
                   "nonce_hex": b(struct.pack(">IQ", sid, ctr))}
                  for sid, ctr in [(0x4e31, 0), (0x4e31, 1), (1, 0xFFFFFFFF), (0, 2 ** 64 - 1),
                                   (0x10203040, 0x0102030405060708)]],
    })
    for sid, ctr in [(0x4e31, 0), (0x4e31, 1), (1, 0xFFFFFFFF), (0, 2 ** 64 - 1),
                     (0x10203040, 0x0102030405060708)]:
        flat.append(("nonce", [b(struct.pack(">I", sid)), ctr, b(struct.pack(">IQ", sid, ctr))]))
    write_flat(flat)


def pack_bits(bits):
    out = bytearray()
    for i in range(0, len(bits), 8):
        v = 0
        for j, x in enumerate(bits[i:i + 8]):
            v |= (x & 1) << (7 - j)
        out.append(v)
    return out.hex()


def unpack_bits(h):
    out = []
    for c in bytes.fromhex(h):
        for j in range(8):
            out.append((c >> (7 - j)) & 1)
    return out


def write_flat(cases):
    """protocol_vectors/vectors.txt: one check per line, `name arg arg ...`, everything else
    hex. Flat text is deliberate: every platform has to read this file, including the smallest
    embedded one, so the format must not need a JSON parser."""
    lines = ["# noise-internet protocol vectors, generated by python/gen_vectors.py",
             "# sha256 <input_hex> <digest_hex>",
             "# hmac_sha256 <key_hex> <msg_hex> <mac_hex>",
             "# hkdf_sha256 <ikm_hex> <salt_hex> <info_hex> <len> <okm_hex>",
             "# crc8 <data_hex> <value_hex>       (poly 0x07, init 0xff, no reflection)",
             "# crc16 <data_hex> <value_hex>       (CCITT-FALSE: poly 0x1021, init 0xffff)",
             "# crc32 <data_hex> <value_hex>       (IEEE 802.3, reflected)",
             "# fnv1a32 <data_hex> <value_hex>",
             "# scramble <bits_hex> <bits_hex>     (1/(1+x^14+x^15), init 0x1aaa, MSB first)",
             "# cencode <bits_hex> <bits_hex>      (rate-1/2 K=7, G=133/171 octal, 6-bit flush)",
             "# aead <key_hex> <nonce_hex> <aad_hex> <pt_hex> <ct_hex> <tag_hex>",
             "# nonce <sid_be_hex> <counter_dec> <nonce_hex>",
             ""]
    for kind, args in cases:
        lines.append(" ".join([kind] + [str(a) for a in args]))
    path = os.path.join(OUT, "vectors.txt")
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote", os.path.relpath(path, os.path.dirname(OUT)), "(%d vectors)" % (len(lines) - 13))


if __name__ == "__main__":
    main()
