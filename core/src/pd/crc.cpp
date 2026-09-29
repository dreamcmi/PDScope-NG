// crc.cpp — 见 crc.h

#include "crc.h"

namespace pdscope { namespace pd {

namespace {
const uint32_t* table() {
  static uint32_t t[256];
  static bool inited = false;
  if (!inited) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[n] = c;
    }
    inited = true;
  }
  return t;
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
  const uint32_t* t = table();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return (c ^ 0xFFFFFFFFu);
}

uint32_t crc32(const std::vector<uint8_t>& data) {
  return crc32(data.data(), data.size());
}

}}  // namespace pdscope::pd
