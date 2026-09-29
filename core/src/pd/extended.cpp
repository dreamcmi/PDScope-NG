// extended.cpp — 见 extended.h

#include "extended.h"
#include "format.h"
#include "tables.h"
#include "pdo.h"
#include "svid.h"
#include "data.h"

#include <algorithm>
#include <functional>

namespace pdscope { namespace pd {

namespace {

/**
 * **小写**、不补零 —— 同一个字段的呈现必须处处一致，别顺手改成大写。
 */
std::string hexVarLower(uint32_t v) {
    if (v == 0) return "0";
    char buf[9];
    int n = 0;
    while (v != 0) { buf[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    std::string s;
    while (n > 0) s.push_back(buf[--n]);
    return s;
}

std::optional<int> rd8(const ExtBlock& bk, int i) {
  if (i >= bk.off && i < bk.off + static_cast<int>(bk.bytes.size())) return bk.bytes[i - bk.off];
  return std::nullopt;
}
std::optional<int> rd16(const ExtBlock& bk, int i) {
  auto a = rd8(bk, i), b = rd8(bk, i + 1);
  if (!a || !b) return std::nullopt;
  return (*a) | ((*b) << 8);
}
std::optional<uint32_t> rd32(const ExtBlock& bk, int i) {
  auto v = rd16(bk, i), w = rd16(bk, i + 2);
  if (!v || !w) return std::nullopt;
  return static_cast<uint32_t>((*v) | ((*w) << 16));
}

inline std::string needI(const std::optional<int>& v, const std::function<std::string(int)>& f) {
  return v ? f(*v) : std::string("（本分块不含该字段）");
}
inline std::string needU(const std::optional<uint32_t>& v, const std::function<std::string(uint32_t)>& f) {
  return v ? f(*v) : std::string("（本分块不含该字段）");
}

void mkGroup(pdscope::DetailEmitter& em, ExtBlock& bk, int byteIdx) {
  int g = byteIdx / 4;
  if (bk.group != g) {
    bk.group = g;
    em.object("数据对象 #" + std::to_string(g + 1) + " · Byte " + std::to_string(g * 4) + "~" + std::to_string(g * 4 + 3));
  }
}
void fDet(pdscope::DetailEmitter& em, ExtBlock& bk, int byteIdx, const std::string& key, const std::string& value) {
  mkGroup(em, bk, byteIdx);
  em.detail(key, value);
}

// 前向声明（定义见下方，函数体在匿名命名空间内按调用顺序靠后）
void peak(pdscope::DetailEmitter& em, ExtBlock& bk, int byteIdx, const std::string& name, const std::optional<int>& v);
std::string join_(const std::vector<std::string>& v, const std::string& sep);
void hexDump(pdscope::DetailEmitter& em, ExtBlock& bk);

const std::string tempText(int v) {
  if (v == 0) return "不支持温度检测 (0)";
  if (v == 1) return "< 2 °C";
  return std::to_string(v) + " °C";
}

std::string scedb(PdState& /*st*/, pdscope::DetailEmitter& em, ExtBlock& bk) {
  // ⚠ 一条声明里不能混 rd16 / rd32 —— `auto` 必须整体推出同一个类型
  auto vid = rd16(bk, 0);
  auto pid = rd16(bk, 2);
  auto xid = rd32(bk, 4);
  fDet(em, bk, 0, "Vendor ID [Byte1-0]", needI(vid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 2, "Product ID [Byte3-2]", needI(pid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 4, "XID [Byte7-4]", needU(xid, [](uint32_t v) { return "0x" + pdHex(v) + "（应与 Cert Stat VDO 一致）"; }));
  fDet(em, bk, 8, "FW Version [Byte8]", needI(rd8(bk, 8), [](int v) { return std::to_string(v); }));
  fDet(em, bk, 9, "HW Version [Byte9]", needI(rd8(bk, 9), [](int v) { return std::to_string(v); }));

  auto vreg = rd8(bk, 10);
  fDet(em, bk, 10, "Voltage Regulation [Byte10]", needI(vreg, [](int v) {
    return "Load Step " + tbl(LOAD_STEP, v & 3, "") + " · IoC " + ((v >> 2) & 1 ? "90%" : "25% (default)");
  }));
  fDet(em, bk, 11, "Holdup Time [Byte11]", needI(rd8(bk, 11), [](int v) {
    return v == 0 ? "不支持该特性 (0)" : (std::to_string(v) + " ms（断 AC 后维持稳压的时间）");
  }));

  auto comp = rd8(bk, 12);
  fDet(em, bk, 12, "Compliance [Byte12]", needI(comp, [](int v) {
    std::vector<std::string> parts;
    if (v & 1) parts.push_back("LPS");
    if ((v >> 1) & 1) parts.push_back("PS1");
    if ((v >> 2) & 1) parts.push_back("PS2");
    return parts.empty() ? std::string("无") : join_(parts, " + ");
  }));
  auto touch = rd8(bk, 13);
  fDet(em, bk, 13, "Touch Current [Byte13]", needI(touch, [](int v) {
    std::vector<std::string> parts;
    if (v & 1) parts.push_back("低接触电流 EPS");
    if ((v >> 1) & 1) parts.push_back("有接地脚");
    if ((v >> 2) & 1) parts.push_back("接地脚接保护地 (PE)");
    return parts.empty() ? std::string("无") : join_(parts, " + ");
  }));

  auto pc1 = rd16(bk, 14), pc2 = rd16(bk, 16), pc3 = rd16(bk, 18);
  peak(em, bk, 14, "Peak Current 1", pc1);
  peak(em, bk, 16, "Peak Current 2", pc2);
  peak(em, bk, 18, "Peak Current 3", pc3);

  fDet(em, bk, 20, "Touch Temp [Byte20]", needI(rd8(bk, 20), [](int v) { return tbl(TOUCH_TEMP_SOURCE, v, "取值无效，按默认处理"); }));
  auto si = rd8(bk, 21);
  fDet(em, bk, 21, "Source Inputs [Byte21]", needI(si, [](int v) {
    std::vector<std::string> parts;
    if (v & 1) parts.push_back((v >> 1) & 1 ? "外部电源在位（不受限）" : "外部电源在位（受限）");
    else parts.push_back("无外部电源");
    if (v & 4) parts.push_back("有内部电池");
    return join_(parts, " · ");
  }));
  auto nb = rd8(bk, 22);
  fDet(em, bk, 22, "Number of Batteries/Slots [Byte22]", needI(nb, [](int v) {
    return "固定电池 " + std::to_string(v & 0xF) + " 个 · 热插拔电池槽 " + std::to_string((v >> 4) & 0xF) + " 个";
  }));
  fDet(em, bk, 23, "SPR Source PDP Rating [Byte23]", needI(rd8(bk, 23), [](int v) { return v > 100 ? (std::to_string(v) + "（>100 视为无效）") : (std::to_string(v) + " W"); }));
  fDet(em, bk, 24, "EPR Source PDP Rating [Byte24]", needI(rd8(bk, 24), [](int v) { return v > 240 ? (std::to_string(v) + "（>240 视为无效）") : (std::to_string(v) + " W"); }));

  std::vector<std::string> parts;
  if (vid) parts.push_back("VID 0x" + pdscope::hexU(*vid, 4));
  if (rd8(bk, 23)) parts.push_back("SPR PDP " + std::to_string(*rd8(bk, 23)) + "W");
  if (rd8(bk, 24) && *rd8(bk, 24)) parts.push_back("EPR PDP " + std::to_string(*rd8(bk, 24)) + "W");
  return join_(parts, " · ");
}

void peak(pdscope::DetailEmitter& em, ExtBlock& bk, int byteIdx, const std::string& name, const std::optional<int>& v) {
  if (!v) { fDet(em, bk, byteIdx, name + " [Byte" + std::to_string(byteIdx + 1) + "-" + std::to_string(byteIdx) + "]", "（本分块不含该字段）"); return; }
  const int val = *v;
  int overload = std::min(val & 0x1F, 25) * 10;
  int period = ((val >> 5) & 0x3F) * 20;
  int duty = ((val >> 11) & 0xF) * 5;
  int droop = (val >> 15) & 1;
  fDet(em, bk, byteIdx, name + " [Byte" + std::to_string(byteIdx + 1) + "-" + std::to_string(byteIdx) + "]",
    overload == 0
      ? "未提供峰值能力（全 0）"
      : ("过载 " + std::to_string(overload) + "% · 周期 " + std::to_string(period) + " ms · 占空比 " + std::to_string(duty) + "% · 允许 VBUS 跌落 " + (droop ? "是（额外 5%）" : "否")));
}

std::string statusBlock(pdscope::DetailEmitter& em, ExtBlock& bk, const PdCtx& ctx) {
  if (ctx.link == "cable" || bk.dataSize <= 2) {
    fDet(em, bk, 0, "Internal Temp [Byte0]", needI(rd8(bk, 0), [](int v) { return tempText(v); }));
    fDet(em, bk, 1, "Flags [Byte1]", needI(rd8(bk, 1), [](int v) {
      if (!(v & 1)) return std::string("正常（未进入热关断）");
      std::string extra = (v & 0xFE) ? ("（高位 0x" + pdscope::hexU(v & 0xFE, 2) + " 为保留位）") : "";
      return "已进入热关断 Thermal Shutdown" + extra + " —— 置位后保持，只有 Hard Reset 或线缆断电才清除（Cable Reset 无效）";
    }));
    auto tc = rd8(bk, 0), fl = rd8(bk, 1);
    std::vector<std::string> out;
    if (tc) out.push_back("线缆插头温度 " + tempText(*tc));
    if (fl) out.push_back(std::string("热关断标记 ") + ((*fl & 1) ? "已置位" : "未置位"));
    return join_(out, " · ");
  }

  fDet(em, bk, 0, "Internal Temp [Byte0]", needI(rd8(bk, 0), [](int v) { return tempText(v); }));
  auto pi = rd8(bk, 1);
  fDet(em, bk, 1, "Present Input [Byte1]", needI(pi, [](int v) {
    const std::string srcArr[4] = { "内部供电", "外部直流 (DC)", "取值无效", "外部交流 (AC)" };
    const std::string src = srcArr[(v >> 1) & 3];
    std::vector<std::string> list;
    list.push_back(src);
    if (v & 8) list.push_back("由电池供电");
    if (v & 16) list.push_back("由非电池内部电源供电");
    return join_(list, " · ");
  }));
  auto bi = rd8(bk, 2);
  fDet(em, bk, 2, "Present Battery Input [Byte2]", needI(bi, [pi](int v) {
    if (!(pi && (*pi & 8))) return "0x" + pdscope::hexU(v, 2) + "（Present Input 未置电池供电，此域 Reserved）";
    std::vector<std::string> on;
    for (int i = 0; i < 8; i++) if (v & (1 << i)) on.push_back(batteryRefText(i));
    return on.empty() ? std::string("无电池供电") : join_(on, " · ");
  }));
  auto ev = rd8(bk, 3);
  fDet(em, bk, 3, "Event Flag [Byte3]", needI(ev, [](int v) {
    std::vector<std::string> on;
    for (const auto& b : STATUS_EVENT_BITS) if (v & (1 << b.bit)) on.push_back(b.name);
    return on.empty() ? std::string("无事件") : join_(on, " · ");
  }));
  auto ts = rd8(bk, 4);
  fDet(em, bk, 4, "Temperature Status [Byte4]", needI(ts, [](int v) { return tbl(TEMP_STATUS, (v >> 1) & 3, ""); }));
  fDet(em, bk, 5, "Power Status [Byte5]", needI(rd8(bk, 5), [&ctx](int v) {
    if (ctx.link == "cable") return "0x" + pdscope::hexU(v, 2);
    std::vector<std::string> on;
    for (const auto& b : POWER_STATUS_BITS) if (v & (1 << b.bit)) on.push_back(b.name);
    return v == 0 ? std::string("未受限（Sink 恒为 0）") : join_(on, " · ");
  }));
  auto ps = rd8(bk, 6);
  fDet(em, bk, 6, "Power State Change [Byte6]", needI(ps, [](int v) {
    return tbl(POWER_STATE, v & 7, "取值无效，按 0 处理") + " · 指示灯 " + tbl(STATE_INDICATOR, (v >> 3) & 3, "取值无效");
  }));
  fDet(em, bk, 7, "Reserved [Byte7]", needI(rd8(bk, 7), [](int v) { return "0x" + pdscope::hexU(v, 2); }));

  auto t = rd8(bk, 0), st = rd8(bk, 6);
  std::vector<std::string> out;
  if (t) out.push_back("内部温度 " + tempText(*t));
  if (st) out.push_back("电源状态 " + tbl(POWER_STATE, *st & 7, "—"));
  return join_(out, " · ");
}

std::string batteryRef(pdscope::DetailEmitter& em, ExtBlock& bk, const std::string& label, const std::string& fieldName) {
  auto r = rd8(bk, 0);
  em.object("数据对象 #1 · Byte 0~3");
  fDet(em, bk, 0, fieldName + " [Byte0]", needI(r, [](int v) {
    return v < 8 ? (std::to_string(v) + " · " + batteryRefText(v)) : (std::to_string(v) + " · 无效索引（对方应回 Invalid Battery Reference）");
  }));
  return r ? (label + " 电池 " + std::to_string(*r)) : label;
}

std::string bcdb(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto vid = rd16(bk, 0), pid = rd16(bk, 2), design = rd16(bk, 4), full = rd16(bk, 6), type = rd8(bk, 8);
  auto capText = [](const std::optional<int>& v) -> std::string {
    if (!v) return "（本分块不含该字段）";
    if (*v == 0) return "电池不存在 (0000h)";
    if (*v == 0xFFFF) return "未知 (FFFFh)";
    return pdNum(*v * 0.1) + " Wh";
  };
  fDet(em, bk, 0, "Vendor ID [Byte1-0]", needI(vid, [](int v) { return "0x" + pdscope::hexU(v, 4) + "（电池厂商）"; }));
  fDet(em, bk, 2, "Product ID [Byte3-2]", needI(pid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 4, "Battery Design Capacity [Byte5-4]", capText(design));
  fDet(em, bk, 6, "Battery Last Full Charge Capacity [Byte7-6]", capText(full));
  fDet(em, bk, 8, "Battery Type [Byte8]", needI(type, [](int v) { return pdFlag(v & 1, "无效电池引用", "电池引用有效"); }));
  return "设计容量 " + capText(design) + " · 满充容量 " + capText(full);
}

std::string getManufacturerInfo(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto target = rd8(bk, 0), ref = rd8(bk, 1);
  em.object("数据对象 #1 · Byte 0~3");
  fDet(em, bk, 0, "Manufacturer Info Target [Byte0]", needI(target, [](int v) {
    return v == 0 ? "0 · Port / Cable Plug" : (v == 1 ? "1 · Battery" : std::to_string(v) + " · 无效取值");
  }));
  fDet(em, bk, 1, "Manufacturer Info Ref [Byte1]", needI(ref, [target](int v) {
    if (target && *target != 1) return std::to_string(v) + "（Target ≠ Battery 时 Reserved）";
    return v < 8 ? (std::to_string(v) + " · " + batteryRefText(v)) : (std::to_string(v) + " · 无效索引");
  }));
  return target ? ("Get Manufacturer Info（" + std::string(*target == 1 ? "电池" : "端口/线缆") + "）") : "Get Manufacturer Info";
}

std::string manufacturerInfo(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto vid = rd16(bk, 0), pid = rd16(bk, 2);
  std::vector<uint8_t> strBytes;
  for (int i = 4; i < bk.dataSize; i++) {
    auto b = rd8(bk, i);
    if (!b) break;
    strBytes.push_back(static_cast<uint8_t>(*b));
  }
  const std::string text = pdAscii(strBytes);
  fDet(em, bk, 0, "Vendor ID [Byte1-0]", needI(vid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 2, "Product ID [Byte3-2]", needI(pid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 4, "Manufacturer String [Byte4…]", strBytes.empty() ? "（本分块不含该字段）" : ("\"" + text + "\""));
  return text.empty() ? "厂商串（空）" : ("厂商串 \"" + text + "\"");
}

std::string security(pdscope::DetailEmitter& em, ExtBlock& bk, const std::string& label) {
  em.object("数据块");
  em.detail("长度 [Data Size]", std::to_string(bk.dataSize) + " 字节" + (bk.chunked ? ("（分块 " + std::to_string(bk.chunkNum) + "）") : ""));
  hexDump(em, bk);
  em.detail("说明", "内容为 [USBC Auth] 定义的认证数据结构，USB PD 规范不定义其内部格式");
  return label + "（" + std::to_string(bk.dataSize) + " 字节" + (bk.chunked ? "，分块" : "") + "）";
}

std::string firmware(pdscope::DetailEmitter& em, ExtBlock& bk, const std::string& label) {
  em.object("数据块");
  em.detail("长度 [Data Size]", std::to_string(bk.dataSize) + " 字节" + (bk.chunked ? ("（分块 " + std::to_string(bk.chunkNum) + "）") : ""));
  hexDump(em, bk);
  em.detail("说明", "内容为 [PDFU] 定义的固件升级数据结构，USB PD 规范不定义其内部格式");
  return label + "（" + std::to_string(bk.dataSize) + " 字节" + (bk.chunked ? "，分块" : "") + "）";
}

void hexDump(pdscope::DetailEmitter& em, ExtBlock& bk) {
  for (size_t i = 0; i < bk.bytes.size(); i += 4) {
    int abs = bk.off + static_cast<int>(i);
    mkGroup(em, bk, abs);
    std::vector<uint8_t> chunk(bk.bytes.begin() + i, bk.bytes.begin() + std::min(i + 4, bk.bytes.size()));
    std::string s;
    for (size_t j = 0; j < chunk.size(); j++) s += pdscope::hexU(chunk[j], 2) + (j + 1 < chunk.size() ? " " : "");
    em.detail("Byte " + std::to_string(abs) + "~" + std::to_string(abs + static_cast<int>(chunk.size()) - 1), s);
  }
}

std::string ppsStatus(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto v = rd16(bk, 0), i = rd8(bk, 2), fl = rd8(bk, 3);
  auto vText = needI(v, [](int x) { return x == 0xFFFF ? "不支持 (FFFFh)" : (pdNum(x * 0.02) + " V"); });
  auto iText = needI(i, [](int x) { return x == 0xFF ? "不支持 (FFh)" : (pdNum(x * 0.05) + " A"); });
  if (!v) vText = "（本分块不含该字段）";
  if (!i) iText = "（本分块不含该字段）";
  fDet(em, bk, 0, "Output Voltage [Byte1-0]", vText);
  fDet(em, bk, 2, "Output Current [Byte2]", iText);
  fDet(em, bk, 3, "Real Time Flags [Byte3]", needI(fl, [](int x) {
    return "PTF " + tbl(TEMP_STATUS, (x >> 1) & 3, "") + " · OMF " + (x & 8 ? "Current Limit (CL)" : "Constant Voltage (CV)");
  }));
  return "输出 " + vText + " / " + iText;
}

std::string countryInfo(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto code = rd16(bk, 0);
  int cv = code ? *code : 0;
  fDet(em, bk, 0, "Country Code [Byte1-0]", needI(code, [](int v) { return "\"" + pdCharPair(v & 0xFF, (v >> 8) & 0xFF) + "\""; }));
  fDet(em, bk, 2, "Reserved [Byte3-2]", needI(rd16(bk, 2), [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  std::vector<uint8_t> data;
  for (int i = 4; i < bk.dataSize; i++) { auto b = rd8(bk, i); if (!b) break; data.push_back(static_cast<uint8_t>(*b)); }
  fDet(em, bk, 4, "Country Specific Data [Byte4…]", data.empty() ? "（本分块不含该字段）" : ("\"" + pdAscii(data) + "\""));
  return "国家码 \"" + pdCharPair(cv & 0xFF, (cv >> 8) & 0xFF) + "\"";
}

std::string countryCodes(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto len = rd8(bk, 0);
  fDet(em, bk, 0, "Length [Byte0]", needI(len, [](int v) { return std::to_string(v) + " 个国家和地区码（有效范围 1~12）"; }));
  fDet(em, bk, 1, "Reserved [Byte1]", needI(rd8(bk, 1), [](int v) { return "0x" + hexVarLower(static_cast<uint32_t>(v)); }));
  std::vector<std::string> list;
  for (int n = 0; n < 12; n++) {
    int i = 2 + n * 2;
    if (i >= bk.dataSize) break;
    auto v = rd16(bk, i);
    if (!v) break;
    std::string code = pdCharPair(*v & 0xFF, (*v >> 8) & 0xFF);
    if (!code.empty()) list.push_back(code);
    fDet(em, bk, i, "Country Code " + std::to_string(n + 1) + " [Byte" + std::to_string(i + 1) + "-" + std::to_string(i) + "]", "\"" + code + "\"");
  }
  return "共 " + std::to_string(list.size()) + " 个国家和地区码：" + join_(list, " ");
}

std::string skedb(PdState& /*st*/, pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto vid = rd16(bk, 0);
  auto pid = rd16(bk, 2);
  auto xid = rd32(bk, 4);
  fDet(em, bk, 0, "VID [Byte1-0]", needI(vid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 2, "PID [Byte3-2]", needI(pid, [](int v) { return "0x" + pdscope::hexU(v, 4); }));
  fDet(em, bk, 4, "XID [Byte7-4]", needU(xid, [](uint32_t v) { return "0x" + pdHex(v) + "（应与 Cert Stat VDO 一致）"; }));
  fDet(em, bk, 8, "FW Version [Byte8]", needI(rd8(bk, 8), [](int v) { return std::to_string(v); }));
  fDet(em, bk, 9, "HW Version [Byte9]", needI(rd8(bk, 9), [](int v) { return std::to_string(v); }));
  fDet(em, bk, 10, "SKEDB Version [Byte10]", needI(rd8(bk, 10), [](int v) { return v == 1 ? "Version 1.0" : (std::to_string(v) + " · 无效取值（接收端应忽略本条）"); }));
  fDet(em, bk, 11, "Load Step [Byte11]", needI(rd8(bk, 11), [](int v) { return tbl(LOAD_STEP, v & 3, ""); }));

  auto load = rd16(bk, 12);
  fDet(em, bk, 12, "Sink Load Characteristics [Byte13-12]", needI(load, [](int v) {
    int ov = std::min(v & 0x1F, 25) * 10;
    if ((v & 0x1F) == 0) return std::string("未指定过载需求（全 0）");
    return "过载 " + std::to_string(ov) + "% · 周期 " + std::to_string(((v >> 5) & 0x3F) * 20) + " ms · 占空比 "
      + std::to_string(((v >> 11) & 0xF) * 5) + "% · 允许额外 5% VBUS 跌落 " + (((v >> 15) & 1) ? "是" : "否");
  }));
  fDet(em, bk, 14, "Compliance [Byte14]", needI(rd8(bk, 14), [](int v) {
    std::vector<std::string> parts;
    if (v & 1) parts.push_back("LPS");
    if ((v >> 1) & 1) parts.push_back("PS1");
    if ((v >> 2) & 1) parts.push_back("PS2");
    return parts.empty() ? std::string("无") : join_(parts, " + ");
  }));
  fDet(em, bk, 15, "Touch Temp [Byte15]", needI(rd8(bk, 15), [](int v) { return tbl(TOUCH_TEMP_SINK, v, "取值无效，按默认处理"); }));
  auto binfo = rd8(bk, 16);
  fDet(em, bk, 16, "Battery Info [Byte16]", needI(binfo, [](int v) {
    return "固定电池 " + std::to_string(v & 0xF) + " 个 · 热插拔电池槽 " + std::to_string((v >> 4) & 0xF) + " 个";
  }));
  auto modes = rd8(bk, 17);
  fDet(em, bk, 17, "Sink Modes [Byte17]", needI(modes, [](int v) {
    std::vector<std::string> parts;
    if (v & 1) parts.push_back("支持 PPS 充电");
    if ((v >> 1) & 1) parts.push_back("可由 VBUS 供电");
    if ((v >> 2) & 1) parts.push_back("可由 AC 供电");
    if ((v >> 3) & 1) parts.push_back("可由电池供电");
    if ((v >> 4) & 1) parts.push_back("电池容量视为无限");
    if ((v >> 5) & 1) parts.push_back("支持 AVS");
    return parts.empty() ? std::string("无") : join_(parts, " · ");
  }));
  auto sprs = rd8(bk, 18), sprOp = rd8(bk, 19), sprMax = rd8(bk, 20);
  auto eprs = rd8(bk, 21), eprOp = rd8(bk, 22), eprMax = rd8(bk, 23);
  auto w = [](const std::optional<int>& v) -> std::string {
    if (!v) return "（本分块不含该字段）";
    return *v > 240 ? (std::to_string(*v) + "（超出有效范围）") : (std::to_string(*v) + " W");
  };
  fDet(em, bk, 18, "SPR Sink Minimum PDP [Byte18]", w(sprs));
  fDet(em, bk, 19, "SPR Sink Operational PDP [Byte19]", w(sprOp));
  fDet(em, bk, 20, "SPR Sink Maximum PDP [Byte20]", w(sprMax));
  fDet(em, bk, 21, "EPR Sink Minimum PDP [Byte21]", w(eprs));
  fDet(em, bk, 22, "EPR Sink Operational PDP [Byte22]", w(eprOp));
  fDet(em, bk, 23, "EPR Sink Maximum PDP [Byte23]", w(eprMax));

  std::string out = "SPR PDP " + (sprs ? std::to_string(*sprs) : "—") + "/"
    + (sprOp ? std::to_string(*sprOp) : "—") + "/" + (sprMax ? std::to_string(*sprMax) : "—") + " W";
  if (eprMax && *eprMax) out += " · EPR PDP " + (eprs ? std::to_string(*eprs) : "—") + "/"
    + (eprOp ? std::to_string(*eprOp) : "—") + "/" + std::to_string(*eprMax) + " W";
  return out;
}

std::string extControl(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto type = rd8(bk, 0), data = rd8(bk, 1);
  em.object("数据对象 #1 · Byte 0~3");
  fDet(em, bk, 0, "Type [Byte0]", needI(type, [](int v) { return std::to_string(v) + " · " + tbl(EXT_CONTROL_MSG_TYPES, v, "无效，接收端应回 Not_Supported"); }));
  fDet(em, bk, 1, "Data [Byte1]", needI(data, [](int v) { return "0x" + pdscope::hexU(v, 2) + "（表 6.63 规定为 0）"; }));
  return type ? tbl(EXT_CONTROL_MSG_TYPES, *type, "未知扩展控制类型 " + std::to_string(*type)) : "Extended Control";
}

struct Slot { int index; std::optional<uint32_t> value; std::string note; };

uint32_t bytesToU32(const std::vector<uint8_t>& arr) {
  uint32_t v = 0;
  for (size_t i = 0; i < arr.size() && i < 4; i++) v |= (static_cast<uint32_t>(arr[i]) & 0xFFu) << (8 * i);
  return v;
}

bool isEprPdo(uint32_t pdo, int objPos) {
  if (objPos >= 8) return true;
  int supplyType = static_cast<int>(pdField(pdo, 31, 30));
  if (supplyType == 0) return pdField(pdo, 19, 10) * 0.05 > 20.0;
  if (supplyType == 3) return pdField(pdo, 29, 28) == 1;
  return false;
}

std::vector<Slot> pdoSlots(ExtBlock& bk) {
  std::vector<Slot> slots;
  int lead = bk.off % 4;
  int cur = bk.off;
  int end = bk.off + static_cast<int>(bk.bytes.size());

  if (lead != 0) {
    int objIdx = bk.off / 4;
    int have = lead;
    if (bk.carry && bk.carry->objIdx == objIdx
        && static_cast<int>(bk.carry->bytes.size()) == have) {
      std::vector<uint8_t> full = bk.carry->bytes;
      int take = std::min(4 - have, static_cast<int>(bk.bytes.size()));
      for (int k = 0; k < take; k++) full.push_back(bk.bytes[k]);
      slots.push_back({objIdx + 1, bytesToU32(full), "由上一分块 + 本分块拼接"});
    } else {
      slots.push_back({objIdx + 1, std::nullopt, "跨分块（本包只有该对象的第 " + std::to_string(have) + "~3 字节）"});
    }
    cur = bk.off + (4 - have);
  }
  while (cur + 4 <= end) {
    std::vector<uint8_t> chunk(bk.bytes.begin() + (cur - bk.off), bk.bytes.begin() + (cur - bk.off) + 4);
    slots.push_back({cur / 4 + 1, bytesToU32(chunk), ""});
    cur += 4;
  }
  if (cur < end) {
    ExtCarry tail;
    tail.objIdx = cur / 4;
    for (int k = cur - bk.off; k < end - bk.off; k++) tail.bytes.push_back(bk.bytes[k]);
    bk.tail = tail;
  }
  return slots;
}

std::string eprCaps(PdState& st, pdscope::DetailEmitter& em, ExtBlock& bk, const std::string& role) {
  std::vector<Slot> slots = pdoSlots(bk);
  std::vector<std::string> found;
  for (const auto& s : slots) {
    em.object("PDO #" + std::to_string(s.index));
    if (!s.value) {
      em.detail("状态", s.note);
      continue;
    }
    em.detail("对象位置", std::to_string(s.index) + "（Enter/Exit Mode 与 EPR_Request 用该位置引用）");
    if (!s.note.empty()) em.detail("分块对齐", s.note);
    int objPos = s.index;
    if (objPos <= 7 && *s.value == 0) {
      em.detail("填充", "SPR 槽位未使用，按 0 填充");
      found.push_back("#" + std::to_string(objPos) + " SPR 填充");
      continue;
    }
    bool isEpr = isEprPdo(*s.value, objPos);
    std::string r = pdoParse(st, em, *s.value, role, objPos, isEpr, "");
    found.push_back("#" + std::to_string(objPos) + " " + r);
  }
  return found.empty() ? "（无 PDO）" : join_(found, " · ");
}

std::string vendorDefinedExtended(pdscope::DetailEmitter& em, ExtBlock& bk) {
  auto svid = rd16(bk, 0), cmd = rd16(bk, 2);
  fDet(em, bk, 0, "SVID [Byte1-0]", needI(svid, [](int v) { return svidText(static_cast<uint16_t>(v)); }));
  fDet(em, bk, 2, "Command Space [Byte3-2]", needI(cmd, [](int v) {
    return "0x" + pdscope::hexU(v, 4) + "（B15 Reserved = " + std::to_string((v >> 15) & 1) + "，B14-0 厂商命令 " + std::to_string(v & 0x7FFF) + "）";
  }));
  std::vector<uint8_t> data;
  for (int i = 4; i < bk.dataSize; i++) { auto b = rd8(bk, i); if (!b) break; data.push_back(static_cast<uint8_t>(*b)); }
  if (data.empty()) {
    fDet(em, bk, 4, "Vendor Defined Data [Byte4…]", "（本分块不含该字段）");
  } else {
    std::string hex;
    for (size_t j = 0; j < data.size() && j < 16; j++) hex += pdscope::hexU(data[j], 2) + (j + 1 < std::min(data.size(), (size_t)16) ? " " : "");
    if (data.size() > 16) hex += " …";
    fDet(em, bk, 4, "Vendor Defined Data [Byte4…]", std::to_string(data.size()) + " 字节 · " + hex);
  }
  int sv = svid ? *svid : 0, cm = cmd ? *cmd : 0;
  return "厂商扩展消息 SID 0x" + pdscope::hexU(sv, 4) + " · 命令 0x" + pdscope::hexU(cm, 4);
}

std::string join_(const std::vector<std::string>& v, const std::string& sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) { if (i) s += sep; s += v[i]; }
  return s;
}

}  // namespace

std::string extendedParse(PdState& st, pdscope::DetailEmitter& em, int t, ExtBlock& bk, const PdCtx& ctx) {
  bk.group = -1;
  switch (t) {
    case 1: return scedb(st, em, bk);
    case 2: return statusBlock(em, bk, ctx);
    case 3: return batteryRef(em, bk, "Get Battery Cap", "Battery Cap Ref");
    case 4: return batteryRef(em, bk, "Get Battery Status", "Battery Status Ref");
    case 5: return bcdb(em, bk);
    case 6: return getManufacturerInfo(em, bk);
    case 7: return manufacturerInfo(em, bk);
    case 8: case 9: return security(em, bk, t == 8 ? "Security_Request" : "Security_Response");
    case 10: case 11: return firmware(em, bk, t == 10 ? "Firmware_Update_Request" : "Firmware_Update_Response");
    case 12: return ppsStatus(em, bk);
    case 13: return countryInfo(em, bk);
    case 14: return countryCodes(em, bk);
    case 15: return skedb(st, em, bk);
    case 16: return extControl(em, bk);
    case 17: case 18: return eprCaps(st, em, bk, t == 17 ? "source" : "sink");
    case 30: return vendorDefinedExtended(em, bk);
    default:
      hexDump(em, bk);
      return "未定义的扩展消息类型 " + std::to_string(t);
  }
}

}}  // namespace pdscope::pd
