// svid.cpp — 见 svid.h

#include "svid.h"

#include <cstdio>

namespace pdscope { namespace pd {

const std::map<int, std::string> STANDARD_SVID = {
  {0x0000, "None / 列表结束符"},
  {0xFF00, "USB-IF PD SID（Power Delivery 规范自身）"},
  {0xFF01, "DPTC SID（DisplayPort Alt Mode）"},
};

const std::map<int, std::string> VENDOR_VID = {
  {0x03F0, "HP, Inc."},
  {0x0451, "Texas Instruments"},
  {0x046D, "Logitech, Inc."},
  {0x0483, "STMicroelectronics"},
  {0x04B4, "Cypress Semiconductor (Infineon)"},
  {0x04D8, "Microchip Technology"},
  {0x04E8, "Samsung Electronics"},
  {0x05AC, "Apple, Inc."},
  {0x05E3, "Genesys Logic, Inc."},
  {0x0B05, "ASUSTek Computer Inc."},
  {0x0B95, "ASIX Electronics Corp."},
  {0x0BB4, "HTC Corporation"},
  {0x0BDA, "Realtek Semiconductor Corp."},
  {0x0955, "NVIDIA Corp."},
  {0x12D1, "Huawei Technologies"},
  {0x174C, "ASMedia Technology Inc."},
  {0x17EF, "Lenovo"},
  {0x18D1, "Google Inc."},
  {0x1A86, "QinHeng Electronics (WCH)"},
  {0x1D6B, "Linux Foundation"},
  {0x2109, "VIA Labs, Inc."},
  {0x2717, "Xiaomi Inc."},
  {0x2E8A, "Raspberry Pi (Trading) Ltd."},
  {0x413C, "Dell Inc."},
  {0x8087, "Intel Corporation"},
};

std::optional<std::string> svidName(uint16_t svid) {
  const int v = svid & 0xFFFF;
  auto it = STANDARD_SVID.find(v);
  if (it != STANDARD_SVID.end()) return it->second;
  auto jt = VENDOR_VID.find(v);
  if (jt != VENDOR_VID.end()) return jt->second + "（厂商 ID）";
  return std::nullopt;
}

std::string svidText(uint16_t svid) {
  const int v = svid & 0xFFFF;
  auto name = svidName(static_cast<uint16_t>(v));
  char hex[8];
  std::snprintf(hex, sizeof(hex), "0x%04X", v);
  if (name) return std::string(hex) + " · " + *name;
  return std::string(hex) + " · 未登记的 SVID";
}

}}  // namespace pdscope::pd
