// crc.cpp — UFCS 协议层 CRC-8（实现见 crc.h）

#include "crc.h"

namespace pdscope { namespace ufcs {

uint8_t ufcsCrc8(const uint8_t* data, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) {
    c ^= static_cast<uint8_t>(data[i] & 0xFF);
    for (int b = 0; b < 8; b++) {
      if (c & 0x80)
        c = static_cast<uint8_t>(((c << 1) ^ kCrc8Poly) & 0xFF);
      else
        c = static_cast<uint8_t>((c << 1) & 0xFF);
    }
  }
  return c;
}

}}  // namespace pdscope::ufcs
