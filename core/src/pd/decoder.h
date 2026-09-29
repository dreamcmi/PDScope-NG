// decoder.h — USB Power Delivery 报文解码器（独立库的主入口）
//
// 命名空间：pdscope::pd
//
// 公共接口（契约，不可改）：
//   crc32(...)                          —— 见 crc.h
//   class PdDecoder { decode / decodeWire / reset }

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>

#include "util.h"
#include "packet.h"
#include "tables.h"

namespace pdscope { namespace pd {

/* ── PDO 元信息（供 RDO / EPR_Request 反查）── */
struct PdoMeta {
  int type = 0;
  std::string role;
  bool isEpr = false;
  std::string kind = "unknown";
  int position = 0;
  int apdoType = -1;
};

/* ── Discover Identity 结果（仅存储，跨包关联用）── */
struct PdIdentity {
  int ptUfp = 0, ptDfp = 0, conn = 0, vid = 0;
  std::string productTypeText;
  std::string vdoKind;
  bool cable = false;
  int host = 0, device = 0, modal = 0;
  uint32_t certStat = 0;
  uint32_t product = 0;
};

/* ── 最近一次 VDM ── */
struct PdLastVdm {
  uint16_t svid = 0;
  int cmd = 0;
  int cmdType = 0;
  int objPos = 0;
};

/* ── EPR 能力分块拼接的尾巴 ── */
struct ExtCarry {
  int objIdx = 0;
  std::vector<uint8_t> bytes;
};

/* ── 跨报文解码状态 ── */
struct PdState {
  std::map<int, std::string> pdosSource;
  std::map<int, std::string> pdosSink;
  std::map<int, PdoMeta> pdoMetaSource;
  std::map<int, PdoMeta> pdoMetaSink;
  std::vector<uint16_t> svidList;
  PdIdentity identity;
  bool identitySet = false;
  PdLastVdm lastVdm;
  std::optional<ExtCarry> extCarry;
};

/* ── 解析函数共用的上下文 ── */
struct PdCtx {
  int rev = 0;
  std::string revText;
  std::string sop;
  std::string link;
  std::string role;
};

class PdDecoder {
public:
  explicit PdDecoder(double sampleRate);
  void reset();

  // .atkcc 路径：从 BMC 解出的位流解码
  std::optional<Packet> decode(const BmcRawPacket& raw, int channel = 0);

  // POWER-Z 路径：从已解好的逻辑字节解码（不含 CRC / SOP / EOP）
  std::optional<Packet> decodeWire(const uint8_t* wire, size_t len,
                                   const std::string& sop, double timeMs, int channel,
                                   bool crcRecorded, int sopByte, bool powerz);

private:
  uint8_t _sym(size_t i);
  uint16_t _byte();
  int _nibble();
  uint32_t _short();
  uint32_t _word();
  void warn(const std::string& longm, const std::string& shortm);

  long long _scanSop();

  int headRevCode() const { return (head_ >> 6) & 3; }
  int headRev() const { return headRevCode() + 1; }
  std::string headRevText() const { return tbl(SPEC_REV, headRevCode()); }
  bool headModern() const { return headRevCode() >= 2; }
  int headExt() const { return headModern() ? static_cast<int>((head_ >> 15) & 1) : 0; }
  int headId() const { return static_cast<int>((head_ >> 9) & 7); }
  int headPowerRole() const { return static_cast<int>((head_ >> 8) & 1); }
  int headDataRole() const { return static_cast<int>((head_ >> 5) & 1); }
  int headCount() const { return static_cast<int>((head_ >> 12) & 7); }
  int headType() const { return headModern() ? static_cast<int>(head_ & 0x1F)
                                             : static_cast<int>(head_ & 0x0F); }

  void _makeEmitter();

  std::string _resolveRole(const std::string& link);
  void _versionHints(int t, bool isExt, int nObjects, const std::string& shortm);
  uint32_t _computeCrc();
  void _payload(int idx, int t, const std::string& link);
  void _vdm(const std::string& link);

  struct ExtTry {
    int payload = 0, pad = 0, off = 0;
    std::vector<uint8_t> bytes;
    uint32_t crc = 0, calc = 0;
    bool ok = false;
  };
  struct ExtCandidate { int payload, pad, off; };
  std::vector<ExtCandidate> _extCandidates(bool chunked, int chunkNum, bool reqChunk, int dataSize);
  ExtTry _readExtTry(const ExtCandidate& cand,
                    const std::vector<uint8_t>& hdrBytes,
                    const std::vector<uint8_t>& extBytes);
  struct ExtResult { int extHead; int objCount; uint32_t crc; uint32_t calc; bool crcOk; };
  ExtResult _readExtended(int t, const std::string& link);

  Packet _specialPacket(const BmcRawPacket& raw, int channel);

  /** 把本次累积的 details / summary / 数据 组装成 Packet。 */
  struct FinishOpts {
    std::string sop;
    std::string msgType;
    int msgTypeRaw = -1; bool hasMsgTypeRaw = false;
    std::string msgKind;
    std::string role;
    uint16_t header = 0; bool hasHeader = false;
    int extHeader = -1; bool hasExtHeader = false;
    int msgId = -1; bool hasMsgId = false;
    int rev = -1; bool hasRev = false;
    std::string revText;
    int powerRole = -1, dataRole = -1;
    std::string link;
    int nObjects = 0;
    uint32_t crc = 0; bool hasCrc = false;
    uint32_t crcCalc = 0;
    CrcState crcState = CrcState::Unrecorded;
    bool crcRecorded = true;
    bool eop = false;
    std::string category;
  };
  Packet _finishTail(const FinishOpts& o, const BmcRawPacket& raw, int channel);

  double sampleRate_ = 0;
  uint64_t packetSeq_ = 0;
  PdState st_;
  std::string lastSopPowerRole_ = "SRC";

  // per-packet 工作区（每次 decode 重置）
  std::vector<uint8_t> bits_;
  size_t idx_ = 0;
  std::vector<PacketWarning> warnings_;
  std::vector<DetailItem> details_;
  std::vector<std::string> summaryParts_;
  std::vector<uint32_t> dataWords_;
  std::string text_;
  std::string specialPacket_;
  std::string packetSop_;
  std::string resolvedRole_;
  long long sopBitOffset_ = -1;
  bool hasDataBytesOverride_ = false;
  std::vector<uint8_t> dataBytesOverride_;
  std::vector<uint8_t> crcExtra_;
  int channel_ = 0;
  DetailEmitter em_;
  uint16_t head_ = 0;
};

}}  // namespace pdscope::pd
