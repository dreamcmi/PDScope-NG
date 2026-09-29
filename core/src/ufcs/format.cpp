// format.cpp — UFCS 载荷的位域取值与文本格式化（实现见 format.h）

#include "format.h"
#include "util.h"

#include <cmath>
#include <cstdio>

namespace pdscope { namespace ufcs {

uint32_t ufcsBits(const uint8_t* bytes, size_t n, int msb, int lsb) {
  uint32_t v = 0;
  for (int b = lsb; b <= msb; b++) {
    int byteIdx = static_cast<int>(n) - 1 - (b >> 3);
    if (byteIdx < 0 || byteIdx >= static_cast<int>(n)) continue;
    if ((bytes[byteIdx] >> (b & 7)) & 1) v += (1u << (b - lsb));
  }
  return v;
}

int ufcsBit(const uint8_t* bytes, size_t n, int i) {
  return static_cast<int>(ufcsBits(bytes, n, i, i));
}

std::string ufcsRange(int msb, int lsb) {
  if (msb == lsb) return "B" + std::to_string(msb);
  return "B" + std::to_string(msb) + "-" + std::to_string(lsb);
}

std::string ufcsHex(const uint8_t* bytes, size_t n) {
  std::string s;
  s.reserve(n * 2);
  char buf[3];
  for (size_t i = 0; i < n; i++) {
    std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
    s += buf;
  }
  return s;
}

uint16_t ufcsU16BE(const uint8_t* bytes, size_t n, size_t off) {
  uint16_t hi = (off < n) ? bytes[off] : 0;
  uint16_t lo = (off + 1 < n) ? bytes[off + 1] : 0;
  return static_cast<uint16_t>((hi << 8) | lo);
}

std::string ufcsNum(double v) {
  if (!std::isfinite(v)) return "NaN";
  double ip;
  if (std::modf(v, &ip) == 0.0) {
    return std::to_string(static_cast<long long>(v));
  }
  long long r = std::llround(v * 1000.0);
  double val = static_cast<double>(r) / 1000.0;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", val);
  std::string s(buf);
  std::size_t dot = s.find('.');
  if (dot != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

std::string ufcsFlag(bool on, const std::string& t, const std::string& f) {
  return on ? ("1 (" + t + ")") : ("0 (" + f + ")");
}

std::string ufcsReserved(const uint8_t* bytes, size_t n, int msb, int lsb) {
  uint32_t raw = ufcsBits(bytes, n, msb, lsb);
  if (raw == 0) return "0（未使用）";
  // 与 JS 一样是**不补零**大写（`toString(16).toUpperCase()`）—— 别用 hexU(raw, 1) 截断
  return "0x" + pdscope::hexVar(raw) + " ⚠ 规范要求此域为 0";
}

std::string ufcsVolt(int v10mv) {
  return ufcsNum(static_cast<double>(v10mv) / 100.0) + " V";
}

std::string ufcsAmp(int v10ma) {
  return ufcsNum(static_cast<double>(v10ma) / 100.0) + " A";
}

std::string ufcsTemp(int raw) {
  if (raw == 0) return "无数据（00h）";
  return std::to_string(raw - 50) + " °C";
}

std::string ufcsHexNum(int v, int digits) {
  return "0x" + pdscope::hexU(static_cast<uint64_t>(v) & 0xFFFFFFFFu, digits);
}

std::string ufcsAscii(const uint8_t* bytes, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = bytes[i];
    if (b == 0) break;
    s += (b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '·';
  }
  return s;
}

std::string ufcsHexSpaced(const uint8_t* bytes, size_t n) {
  std::string out;
  char buf[3];
  for (size_t i = 0; i < n; i++) {
    if (i) out += ' ';
    std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
    out += buf;
  }
  return out;
}

}}  // namespace pdscope::ufcs
