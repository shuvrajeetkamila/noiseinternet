// crc.cpp
#include "ni/crc.h"
#include "ni/common.h"
#include <mutex>

namespace ni {
namespace {
struct Tables {
  uint32_t c32[256];
  uint16_t c16[256];
  bool ready = false;
};
Tables g;
std::once_flag g_once;

void build() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    g.c32[i] = c;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint16_t c = (uint16_t)(i << 8);
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    g.c16[i] = c;
  }
  g.ready = true;
}
}  // namespace

void ni_crc_tables_init() { std::call_once(g_once, build); }

uint32_t ni_crc32(const void* data, size_t n, uint32_t seed) {
  ni_crc_tables_init();
  const uint8_t* p = (const uint8_t*)data;
  uint32_t c = seed ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = g.c32[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint16_t ni_crc16(const void* data, size_t n, uint16_t seed) {
  ni_crc_tables_init();
  const uint8_t* p = (const uint8_t*)data;
  uint16_t c = seed;
  for (size_t i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ g.c16[((c >> 8) ^ p[i]) & 0xFF]);
  return c;
}

uint32_t ni_fnv1a(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace ni
