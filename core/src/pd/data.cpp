// data.cpp — 见 data.h

#include "data.h"
#include "format.h"
#include "tables.h"

#include <cstdio>

namespace pdscope { namespace pd {

namespace {

const double atLeast(const std::string& revText, double target) {
  return revTextNum(revText) >= target;
}

inline std::string hxv(uint32_t v) { return pdHexVar(v); }   // 不补零大写十六进制
inline std::string chr(int v) { return std::string(1, static_cast<char>(static_cast<unsigned char>(v))); }

std::string binPad(int v, int n) {
  std::string s = "0b";
  for (int i = n - 1; i >= 0; i--) s += ((v >> i) & 1) ? '1' : '0';
  return s;
}

std::vector<std::string> batteryList(int mask, int base) {
  std::vector<std::string> out;
  for (int i = 0; i < 4; i++)
    if (mask & (1 << i)) out.push_back(std::to_string(base + i));
  return out;
}

std::string join_(const std::vector<std::string>& v, const std::string& sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) { if (i) s += sep; s += v[i]; }
  return s;
}

}  // namespace

std::string bistParse(pdscope::DetailEmitter& em, uint32_t data, int idx, const std::string& revText) {
  const int mode = static_cast<int>(pdField(data, 31, 28));
  const bool hasLegacy = !atLeast(revText, 3.0);
  const std::string modern = tbl(BIST_MODES_V3, mode, "");
  const std::string legacy = hasLegacy ? tbl(BIST_MODES_V2, mode, "") : "";
  em.object("BIST 数据对象 #" + std::to_string(idx + 1));
  em.detail("BIST Test Mode [" + pdRange(31, 28) + "]",
    "0x" + hxv(mode) + " · " + (!modern.empty() ? modern : (!legacy.empty() ? legacy : "无效取值（接收端应忽略本条报文）")));
  if (!modern.empty() && !legacy.empty())
    em.detail("PD 2.0 旧值", legacy + "（同一数值在旧版规范里含义不同）");
  em.detail("Reserved [" + pdRange(27, 0) + "]", "0x" + pdHex(data).substr(2));

  if (idx > 0) {
    em.note("非法：BIST 只应有 1 个数据对象");
    return "BIST 数据对象数量非法";
  }
  const std::string name = !modern.empty() ? modern : (!legacy.empty() ? legacy : ("未知模式 " + std::to_string(mode)));
  em.note("模式 " + name);
  return "模式 " + name;
}

std::string batteryStatusParse(pdscope::DetailEmitter& em, uint32_t data) {
  const int cap = static_cast<int>(pdField(data, 31, 16));
  const int status = static_cast<int>(pdField(data, 11, 10));
  const int present = static_cast<int>(pdBit(data, 9));
  const int invalidRef = static_cast<int>(pdBit(data, 8));
  const std::string capText = (cap == 0xFFFF) ? "未知 (FFFFh)" : (pdNum(cap * 0.1) + " Wh");

  em.detail("电池当前容量 [" + pdRange(31, 16) + "]", capText);
  em.detail("Reserved [" + pdRange(15, 12) + "]", "0x" + hxv(pdField(data, 15, 12)));
  em.detail("充电状态 [" + pdRange(11, 10) + "]",
    present ? tbl(CHARGE_STATE, status, "Invalid") : "Reserved（电池不存在）");
  em.detail("Battery Present [B9]", pdFlag(present, "电池在位", "电池不在位"));
  em.detail("Invalid Battery Reference [B8]", pdFlag(invalidRef, "引用的电池不存在", "引用有效"));
  em.detail("Reserved [" + pdRange(7, 0) + "]", "0x" + pdscope::hexU(pdField(data, 7, 0), 2));

  std::string s = "容量 " + capText + " · " + (present ? tbl(CHARGE_STATE, status, "Invalid") : "电池不在位")
    + (invalidRef ? " · 电池引用无效" : "");
  em.note(s);
  return s;
}

std::string alertParse(pdscope::DetailEmitter& em, uint32_t data, int idx) {
  const int flags = static_cast<int>(pdField(data, 31, 24));
  em.object("Alert 数据对象 #" + std::to_string(idx + 1) + "（ADO）");
  em.detail("Type of Alert [" + pdRange(31, 24) + "]",
    "0x" + pdscope::hexU(flags, 2) + " = " + (flags ? "" : "无告警位"));

  std::vector<std::string> fired;
  for (const auto& e : ALERT_BITS) {
    if (e.bit == 24) { em.detail("Reserved [B24]", std::to_string(pdBit(data, 24))); continue; }
    const int on = static_cast<int>(pdBit(data, e.bit));
    em.detail(e.name + " [B" + std::to_string(e.bit) + "]", pdFlag(on));
    if (on) fired.push_back(e.name);
  }
  const int fixed = static_cast<int>(pdField(data, 23, 20));
  const int hot = static_cast<int>(pdField(data, 19, 16));
  const int statusChange = static_cast<int>(pdBit(data, 25));
  em.detail("Fixed Batteries [" + pdRange(23, 20) + "]", statusChange
    ? (fixed ? (binPad(fixed, 4) + " → 电池 " + join_(batteryList(fixed, 0), "、")) : "无")
    : ("0x" + hxv(fixed) + "（未置 Battery Status Change，此域 Reserved）"));
  em.detail("Hot Swappable Batteries [" + pdRange(19, 16) + "]", statusChange
    ? (hot ? (binPad(hot, 4) + " → 电池 " + join_(batteryList(hot, 4), "、")) : "无")
    : ("0x" + hxv(hot) + "（未置 Battery Status Change，此域 Reserved）"));
  em.detail("Reserved [" + pdRange(15, 4) + "]", "0x" + hxv(pdField(data, 15, 4)));

  const int extType = static_cast<int>(pdField(data, 3, 0));
  em.detail("Extended Alert Event Type [" + pdRange(3, 0) + "]", pdBit(data, 31)
    ? (std::to_string(extType) + " · " + tbl(EXT_ALERT_EVENT, extType, "自定义/保留事件"))
    : ("0x" + hxv(extType) + "（未置 Extended Alert Event）"));

  std::string s = fired.empty() ? "无告警位" : ("告警：" + join_(fired, "、"));
  if (pdBit(data, 31)) s += " · 扩展事件：" + tbl(EXT_ALERT_EVENT, extType, "保留值 " + std::to_string(extType));
  if (statusChange && (fixed || hot)) {
    auto all = batteryList(fixed, 0);
    auto hotv = batteryList(hot, 4);
    all.insert(all.end(), hotv.begin(), hotv.end());
    s += " · 电池 " + join_(all, "/");
  }
  em.note(s);
  return s;
}

std::string enterUsbParse(pdscope::DetailEmitter& em, uint32_t data) {
  const int mode = static_cast<int>(pdField(data, 30, 28));
  const int speed = static_cast<int>(pdField(data, 23, 21));
  const int ctype = static_cast<int>(pdField(data, 20, 19));
  const int ccur = static_cast<int>(pdField(data, 18, 17));

  em.detail("Reserved [B31]", std::to_string(pdBit(data, 31)));
  em.detail("USB Mode [" + pdRange(30, 28) + "]", std::to_string(mode) + " · " + (mode <= 2 ? tbl(USB_MODE, mode, "") : USB_MODE_UNKNOWN));
  em.detail("Reserved [B27]", std::to_string(pdBit(data, 27)));
  em.detail("USB4 DRD [B26]", pdFlag(pdBit(data, 26), "Host DFP 可作 USB4 Device", "否"));
  em.detail("USB3 DRD [B25]", pdFlag(pdBit(data, 25), "Host DFP 可作 USB3 Device", "否"));
  em.detail("Reserved [B24]", std::to_string(pdBit(data, 24)));
  em.detail("Cable Speed [" + pdRange(23, 21) + "]", std::to_string(speed) + " · " + (speed <= 4 ? tbl(USB_SPEED, speed, "") : USB_SPEED_UNKNOWN));
  em.detail("Cable Type [" + pdRange(20, 19) + "]", ctype == 3 ? CABLE_TYPE.at(3) : (std::to_string(ctype) + " · " + tbl(CABLE_TYPE, ctype, "")));
  em.detail("Cable Current [" + pdRange(18, 17) + "]", std::to_string(ccur) + " · " + tbl(CABLE_CURRENT_EUDO, ccur, ""));
  em.detail("PCIe Support [B16]", pdFlag(pdBit(data, 16), "USB4 PCIe 隧道支持", "不支持"));
  em.detail("DP Support [B15]", pdFlag(pdBit(data, 15), "USB4 DP 隧道支持", "不支持"));
  em.detail("TBT Support [B14]", pdFlag(pdBit(data, 14), "支持 Thunderbolt", "不支持"));
  em.detail("Host Present [B13]", pdFlag(pdBit(data, 13), "USB 树顶存在 Host", "无 Host"));
  em.detail("Reserved [" + pdRange(12, 0) + "]", "0x" + hxv(pdField(data, 12, 0)));

  const std::string s = (mode <= 2 ? tbl(USB_MODE, mode, "") : USB_MODE_UNKNOWN) + " · 线缆 "
    + (ctype == 3 ? CABLE_TYPE.at(3) : tbl(CABLE_TYPE, ctype, "")) + " "
    + (speed <= 4 ? tbl(USB_SPEED, speed, "") : USB_SPEED_UNKNOWN);
  em.note(s);
  return s;
}

std::string sourceInfoParse(pdscope::DetailEmitter& em, uint32_t data, int idx) {
  if (idx == 0) {
    const int portType = static_cast<int>(pdBit(data, 31));
    em.object("Source_Info 数据对象 #1（SIDO1）");
    em.detail("Port Type [B31]", pdFlag(portType, "Guaranteed Capability Port（供电能力固定）", "Managed Capability Port（可动态调整）"));
    em.detail("Reserved [" + pdRange(30, 24) + "]", "0x" + hxv(pdField(data, 30, 24)));
    em.detail("Port Maximum PDP [" + pdRange(23, 16) + "]", std::to_string(pdField(data, 23, 16)) + " W（1W 步长，端口最大能提供的功率）");
    em.detail("Port Present PDP [" + pdRange(15, 8) + "]", std::to_string(pdField(data, 15, 8)) + " W（当前实际可提供，已扣除线缆/温度等限制）");
    em.detail("Port Reported PDP [" + pdRange(7, 0) + "]", std::to_string(pdField(data, 7, 0)) + " W（Source_Capabilities 里报出的功率）");
    const std::string s = "最大 " + std::to_string(pdField(data, 23, 16)) + "W 当前 "
      + std::to_string(pdField(data, 15, 8)) + "W 报出 " + std::to_string(pdField(data, 7, 0)) + "W";
    em.note(s);
    return s;
  }
  if (idx == 1) {
    const int portType = static_cast<int>(pdBit(data, 31));
    const int dps = static_cast<int>(pdBit(data, 30));
    em.object("Source_Info 数据对象 #2（SIDO2）");
    em.detail("Port Type [B31]", pdFlag(portType, "Guaranteed Capability Port", "Managed Capability Port"));
    em.detail("DPS Port [B30]", pdFlag(dps, "动态电源（DPS），Port Type 应为 0b", "非 DPS"));
    em.detail("Reserved [" + pdRange(29, 18) + "]", "0x" + hxv(pdField(data, 29, 18)));
    em.detail("Port Maximum PDP [" + pdRange(17, 9) + "]", pdNum(pdField(data, 17, 9) * 0.5) + " W（0.5W 步长）");
    em.detail("Port Guaranteed PDP [" + pdRange(8, 0) + "]", pdNum(pdField(data, 8, 0) * 0.5) + " W（保证始终能提供的功率）");
    const std::string s = "最大 " + pdNum(pdField(data, 17, 9) * 0.5) + "W 保证 "
      + pdNum(pdField(data, 8, 0) * 0.5) + "W" + (dps ? " · DPS" : "");
    em.note(s);
    return s;
  }
  em.object("Source_Info 数据对象 #" + std::to_string(idx + 1));
  em.detail("原始值", "0x" + pdHex(data));
  return "Source_Info 数据对象 #" + std::to_string(idx + 1) + "（规范只定义 2 个）";
}

std::string revisionParse(pdscope::DetailEmitter& em, uint32_t data) {
  const int rm = static_cast<int>(pdField(data, 31, 28)), rn = static_cast<int>(pdField(data, 27, 24));
  const int vm = static_cast<int>(pdField(data, 23, 20)), vn = static_cast<int>(pdField(data, 19, 16));
  em.detail("Revision Major [" + pdRange(31, 28) + "]", std::to_string(rm));
  em.detail("Revision Minor [" + pdRange(27, 24) + "]", std::to_string(rn));
  em.detail("Version Major [" + pdRange(23, 20) + "]", std::to_string(vm));
  em.detail("Version Minor [" + pdRange(19, 16) + "]", std::to_string(vn));
  em.detail("Reserved [" + pdRange(15, 0) + "]", "0x" + pdscope::hexU(pdField(data, 15, 0), 4));
  const std::string s = "本端口支持的规范版本：Revision " + std::to_string(rm) + "." + std::to_string(rn)
    + " Version " + std::to_string(vm) + "." + std::to_string(vn);
  em.note(s);
  return s;
}

std::string eprModeParse(pdscope::DetailEmitter& em, uint32_t data, int idx) {
  const int action = static_cast<int>(pdField(data, 31, 24));
  const int d = static_cast<int>(pdField(data, 23, 16));
  const std::string actionText = tbl(EPR_MODE_ACTION, action, "无效取值（接收端应忽略本条）");
  em.object("EPR_Mode 数据对象 #" + std::to_string(idx + 1) + "（EPRMDO）");
  em.detail("Action [" + pdRange(31, 24) + "]", "0x" + pdscope::hexU(action, 2) + " · " + actionText);
  std::string dText = "0x" + pdscope::hexU(d, 2);
  if (action == 1) dText += " · EPR Sink Operational PDP = " + std::to_string(d) + " W";
  else if (action == 4) dText += " · " + tbl(EPR_MODE_DATA, d, "无效原因码 " + std::to_string(d));
  else dText += "（该 Action 下本域 Reserved）";
  em.detail("Data [" + pdRange(23, 16) + "]", dText);
  em.detail("Reserved [" + pdRange(15, 0) + "]", "0x" + pdscope::hexU(pdField(data, 15, 0), 4));
  const std::string s = actionText
    + (action == 4 ? (" · " + tbl(EPR_MODE_DATA, d, "原因码 " + std::to_string(d)))
       : (action == 1 ? (" · PDP " + std::to_string(d) + "W") : ""));
  em.note(s);
  return s;
}

std::string countryCodeParse(pdscope::DetailEmitter& em, uint32_t data) {
  const int first = static_cast<int>(pdField(data, 31, 24)), second = static_cast<int>(pdField(data, 23, 16));
  const std::string code = pdCharPair(static_cast<uint8_t>(first), static_cast<uint8_t>(second));
  em.object("Country Code 数据对象（CCDO）");
  em.detail("First Character [" + pdRange(31, 24) + "]", "0x" + pdscope::hexU(first, 2) + " · " + chr(first));
  em.detail("Second Character [" + pdRange(23, 16) + "]", "0x" + pdscope::hexU(second, 2) + " · " + chr(second));
  em.detail("Reserved [" + pdRange(15, 0) + "]", "0x" + pdscope::hexU(pdField(data, 15, 0), 4));
  const std::string s = "国家码 " + code;
  em.note(s);
  return s;
}

std::string manufacturerString(pdscope::DetailEmitter& em, const std::vector<uint8_t>& bytes) {
  const std::string text = pdAscii(bytes);
  em.detail("Manufacturer String", text.empty() ? "（空）" : ("\"" + text + "\""));
  em.note(text.empty() ? "厂商串为空" : ("厂商串 \"" + text + "\""));
  return text;
}

}}  // namespace pdscope::pd
