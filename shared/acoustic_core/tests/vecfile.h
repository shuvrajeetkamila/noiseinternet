// vecfile.h — reader for protocol/protocol_vectors/vectors.txt (see python/gen_vectors.py).
// The format is one vector per line: `name arg arg ...`, hex for every byte string, decimal for
// lengths and counters, '#' to start a comment line. Deliberately trivial to parse so that the
// Kotlin and Win32 sides can share the same file without a JSON dependency.
#ifndef NI_VECFILE_H
#define NI_VECFILE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace vecfile {

inline std::vector<std::vector<std::string> > load(const char* name = "vectors.txt") {
  std::vector<std::vector<std::string> > out;
  const char* dir = getenv("NI_PROTOCOL_VECTORS");
  std::string path;
  if (dir && *dir) {
    path = std::string(dir) + "/" + name;
  } else {
#ifdef NI_VECTOR_DIR
    path = std::string(NI_VECTOR_DIR) + "/" + name;
#else
    path = std::string("../../protocol/protocol_vectors/") + name;
#endif
  }
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    path = std::string("protocol/protocol_vectors/") + name;  // running from the repo root
    f = fopen(path.c_str(), "rb");
  }
  if (!f) return out;
  char line[16384];
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n || line[0] == '#') continue;
    std::vector<std::string> toks;
    // Hand-rolled split instead of strtok_r/strtok_s: the POSIX and MSVC spellings of the
    // thread-safe tokenizer differ in arity, and a test helper should not care.
    size_t i = 0;
    while (i < n) {
      while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
      size_t j = i;
      while (j < n && line[j] != ' ' && line[j] != '\t') j++;
      if (j > i) toks.push_back(std::string(line + i, j - i));
      i = j;
    }
    if (!toks.empty()) out.push_back(toks);
  }
  fclose(f);
  return out;
}

inline std::string path_used() {
  const char* dir = getenv("NI_PROTOCOL_VECTORS");
  if (dir && *dir) return std::string(dir);
#ifdef NI_VECTOR_DIR
  return NI_VECTOR_DIR;
#else
  return "(default relative path)";
#endif
}

inline std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    v.push_back((uint8_t)(nib(h[i]) * 16 + nib(h[i + 1])));
  }
  return v;
}
inline std::string hex(const void* p, size_t n) {
  static const char* d = "0123456789abcdef";
  const uint8_t* b = (const uint8_t*)p;
  std::string s;
  for (size_t i = 0; i < n; i++) s += std::string{d[b[i] >> 4], d[b[i] & 15]};
  return s;
}
// bit strings in the vectors file are packed MSB-first, one bit per bit of a byte
inline std::vector<uint8_t> unpack_bits(const std::string& h) {
  std::vector<uint8_t> raw = unhex(h), bits;
  for (size_t i = 0; i < raw.size(); i++)
    for (int j = 7; j >= 0; j--) bits.push_back((uint8_t)((raw[i] >> j) & 1));
  return bits;
}
inline std::string pack_bits(const uint8_t* bits, size_t n) {
  std::vector<uint8_t> raw((n + 7) / 8, 0);
  for (size_t i = 0; i < n; i++) raw[i / 8] |= (uint8_t)((bits[i] & 1) << (7 - (i % 8)));
  return hex(raw.data(), raw.size());
}
}  // namespace vecfile
#endif
