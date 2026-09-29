// format.cpp — 见 format.h

#include "format.h"

#include <cstdio>

namespace pdscope { namespace pd {

std::string pdBin(uint32_t v, int msb, int lsb) {
  int n = msb - lsb + 1;
  std::string s = "0b" + std::to_string(pdField(v, msb, lsb));
  if (static_cast<int>(s.size()) - 2 < n)
    s.insert(2, n - (static_cast<int>(s.size()) - 2), '0');
  return s;
}

std::string pdReserved(uint32_t v, int msb, int lsb) {
  uint32_t raw = pdField(v, msb, lsb);
  if (raw == 0) return "0（未使用）";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%X", raw);
  return std::string(buf) + " ⚠ 规范要求此域为 0";
}

std::string pdLookup(const std::map<int, std::string>& table, int key,
                     const std::string& fallback) {
  auto it = table.find(key);
  if (it == table.end()) return fallback + " (" + std::to_string(key) + ")";
  return it->second;
}

std::vector<uint8_t> pdBytes4(uint32_t v) {
  return {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
          static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
}

uint32_t pdBytesToU32(const std::vector<uint8_t>& bytes) {
  uint32_t v = 0;
  for (size_t i = 0; i < bytes.size() && i < 4; i++)
    v |= (static_cast<uint32_t>(bytes[i]) & 0xFFu) << (8 * i);
  return v;
}

std::string pdAscii(const std::vector<uint8_t>& bytes) {
  std::string s;
  for (uint8_t b : bytes) {
    if (b == 0) break;
    s += (b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '·';
  }
  // 按空白字符裁剪两端
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

std::string pdCharPair(uint8_t lo, uint8_t hi) {
  if (!lo && !hi) return "";
  auto c = [](uint8_t v) -> char {
    return (v >= 0x20 && v <= 0x7E) ? static_cast<char>(v) : '·';
  };
  std::string s;
  s += c(lo);
  s += c(hi);
  return s;
}

}}  // namespace pdscope::pd
