// decoder.h — UFCS 报文解码器（主入口）
//
// 流水线：逻辑字节（消息头 2B + 消息主体 + CRC 1B）
//   → 消息头四段位域 → 按类型取主体 → CRC-8 校验
//   → 结合物理链路（D+/D-）还原「谁发给谁」→ 逐字段语义解析（payload.cpp）
//
// 依据：T/CCSA 393—2024 / T/TAF 083—2024《移动终端融合快速充电技术要求》。
//
// 与 PD 只在 Packet 这一层对齐（sop/msgType/role/details/crcOk/startSample…），
// 物理层 / 协议结构均不相同（UFCS 是 UART，没有 4B5B / 32 位报文头 / PDO 表）。

#pragma once

#include "packet.h"
#include "util.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

namespace pdscope { namespace ufcs {

/** 解码一条报文所需的容器上下文（由调用方从 POWER-Z 导出样本填充）。 */
struct DecodeOpts {
  uint32_t crc = 0;          // 读到的 CRC 字节；容器没存 CRC 时 hasCrc=false
  bool hasCrc = false;
  uint32_t crcCalc = 0;      // 已知的计算值；为 0 时由解码器自算
  bool withCrc = false;      // 同 hasCrc（保留，供兼容）
  double timeMs = 0;         // 报文时间戳（毫秒），取 SQLite 的 Time 列 ×1000
  std::string line;          // 容器给出的物理链路 "D+" / "D-"，空表示没有
  int dirByte = -1;          // 容器链路字节， -1 表示没有
  size_t prefixBytes = 0;
  long long counterX0 = 0, counterX1 = 0; bool hasCounter = false;
  long long lenField = 0; bool hasLenField = false;
  bool training = false;
  std::string layout;        // "record" | "scan"
  int channel = 0;
};

class UfcsDecoder {
public:
  explicit UfcsDecoder(double sampleRate);
  void reset();
  /** 解一条报文。frame = 消息头 + 消息主体（不含 CRC），len 为其长度。 */
  std::optional<Packet> decode(const uint8_t* frame, size_t len, const DecodeOpts& opts);

private:
  double sampleRate_;
  uint64_t packetSeq_;
};

/** ACK / NCK 与被确认报文配对。packets 应已按时间排序并分配 index。 */
void ufcsLinkAck(std::vector<Packet>& packets);

}}  // namespace pdscope::ufcs
