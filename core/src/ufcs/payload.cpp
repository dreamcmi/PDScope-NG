// payload.cpp — UFCS 各类消息的载荷逐字段解析（实现见 payload.h）
//
// 中文字面值原样保留（UTF-8）。

#include "payload.h"
#include "tables.h"
#include "format.h"
#include "util.h"

#include <map>
#include <set>

namespace pdscope { namespace ufcs {

/* ────────────────────────── 小工具 ────────────────────────── */

static std::string at(const std::map<int, std::string>& m, int k, const std::string& def) {
  auto it = m.find(k);
  return it == m.end() ? def : it->second;
}

/** 一段数据按固定长度切成若干项；返回项数与余数。 */
static void chunk(size_t n, size_t unit, size_t& items, size_t& rest) {
  items = n / unit;
  rest = n % unit;
}

/** 长度校验：不符就记一条 detail 告警（不丢信息）。 */
static void checkLen(pdscope::DetailEmitter& em, size_t actual, size_t expect) {
  if (actual == expect) return;
  em.detail("⚠ 长度异常", "规范要求 " + std::to_string(expect) + " 字节，实际 " + std::to_string(actual) + " 字节");
}

static std::string join(const std::vector<std::string>& parts) {
  std::string s;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) s += "、";
    s += parts[i];
  }
  return s;
}

/* ────────────────────────── 输出模式（表 16）────────────────────────── */

static std::string outputMode(pdscope::DetailEmitter& em, const uint8_t* m, int i) {
  int modeNo = static_cast<int>(ufcsBits(m, 8, 63, 60));
  int curStep = static_cast<int>(ufcsBits(m, 8, 59, 57));
  int voltStep = ufcsBit(m, 8, 56);
  int maxV = static_cast<int>(ufcsBits(m, 8, 55, 40));
  int minV = static_cast<int>(ufcsBits(m, 8, 39, 24));
  int maxI = static_cast<int>(ufcsBits(m, 8, 23, 8));
  int minI = static_cast<int>(ufcsBits(m, 8, 7, 0));

  em.object("输出模式 #" + std::to_string(i + 1));
  em.detail("输出模式编号 [" + ufcsRange(63, 60) + "]",
            std::to_string(modeNo) + "（规范要求 1…7，且与顺序一致）");
  em.detail("电流调节步进 [" + ufcsRange(59, 57) + "]",
            std::to_string(curStep) + " · " + at(kCurrentStep, curStep, "保留值"));
  em.detail("电压调节步进 [B56]", std::to_string(voltStep) + " · " + at(kVoltageStep, voltStep, "保留值"));
  em.detail("最大输出电压 [" + ufcsRange(55, 40) + "]", std::to_string(maxV) + " × 10mV = " + ufcsVolt(maxV));
  em.detail("最小输出电压 [" + ufcsRange(39, 24) + "]", std::to_string(minV) + " × 10mV = " + ufcsVolt(minV));
  em.detail("最大输出电流 [" + ufcsRange(23, 8) + "]", std::to_string(maxI) + " × 10mA = " + ufcsAmp(maxI));
  em.detail("最小输出电流 [" + ufcsRange(7, 0) + "]", std::to_string(minI) + " × 10mA = " + ufcsAmp(minI));
  return "模式" + std::to_string(modeNo) + ": " + ufcsVolt(minV) + "–" + ufcsVolt(maxV)
       + " / " + ufcsAmp(minI) + "–" + ufcsAmp(maxI)
       + "（步进 " + at(kVoltageStep, voltStep, "?") + "/" + at(kCurrentStep, curStep, "?") + "）";
}

/* ────────────────────────── 各命令解析器 ────────────────────────── */

static std::string payloadOutputCapabilities(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  size_t items, rest;
  chunk(dn, 8, items, rest);
  if (items == 0) {
    em.detail("数据", "0x" + ufcsHex(d, dn));
    return "数据长度为 0，无法解析输出模式";
  }
  if (rest) checkLen(em, dn, items * 8);
  // if (items > 7) … MODE 告警（badge 由 decoder 侧补）
  std::vector<std::string> parts;
  for (size_t i = 0; i < items; i++) parts.push_back(outputMode(em, d + i * 8, static_cast<int>(i)));
  std::string s = std::to_string(items) + " 种输出模式 · " + parts[0] + (items > 1 ? " …" : "");
  em.note(s);
  return s;
}

static std::string payloadRequest(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 8);
  int modeNo = static_cast<int>(ufcsBits(d, dn, 63, 60));
  int volt = static_cast<int>(ufcsBits(d, dn, 31, 16));
  int cur = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("请求数据（Request）");
  em.detail("输出模式编号 [" + ufcsRange(63, 60) + "]",
            std::to_string(modeNo) + "（引用 Output_Capabilities 中的第几个输出模式）");
  em.detail("保留 [" + ufcsRange(59, 32) + "]", ufcsReserved(d, dn, 59, 32));
  em.detail("请求输出电压 [" + ufcsRange(31, 16) + "]", std::to_string(volt) + " × 10mV = " + ufcsVolt(volt));
  em.detail("请求输出电流 [" + ufcsRange(15, 0) + "]", std::to_string(cur) + " × 10mA = " + ufcsAmp(cur));
  std::string s = "请求模式 " + std::to_string(modeNo) + "：" + ufcsVolt(volt) + " / " + ufcsAmp(cur);
  em.note(s);
  return s;
}

static std::string payloadSourceInfo(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 8);
  int period = static_cast<int>(ufcsBits(d, dn, 51, 48));
  int tIn = static_cast<int>(ufcsBits(d, dn, 47, 40));
  int tOut = static_cast<int>(ufcsBits(d, dn, 39, 32));
  int volt = static_cast<int>(ufcsBits(d, dn, 31, 16));
  int cur = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("状态信息（Source_Information）");
  em.detail("保留 [" + ufcsRange(63, 52) + "]", ufcsReserved(d, dn, 63, 52));
  em.detail("Sink_Info_Extended 最小周期 [" + ufcsRange(51, 48) + "]",
            period == 0 ? "0000b：不支持充电设备主动上报 Sink_Information_Extended"
                        : std::to_string(period) + " × 100ms = " + std::to_string(period * 100) + " ms");
  em.detail("内部温度 [" + ufcsRange(47, 40) + "]", std::to_string(tIn) + " → " + ufcsTemp(tIn));
  em.detail("输出口温度 [" + ufcsRange(39, 32) + "]", std::to_string(tOut) + " → " + ufcsTemp(tOut));
  em.detail("当前输出电压 [" + ufcsRange(31, 16) + "]", std::to_string(volt) + " × 10mV = " + ufcsVolt(volt));
  em.detail("当前输出电流 [" + ufcsRange(15, 0) + "]", std::to_string(cur) + " × 10mA = " + ufcsAmp(cur));
  std::string s = "输出 " + ufcsVolt(volt) + " / " + ufcsAmp(cur) + " · 内部 " + ufcsTemp(tIn) + " · 接口 " + ufcsTemp(tOut);
  em.note(s);
  return s;
}

static std::string payloadSinkInfo(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 8);
  int tBat = static_cast<int>(ufcsBits(d, dn, 47, 40));
  int tIn = static_cast<int>(ufcsBits(d, dn, 39, 32));
  int volt = static_cast<int>(ufcsBits(d, dn, 31, 16));
  int cur = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("状态信息（Sink_Information）");
  em.detail("保留 [" + ufcsRange(63, 48) + "]", ufcsReserved(d, dn, 63, 48));
  em.detail("电池温度 [" + ufcsRange(47, 40) + "]", std::to_string(tBat) + " → " + ufcsTemp(tBat));
  em.detail("输入接口温度 [" + ufcsRange(39, 32) + "]", std::to_string(tIn) + " → " + ufcsTemp(tIn));
  em.detail("当前充电电压 [" + ufcsRange(31, 16) + "]",
            std::to_string(volt) + " × 10mV = " + ufcsVolt(volt) + "（进入充电 IC 之前）");
  em.detail("当前充电电流 [" + ufcsRange(15, 0) + "]",
            std::to_string(cur) + " × 10mA = " + ufcsAmp(cur) + "（进入充电 IC 之前）");
  std::string s = "充电 " + ufcsVolt(volt) + " / " + ufcsAmp(cur) + " · 电池 " + ufcsTemp(tBat) + " · 接口 " + ufcsTemp(tIn);
  em.note(s);
  return s;
}

static std::string payloadCableInfo(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 10);
  int vid = static_cast<int>(ufcsBits(d, dn, 79, 64));
  int custom = static_cast<int>(ufcsBits(d, dn, 63, 48));
  int imp = static_cast<int>(ufcsBits(d, dn, 47, 32));
  int maxV = static_cast<int>(ufcsBits(d, dn, 31, 16));
  int maxI = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("线缆信息（Cable_Information）");
  em.detail("厂家识别码 [" + ufcsRange(79, 64) + "]", ufcsHexNum(vid));
  em.detail("厂家自定义识别码 [" + ufcsRange(63, 48) + "]", ufcsHexNum(custom));
  em.detail("线缆阻抗 [" + ufcsRange(47, 32) + "]", std::to_string(imp) + " mΩ");
  em.detail("最大承载电压 [" + ufcsRange(31, 16) + "]", std::to_string(maxV) + " × 10mV = " + ufcsVolt(maxV));
  em.detail("最大承载电流 [" + ufcsRange(15, 0) + "]", std::to_string(maxI) + " × 10mA = " + ufcsAmp(maxI));
  std::string s = "线缆承载 " + ufcsVolt(maxV) + " / " + ufcsAmp(maxI) + " · 阻抗 " + std::to_string(imp)
                + " mΩ · VID " + ufcsHexNum(vid);
  em.note(s);
  return s;
}

static std::string payloadDeviceInfo(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 8);
  int vid = static_cast<int>(ufcsBits(d, dn, 63, 48));
  int custom = static_cast<int>(ufcsBits(d, dn, 47, 32));
  int hw = static_cast<int>(ufcsBits(d, dn, 31, 16));
  int sw = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("设备信息（Device_Information）");
  em.detail("厂家识别码 [" + ufcsRange(63, 48) + "]", ufcsHexNum(vid));
  em.detail("厂家自定义识别码 [" + ufcsRange(47, 32) + "]", ufcsHexNum(custom));
  em.detail("设备硬件版本号 [" + ufcsRange(31, 16) + "]", ufcsHexNum(hw) + "（厂家自定义格式，0 = 未填写）");
  em.detail("设备软件版本号 [" + ufcsRange(15, 0) + "]", ufcsHexNum(sw) + "（厂家自定义格式，0 = 未填写）");
  std::string s = "VID " + ufcsHexNum(vid) + " · HW " + ufcsHexNum(hw) + " / SW " + ufcsHexNum(sw);
  em.note(s);
  return s;
}

static std::string payloadErrorInfo(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 4);
  em.object("异常信息（Error_Information）");
  std::vector<std::string> fired;
  for (const auto& e : kErrorBits) {
    bool on = ufcsBit(d, dn, e.bit) != 0;
    em.detail(e.name + " [B" + std::to_string(e.bit) + "]", ufcsFlag(on, e.text, "正常"));
    if (on) fired.push_back(e.name);
  }
  em.detail("保留 [" + ufcsRange(31, 9) + "]", ufcsReserved(d, dn, 31, 9));
  em.detail("保留 [" + ufcsRange(5, 0) + "]", ufcsReserved(d, dn, 5, 0));
  std::string s = fired.empty() ? "无异常（D+/D-/CC 均正常）" : ("异常：" + join(fired));
  em.note(s);
  return s;
}

static std::string payloadConfigWatchdog(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 2);
  int ms = static_cast<int>(ufcsBits(d, dn, 15, 0));
  em.object("配置信息（Config_Watchdog）");
  em.detail("看门狗溢出时间 [" + ufcsRange(15, 0) + "]",
            ms == 0 ? "0 → 关闭看门狗功能" : (std::to_string(ms) + " ms"));
  std::string s = (ms == 0) ? "关闭看门狗" : ("看门狗 " + std::to_string(ms) + " ms");
  em.note(s);
  return s;
}

static std::string payloadRefuse(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 4);
  int msgNo = static_cast<int>(ufcsBits(d, dn, 27, 24));
  int mtype = static_cast<int>(ufcsBits(d, dn, 18, 16));
  int cmd = static_cast<int>(ufcsBits(d, dn, 15, 8));
  int reason = static_cast<int>(ufcsBits(d, dn, 7, 0));
  em.object("反馈信息（Refuse）");
  em.detail("保留 [" + ufcsRange(31, 28) + "]", ufcsReserved(d, dn, 31, 28));
  em.detail("被拒消息的消息编号 [" + ufcsRange(27, 24) + "]", std::to_string(msgNo));
  em.detail("保留 [" + ufcsRange(23, 19) + "]", ufcsReserved(d, dn, 23, 19));
  em.detail("被拒消息的消息类型 [" + ufcsRange(18, 16) + "]",
            std::to_string(mtype) + " · " + at(kMsgType, mtype, "保留值"));
  em.detail("被拒消息的命令编号 [" + ufcsRange(15, 8) + "]", "0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
  em.detail("拒绝原因 [" + ufcsRange(7, 0) + "]",
            "0x" + pdscope::hexU(static_cast<uint64_t>(reason), 2) + " · " + at(kRefuseReason, reason, "保留值"));
  std::string s = "拒绝 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2) + "（消息类型 " + std::to_string(mtype)
                + "）：" + at(kRefuseReason, reason, "原因码 0x" + pdscope::hexU(static_cast<uint64_t>(reason), 2));
  em.note(s);
  return s;
}

static std::string payloadVerifyRequest(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 17);
  int key = (dn > 0) ? d[0] : 0;
  size_t randN = (dn > 1) ? (dn - 1) : 0;
  em.object("鉴权请求（Verify_Request）");
  em.detail("密钥编号 [B0]", "0x" + pdscope::hexU(static_cast<uint64_t>(key), 2));
  em.detail("随机数据 [16 字节]", "0x" + ufcsHex(d + 1, randN));
  std::string s = "密钥编号 0x" + pdscope::hexU(static_cast<uint64_t>(key), 2) + " · 随机数 0x"
                + ufcsHex(d + 1, randN).substr(0, 8) + "…";
  em.note(s);
  return s;
}

static std::string payloadVerifyResponse(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 48);
  size_t encN = (dn > 32) ? 32 : dn;
  size_t randOff = (dn > 32) ? 32 : dn;
  size_t randN = (dn > 32) ? (dn - 32) : 0;
  em.object("鉴权应答（Verify_Response）");
  em.detail("加密数据 [32 字节]", "0x" + ufcsHex(d, encN));
  em.detail("随机数据 [16 字节]", "0x" + ufcsHex(d + randOff, randN));
  std::string s = "加密数据 0x" + ufcsHex(d, encN).substr(0, 8) + "… · 随机数 0x"
                + ufcsHex(d + randOff, randN).substr(0, 8) + "…";
  em.note(s);
  return s;
}

static std::string payloadPowerChange(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  size_t items, rest;
  chunk(dn, 3, items, rest);
  if (items == 0) {
    em.detail("数据", "0x" + ufcsHex(d, dn));
    return "数据长度为 0";
  }
  if (rest) checkLen(em, dn, items * 3);
  // if (items > 7) … MODE 告警（badge 由 decoder 侧补）
  std::vector<std::string> parts;
  for (size_t i = 0; i < items; i++) {
    const uint8_t* m = d + i * 3;
    int modeNo = static_cast<int>(ufcsBits(m, 3, 23, 20));
    bool fast = ufcsBit(m, 3, 19) != 0;
    int maxI = static_cast<int>(ufcsBits(m, 3, 15, 0));
    em.object("输出模式 #" + std::to_string(i + 1));
    em.detail("输出模式编号 [" + ufcsRange(23, 20) + "]", std::to_string(modeNo));
    em.detail("快速调整输出功率 [B19]", ufcsFlag(fast, "要求立即一次性降到该电流", "可在 1 秒内逐步调整"));
    em.detail("保留 [" + ufcsRange(18, 16) + "]", ufcsReserved(m, 3, 18, 16));
    em.detail("最大输出电流 [" + ufcsRange(15, 0) + "]", std::to_string(maxI) + " × 10mA = " + ufcsAmp(maxI));
    parts.push_back("模式" + std::to_string(modeNo) + "→" + ufcsAmp(maxI));
  }
  std::string s = "最大输出电流变更：" + join(parts);
  em.note(s);
  return s;
}

static std::string payloadSinkInfoExtended(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  size_t items, rest;
  chunk(dn, 3, items, rest);
  if (items == 0) {
    em.detail("数据", "0x" + ufcsHex(d, dn));
    return "数据长度为 0";
  }
  if (rest) checkLen(em, dn, items * 3);
  // if (items > 15) … ITEM 告警（badge 由 decoder 侧补）
  std::vector<std::string> parts;
  std::set<int> seen;
  for (size_t i = 0; i < items; i++) {
    const uint8_t* m = d + i * 3;
    int type = static_cast<int>(ufcsBits(m, 3, 23, 20));
    int val = static_cast<int>(ufcsBits(m, 3, 15, 0));
    em.object("状态信息 #" + std::to_string(i + 1));
    // type.toString(2).padStart(4,'0') + 'b'
    std::string bin;
    for (int b = 3; b >= 0; b--) bin += ((type >> b) & 1) ? '1' : '0';
    em.detail("状态信息类型 [" + ufcsRange(23, 20) + "]", bin + "b · " + at(kExtStatusType, type, "保留值"));
    em.detail("保留 [" + ufcsRange(19, 16) + "]", ufcsReserved(m, 3, 19, 16));
    std::string text;
    if (type == 0b0001) text = ufcsNum(static_cast<double>(val) / 100.0) + " %（" + std::to_string(val) + " × 0.01%）";
    else if (type == 0b0010) text = std::to_string(val) + " W";
    else text = "原始值 " + std::to_string(val);
    em.detail("状态数据 [" + ufcsRange(15, 0) + "]", text);
    // if (seen.has(type) && (type==1||type==2)) … DUP 告警（badge 由 decoder 侧补）
    seen.insert(type);
    parts.push_back(at(kExtStatusType, type, "类型" + std::to_string(type)) + "=" + text);
  }
  std::string s = join(parts);
  em.note(s);
  return s;
}

static std::string payloadTestRequest(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn) {
  checkLen(em, dn, 2);
  bool en = ufcsBit(d, dn, 15) != 0;
  bool vacc = ufcsBit(d, dn, 14) != 0;
  int addr = static_cast<int>(ufcsBits(d, dn, 13, 11));
  int mtype = static_cast<int>(ufcsBits(d, dn, 10, 8));
  int cmd = static_cast<int>(ufcsBits(d, dn, 7, 0));
  em.object("测试内容（Test_Request）");
  em.detail("使能测试模式 [B15]", ufcsFlag(en, "被测设备工作在测试模式", "正常模式"));
  em.detail("电压精度测试模式 [B14]", ufcsFlag(vacc, "输出电流可比设置值偏大 10%", "关闭"));
  std::string addrText = at(kDevAddr, addr, addr == 0 ? "未指定" : "保留");
  // addr.toString(2).padStart(3,'0') + 'b'
  std::string abin;
  for (int b = 2; b >= 0; b--) abin += ((addr >> b) & 1) ? '1' : '0';
  em.detail("设备地址 [" + ufcsRange(13, 11) + "]", abin + "b · " + addrText);
  em.detail("消息类型 [" + ufcsRange(10, 8) + "]", std::to_string(mtype) + " · " + at(kMsgType, mtype, "保留值"));
  em.detail("命令编号 [" + ufcsRange(7, 0) + "]", "0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
  bool onlyEnable = (addr == 0b111 && mtype == 0b111 && cmd == 0xFF);
  std::string s;
  if (onlyEnable) {
    s = "仅" + std::string(en ? "使能" : "关闭") + "测试模式" + (vacc ? " + 电压精度测试" : "")
      + "（未命令发送具体消息）";
  } else {
    std::string tgt = at(kDevAddr, addr, "地址" + std::to_string(addr));
    s = "命令 " + tgt + " 发送 类型" + std::to_string(mtype) + "/命令0x"
      + pdscope::hexU(static_cast<uint64_t>(cmd), 2);
  }
  em.note(s);
  return s;
}

static std::string payloadUnknown(pdscope::DetailEmitter& em, const uint8_t* d, size_t dn, int cmd) {
  em.object("未知命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2) + " 的数据");
  em.detail("原始数据", "0x" + ufcsHex(d, dn));
  std::string s = "未定义的数据命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2)
                + "（" + std::to_string(dn) + " 字节，按原始字节列出）";
  em.note(s);
  return s;
}

/* ────────────────────────── 公开入口 ────────────────────────── */

PayloadResult ufcsDataPayload(pdscope::DetailEmitter& em, int cmd, const uint8_t* p, size_t n,
                              const std::string& ctxRevText) {
  (void)ctxRevText;  // 当前语义下版本文本不参与解析，仅占位保持契约签名
  PayloadResult r;
  switch (cmd) {
    case 0x01: r.summary = payloadOutputCapabilities(em, p, n); break;
    case 0x02: r.summary = payloadRequest(em, p, n); break;
    case 0x03: r.summary = payloadSourceInfo(em, p, n); break;
    case 0x04: r.summary = payloadSinkInfo(em, p, n); break;
    case 0x05: r.summary = payloadCableInfo(em, p, n); break;
    case 0x06: r.summary = payloadDeviceInfo(em, p, n); break;
    case 0x07: r.summary = payloadErrorInfo(em, p, n); break;
    case 0x08: r.summary = payloadConfigWatchdog(em, p, n); break;
    case 0x09: r.summary = payloadRefuse(em, p, n); break;
    case 0x0A: r.summary = payloadVerifyRequest(em, p, n); break;
    case 0x0B: r.summary = payloadVerifyResponse(em, p, n); break;
    case 0x0C: r.summary = payloadPowerChange(em, p, n); break;
    case 0x0D: r.summary = payloadSinkInfoExtended(em, p, n); break;
    case 0xFF: r.summary = payloadTestRequest(em, p, n); break;
    default:    r.summary = payloadUnknown(em, p, n, cmd); break;
  }
  return r;
}

PayloadResult ufcsCustomPayload(pdscope::DetailEmitter& em, const uint8_t* p, size_t n) {
  int vid = (n >= 2) ? ((p[0] << 8) | p[1]) : 0;
  size_t dataOff = (n >= 3) ? 3 : n;
  size_t dataN = (n >= 3) ? (n - 3) : 0;
  PayloadResult r;
  em.object("厂家自定义消息");
  em.detail("厂家识别码 [2 字节]", ufcsHexNum(vid));
  em.detail("数据长度", std::to_string(dataN) + " 字节");
  em.detail("数据", dataN ? ("0x" + ufcsHex(p + dataOff, dataN)) : "（空）");
  r.summary = "厂家自定义（识别码 " + ufcsHexNum(vid) + "，" + std::to_string(dataN) + " 字节数据）";
  em.note(r.summary);
  return r;
}

std::string ufcsDumpBytes(const uint8_t* p, size_t n) {
  size_t m = (n > 48) ? 48 : n;
  if (m == 0) return "（空）";
  return ufcsHexSpaced(p, m);
}

}}  // namespace pdscope::ufcs
