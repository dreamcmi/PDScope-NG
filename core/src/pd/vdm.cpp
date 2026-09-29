// vdm.cpp — 见 vdm.h

#include "vdm.h"
#include "decoder.h"
#include "format.h"
#include "tables.h"
#include "svid.h"

namespace pdscope { namespace pd {

namespace {

std::string binPad(int v, int n) {
  std::string s = "0b";
  for (int i = n - 1; i >= 0; i--) s += ((v >> i) & 1) ? '1' : '0';
  return s;
}

std::string stripParen(const std::string& s) {
  size_t p = s.find('（');
  if (p == std::string::npos) return s;
  return s.substr(0, p);
}

std::vector<std::string> dedupe(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (const auto& x : in) {
    if (x.empty()) continue;
    bool found = false;
    for (const auto& y : out) if (y == x) { found = true; break; }
    if (!found) out.push_back(x);
  }
  return out;
}

std::string join_(const std::vector<std::string>& v, const std::string& sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) { if (i) s += sep; s += v[i]; }
  return s;
}

// 辅助：把 SVID 列表格式化成 0xXXXX
std::vector<std::string> mapHex(const std::vector<uint16_t>& list);

bool is3x(const PdCtx& ctx) { return ctx.revText == "3.x"; }

void pushNonAckFact(std::vector<std::string>& facts, const std::string& ack,
                    const std::vector<uint32_t>& vdos, const std::string& cmdName) {
  if (ack == "REQ") return;
  const std::string extra = vdos.empty() ? "" : "，且不应携带数据对象";
  if (ack == "NAK") facts.push_back("对端未实现/拒绝 " + cmdName + extra);
  else facts.push_back("⚠ " + ack + " 不是 " + cmdName + " 的合法响应（规范只允许 ACK/NAK）" + extra);
}

std::string cableSpeedText(int spd) {
  const std::string b = binPad(spd, 3);
  const std::string t = tbl(USB_HIGHEST_SPEED, spd, "");
  if (t.empty()) return b + " · 无效取值（不得使用）";
  return spd >= 3 ? (b + " · " + t + "（PD 3.0 时该编码为保留值）") : (b + " · " + t);
}

std::string vmaxNote(int code, const PdCtx& ctx) {
  if (code != 1 && code != 2) return "";
  return is3x(ctx) ? "（PD 3.0 下这两个码是 30 V / 40 V）" : "（PD 3.1 起已废弃，接收端按 20 V 处理）";
}

struct IdInfo {
  int ptUfp = 0, ptDfp = 0, conn = 0, vid = 0;
  std::string productTypeText;
  std::string vdoKind;
  bool cable = false;
  int host = 0, device = 0, modal = 0;
};

void idHeaderVdo(pdscope::DetailEmitter& em, uint32_t v, const PdCtx& ctx, IdInfo& info) {
  const bool cable = ctx.link == "cable";
  const int ptUfp = static_cast<int>(pdField(v, 29, 27));
  const int ptDfp = static_cast<int>(pdField(v, 25, 23));
  const int conn = static_cast<int>(pdField(v, 22, 21));
  const int vid = static_cast<int>(pdField(v, 15, 0));

  em.detail("USB Host 能力 [B31]", pdFlag(pdBit(v, 31), "可枚举 USB 设备", "不可作 USB Host"));
  em.detail("USB Device 能力 [B30]", pdFlag(pdBit(v, 30), "可被枚举为 USB 设备", "不可作 USB 设备"));

  std::string productTypeText, vdoKind;
  if (cable) {
    productTypeText = tbl(PRODUCT_TYPE_CABLE, ptUfp, "");
    const std::string* k = productTypeVdoKind("cable", ptUfp);
    vdoKind = k ? *k : "";
    em.detail("Product Type (Cable Plug/VPD) [" + pdRange(29, 27) + "]",
      binPad(ptUfp, 3) + "b · " + productTypeText);
  } else {
    productTypeText = tbl(PRODUCT_TYPE_UFP, ptUfp, "");
    const std::string* k = productTypeVdoKind("ufp", ptUfp);
    vdoKind = k ? *k : "";
    em.detail("Product Type (UFP) [" + pdRange(29, 27) + "]",
      binPad(ptUfp, 3) + "b · " + productTypeText);
  }
  em.detail("Modal Operation Supported [B26]", pdFlag(pdBit(v, 26),
    "支持 Alternate Mode（会响应 Discover SVIDs/Modes）", "不支持 Alternate Mode"));
  if (cable) {
    em.detail("Product Type (DFP) [" + pdRange(25, 23) + "]",
      "Reserved（SOP' 链路不使用）· 0x" + pdscope::hexU(ptDfp, 1));
  } else {
    em.detail("Product Type (DFP) [" + pdRange(25, 23) + "]",
      binPad(ptDfp, 3) + "b · " + tbl(PRODUCT_TYPE_DFP, ptDfp, ""));
  }
  em.detail("Connector Type [" + pdRange(22, 21) + "]", binPad(conn, 2) + "b · " + tbl(CONNECTOR_TYPE, conn, ""));
  em.detail("Reserved [" + pdRange(20, 16) + "]", "0x" + pdscope::hexU(pdField(v, 20, 16), 1));
  const std::string vidHex = "0x" + pdscope::hexU(vid, 4);
  auto vname = svidName(static_cast<uint16_t>(vid));
  em.detail("USB Vendor ID [" + pdRange(15, 0) + "]", vidHex
    + (vname ? (" · " + vname->substr(0, vname->find("（厂商 ID）"))) : ""));

  info.ptUfp = ptUfp; info.ptDfp = ptDfp; info.conn = conn; info.vid = vid;
  info.productTypeText = productTypeText; info.vdoKind = vdoKind; info.cable = cable;
  info.host = static_cast<int>(pdBit(v, 31)); info.device = static_cast<int>(pdBit(v, 30));
  info.modal = static_cast<int>(pdBit(v, 26));
}

uint32_t certStatVdo(pdscope::DetailEmitter& em, uint32_t v) {
  em.detail("XID [B31-0]", pdHex(v) + (v == 0 ? "（厂商未申请 XID）" : ""));
  return v;
}

void productVdo(pdscope::DetailEmitter& em, uint32_t v) {
  const int pid = static_cast<int>(pdField(v, 31, 16)), bcd = static_cast<int>(pdField(v, 15, 0));
  em.detail("USB Product ID [B31-16]", "0x" + pdscope::hexU(pid, 4));
  em.detail("bcdDevice [B15-0]", "0x" + pdscope::hexU(bcd, 4));
}

void ufpVdo(pdscope::DetailEmitter& em, uint32_t v, std::vector<std::string>& facts) {
  const int ver = static_cast<int>(pdField(v, 31, 29));
  em.detail("UFP VDO Version [" + pdRange(31, 29) + "]", tbl(UFP_VDO_VERSION, ver, "无效取值（不得使用）") + " · " + binPad(ver, 3) + "b");
  em.detail("Reserved [B28]", std::to_string(pdBit(v, 28)));
  const int usb4 = static_cast<int>(pdBit(v, 27)), usb32 = static_cast<int>(pdBit(v, 26));
  const int usb2 = static_cast<int>(pdField(v, 25, 24));
  em.detail("USB4 Device 能力 [B27]", pdFlag(usb4, "USB4 capable", "否"));
  em.detail("USB 3.2 Device 能力 [B26]", pdFlag(usb32, "USB 3.2 capable", "否"));
  em.detail("USB 2.0 Device 能力 [" + pdRange(25, 24) + "]", binPad(usb2, 2) + "b · " + tbl(UFP_USB2, usb2, ""));
  em.detail("Connector Type (Legacy) [" + pdRange(23, 22) + "]", "00b（已废弃，连接器类型看 ID Header VDO）");
  em.detail("Reserved [" + pdRange(21, 11) + "]", "0x" + pdscope::hexU(pdField(v, 21, 11), 1));
  const int vconnReq = static_cast<int>(pdBit(v, 7)), vbusReq = static_cast<int>(pdBit(v, 6));
  em.detail("VCONN Power [" + pdRange(10, 8) + "]", vconnReq
    ? tbl(VCONN_POWER, static_cast<int>(pdField(v, 10, 8)), "")
    : ("Reserved（未要求 VCONN）· 0x" + pdscope::hexU(pdField(v, 10, 8), 1)));
  em.detail("VCONN Required [B7]", pdFlag(vconnReq, "需要 VCONN 才能工作", "不需要"));
  em.detail("VBUS Required [B6]", pdBit(v, 6) ? "1（不需要 VBUS）" : "0（需要 VBUS）");
  em.detail("No Signal Reconfig Alt Mode [B5]", pdFlag(pdBit(v, 5), "支持不改动信号的 Alt Mode", "否"));
  em.detail("Non-TBT3 Signal Reconfig Alt Mode [B4]", pdFlag(pdBit(v, 4), "支持改动信号的 Alt Mode（TBT3 除外）", "否"));
  em.detail("TBT3 Alt Mode [B3]", pdFlag(pdBit(v, 3), "支持 Thunderbolt 3 Alt Mode", "否"));
  const int spd = static_cast<int>(pdField(v, 2, 0));
  em.detail("USB Highest Speed [" + pdRange(2, 0) + "]", binPad(spd, 3) + "b · " + tbl(USB_HIGHEST_SPEED, spd, "无效取值（不得使用）"));

  std::vector<std::string> caps;
  if (usb4) caps.push_back("USB4");
  if (usb32) caps.push_back("USB3.2");
  if (usb2 == 2) caps.push_back("USB2");
  if (!caps.empty()) facts.push_back("UFP " + join_(caps, "/"));
  if (USB_HIGHEST_SPEED.count(spd)) facts.push_back("最高速率 " + tbl(USB_HIGHEST_SPEED, spd, ""));
  if (pdBit(v, 26) || pdBit(v, 3) || pdBit(v, 5) || pdBit(v, 4)) facts.push_back("支持 Alt Mode");
}

void dfpVdo(pdscope::DetailEmitter& em, uint32_t v, std::vector<std::string>& facts) {
  const int ver = static_cast<int>(pdField(v, 31, 29));
  em.detail("DFP VDO Version [" + pdRange(31, 29) + "]", tbl(DFP_VDO_VERSION, ver, "无效取值（不得使用）") + " · " + binPad(ver, 3) + "b");
  em.detail("Reserved [" + pdRange(28, 27) + "]", "0x" + pdscope::hexU(pdField(v, 28, 27), 1));
  const int usb4 = static_cast<int>(pdBit(v, 26)), usb32 = static_cast<int>(pdBit(v, 25)), usb2 = static_cast<int>(pdBit(v, 24));
  em.detail("USB4 Host 能力 [B26]", pdFlag(usb4, "Host 支持 USB4", "否"));
  em.detail("USB 3.2 Host 能力 [B25]", pdFlag(usb32, "Host 支持 USB 3.2", "否（Power Brick / Hub 应为 0）"));
  em.detail("USB 2.0 Host 能力 [B24]", pdFlag(usb2, "Host 支持 USB 2.0", "否（Power Brick / Hub 应为 0）"));
  em.detail("Connector Type (Legacy) [" + pdRange(23, 22) + "]", "00b（已废弃）");
  em.detail("Reserved [" + pdRange(21, 5) + "]", "0x" + pdscope::hexU(pdField(v, 21, 5), 1));
  em.detail("Port Number [" + pdRange(4, 0) + "]", std::to_string(pdField(v, 4, 0)) + "（设备内 DFP 端口编号）");
  std::vector<std::string> caps;
  if (usb4) caps.push_back("USB4");
  if (usb32) caps.push_back("USB3.2");
  if (usb2) caps.push_back("USB2");
  if (!caps.empty()) facts.push_back("DFP " + join_(caps, "/"));
}

void passiveCableVdo(pdscope::DetailEmitter& em, uint32_t v, const PdCtx& ctx, std::vector<std::string>& facts) {
  const int hw = static_cast<int>(pdField(v, 31, 28)), fw = static_cast<int>(pdField(v, 27, 24));
  const int vdoVer = static_cast<int>(pdField(v, 23, 21));
  em.detail("Hardware Version [" + pdRange(31, 28) + "]", "0x" + pdscope::hexU(hw, 1) + "（厂商自定义）");
  em.detail("Firmware Version [" + pdRange(27, 24) + "]", "0x" + pdscope::hexU(fw, 1) + "（厂商自定义）");
  em.detail("VDO Version [" + pdRange(23, 21) + "]", vdoVer == 0 ? "Version 1.0" : (binPad(vdoVer, 3) + "b · 无效取值（不得使用）"));
  em.detail("Reserved [B20]", std::to_string(pdBit(v, 20)));
  const int conn = static_cast<int>(pdField(v, 19, 18));
  em.detail("USB Type-C plug to USB Type-C/Captive [" + pdRange(19, 18) + "]",
    binPad(conn, 2) + "b · " + tbl(CABLE_CONNECTOR, conn, ""));
  const int epr = static_cast<int>(pdBit(v, 17));
  em.detail("EPR Capable [B17]", pdFlag(epr, "线缆支持 48V/5A 的 EPR 运行", "不支持 EPR"));
  if (is3x(ctx)) {
    em.detail("说明", "B17「EPR Capable」是 PD 3.1 才定义的字段，PD 3.0 里该位为 Reserved"
      "（报文头只标到 3.x，无法区分 3.0 / 3.1）");
  }
  const int lat = static_cast<int>(pdField(v, 16, 13));
  em.detail("Cable Latency [" + pdRange(16, 13) + "]", tbl(CABLE_LATENCY_PASSIVE, lat, ""));
  const int term = static_cast<int>(pdField(v, 12, 11));
  em.detail("Cable Termination Type [" + pdRange(12, 11) + "]", tbl(CABLE_TERMINATION_PASSIVE, term, ""));
  const int vmaxCode = static_cast<int>(pdField(v, 10, 9));
  const auto& vmaxTable = is3x(ctx) ? CABLE_VBUS_VOLTAGE_V31 : CABLE_VBUS_VOLTAGE_V30;
  em.detail("Maximum VBUS Voltage [" + pdRange(10, 9) + "]",
    binPad(vmaxCode, 2) + "b · " + tbl(vmaxTable, vmaxCode, "") + vmaxNote(vmaxCode, ctx));
  em.detail("Reserved [" + pdRange(8, 7) + "]", "0x" + pdscope::hexU(pdField(v, 8, 7), 1));
  const int cur = static_cast<int>(pdField(v, 6, 5));
  em.detail("VBUS Current Handling [" + pdRange(6, 5) + "]", tbl(CABLE_VBUS_CURRENT, cur, ""));
  em.detail("Reserved [" + pdRange(4, 3) + "]", "0x" + pdscope::hexU(pdField(v, 4, 3), 1));
  const int spd = static_cast<int>(pdField(v, 2, 0));
  em.detail("USB Highest Speed [" + pdRange(2, 0) + "]", cableSpeedText(spd));

  facts.push_back("无源线缆");
  facts.push_back(tbl(vmaxTable, vmaxCode, "") + " / " + stripParen(tbl(CABLE_VBUS_CURRENT, cur, "")));
  if (epr) facts.push_back("EPR");
  facts.push_back(tbl(CABLE_LATENCY_PASSIVE, lat, ""));
}

void activeCableVdo1(pdscope::DetailEmitter& em, uint32_t v, const PdCtx& ctx, std::vector<std::string>& facts) {
  const int hw = static_cast<int>(pdField(v, 31, 28)), fw = static_cast<int>(pdField(v, 27, 24)), vdoVer = static_cast<int>(pdField(v, 23, 21));
  em.detail("Hardware Version [" + pdRange(31, 28) + "]", "0x" + pdscope::hexU(hw, 1));
  em.detail("Firmware Version [" + pdRange(27, 24) + "]", "0x" + pdscope::hexU(fw, 1));
  static const std::map<int, std::string> avVer = {
    {0, "Version 1.0（已废弃）"}, {1, "无效取值（不得使用）"}, {2, "Version 1.2（已废弃）"}, {3, "Version 1.3"}};
  em.detail("VDO Version [" + pdRange(23, 21) + "]", tbl(avVer, vdoVer, "无效取值（不得使用）"));
  em.detail("Reserved [B20]", std::to_string(pdBit(v, 20)));
  const int conn = static_cast<int>(pdField(v, 19, 18));
  em.detail("USB Type-C plug to USB Type-C/Captive [" + pdRange(19, 18) + "]",
    binPad(conn, 2) + "b · " + tbl(CABLE_CONNECTOR, conn, ""));
  const int epr = static_cast<int>(pdBit(v, 17));
  em.detail("EPR Capable [B17]", pdFlag(epr, "线缆支持 48V/5A 的 EPR 运行", "不支持 EPR"));
  if (is3x(ctx)) {
    em.detail("说明", "B17「EPR Capable」是 PD 3.1 才定义的字段，PD 3.0 里该位为 Reserved"
      "（报文头只标到 3.x，无法区分 3.0 / 3.1）");
  }
  const int lat = static_cast<int>(pdField(v, 16, 13));
  em.detail("Cable Latency [" + pdRange(16, 13) + "]", tbl(CABLE_LATENCY_ACTIVE, lat, ""));
  em.detail("Cable Termination Type [" + pdRange(12, 11) + "]", tbl(CABLE_TERMINATION_ACTIVE, pdField(v, 12, 11), ""));
  const int vmaxCode = static_cast<int>(pdField(v, 10, 9));
  const auto& vmaxTable = is3x(ctx) ? CABLE_VBUS_VOLTAGE_V31 : CABLE_VBUS_VOLTAGE_V30;
  em.detail("Maximum VBUS Voltage [" + pdRange(10, 9) + "]",
    binPad(vmaxCode, 2) + "b · " + tbl(vmaxTable, vmaxCode, "") + vmaxNote(vmaxCode, ctx));
  em.detail("SBU Supported [B8]", pdBit(v, 8) ? "1（不支持 SBU）" : "0（支持 SBU）");
  em.detail("SBU Type [B7]", pdBit(v, 7) ? "1（SBU 为有源/数字化）" : "0（SBU 为无源）");
  const int cur = static_cast<int>(pdField(v, 6, 5));
  em.detail("VBUS Current Handling [" + pdRange(6, 5) + "]", tbl(CABLE_VBUS_CURRENT_ACTIVE, cur, ""));
  em.detail("VBUS Through Cable [B4]", pdFlag(pdBit(v, 4), "线缆内有 VBUS 导线", "无 VBUS 导线"));
  em.detail("SOP'' Controller Present [B3]", pdFlag(pdBit(v, 3), "支持 SOP'' 通信", "仅 SOP'"));
  const int spd = static_cast<int>(pdField(v, 2, 0));
  em.detail("USB Highest Speed [" + pdRange(2, 0) + "]", cableSpeedText(spd));

  facts.push_back("有源线缆");
  facts.push_back(tbl(vmaxTable, vmaxCode, "") + " / " + tbl(CABLE_VBUS_CURRENT_ACTIVE, cur, ""));
  facts.push_back(tbl(CABLE_LATENCY_ACTIVE, lat, ""));
}

void activeCableVdo2(pdscope::DetailEmitter& em, uint32_t v, std::vector<std::string>& facts) {
  em.detail("Maximum Operating Temperature [" + pdRange(31, 24) + "]", std::to_string(pdField(v, 31, 24)) + " °C（插头内最高工作温度）");
  em.detail("Shutdown Temperature [" + pdRange(23, 16) + "]", std::to_string(pdField(v, 23, 16)) + " °C（超过即关断有源器件）");
  em.detail("Reserved [B15]", std::to_string(pdBit(v, 15)));
  em.detail("U3/CLd Power [" + pdRange(14, 12) + "]", tbl(U3_CLD_POWER, static_cast<int>(pdField(v, 14, 12)), "（U3/CLd 状态下的自身功耗）"));
  em.detail("U3 to U0 transition [B11]", pdBit(v, 11) ? "1（经 U3S 过渡）" : "0（U3 直通 U0）");
  em.detail("Physical Connection [B10]", pdBit(v, 10) ? "1（光连接）" : "0（铜连接）");
  em.detail("Active Element [B9]", pdBit(v, 9) ? "1（Active Re-timer）" : "0（Active Re-driver）");
  em.detail("USB4 Supported [B8]", pdBit(v, 8) ? "1（不支持 USB4）" : "0（支持 USB4）");
  em.detail("USB 2.0 Hub Hops Consumed [" + pdRange(7, 6) + "]", std::to_string(pdField(v, 7, 6)));
  em.detail("USB 2.0 Supported [B5]", pdBit(v, 5) ? "1（不支持 USB2）" : "0（支持 USB2）");
  em.detail("USB 3.2 Supported [B4]", pdBit(v, 4) ? "1（不支持 USB3 SuperSpeed）" : "0（支持 USB3 SuperSpeed）");
  em.detail("USB Lanes Supported [B3]", pdBit(v, 3) ? "1（两条 lane）" : "0（一条 lane）");
  em.detail("Optically Isolated Active Cable [B2]", pdFlag(pdBit(v, 2)));
  em.detail("[USB4] Asymmetric Mode [B1]", pdFlag(pdBit(v, 1)));
  em.detail("USB Gen [B0]", pdBit(v, 0) ? "1（Gen2 及以上）" : "0（Gen1，仅 USB3 线缆）");
  facts.push_back("最高工作温度 " + std::to_string(pdField(v, 31, 24)) + "°C");
}

void vpdVdo(pdscope::DetailEmitter& em, uint32_t v, std::vector<std::string>& facts) {
  const int hw = static_cast<int>(pdField(v, 31, 28)), fw = static_cast<int>(pdField(v, 27, 24)), vdoVer = static_cast<int>(pdField(v, 23, 21));
  em.detail("HW Version [" + pdRange(31, 28) + "]", "0x" + pdscope::hexU(hw, 1));
  em.detail("Firmware Version [" + pdRange(27, 24) + "]", "0x" + pdscope::hexU(fw, 1));
  em.detail("VDO Version [" + pdRange(23, 21) + "]", vdoVer == 0 ? "Version 1.0" : "无效取值（不得使用）");
  em.detail("Reserved [" + pdRange(20, 17) + "]", "0x" + pdscope::hexU(pdField(v, 20, 17), 1));
  const int ct = static_cast<int>(pdBit(v, 0));
  em.detail("Maximum VBUS Voltage [" + pdRange(16, 15) + "]", tbl(VPD_VBUS_VOLTAGE, static_cast<int>(pdField(v, 16, 15)), ""));
  em.detail("Charge Through Current Support [B14]", ct
    ? (pdBit(v, 14) ? "1（5 A）" : "0（3 A）")
    : (std::to_string(pdBit(v, 14)) + "（未开启 Charge Through，此域 Reserved）"));
  em.detail("Reserved [B13]", std::to_string(pdBit(v, 13)));
  const int vbusImp = static_cast<int>(pdField(v, 12, 7)), gndImp = static_cast<int>(pdField(v, 6, 1));
  em.detail("VBUS Impedance [" + pdRange(12, 7) + "]", ct ? (std::to_string(vbusImp * 2) + " mΩ（2mΩ 步长）") : "Reserved（未开启 Charge Through）");
  em.detail("Ground Impedance [" + pdRange(6, 1) + "]", ct ? (std::to_string(gndImp) + " mΩ（1mΩ 步长）") : "Reserved（未开启 Charge Through）");
  em.detail("Charge Through Support [B0]", pdFlag(ct, "支持 Charge Through（可为下游供电）", "不支持"));
  facts.push_back(std::string("VPD") + (ct ? " 支持 Charge Through" : ""));
}

void amaVdo(pdscope::DetailEmitter& em, uint32_t v, const PdCtx& ctx, std::vector<std::string>& facts) {
  em.detail("说明", "AMA（Alternate Mode Adapter）产品类型已被 PD 3.1 废弃，新设计应改用 UFP VDO");
  em.detail("HW Version [" + pdRange(31, 28) + "]", "0x" + pdscope::hexU(pdField(v, 31, 28), 1));
  em.detail("Firmware Version [" + pdRange(27, 24) + "]", "0x" + pdscope::hexU(pdField(v, 27, 24), 1));
  if (!(ctx.revText == "2.0" || ctx.revText == "1.0")) {
    em.detail("VDO Version [" + pdRange(23, 21) + "]", pdField(v, 23, 21) == 0 ? "Version 1.0" : "保留值（不得使用）");
    em.detail("Reserved [" + pdRange(20, 8) + "]", "0x" + pdscope::hexU(pdField(v, 20, 8), 1));
  } else {
    em.detail("Reserved [" + pdRange(23, 12) + "]", "0x" + pdscope::hexU(pdField(v, 23, 12), 1));
    em.detail("SSTX1 Directionality [B11]", pdBit(v, 11) ? "1（可配置）" : "0（固定）");
    em.detail("SSTX2 Directionality [B10]", pdBit(v, 10) ? "1（可配置）" : "0（固定）");
    em.detail("SSRX1 Directionality [B9]", pdBit(v, 9) ? "1（可配置）" : "0（固定）");
    em.detail("SSRX2 Directionality [B8]", pdBit(v, 8) ? "1（可配置）" : "0（固定）");
  }
  em.detail("VCONN power [" + pdRange(7, 5) + "]", pdBit(v, 4) ? tbl(VCONN_POWER, static_cast<int>(pdField(v, 7, 5)), "") : "Reserved（未要求 VCONN）");
  em.detail("VCONN required [B4]", pdFlag(pdBit(v, 4)));
  em.detail("VBUS required [B3]", pdBit(v, 3) ? "1（不需要 VBUS）" : "0（需要 VBUS）");
  em.detail("USB SuperSpeed Signaling Support [" + pdRange(2, 0) + "]", tbl(AMA_SUPERSPEED_V30, static_cast<int>(pdField(v, 2, 0)), ""));
  facts.push_back("AMA（已废弃形态）");
}

const std::map<std::string, std::map<int, std::string>> PT_SHORT = {
  {"cable", {{0, "非线缆插头"}, {3, "无源线缆"}, {4, "有源线缆"}, {6, "VPD（VCONN 供电设备）"}}},
  {"ufp", {{0, "非 UFP"}, {1, "PDUSB Hub"}, {2, "PDUSB 外设"}, {3, "PSD（电源设备）"}, {5, "AMA（已废弃）"}}},
  {"dfp", {{0, "非 DFP"}, {1, "PDUSB Hub"}, {2, "PDUSB Host"}, {3, "Power Brick"}}},
};

std::vector<std::string> identityFacts(const IdInfo& info) {
  std::vector<std::string> out;
  if (info.cable) {
    if (info.ptUfp) out.push_back(tbl(PT_SHORT.at("cable"), info.ptUfp, "保留产品类型 " + std::to_string(info.ptUfp)));
  } else {
    const std::string u = info.ptUfp ? tbl(PT_SHORT.at("ufp"), info.ptUfp, "保留产品类型 " + std::to_string(info.ptUfp)) : "";
    const std::string d = info.ptDfp ? tbl(PT_SHORT.at("dfp"), info.ptDfp, "保留产品类型 " + std::to_string(info.ptDfp)) : "";
    if (!u.empty() && !d.empty() && u != d) out.push_back(u + " / " + d);
    else if (!u.empty()) out.push_back(u);
    else if (!d.empty()) out.push_back(d);
  }
  if (info.vid) {
    auto vn = svidName(static_cast<uint16_t>(info.vid));
    const std::string vtext = vn ? vn->substr(0, vn->find("（厂商 ID）")) : "";
    out.push_back("VID 0x" + pdscope::hexU(info.vid, 4) + (vtext.empty() ? "" : ("（" + vtext + "）")));
  }
  return out;
}

void decodeDiscoverIdentity(PdState& st, pdscope::DetailEmitter& em,
                            const std::vector<uint32_t>& vdos, const PdCtx& ctx,
                            const std::string& ack, std::vector<std::string>& facts) {
  if (ack != "ACK") {
    if (!vdos.empty()) {
      em.object("VDO #2 · " + ack + " 不应携带数据对象");
      em.detail("原始值", "0x" + pdHex(vdos[0]));
    }
    if (ack == "REQ") {
      em.detail("说明", "Discover Identity 的请求方只发 VDM Header，不带数据对象");
      return;
    }
    pushNonAckFact(facts, ack, vdos, "Discover Identity");
    if (ack == "NAK") {
      em.detail("说明", "对端未实现 Discover Identity，故无数据对象");
    } else {
      em.detail("说明", "BUSY 不是 Discover Identity 的合法响应（规范只允许 ACK/NAK）");
    }
    return;
  }
  if (vdos.empty()) {
    facts.push_back("ACK 响应缺少数据对象（规范要求 4~7 个）");
    em.note("⚠ Discover Identity ACK 未携带 VDO");
    return;
  }

  IdInfo info;
  em.object("VDO #2 · ID Header VDO");
  idHeaderVdo(em, vdos[0], ctx, info);

  if (vdos.size() > 1) {
    em.object("VDO #3 · Cert Stat VDO");
    certStatVdo(em, vdos[1]);
  }
  if (vdos.size() > 2) {
    em.object("VDO #4 · Product VDO");
    productVdo(em, vdos[2]);
  }

  const auto rest = std::vector<uint32_t>(vdos.begin() + 3, vdos.end());
  const std::string kind = info.vdoKind;

  if (rest.empty()) {
    em.object("无 Product Type VDO");
    em.detail("说明", "ID Header VDO 声明的产品类型为「" + info.productTypeText + "」，该类型不返回 Product Type VDO");
  } else if (ctx.link == "cable") {
    if (kind == "activeCable") {
      em.object("VDO #5 · Active Cable VDO1");
      activeCableVdo1(em, rest[0], ctx, facts);
      if (rest.size() > 1) {
        em.object("VDO #6 · Active Cable VDO2");
        activeCableVdo2(em, rest[1], facts);
      } else {
        em.note("⚠ 有源线缆缺少 Active Cable VDO2");
      }
    } else if (kind == "vpd") {
      em.object("VDO #5 · VPD VDO");
      vpdVdo(em, rest[0], facts);
    } else {
      em.object("VDO #5 · Passive Cable VDO");
      passiveCableVdo(em, rest[0], ctx, facts);
    }
  } else if (rest.size() >= 3) {
    em.object("VDO #5 · UFP VDO");
    ufpVdo(em, rest[0], facts);
    em.object("VDO #6 · Padding");
    em.detail("值", "0x" + pdHex(rest[1]) + (rest[1] == 0 ? "（全 0，符合规范的 DRD 格式）" : " ⚠ 规范要求此对象为全 0"));
    em.object("VDO #7 · DFP VDO");
    dfpVdo(em, rest[2], facts);
  } else if (kind == "dfp") {
    em.object("VDO #5 · DFP VDO");
    dfpVdo(em, rest[0], facts);
  } else if (kind == "ama") {
    em.object("VDO #5 · AMA VDO（已废弃）");
    amaVdo(em, rest[0], ctx, facts);
  } else {
    em.object("VDO #5 · UFP VDO");
    ufpVdo(em, rest[0], facts);
  }

  auto idf = identityFacts(info);
  for (auto it = idf.rbegin(); it != idf.rend(); ++it) facts.insert(facts.begin(), *it);
  st.identity = PdIdentity{};
  st.identity.ptUfp = info.ptUfp;
  st.identity.ptDfp = info.ptDfp;
  st.identity.conn = info.conn;
  st.identity.vid = info.vid;
  st.identity.productTypeText = info.productTypeText;
  st.identity.vdoKind = info.vdoKind;
  st.identity.cable = info.cable;
  st.identity.host = info.host;
  st.identity.device = info.device;
  st.identity.modal = info.modal;
  st.identity.certStat = vdos.size() > 1 ? vdos[1] : 0;
  st.identity.product = vdos.size() > 2 ? vdos[2] : 0;
  st.identitySet = true;
}

void decodeDiscoverSvids(PdState& st, pdscope::DetailEmitter& em,
                         const std::vector<uint32_t>& vdos, const PdCtx& /*ctx*/,
                         const std::string& ack, std::vector<std::string>& facts) {
  if (ack != "ACK") {
    pushNonAckFact(facts, ack, vdos, "Discover SVIDs");
    return;
  }
  if (vdos.empty()) {
    em.note("⚠ Discover SVIDs ACK 未携带 VDO");
    return;
  }
  std::vector<uint16_t> list;
  for (size_t i = 0; i < vdos.size(); i++) {
    const int a = static_cast<int>(pdField(vdos[i], 31, 16)), b = static_cast<int>(pdField(vdos[i], 15, 0));
    em.object("VDO #" + std::to_string(i + 2) + " · Responder VDO（两个 SVID）");
    em.detail("SVID n [B31-16]", a == 0 ? "0x0000（列表结束）" : svidText(static_cast<uint16_t>(a)));
    em.detail("SVID n+1 [B15-0]", b == 0 ? "0x0000（列表结束）" : svidText(static_cast<uint16_t>(b)));
    if (a) list.push_back(static_cast<uint16_t>(a));
    if (b) list.push_back(static_cast<uint16_t>(b));
  }
  st.svidList = list;
  facts.push_back(list.empty() ? "无 SVID" : ("SVID: " + join_(mapHex(list), ", ")));
}

void decodeDiscoverModes(PdState& st, pdscope::DetailEmitter& em,
                         const std::vector<uint32_t>& vdos, const PdCtx& /*ctx*/,
                         const std::string& ack, uint16_t svid, std::vector<std::string>& facts) {
  (void)st;
  if (ack != "ACK") {
    pushNonAckFact(facts, ack, vdos, "Discover Modes");
    return;
  }
  if (vdos.empty()) {
    em.note("⚠ Discover Modes ACK 未携带 VDO");
    return;
  }
  auto name = svidName(svid);
  for (size_t i = 0; i < vdos.size(); i++) {
    em.object("VDO #" + std::to_string(i + 2) + " · Mode VDO #" + std::to_string(i + 1));
    em.detail("对象位置", std::to_string(i + 1) + "（Enter/Exit Mode 用该值引用本 Mode）");
    em.detail("原始值", "0x" + pdHex(vdos[i]));
    if (svid == SVID_PD) {
      em.detail("说明", "PD SID（0xFF00）下不应出现 Mode VDO");
    } else {
      em.detail("说明", "Mode VDO 的内容由 SVID " + (name ? *name : ("0x" + pdscope::hexU(svid, 4))) + " 的标准/厂商定义，本工具按原始值展示");
    }
  }
  facts.push_back("SVID 0x" + pdscope::hexU(svid, 4) + " 的 " + std::to_string(vdos.size()) + " 个 Mode VDO");
}

void decodeEnterExitMode(pdscope::DetailEmitter& em, const std::vector<uint32_t>& vdos,
                         const std::string& ack, const std::string& label, std::vector<std::string>& facts) {
  if (!vdos.empty()) {
    em.object("VDO #2 · " + label + " 携带的 SVID 自定义数据");
    em.detail("原始值", "0x" + pdHex(vdos[0]));
    em.detail("说明", "Enter Mode 请求最多带 1 个 VDO，内容由 Alternate Mode 定义；ACK/NAK 不带 VDO");
  }
  if (ack == "BUSY") facts.push_back("⚠ BUSY 不是 Enter/Exit Mode 的合法响应（规范只允许 ACK/NAK）");
}

void decodeAttention(pdscope::DetailEmitter& em, const std::vector<uint32_t>& vdos,
                     uint16_t svid, std::vector<std::string>& /*facts*/) {
  (void)svid;
  if (!vdos.empty()) {
    em.object("VDO #2 · Attention 携带的 SVID 自定义数据");
    em.detail("原始值", "0x" + pdHex(vdos[0]));
    em.detail("说明", "Attention 最多带 1 个 VDO，内容由 Alternate Mode 定义");
  }
  em.detail("响应要求", "Attention 不需要响应（GoodCRC 之外无应答）");
}

// 辅助：把 SVID 列表格式化成 0xXXXX
std::vector<std::string> mapHex(const std::vector<uint16_t>& list) {
  std::vector<std::string> out;
  for (auto s : list) out.push_back("0x" + pdscope::hexU(s, 4));
  return out;
}

}  // namespace

std::string vdmCommandName(int cmd) {
  auto it = VDM_CMDS.find(cmd);
  if (it != VDM_CMDS.end()) return it->second;
  if (cmd >= VDM_CMD_SVID_MIN) return "SVID 自定义命令 " + std::to_string(cmd);
  return "无效命令 " + std::to_string(cmd);
}

std::string vdmParse(PdState& st, pdscope::DetailEmitter& em,
                     const std::vector<uint32_t>& vdos, const PdCtx& ctx) {
  const uint32_t head = vdos.empty() ? 0 : vdos[0];
  const int svid = static_cast<int>(pdField(head, 31, 16));
  const bool structured = pdBit(head, 15) == 1;
  const auto extra = std::vector<uint32_t>(vdos.begin() + (vdos.empty() ? 0 : 1), vdos.end());
  std::vector<std::string> facts;

  em.object("VDO #1 · VDM Header");
  em.detail("SVID [B31-16]", svidText(static_cast<uint16_t>(svid)));
  em.detail("VDM Type [B15]", pdFlag(structured, "Structured VDM（结构化）", "Unstructured VDM（厂商私有）"));

  if (!structured) {
    const int payload = static_cast<int>(pdField(head, 14, 0));
    em.detail("Available for Vendor Use [B14-0]", "0x" + pdscope::hexU(payload, 4));
    auto vendor = svidName(static_cast<uint16_t>(svid));
    const std::string vendorNoSuffix = vendor ? vendor->substr(0, vendor->find("（厂商 ID）")) : "";
    const std::string s = "厂商私有 VDM · SVID 0x" + pdscope::hexU(svid, 4)
      + (vendor ? (" (" + vendorNoSuffix + ")") : "")
      + " · 私有载荷 0x" + pdHexVar(payload);
    for (size_t i = 0; i < extra.size(); i++) {
      em.object("VDO #" + std::to_string(i + 2) + " · 厂商私有数据");
      em.detail("原始值", "0x" + pdHex(extra[i]));
    }
    em.note(s);
    return s;
  }

  const int verMajor = static_cast<int>(pdField(head, 14, 13));
  const int verMinor = static_cast<int>(pdField(head, 12, 11));
  const int objPos = static_cast<int>(pdField(head, 10, 8));
  const int cmdType = static_cast<int>(pdField(head, 7, 6));
  const int cmd = static_cast<int>(pdField(head, 4, 0));
  const std::string cmdName = vdmCommandName(cmd);
  const std::string ack = VDM_ACK[cmdType];

  em.detail("Structured VDM Version (Major) [B14-13]",
    tbl(VDM_VER_MAJOR, verMajor, "无效") + " · " + binPad(verMajor, 2) + "b");
  em.detail("Structured VDM Version (Minor) [B12-11]",
    cmd <= 15 ? (tbl(VDM_VER_MINOR, verMinor, "无效取值（不得使用）") + " · " + binPad(verMinor, 2) + "b")
              : ("由 SVID 定义 · " + binPad(verMinor, 2) + "b"));
  em.detail("Object Position [B10-8]", std::to_string(objPos)
    + (cmd == 4 || cmd == 5 ? ("（指向 Discover Modes 列表里的第 " + std::to_string(objPos) + " 个 Mode）")
       : (objPos ? "（本命令下应为 0，接收端应忽略）" : "（本命令下应为 000b）")));
  em.detail("Command Type [B7-6]", ack + " · " + binPad(cmdType, 2) + "b");
  em.detail("Reserved [B5]", std::to_string(pdBit(head, 5)));
  em.detail("Command [B4-0]", std::to_string(cmd) + " · " + cmdName);

  switch (cmd) {
    case 1: decodeDiscoverIdentity(st, em, extra, ctx, ack, facts); break;
    case 2: decodeDiscoverSvids(st, em, extra, ctx, ack, facts); break;
    case 3: decodeDiscoverModes(st, em, extra, ctx, ack, static_cast<uint16_t>(svid), facts); break;
    case 4: decodeEnterExitMode(em, extra, ack, "Enter Mode", facts); break;
    case 5: decodeEnterExitMode(em, extra, ack, "Exit Mode", facts); break;
    case 6: decodeAttention(em, extra, static_cast<uint16_t>(svid), facts); break;
    default:
      if (cmd == 0) {
        em.note("命令 0 无效，接收端应回 NAK");
      } else if (cmd > 6 && cmd < VDM_CMD_SVID_MIN) {
        em.note("命令 " + std::to_string(cmd) + " 无效（规范只定义 1…6 与 16…31），接收端应回 NAK");
      } else {
        em.note("SVID 自定义命令 " + std::to_string(cmd));
      }
      for (size_t i = 0; i < extra.size(); i++) {
        em.object("VDO #" + std::to_string(i + 2) + " · SVID 自定义数据");
        em.detail("原始值", "0x" + pdHex(extra[i]));
      }
  }

  const std::string head4 = ack + " " + cmdName;
  const std::vector<std::string> uniq = dedupe(facts);
  const std::string summary = uniq.empty() ? head4 : (head4 + " · " + join_(uniq, " · "));
  em.note(summary);
  st.lastVdm = PdLastVdm{static_cast<uint16_t>(svid), cmd, cmdType, objPos};
  return summary;
}

}}  // namespace pdscope::pd
