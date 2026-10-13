// crc.h — CRC16-CCITT (header guard) and CRC32 (IEEE 802.3, used by Ethernet/zlib).
// Both are table driven, table built once at first use, no dynamic allocation.
#ifndef NI_CRC_H
#define NI_CRC_H
#include <stddef.h>
#include <stdint.h>
namespace ni {
void ni_crc_tables_init();
uint16_t ni_crc16(const void* data, size_t n, uint16_t seed = 0xFFFF);
uint32_t ni_crc32(const void* data, size_t n, uint32_t seed = 0);
// 32-bit FNV-1a used for fast content checks in tests and the leak detector.
uint32_t ni_fnv1a(const void* data, size_t n);
}  // namespace ni
#endif
