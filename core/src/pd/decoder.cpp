// decoder.cpp — 见 decoder.h

#include "decoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>

#include "symbols.h"
#include "crc.h"
#include "format.h"
#include "tables.h"
#include "pdo.h"
#include "data.h"
#include "vdm.h"
#include "extended.h"

namespace pdscope { namespace pd {

/* ── 有序集名称 → 链路（'port' | 'cable'）── */
static const std::map<std::string, std::string>& linkMap() {
  static const std::map<std::string, std::string> m = {
    {"SOP",        "port"},
    {"SOP'",       "cable"},
    {"SOP''",      "cable"},
    {"SOP' Debug", "cable"},
    {"SOP'' Debug","cable"},
    {"Cable Reset","cable"},
    {"Hard Reset", "port"},
  };
  return m;
}
static std::string mapLinkOfSop(const std::string& sop) {
  auto it = linkMap().find(sop);
  return it != linkMap().end() ? it->second : "port";
}

/* ── 控制消息的一句话说明 ── */
static const std::map<int, std::string> CTRL_SUMMARY = {
  {3,  "接收请求"},
  {4,  "拒绝请求"},
  {6,  "电源就绪"},
  {9,  "请求交换数据角色 (DR_Swap)"},
  {10, "请求交换电源角色 (PR_Swap)"},
  {11, "请求交换 VCONN 提供方 (VCONN_Swap)"},
  {12, "暂不能响应，稍后重试"},
  {13, "软复位：双方回到默认状态但保持供电"},
  {14, "数据复位"},
  {15, "数据复位完成"},
  {16, "不支持该请求"},
  {19, "快速角色交换请求"},
};

/* ── CODE_TO_INDEX：DEC4B5B 的逆（符号值 → 5bit 采样下标）── */
static void pushSymbol(std::vector<uint8_t>& bits, uint8_t value) {
  static const int8_t* tbl = []() {
    static int8_t t[0x17];
    for (int i = 0; i < 0x17; i++) t[i] = -1;
    for (int c = 0; c < 32; c++) {
      uint8_t v = DEC4B5B[c];
      if (v <= 0x16 && t[v] < 0) t[v] = static_cast<int8_t>(c);
    }
    return t;
  }();
  int c = (value <= 0x16) ? tbl[value] : -1;
  if (c < 0) c = 0;
  for (int k = 0; k < 5; k++) bits.push_back(static_cast<uint8_t>((c >> k) & 1));
}
static void pushByte(std::vector<uint8_t>& bits, uint8_t b) {
  pushSymbol(bits, static_cast<uint8_t>(b & 0x0F));
  pushSymbol(bits, static_cast<uint8_t>((b >> 4) & 0x0F));
}

/* ────────────────────────── 构造 / 重置 ────────────────────────── */

PdDecoder::PdDecoder(double sampleRate) : sampleRate_(sampleRate) {
  reset();
}

void PdDecoder::reset() {
  packetSeq_ = 0;
  st_ = PdState{};
  lastSopPowerRole_ = "SRC";
}

/* ────────────────────────── 位 / 符号读取 ────────────────────────── */

uint8_t PdDecoder::_sym(size_t i) {
  if (i + 4 >= bits_.size()) return 0x10;
  const uint32_t byte = static_cast<uint32_t>(
      (bits_[i] & 1) | ((bits_[i + 1] & 1) << 1) | ((bits_[i + 2] & 1) << 2) |
      ((bits_[i + 3] & 1) << 3) | ((bits_[i + 4] & 1) << 4));
  return DEC4B5B[byte];
}

uint16_t PdDecoder::_byte() {
  if (idx_ + 10 > bits_.size()) { warn("数据被截断", "TRUNC"); return 0x0BAD; }
  const uint8_t k0 = _sym(idx_);
  const uint8_t k1 = _sym(idx_ + 5);
  idx_ += 10;
  return static_cast<uint16_t>((k0 & 0x0F) | ((k1 & 0x0F) << 4));
}

int PdDecoder::_nibble() {
  if (idx_ + 5 > bits_.size()) { warn("数据被截断", "TRUNC"); return 0x0BAD; }
  const uint8_t s = _sym(idx_);
  if (s > 0x0F) { warn("非法 4B5B 符号 0x" + pdHex(s, 2), "SYM"); return 0x0BAD; }
  idx_ += 5;
  return s;
}

uint32_t PdDecoder::_short() {
  if (idx_ + 20 > bits_.size()) { warn("报文被截断", "TRUNC"); return 0x0BAD; }
  uint32_t val = 0;
  for (int i = 0; i < 4; i++) {
    const uint8_t s = _sym(idx_ + i * 5);
    if (s > 0x0F) { warn("非法 4B5B 符号 0x" + pdHex(s, 2), "SYM"); return 0x0BAD; }
    val |= (static_cast<uint32_t>(s) << (4 * i));   // 低 4bit 先行 → 小端
  }
  idx_ += 20;
  return val;
}

uint32_t PdDecoder::_word() {
  const uint32_t lo = _short();
  const uint32_t hi = _short();
  if (lo == 0x0BAD || hi == 0x0BAD) { warn("读取 32bit 对象失败", "WORD"); return 0x0BAD0BAD; }
  return (hi << 16) | lo;
}

void PdDecoder::warn(const std::string& longm, const std::string& shortm) {
  if (warnings_.size() < 16) warnings_.push_back({longm, shortm});
}

/* ────────────────────────── SOP 扫描 ────────────────────────── */

long long PdDecoder::_scanSop() {
  for (size_t i = 0; i + 19 < bits_.size(); i++) {
    uint8_t k[4] = { _sym(i), _sym(i + 5), _sym(i + 10), _sym(i + 15) };
    const OrderedSet* set = findOrderedSetByKey(sopKey(k));
    if (!set) {
      std::vector<uint8_t> ks(k, k + 4);
      const OrderedSetMatch best = matchOrderedSet(ks);
      if (best.set && best.matched >= 3) set = best.set;
    }
    if (!set) continue;

    sopBitOffset_ = static_cast<long long>(i);
    if (set->name == "Hard Reset" || set->name == "Cable Reset") {
      specialPacket_ = set->name;
      packetSop_.clear();
      return -1;
    }
    packetSop_ = set->name;
    return static_cast<long long>(i) + 20;
  }
  warn("未找到报文起始有序集", "NOSOP");
  return -1;
}

/* ────────────────────────── 详情发射器 ────────────────────────── */

void PdDecoder::_makeEmitter() {
  em_.details = &details_;
  em_.summaryParts = &summaryParts_;
}

/* ────────────────────────── 主入口 ────────────────────────── */

std::optional<Packet> PdDecoder::decode(const BmcRawPacket& raw, int channel) {
  bits_ = raw.bits;
  idx_ = 0;
  warnings_.clear();
  details_.clear();
  summaryParts_.clear();
  dataWords_.clear();
  text_.clear();
  specialPacket_.clear();
  packetSop_.clear();
  sopBitOffset_ = -1;
  hasDataBytesOverride_ = false;
  dataBytesOverride_.clear();
  crcExtra_.clear();
  channel_ = channel;
  _makeEmitter();

  const long long hdrIdx = _scanSop();

  if (hdrIdx < 0 && !specialPacket_.empty()) return _specialPacket(raw, channel);
  if (hdrIdx < 0) return std::nullopt;

  packetSeq_++;
  const double tms = static_cast<double>(raw.startSample) / sampleRate_ * 1000.0;
  text_ = "#" + std::to_string(packetSeq_) + " (" + pdscope::jsToFixed(tms, 4) + "ms): ";
  idx_ = static_cast<size_t>(hdrIdx);
  head_ = static_cast<uint16_t>(_short());
  if (head_ == 0x0BAD) return std::nullopt;

  const int t = headType();
  const bool isExt = (headExt() == 1);
  const int nObjects = headCount();

  std::string shortm;
  if (isExt) {
    auto it = EXT_TYPES.find(t);
    shortm = (it != EXT_TYPES.end()) ? it->second : ("EXT?" + std::to_string(t));
  } else if (nObjects == 0) {
    auto it = CTRL_TYPES.find(t);
    shortm = (it != CTRL_TYPES.end()) ? it->second : ("CTRL?" + std::to_string(t));
  } else {
    auto it = DATA_TYPES.find(t);
    shortm = (it != DATA_TYPES.end()) ? it->second : ("DATA?" + std::to_string(t));
  }

  const std::string link = mapLinkOfSop(packetSop_);
  const std::string role = _resolveRole(link);
  resolvedRole_ = role;

  const std::string longm = "(r" + std::to_string(headRev()) + ") " + role + "[" +
                            std::to_string(headId()) + "]: " + shortm;
  text_ += longm;

  _versionHints(t, isExt, nObjects, shortm);

  int extHeader = -1; bool hasExtHeader = false;
  int nObj = nObjects;
  uint32_t crc = 0, calc = 0;
  bool crcOk = false;

  if (isExt) {
    const ExtResult r = _readExtended(t, link);
    extHeader = r.extHead; hasExtHeader = true;
    nObj = r.objCount;
    crc = r.crc; calc = r.calc; crcOk = r.crcOk;
  } else {
    for (int i = 0; i < nObjects; i++) dataWords_.push_back(_word());
    if (t == 15) {
      _vdm(link);
    } else {
      for (int i = 0; i < nObjects; i++) _payload(i, t, link);
    }
    if (nObjects == 0) {
      auto it = CTRL_SUMMARY.find(t);
      if (it != CTRL_SUMMARY.end()) summaryParts_.push_back(it->second);
    }

    crc = _word();
    calc = _computeCrc();
    crcOk = (crc == calc);
  }
  if (!crcOk) warn("CRC 校验失败：读到的 0x" + pdHex(crc) + " ≠ 计算的 0x" + pdHex(calc), "CRC");

  bool eop = false;
  if (bits_.size() >= idx_ + 5 && _sym(idx_) == SYM_EOP) { eop = true; idx_ += 5; }
  else warn("缺少 EOP（报文结束符）", "EOP");

  FinishOpts o;
  o.sop = packetSop_;
  o.msgType = shortm;
  o.msgTypeRaw = t; o.hasMsgTypeRaw = true;
  o.msgKind = isExt ? "ext" : (nObjects == 0 ? "control" : "data");
  o.role = role;
  o.header = static_cast<int>(head_); o.hasHeader = true;
  o.extHeader = extHeader; o.hasExtHeader = hasExtHeader;
  o.msgId = headId(); o.hasMsgId = true;
  o.rev = headRev(); o.hasRev = true;
  o.revText = headRevText();
  o.powerRole = headPowerRole();
  o.dataRole = headDataRole();
  o.link = link;
  o.nObjects = nObj;
  o.crc = crc; o.hasCrc = true;
  o.crcCalc = calc;
  o.crcState = crcOk ? CrcState::Ok : CrcState::Bad;
  o.crcRecorded = true;
  o.eop = eop;
  {
    auto cit = MSG_CATEGORY.find(shortm);
    o.category = (cit != MSG_CATEGORY.end()) ? cit->second : (isExt ? "data" : "control");
  }
  return _finishTail(o, raw, channel);
}

std::optional<Packet> PdDecoder::decodeWire(const uint8_t* wire, size_t len,
                                            const std::string& sop, double timeMs, int channel,
                                            bool crcRecorded, int sopByte, bool powerz) {
  const OrderedSet* set = findOrderedSetByName(sop);
  if (!set) set = findOrderedSetByName("SOP");

  std::vector<uint8_t> bits;
  for (int i = 0; i < 4; i++) pushSymbol(bits, set->sequence[i]);
  for (size_t i = 0; i < len; i++) pushByte(bits, wire[i]);

  const uint32_t calc = crc32(wire, len);
  const uint8_t cb[4] = {
    static_cast<uint8_t>(calc & 0xFF),
    static_cast<uint8_t>((calc >> 8) & 0xFF),
    static_cast<uint8_t>((calc >> 16) & 0xFF),
    static_cast<uint8_t>((calc >> 24) & 0xFF),
  };
  for (int i = 0; i < 4; i++) pushByte(bits, cb[i]);
  pushSymbol(bits, SYM_EOP);

  const double durMs = static_cast<double>(bits.size()) / 600.0;

  BmcRawPacket raw;
  raw.bits = std::move(bits);
  // 采样点坐标是整数（Packet::startSample 是 uint64_t），毫秒时间戳是浮点。
  // 取整口径必须与 UFCS 那条路径一致（ufcs/decoder.cpp 用 llround），
  // 否则同一份数据在两条协议出口上会差一格。
  raw.startSample = static_cast<uint64_t>(std::llround(timeMs));
  raw.endSample = static_cast<uint64_t>(std::llround(timeMs + durMs));
  raw.bitrate = 600000;          // 标称 BMC 时钟（分析仪不测这个，取协议定值）
  raw.synthetic = true;
  raw.wireBytes.assign(wire, wire + len);

  auto pkt = decode(raw, channel);
  if (!pkt) return std::nullopt;

  pkt->synthetic = true;                 // 报文不是从采样波形解出来的
  pkt->crcCalc = calc;
  pkt->crcRecorded = crcRecorded;
  // `decode()` 是从取整后的 startSample 反推 timeMs 的，会把小数部分抹掉。
  // 这里把**原始毫秒数**放回去（durationUs 同理），保住亚毫秒分辨率 ——
  // 实机数据 ts 是整数毫秒，两者本来就相等，只有合成/测试输入才看得出差别。
  pkt->timeMs = timeMs;
  pkt->endTimeMs = timeMs + durMs;
  pkt->durationUs = durMs * 1000.0;
  // 分析仪只给逻辑字节，测不到线上的码率与时长 —— 600 kbps 是 BMC 标称时钟
  pkt->bitrateNominal = true;
  pkt->sopByte = sopByte;
  pkt->powerz = powerz;
  // CRC 三态：分析仪没记 CRC 时（POWER-Z），绝不能因为重算对上就报通过
  if (!crcRecorded) {
    pkt->hasCrc = false;
    pkt->crc = 0;
    pkt->crcOk = CrcState::Unrecorded;
  }
  return pkt;
}

/* ────────────────────────── 方向判定 ────────────────────────── */

std::string PdDecoder::_resolveRole(const std::string& link) {
  const int ppr = headPowerRole();
  if (packetSop_ == "SOP") {
    const std::string role = ppr ? "SRC" : "SNK";
    lastSopPowerRole_ = role;
    return role;
  }
  if (link == "cable") return ppr ? "Plug" : lastSopPowerRole_;
  return lastSopPowerRole_;
}

/* ────────────────────────── 版本合法性提示 ────────────────────────── */

void PdDecoder::_versionHints(int t, bool isExt, int nObjects, const std::string& shortm) {
  const std::string revText = headRevText();
  if (revText.size() >= 8 && revText.compare(0, 8, "Reserved") == 0) {
    warn("Header 的 Specification Revision 是保留值 11b（规范规定不得使用），版本判定不可靠", "REV");
    return;
  }
  const bool legacy1 = (headRevCode() == 0);
  const double revNum = legacy1 ? 2.0 : revTextNum(revText);
  if (legacy1 && nObjects > 0) {
    summaryParts_.push_back("（Revision 域为 00b：已废弃，按 Revision 2.0 解读）");
  }
  const std::map<int, double>* mp =
      isExt ? &EXT_MIN_REV : (nObjects == 0 ? &CTRL_MIN_REV : &DATA_MIN_REV);
  double minv = 0; bool hasMin = false;
  auto it = mp->find(t);
  if (it != mp->end()) { minv = it->second; hasMin = true; }
  if (hasMin && revNum + 1e-9 < minv) {
    if (revText == "3.x" && minv <= 3.2) {
      summaryParts_.push_back("（「" + shortm + "」自 PD " + pdscope::jsToFixed(minv, 1) +
        " 起定义；报文头只能标到 3.x，无法区分 3.0/3.1/3.2）");
    } else {
      warn("消息类型「" + shortm + "」自 PD " + pdscope::jsToFixed(minv, 1) +
        " 起才定义，本包声明为 r" + std::to_string(headRev()), "REV");
    }
  }
  if (!isExt && nObjects == 0) {
    auto dit = CTRL_DEPRECATED.find(shortm);
    if (dit != CTRL_DEPRECATED.end()) warn(shortm + "：" + dit->second, "DEPR");
  }
}

uint32_t PdDecoder::_computeCrc() {
  std::vector<uint8_t> headBytes = {
    static_cast<uint8_t>(head_ & 0xFF),
    static_cast<uint8_t>((head_ >> 8) & 0xFF),
  };
  if (!crcExtra_.empty()) {
    std::vector<uint8_t> all = headBytes;
    all.insert(all.end(), crcExtra_.begin(), crcExtra_.end());
    return crc32(all);
  }
  std::vector<uint8_t> bytes = headBytes;
  for (uint32_t w : dataWords_) {
    bytes.push_back(static_cast<uint8_t>(w & 0xFF));
    bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
  }
  return crc32(bytes);
}

/* ────────────────────────── 载荷解析 ────────────────────────── */

void PdDecoder::_payload(int idx, int t, const std::string& link) {
  DetailEmitter& em = em_;
  const uint32_t data = dataWords_[idx];
  const std::string revText = headRevText();
  std::string summary;

  if (t == 2) {
    rdoParse(st_, em, data, false);
  } else if (t == 1 || t == 4) {
    const std::string role = (t == 1) ? "source" : "sink";
    const std::string r = pdoParse(st_, em, data, role, idx + 1, false, revText);
    em.note(r);
  } else if (t == 9) {
    if (idx == 0) {
      rdoParse(st_, em, data, true);
    } else {
      const std::string r = pdoParse(st_, em, data, "source", idx + 1, true, revText);
      em.note("EPR 能力副本 #" + std::to_string(idx) + " · " + r);
    }
  } else if (t == 15) {
    _vdm(link);                       // VDM 需要整条报文，由 decode() 统一调用
  } else if (t == 3) {
    summary = bistParse(em, data, idx, revText);
  } else if (t == 10) {
    summary = eprModeParse(em, data, idx);
  } else if (t == 5) {
    em.object("Battery_Status 数据对象（BSDO）");
    summary = batteryStatusParse(em, data);
  } else if (t == 6) {
    summary = alertParse(em, data, idx);
  } else if (t == 8) {
    em.object("Enter_USB 数据对象（EUDO）");
    summary = enterUsbParse(em, data);
  } else if (t == 11) {
    summary = sourceInfoParse(em, data, idx);
  } else if (t == 12) {
    em.object("Revision 数据对象（RMDO）");
    summary = revisionParse(em, data);
  } else if (t == 7) {
    summary = countryCodeParse(em, data);
  } else {
    em.object("数据对象 #" + std::to_string(idx + 1));
    em.detail("原始值", "0x" + pdHex(data));
    summary = "0x" + pdHex(data);
    em.note(summary);
  }
  text_ += " - " + summary;
}

void PdDecoder::_vdm(const std::string& link) {
  if (dataWords_.empty()) { em_.note("VDM 未携带数据对象"); return; }
  PdCtx ctx;
  ctx.rev = headRev();
  ctx.revText = headRevText();
  ctx.sop = packetSop_;
  ctx.link = link;
  ctx.role = resolvedRole_;
  vdmParse(st_, em_, dataWords_, ctx);
}

/* ────────────────────────── 扩展消息 ────────────────────────── */

std::vector<PdDecoder::ExtCandidate> PdDecoder::_extCandidates(bool chunked, int chunkNum,
                                                               bool reqChunk, int dataSize) {
  std::vector<ExtCandidate> out;
  if (reqChunk) { out.push_back({0, 0, 0}); return out; }
  if (chunked) {
    const int cap = std::max(headCount() * 4 - 2, 0);          // 分块消息的计数位有效，用它定本块长度
    const int off = chunkNum * EXT_MSG_LIMITS.chunkLen;
    const int payload = std::min(std::max(dataSize - off, 0), cap);
    out.push_back({payload, std::max(cap - payload, 0), off});
    return out;
  }
  const int size = std::min(dataSize, EXT_MSG_LIMITS.maxLen);
  out.push_back({size, 0, 0});                                 // 规范读法：非分块不补齐
  const int padded = std::max(((2 + size + 3) / 4) * 4 - 2, 0); // 兼容补齐 00h 的发送方
  if (padded > size) out.push_back({size, padded - size, 0});
  if (size > 0) out.push_back({std::max(size - 3, 0), 0, 0});   // 兼容少补的发送方
  return out;
}

PdDecoder::ExtTry PdDecoder::_readExtTry(const ExtCandidate& cand,
                                         const std::vector<uint8_t>& hdrBytes,
                                         const std::vector<uint8_t>& extBytes) {
  const size_t saveIdx = idx_;
  const size_t saveWarn = warnings_.size();
  const int n = cand.payload + cand.pad;
  std::vector<uint8_t> bytes(n);
  for (int i = 0; i < n; i++) bytes[i] = static_cast<uint8_t>(_byte());
  const uint32_t crc = _word();
  std::vector<uint8_t> all;
  all.insert(all.end(), hdrBytes.begin(), hdrBytes.end());
  all.insert(all.end(), extBytes.begin(), extBytes.end());
  all.insert(all.end(), bytes.begin(), bytes.end());
  const uint32_t calc = crc32(all);
  const bool ok = (crc == calc);
  if (!ok) { idx_ = saveIdx; warnings_.resize(saveWarn); }

  ExtTry r;
  r.payload = cand.payload; r.pad = cand.pad; r.off = cand.off;
  r.bytes = bytes; r.crc = crc; r.calc = calc; r.ok = ok;
  return r;
}

PdDecoder::ExtResult PdDecoder::_readExtended(int t, const std::string& link) {
  const uint32_t extHead = _short();
  const bool chunked = !!((extHead >> 15) & 1);
  const int chunkNum = static_cast<int>((extHead >> 11) & 0x0F);
  const bool reqChunk = !!((extHead >> 10) & 1);
  const int dataSize = static_cast<int>(extHead & 0x1FF);

  const std::vector<uint8_t> hdrBytes = {
    static_cast<uint8_t>(head_ & 0xFF),
    static_cast<uint8_t>((head_ >> 8) & 0xFF),
  };
  const std::vector<uint8_t> extBytes = {
    static_cast<uint8_t>(extHead & 0xFF),
    static_cast<uint8_t>((extHead >> 8) & 0xFF),
  };
  const std::vector<ExtCandidate> cands = _extCandidates(chunked, chunkNum, reqChunk, dataSize);

  ExtTry pick;
  bool anyOk = false;
  for (const auto& c : cands) {
    const ExtTry r = _readExtTry(c, hdrBytes, extBytes);
    if (r.ok) { pick = r; anyOk = true; break; }
    if (!anyOk) pick = r;                    // 都不匹配时用第一个候选兜底
  }
  if (!anyOk) pick = _readExtTry(cands[0], hdrBytes, extBytes);   // 让 idx 停在正确位置

  const int payload = pick.payload;
  const int pad = pick.pad;
  std::vector<uint8_t> payloadBytes(pick.bytes.begin(), pick.bytes.begin() + payload);

  // 线上真实字节 = 扩展头(2) + 数据块(+补齐)。非分块扩展消息不补齐时，
  // 总长 % 4 != 0（如 Data Size=25 → 27 字节），末个「数据对象」只占 3 字节。
  std::vector<uint8_t> body = extBytes;
  body.insert(body.end(), pick.bytes.begin(), pick.bytes.end());

  // 「数据对象 (hex)」一栏：按 4 字节回填，让界面与其它消息一致地显示原始字节。
  // 末组不足 4 字节时补 0 只为凑齐对象视图，真实字节另由 dataBytesOverride 给出。
  for (size_t i = 0; i < body.size(); i += 4) {
    uint32_t w = body[i];
    if (i + 1 < body.size()) w |= (static_cast<uint32_t>(body[i + 1]) << 8);
    if (i + 2 < body.size()) w |= (static_cast<uint32_t>(body[i + 2]) << 16);
    if (i + 3 < body.size()) w |= (static_cast<uint32_t>(body[i + 3]) << 24);
    dataWords_.push_back(w);
  }
  dataBytesOverride_ = body;
  hasDataBytesOverride_ = true;

  PdCtx ctx;
  ctx.rev = headRev();
  ctx.revText = headRevText();
  ctx.sop = packetSop_;
  ctx.link = link;
  ctx.role = resolvedRole_;

  ExtBlock bk;
  bk.bytes = payloadBytes;
  bk.off = pick.off;
  bk.dataSize = dataSize;
  bk.chunked = chunked;
  bk.chunkNum = chunkNum;
  bk.reqChunk = reqChunk;
  bk.carry = st_.extCarry;

  const std::string s = extendedParse(st_, em_, t, bk, ctx);
  st_.extCarry = bk.tail;
  if (!s.empty()) em_.note(s);
  if (pick.off > 0 && !bk.tail)
    em_.detail("说明", "本包是分块数据的第 " + std::to_string(chunkNum + 1) +
      " 块（从整块偏移 " + std::to_string(pick.off) + " 字节处开始）");

  ExtResult res;
  res.extHead = static_cast<int>(extHead);
  res.objCount = (2 + payload + pad + 3) / 4;   // = Math.ceil((2+payload+pad)/4)
  res.crc = pick.crc;
  res.calc = pick.calc;
  res.crcOk = pick.ok;
  return res;
}

/* ────────────────────────── 特殊短报文 ────────────────────────── */

Packet PdDecoder::_specialPacket(const BmcRawPacket& raw, int channel) {
  packetSeq_++;
  const double tms = static_cast<double>(raw.startSample) / sampleRate_ * 1000.0;
  text_ = "#" + std::to_string(packetSeq_) + " (" + pdscope::jsToFixed(tms, 4) + "ms): " + specialPacket_;

  FinishOpts o;
  o.sop = specialPacket_;
  o.msgType = specialPacket_;
  o.msgTypeRaw = -1; o.hasMsgTypeRaw = false;
  o.msgKind = "special";
  o.role = lastSopPowerRole_;
  o.header = -1; o.hasHeader = false;
  o.extHeader = -1; o.hasExtHeader = false;
  o.msgId = -1; o.hasMsgId = false;
  o.rev = -1; o.hasRev = false;
  o.revText.clear();
  o.powerRole = -1; o.dataRole = -1;
  o.link = mapLinkOfSop(specialPacket_);
  o.nObjects = 0;
  o.crc = 0; o.hasCrc = false;
  o.crcCalc = 0;
  o.crcState = CrcState::Unrecorded;
  o.crcRecorded = false;
  o.eop = false;
  o.category = "control";
  return _finishTail(o, raw, channel);
}

/* ────────────────────────── 组装 packet ────────────────────────── */

Packet PdDecoder::_finishTail(const FinishOpts& o, const BmcRawPacket& raw, int channel) {
  // summary 装配（与 JS summaryParts.filter(Boolean).join(' ; ') 一致）
  std::string joined;
  for (const auto& p : summaryParts_) {
    if (p.empty()) continue;
    if (!joined.empty()) joined += " ; ";
    joined += p;
  }
  if (!joined.empty()) text_ += " - " + joined;

  // 数据字节
  std::vector<uint8_t> bytes;
  if (hasDataBytesOverride_) {
    bytes = dataBytesOverride_;       // 扩展消息：线上真实字节（可能不是 4 的整数倍）
  } else {
    for (uint32_t w : dataWords_) {
      bytes.push_back(static_cast<uint8_t>(w & 0xFF));
      bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
      bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
      bytes.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
    }
  }

  std::string dataHex;
  for (size_t i = 0; i < bytes.size(); i++) {
    if (i) dataHex += ' ';
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
    dataHex += buf;
  }

  Packet pkt;
  pkt.seq = packetSeq_;
  pkt.channel = channel;
  pkt.sop = o.sop;
  pkt.msgType = o.msgType;
  pkt.msgTypeRaw = o.msgTypeRaw; pkt.hasMsgTypeRaw = o.hasMsgTypeRaw;
  pkt.msgKind = o.msgKind;
  pkt.role = o.role;
  pkt.header = o.header; pkt.hasHeader = o.hasHeader;
  pkt.extHeader = o.extHeader; pkt.hasExtHeader = o.hasExtHeader;
  pkt.msgId = o.msgId; pkt.hasMsgId = o.hasMsgId;
  pkt.rev = o.rev; pkt.hasRev = o.hasRev;
  pkt.revText = o.revText;
  pkt.powerRole = o.powerRole;
  pkt.dataRole = o.dataRole;
  pkt.link = o.link;
  pkt.nObjects = o.nObjects;
  pkt.crc = o.crc; pkt.hasCrc = o.hasCrc;
  pkt.crcCalc = o.crcCalc;
  pkt.crcOk = o.crcState;
  pkt.crcRecorded = o.crcRecorded;
  pkt.eop = o.eop;
  pkt.category = o.category;
  pkt.summary = joined;
  pkt.startSample = raw.startSample;
  pkt.endSample = raw.endSample;
  pkt.timeMs = static_cast<double>(raw.startSample) / sampleRate_ * 1000.0;
  pkt.endTimeMs = static_cast<double>(raw.endSample) / sampleRate_ * 1000.0;
  pkt.durationUs = (static_cast<double>(raw.endSample) - static_cast<double>(raw.startSample)) /
                   sampleRate_ * 1e6;
  pkt.bitrate = raw.bitrate;
  pkt.dataWords = dataWords_;
  pkt.dataBytes = bytes;
  pkt.dataHex = dataHex;
  pkt.details = details_;
  pkt.warnings = warnings_;
  pkt.text = text_;
  pkt.synthetic = raw.synthetic;
  return pkt;
}

}}  // namespace pdscope::pd
