// frame.cpp — UFCS 切帧与分析仪容器行结构识别（实现见 frame.h）

#include "frame.h"
#include "crc.h"
#include "tables.h"
#include "util.h"

#include <algorithm>

namespace pdscope { namespace ufcs {

/* ────────────────────────── 消息头 ────────────────────────── */

HeaderInfo ufcsHeaderInfo(uint16_t hdr) {
  HeaderInfo h;
  h.hdr = hdr & 0xFFFF;
  h.addr = (hdr >> 13) & 0b111;
  h.msgNo = (hdr >> 9) & 0b1111;
  h.verCode = (hdr >> 3) & 0b111111;
  h.mtype = hdr & 0b111;
  auto vit = kVersion.find(h.verCode);
  if (vit != kVersion.end()) {
    h.verText = vit->second;
    h.verKnown = true;
  } else {
    h.verText = ufcsVersionText(h.verCode);
    h.verKnown = false;
  }
  // mtypeText 在此处无独立用途；合法判定用 mtypeValid
  (void)0;
  h.addrValid = h.addr >= 1 && h.addr <= 3;
  h.mtypeValid = h.mtype <= 2;
  return h;
}

int ufcsFrameBodySize(const uint8_t* bytes, size_t n, size_t off) {
  if (off + 2 > n) return -1;
  int mtype = bytes[off + 1] & 0b111;
  if (mtype == 0) return 3;                                   // 头 2 + 命令 1
  if (mtype == 1) {
    if (off + 4 > n) return -1;
    int len = bytes[off + 3];
    if (len < 1 || len > 59) return -1;                       // 规范：数据 1…59 字节
    return 4 + len;                                           // 头 2 + 命令 1 + 长度 1 + 数据
  }
  if (mtype == 2) {
    if (off + 5 > n) return -1;
    int len = bytes[off + 4];
    if (len < 1 || len > 58) return -1;                       // 规范：数据 1…58 字节
    return 5 + len;                                           // 头 2 + 厂家识别码 2 + 长度 1 + 数据
  }
  return -1;                                                  // bit2…0 = 011b 及以上保留
}

int ufcsFrameScore(const uint8_t* bytes, size_t n, size_t off, bool withCrc) {
  int bodySize = ufcsFrameBodySize(bytes, n, off);
  if (bodySize < 0) return -1;
  size_t total = static_cast<size_t>(bodySize) + (withCrc ? 1 : 0);
  if (off + total > n) return -1;

  uint16_t hdr = static_cast<uint16_t>((bytes[off] << 8) | bytes[off + 1]);
  HeaderInfo h = ufcsHeaderInfo(hdr);
  if (!h.addrValid || !h.mtypeValid) return -1;

  int score = 1;
  if (h.verKnown) score += 1;                                 // 版本编号命中已知值
  if (h.mtype == 0) {
    if (kCtrlCmd.count(bytes[off + 2])) score += 2;
  } else if (h.mtype == 1) {
    if (kDataCmd.count(bytes[off + 2])) score += 2;
  } else {
    score += 1;                                               // 自定义消息的厂家域不校验
  }
  return score;
}

/* ────────────────────────── 内部切帧 ────────────────────────── */

struct SplitFrame {
  size_t off = 0;
  size_t bodyEnd = 0;
  size_t total = 0;
  uint16_t hdr = 0;
  uint32_t calc = 0;
  uint32_t crc = 0;
  bool hasCrc = false;
  bool crcOk = false;
  int structScore = 0;
};

/** 从 off 起连续切帧，必须正好消费到字节末尾，否则视为这套读法不成立。 */
static std::optional<std::vector<SplitFrame>> splitFrames(const uint8_t* bytes, size_t n,
                                                         size_t off, bool withCrc) {
  std::vector<SplitFrame> out;
  size_t i = off;
  while (i < n) {
    if (out.size() >= 64) return std::nullopt;                // 最多 64 帧
    int bodySize = ufcsFrameBodySize(bytes, n, i);
    if (bodySize < 0) return std::nullopt;
    size_t bodyEnd = i + static_cast<size_t>(bodySize);
    // `total` 是**本帧占多少字节**（长度），不是结束偏移 —— 下面 `i + total > n` 是这么用的。
    // ⚠ 早先写成 `bodyEnd + (withCrc?1:0)`（绝对结束偏移）会把 i 算两遍：任何
    //   「前缀 ≠ 0」或「不止一帧」的候选都被误判越界，穷举兜底等于全程失效
    //   （实机样本因为快路径总能命中，一直没暴露）。
    size_t total = static_cast<size_t>(bodySize) + (withCrc ? 1 : 0);
    if (i + total > n) return std::nullopt;

    uint16_t hdr = static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1]);
    HeaderInfo h = ufcsHeaderInfo(hdr);
    if (!h.addrValid || !h.mtypeValid) return std::nullopt;

    uint8_t calc = ufcsCrc8(bytes + i, bodySize);
    SplitFrame f;
    f.off = i;
    f.bodyEnd = bodyEnd;
    f.total = total;
    f.hdr = hdr;
    f.calc = calc;
    f.hasCrc = withCrc;
    if (withCrc) {
      f.crc = bytes[bodyEnd];
      f.crcOk = (f.crc == calc);
    } else {
      f.crc = 0;
      f.crcOk = false;
    }
    f.structScore = ufcsFrameScore(bytes, n, i, withCrc);
    out.push_back(f);
    i = bodyEnd + (withCrc ? 1 : 0);
  }
  return (i == n) ? std::optional<std::vector<SplitFrame>>(std::move(out)) : std::nullopt;
}

/* ────────────────────────── 状态事件 ────────────────────────── */

std::optional<Event> ufcsParseEvent(const uint8_t* blob, size_t len) {
  if (len != 8) return std::nullopt;
  if (blob[7] != kUfcsEventTail) return std::nullopt;
  Event e;
  e.tsMs = static_cast<long long>(pdscope::rdU32LE(blob));
  e.code = blob[4];
  return e;
}

/* ────────────────────────── 帧记录（快路径）────────────────────────── */

std::optional<Record> ufcsParseRecord(const uint8_t* blob, size_t len) {
  if (len < 4 + 4 + 1 + 4) return std::nullopt;     // 最短的一帧也要 13 字节
  if (blob[8] != kUfcsTraining) return std::nullopt;

  const size_t prefixBytes = 9;
  if (len < prefixBytes + 4) return std::nullopt;
  const uint8_t* frame = blob + prefixBytes;
  size_t flen = len - prefixBytes;

  int flag = blob[7];
  if (flag > 1) return std::nullopt;                // 只在 0/1 上才是「链路」

  int bodySize = ufcsFrameBodySize(frame, flen, 0);
  if (bodySize < 0) return std::nullopt;
  int total = bodySize + 1;                          // 含 CRC
  if (static_cast<size_t>(total) != flen) return std::nullopt;

  int lenField = blob[6];
  if (lenField != static_cast<int>(flen) + 1) return std::nullopt;

  uint8_t calc = ufcsCrc8(frame, static_cast<size_t>(bodySize));
  uint8_t crc = frame[bodySize];
  uint16_t hdr = static_cast<uint16_t>((frame[0] << 8) | frame[1]);
  HeaderInfo h = ufcsHeaderInfo(hdr);
  if (!h.addrValid || !h.mtypeValid) return std::nullopt;

  Record rec;
  RecordFrame rf;
  rf.off = prefixBytes;
  rf.bodyEnd = prefixBytes + static_cast<size_t>(bodySize);
  rf.crc = crc;
  rf.calc = calc;
  rf.crcOk = (crc == calc);
  rec.frames.push_back(rf);
  rec.prefixBytes = prefixBytes;
  rec.line = (flag == 0) ? "D+" : "D-";
  rec.hasLine = true;
  rec.tsMs = static_cast<long long>(pdscope::rdU32LE(blob));
  rec.hasTs = true;
  rec.lenField = lenField;
  rec.hasLenField = true;
  rec.hasCounter = true;
  rec.counterX0 = blob[4];
  rec.counterX1 = blob[5];
  return rec;
}

/* ────────────────────────── 穷举兜底 ────────────────────────── */

std::optional<Located> ufcsLocateFrames(const uint8_t* blob, size_t len) {
  if (len < 3) return std::nullopt;                 // 最小的一帧是「控制消息 + 无 CRC」= 3 字节

  // 0. 先试分析仪容器布局（快路径），认得出来就用它（额外给出物理链路与容器长度域）。
  auto rec = ufcsParseRecord(blob, len);
  if (rec) {
    Located loc;
    loc.prefixBytes = rec->prefixBytes;
    loc.withCrc = true;
    for (auto& rf : rec->frames) {
      LocatedFrame lf;
      lf.off = rf.off;
      lf.bodyEnd = rf.bodyEnd;
      lf.crc = rf.crc;
      lf.calc = rf.calc;
      lf.crcOk = rf.crcOk;
      loc.frames.push_back(lf);
    }
    return loc;
  }

  struct Cand {
    size_t prefixBytes = 0;
    bool withCrc = false;
    int tier = 0;
    int structScore = 0;
    std::vector<SplitFrame> frames;
  };
  std::optional<Cand> best;

  auto better = [](const Cand& a, const Cand& b) -> bool {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.structScore != b.structScore) return a.structScore > b.structScore;
    return a.prefixBytes < b.prefixBytes;
  };

  size_t limit = std::min(static_cast<size_t>(16), len - 3);
  for (size_t p = 0; p <= limit; p++) {
    for (bool withCrc : {true, false}) {
      auto frames = splitFrames(blob, len, p, withCrc);
      if (!frames || frames->empty()) continue;
      bool allOk = withCrc;
      for (auto& f : *frames) {
        if (!f.crcOk) { allOk = false; break; }
      }
      Cand c;
      c.prefixBytes = p;
      c.withCrc = withCrc;
      c.tier = allOk ? 0 : (withCrc ? 1 : 2);
      c.structScore = 0;
      for (auto& f : *frames) c.structScore += f.structScore;
      c.frames = *frames;
      if (!best || better(c, *best)) best = c;
    }
  }
  if (!best) return std::nullopt;

  Located loc;
  loc.prefixBytes = best->prefixBytes;
  loc.withCrc = best->withCrc;
  for (auto& f : best->frames) {
    LocatedFrame lf;
    lf.off = f.off;
    lf.bodyEnd = f.bodyEnd;
    lf.crc = f.hasCrc ? f.crc : 0;
    lf.calc = f.calc;
    lf.crcOk = f.crcOk;
    loc.frames.push_back(lf);
  }
  return loc;
}

}}  // namespace pdscope::ufcs
