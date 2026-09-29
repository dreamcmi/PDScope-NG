// decoder.cpp — UFCS 报文解码器（实现见 decoder.h）
//
// 逐行移植自 PDScope/src/js/ufcs/decoder.js。中文字面值原样保留（UTF-8）。

#include "decoder.h"
#include "tables.h"
#include "crc.h"
#include "frame.h"
#include "format.h"
#include "payload.h"
#include "util.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace pdscope { namespace ufcs {

namespace {

constexpr int kDefaultBaud = 115200;   // 规范 7.4.6：115200 为缺省支持档位
constexpr int kWarnWindow = 16;         // ACK/NCK 向前配对窗口

std::string binStr(int v, int width) {
  std::string s;
  for (int b = width - 1; b >= 0; b--) s += ((v >> b) & 1) ? '1' : '0';
  return s;
}

std::string at(const std::map<int, std::string>& m, int k, const std::string& def) {
  auto it = m.find(k);
  return it == m.end() ? def : it->second;
}

std::string fmtMs(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  return std::string(buf);
}

std::string joinSummary(const std::vector<std::string>& parts) {
  std::string s;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) s += " ; ";
    s += parts[i];
  }
  return s;
}

struct DirResult {
  std::string sender;     // 角色：SRC / SNK / Plug
  std::string receiver;   // 角色
  bool inferred = false;
  bool ambiguous = false;
  std::string source;     // "spec" / "line" / "addr"
  std::string line;       // 物理链路 "D+" / "D-" / ""
  bool mismatch = false;
  std::string expect;     // 期望的接收方角色，或 ""
};

/**
 * 把「设备地址 + 命令 + 链路」还原成「发送方 / 接收方」。三级判据，越靠前越硬：
 *   ① 规范单向定义（kFixedDir）—— 例如 Output_Capabilities 只可能是供电设备发给充电设备
 *   ② 物理链路（容器给出的链路字节）—— 供电设备 D+ 是 TX、充电设备 D- 是 TX
 *   ③ 按接收方推断 —— 两条都没有时只能推断（规范未规定如何取舍），标记 inferred
 */
DirResult ufcsResolveDirection(int addr, const std::string& line, const std::string& fixedSender) {
  std::string receiver = ufcsRoleOf(addr);

  if (!fixedSender.empty()) {
    std::string expect = ufcsPeerOf(fixedSender);
    DirResult r;
    r.sender = fixedSender;
    r.receiver = receiver;
    r.inferred = false;
    r.ambiguous = false;
    r.source = "spec";
    r.line = (kLine.count(fixedSender) ? kLine.at(fixedSender) : std::string());
    r.mismatch = (!expect.empty() && receiver != expect);
    r.expect = expect;
    return r;
  }

  if (line == "D+" || line == "D-") {
    std::string sender = (line == "D+")
      ? (addr == 0b001 ? std::string("Plug") : std::string("SRC"))
      : (addr == 0b010 ? std::string("Plug") : std::string("SNK"));
    DirResult r;
    r.sender = sender;
    r.receiver = receiver;
    r.inferred = false;
    r.ambiguous = false;
    r.source = "line";
    r.line = line;
    r.mismatch = false;
    r.expect = std::string();
    return r;
  }

  // 第三级兜底：本工具的取舍，规范并未规定此时该取谁。
  std::string sender = (addr == 0b010) ? std::string("SRC")
                      : (addr == 0b001) ? std::string("SNK")
                                        : std::string("SNK");
  DirResult r;
  r.sender = sender;
  r.receiver = receiver;
  r.inferred = true;
  r.ambiguous = (addr == 0b011 || addr == 0b001);
  r.source = "addr";
  r.line = (kLine.count(sender) ? kLine.at(sender) : std::string("D±"));
  r.mismatch = false;
  r.expect = std::string();
  return r;
}

}  // namespace

/* ────────────────────────── UfcsDecoder ────────────────────────── */

UfcsDecoder::UfcsDecoder(double sampleRate) : sampleRate_(sampleRate), packetSeq_(0) {
  reset();
}

void UfcsDecoder::reset() {
  packetSeq_ = 0;
}

std::optional<Packet> UfcsDecoder::decode(const uint8_t* frame, size_t len, const DecodeOpts& opts) {
  (void)sampleRate_;  // 由 timeMs 直接给出采样坐标（1 ms = 1 sample），此处仅占位保持契约
  if (!frame || len < 3) return std::nullopt;

  std::vector<DetailItem> details;
  std::vector<std::string> summaryParts;
  std::vector<PacketWarning> warns;
  DetailEmitter em{&details, &summaryParts};

  uint16_t hdr = static_cast<uint16_t>((frame[0] << 8) | frame[1]);
  HeaderInfo h = ufcsHeaderInfo(hdr);

  if (!h.addrValid)
    warns.push_back({"设备地址 " + binStr(h.addr, 3) + "b 是保留值（规范只定义 001b/010b/011b）", "ADDR"});
  if (!h.mtypeValid)
    warns.push_back({"消息类型 " + binStr(h.mtype, 3) + "b 是保留值（规范只定义 000b/001b/010b）", "MTYPE"});
  if (!h.verKnown)
    warns.push_back({"协议版本编号 " + binStr(h.verCode, 6)
                     + "b 不在规范已知取值内（000001b=1.0.0 / 010001b=1.0.1 / 001001b=1.2.0）", "VER"});

  bool withCrc = opts.hasCrc;
  uint8_t crc = static_cast<uint8_t>(opts.crc);
  uint8_t calc = (opts.crcCalc != 0) ? static_cast<uint8_t>(opts.crcCalc) : ufcsCrc8(frame, len);
  CrcState crcOkState;
  if (!withCrc) crcOkState = CrcState::Unrecorded;
  else crcOkState = (crc == calc) ? CrcState::Ok : CrcState::Bad;
  if (crcOkState == CrcState::Bad) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "CRC-8 校验失败：读到的 0x%02X ≠ 计算的 0x%02X", crc, calc);
    warns.push_back({buf, "CRC"});
  }

  /* ── 消息头位域 ── */
  em.object("消息头（16 bit）");
  em.detail("设备地址 [" + ufcsRange(15, 13) + "]",
            binStr(h.addr, 3) + "b · " + at(kDevAddr, h.addr, "保留值（接收端应忽略本条）"));
  em.detail("消息编号 [" + ufcsRange(12, 9) + "]",
            std::to_string(h.msgNo) + "（接收方不判断其是否变化，回复须跟随）");
  em.detail("协议版本编号 [" + ufcsRange(8, 3) + "]",
            binStr(h.verCode, 6) + "b · UFCS " + h.verText + (h.verKnown ? "" : "（保留值）"));
  em.detail("消息类型 [" + ufcsRange(2, 0) + "]",
            binStr(h.mtype, 3) + "b · " + at(kMsgType, h.mtype, "保留值"));

  /* ── 方向 ── */
  int probCmd = (h.mtype == 0 || h.mtype == 1) ? frame[2] : -1;
  std::string fixed = (h.mtype <= 1) ? ufcsFixedSender(h.mtype, probCmd) : std::string();
  DirResult dir = ufcsResolveDirection(h.addr, opts.line, fixed);
  std::string lineLabel = !dir.line.empty() ? dir.line
                        : (kLine.count(dir.sender) ? kLine.at(dir.sender) : std::string("D±"));
  if (dir.mismatch) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
      "方向与规范不符：该命令按规范表定义为 %s → %s，但消息头里的接收方是「%s」",
      ufcsRoleText(dir.sender).c_str(), ufcsRoleText(ufcsPeerOf(dir.sender)).c_str(),
      at(kDevAddr, h.addr, "保留值").c_str());
    warns.push_back({buf, "DIR"});
  }

  /* ── 主体 ── */
  int cmd = -1;
  int dataLen = 0;
  std::string msgType = "未知";
  std::string msgKind = "control";

  if (h.mtype == 0) {
    msgKind = "control";
    cmd = frame[2];
    auto info = kCtrlCmd.find(cmd);
    msgType = (info != kCtrlCmd.end()) ? info->second.name
                                       : ("CTRL?0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
    em.object("控制命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
    if (info != kCtrlCmd.end()) {
      em.detail("控制命令", info->second.name + "（" + info->second.req + "）");
      em.detail("发送者 → 接收者（规范表 14）", info->second.dir);
      em.detail("规范要求", info->second.req);
      em.note(info->second.sum);
      if (len != 3) warns.push_back({"控制消息主体应为 1 字节命令，实际 " + std::to_string(len - 2) + " 字节", "LEN"});
    } else {
      em.detail("原始主体", "0x" + ufcsHex(frame + 2, len - 2));
      warns.push_back({"控制命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2) + " 未在规范表 14 中定义", "CMD"});
    }
  } else if (h.mtype == 1) {
    msgKind = "data";
    if (len < 4) return std::nullopt;
    cmd = frame[2];
    dataLen = frame[3];
    size_t actualDataLen = (len >= 4 + static_cast<size_t>(dataLen))
      ? static_cast<size_t>(dataLen) : (len > 4 ? len - 4 : 0);
    auto info = kDataCmd.find(cmd);
    msgType = (info != kDataCmd.end()) ? info->second.name
                                       : ("DATA?0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
    em.object("数据命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2));
    em.detail("命令", (info != kDataCmd.end())
      ? (info->second.name + "（" + info->second.req + "）")
      : "规范表 15 未定义该命令编号");
    em.detail("数据长度 [" + ufcsRange(7, 0) + "]", std::to_string(dataLen) + " 字节");
    if (info != kDataCmd.end()) {
      em.detail("发送者 → 接收者（规范表 15）", info->second.dir);
      const DataCmd& d = info->second;
      if (!d.lenVar) {
        if (dataLen != d.lenUnit)
          warns.push_back({d.name + " 的数据长度应为 " + std::to_string(d.lenUnit)
                           + " 字节，实际 " + std::to_string(dataLen) + " 字节", "LEN"});
      } else {
        size_t cnt = static_cast<size_t>(dataLen) / static_cast<size_t>(d.lenUnit);
        if (static_cast<size_t>(dataLen) % static_cast<size_t>(d.lenUnit) != 0
            || cnt < static_cast<size_t>(d.lenMin) || cnt > static_cast<size_t>(d.lenMax))
          warns.push_back({d.name + " 的数据长度应为 " + std::to_string(d.lenUnit) + "×n 字节（1≤n≤"
                           + std::to_string(d.lenMax) + "），实际 " + std::to_string(dataLen) + " 字节", "LEN"});
      }
    } else {
      warns.push_back({"数据命令 0x" + pdscope::hexU(static_cast<uint64_t>(cmd), 2) + " 未在规范表 15 中定义", "CMD"});
    }
    if (actualDataLen < static_cast<size_t>(dataLen))
      warns.push_back({"数据域被截断：声明 " + std::to_string(dataLen) + " 字节，实际只有 "
                       + std::to_string(actualDataLen) + " 字节", "TRUNC"});
    PayloadResult pr = ufcsDataPayload(em, cmd, frame + 4, actualDataLen, h.verText);
    if (pr.summary.empty())
      warns.push_back({msgType + " 未能解出字段", "PARSE"});
  } else {
    msgKind = "custom";
    if (len < 5) return std::nullopt;
    int vid = (frame[2] << 8) | frame[3];
    dataLen = frame[4];
    size_t actualDataLen = (len >= 5 + static_cast<size_t>(dataLen))
      ? static_cast<size_t>(dataLen) : (len > 5 ? len - 5 : 0);
    (void)vid;
    msgType = "Manufacturer_Custom";
    ufcsCustomPayload(em, frame + 2, len - 2);
    if (actualDataLen < static_cast<size_t>(dataLen))
      warns.push_back({"数据域被截断：声明 " + std::to_string(dataLen) + " 字节，实际只有 "
                       + std::to_string(actualDataLen) + " 字节", "TRUNC"});
  }

  /* ── CRC ── */
  em.object("CRC 校验");
  em.detail("算法", "CRC-8，多项式 X⁸+X⁵+X³+1（0x29），初值 0x00，覆盖消息头与消息主体");
  em.detail("计算值", "0x" + pdscope::hexU(calc, 2));
  em.detail("报文值", withCrc ? ("0x" + pdscope::hexU(crc, 2)) : "未记录（分析仪容器未存 CRC）");
  em.detail("结论", crcOkState == CrcState::Unrecorded ? "无法判定"
                   : (crcOkState == CrcState::Ok ? "通过" : "失败"));
  if (crcOkState == CrcState::Unrecorded)
    em.note("CRC 由本工具按规范补算（容器未存），不据此宣布校验通过");

  /* ── 链路与方向 ── */
  em.object("链路与方向");
  std::string how = (dir.source == "spec") ? "规范表单向定义（该命令只有唯一发送方）"
                  : (dir.source == "line") ? "分析仪容器给出的链路字节（实测与规范单向命令表 100% 吻合）"
                                           : "按接收方地址推断（容器未给出链路）";
  em.detail("方向判据", how);
  em.detail("物理链路", lineLabel + (dir.source == "line" ? "（容器给出）" : "（由方向折算）"));
  em.detail("接收方", at(kDevAddr, h.addr, "保留值"));
  em.detail("发送方", ufcsRoleText(dir.sender) + (dir.source == "addr" ? "（推断）" : ""));
  if (dir.ambiguous)
    em.detail("⚠ 方向不确定",
      "此类命令供电设备与充电设备都会发送，单看报文分不出来；容器又没给链路字节，"
      "只能按接收方取反兜底显示（规范未规定此时取谁，本工具如实标为推断）");
  em.detail("线序依据", "规范 7.2：供电设备 D+ 为发送（TX）、充电设备 D- 为发送（TX）");

  /* ── 组装 ── */
  uint64_t seq = ++packetSeq_;
  size_t totalBytes = len + (withCrc ? 1 : 0);
  double durUs = static_cast<double>(totalBytes) * kBitsPerByte / static_cast<double>(kDefaultBaud) * 1e6;
  double endMs = opts.timeMs + durUs / 1000.0;
  std::string summary = joinSummary(summaryParts);
  std::string text = "#" + std::to_string(seq) + " (" + fmtMs(opts.timeMs) + "ms): (UFCS) "
    + dir.sender + "[" + std::to_string(h.addr) + "] " + msgType
    + (summary.empty() ? "" : (" - " + summary));

  std::vector<uint8_t> frameBytes(frame, frame + len);
  if (withCrc) frameBytes.push_back(crc);

  Packet pkt;
  pkt.seq = seq;
  pkt.channel = opts.channel;
  pkt.sop = lineLabel;
  pkt.msgType = msgType;
  if (h.mtype <= 1) {
    pkt.msgTypeRaw = cmd;
    pkt.hasMsgTypeRaw = true;
  }
  pkt.msgKind = msgKind;
  pkt.category = msgKind;
  pkt.role = dir.sender.empty() ? "SNK" : dir.sender;
  pkt.roleInferred = dir.inferred;
  pkt.link = (dir.receiver == "Plug" || dir.sender == "Plug") ? "cable" : "port";
  pkt.header = static_cast<int>(hdr);
  pkt.hasHeader = true;
  pkt.msgId = h.msgNo;
  pkt.hasMsgId = true;
  pkt.rev = h.verCode & 0b11;
  pkt.hasRev = true;
  pkt.revText = h.verText;
  pkt.powerRole = (dir.sender == "SRC") ? 1 : 0;
  pkt.dataRole = -1;
  pkt.nObjects = dataLen;
  pkt.dataLen = dataLen;
  pkt.crc = withCrc ? crc : 0;
  pkt.hasCrc = withCrc;
  pkt.crcCalc = calc;
  pkt.crcOk = crcOkState;
  pkt.crcRecorded = withCrc;
  pkt.eop = false;
  pkt.summary = summary;
  pkt.details = details;
  pkt.warnings = warns;
  pkt.text = text;
  pkt.startSample = static_cast<uint64_t>(std::llround(opts.timeMs));
  pkt.endSample = static_cast<uint64_t>(std::llround(endMs));
  pkt.timeMs = opts.timeMs;
  pkt.endTimeMs = endMs;
  pkt.durationUs = durUs;
  pkt.bitrate = static_cast<uint64_t>(kDefaultBaud);
  pkt.bitrateNominal = true;
  pkt.synthetic = true;
  pkt.powerz = true;
  pkt.dataBytes = frameBytes;
  pkt.dataHex = ufcsHexSpaced(frameBytes.data(), frameBytes.size());
  pkt.hasAck = false;

  return pkt;
}

/* ────────────────────────── ACK / NCK 配对 ────────────────────────── */

void ufcsLinkAck(std::vector<Packet>& packets) {
  for (auto& p : packets) {
    if (p.msgType != "ACK" && p.msgType != "NCK") continue;
    if (p.crcOk == CrcState::Bad) continue;

    int pi = static_cast<int>(p.index);
    int lo = pi - kWarnWindow + 1;
    if (lo < 0) lo = 0;
    for (int j = pi - 1; j >= lo; j--) {
      if (j < 0 || j >= static_cast<int>(packets.size())) continue;
      Packet& q = packets[j];
      if (q.msgType == "ACK" || q.msgType == "NCK") continue;
      if (q.msgId != p.msgId) continue;

      p.hasAck = true;
      p.ackOf = q.index;
      p.ackType = q.msgType;

      // 还原 p 的接收方（ACK 的地址栏必须指向原发送方，规范 8.2.3.2）。
      int addr = ((static_cast<int>(p.header) >> 13) & 0b111);
      std::string pReceiver = ufcsRoleOf(addr);
      std::string want = ufcsOpposite(q.role);
      if (p.roleInferred && !q.roleInferred && pReceiver == q.role) {
        p.role = want;
        p.sop = (kLine.count(want) ? kLine.at(want) : std::string("D±"));
        p.roleInferred = false;
      }
      break;
    }
  }
}

}}  // namespace pdscope::ufcs
