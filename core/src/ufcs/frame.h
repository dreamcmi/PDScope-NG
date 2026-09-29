// frame.h — UFCS 数据包的「切帧」与分析仪容器行结构识别
//
// 规范第 7 章物理层 / 第 8 章协议层给了三种包结构（都由高字节到低字节依次发送）：
//   控制消息          消息头(2B) │ 控制命令(1B) │ CRC(1B)
//   数据消息          消息头(2B) │ 命令(1B) │ 数据长度(1B) │ 数据(N B) │ CRC(1B)
//   自定义消息        消息头(2B) │ 厂家识别码(2B) │ 数据长度(1B) │ 数据(N B) │ CRC(1B)
//
// 线上还有一个 Training 字节 0xAA（规范 7.4.6），它属于物理层、不进 CRC 覆盖范围，
// 不属于协议意义上的帧，但分析仪会把它一并计入长度。

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

namespace pdscope { namespace ufcs {

/** 解析 16 bit 消息头得到的结构。 */
struct HeaderInfo {
  int hdr = 0;
  int addr = 0;       // 接收方设备地址（bit15…13）
  int msgNo = 0;      // 消息编号（bit12…9）
  int verCode = 0;    // 协议版本编号（bit8…3）
  int mtype = 0;      // 消息类型（bit2…0）
  std::string verText;
  bool verKnown = false;
  bool addrValid = false;
  bool mtypeValid = false;
};

HeaderInfo ufcsHeaderInfo(uint16_t hdr);

/**
 * 一个包从消息头开始、到 CRC（不含）为止的字节数。
 * @returns body 相对 off 的偏移；-1 = 结构不成立（截断 / 长度域越界）
 */
int ufcsFrameBodySize(const uint8_t* bytes, size_t n, size_t off);

/**
 * 「这一段结构上像不像一条报文」的结构分（只看消息头字段与命令编号是否合法，不看 CRC）。
 * @returns <0 = 结构不成立；越大越可信
 */
int ufcsFrameScore(const uint8_t* bytes, size_t n, size_t off, bool withCrc);

/* ───────────── 分析仪（POWER-Z）容器行结构 ───────────── */

/** UFCS 的 Training 序列（规范 7.4.6），也在分析仪容器里作为帧前导出现（0xAA）。 */
constexpr uint8_t kUfcsTraining = 0xAA;
/** 状态事件记录的尾标记（0x40）。 */
constexpr uint8_t kUfcsEventTail = 0x40;

/** 一条已切出的帧（快路径 / 穷举路径共用）。 */
struct RecordFrame {
  size_t off = 0;
  size_t bodyEnd = 0;
  uint32_t crc = 0;
  uint32_t calc = 0;
  bool crcOk = false;
};

/** 一行 Raw 按已知容器布局解出的结果（快路径）。 */
struct Record {
  std::vector<RecordFrame> frames;
  size_t prefixBytes = 0;
  std::string line;          // "D+" / "D-" / ""（空表示容器没给链路）
  bool hasLine = false;
  long long tsMs = 0; bool hasTs = false;
  long long lenField = 0; bool hasLenField = false;
  bool hasCounter = false; long long counterX0 = 0, counterX1 = 0;
};

/** 状态事件（8 字节，不承载协议报文）。 */
struct Event {
  long long tsMs = 0;
  int code = 0;
};

/** 认一行 Raw 是不是 UFCS 的状态事件记录（8 字节，以 0x40 结尾）；不是则 nullopt。 */
std::optional<Event> ufcsParseEvent(const uint8_t* blob, size_t len);

/** 按已知容器布局解一行 Raw（快路径）；认不出返回 nullopt。 */
std::optional<Record> ufcsParseRecord(const uint8_t* blob, size_t len);

/** 穷举定位结果里的一条帧。 */
struct LocatedFrame {
  size_t off = 0;
  size_t bodyEnd = 0;
  uint32_t crc = 0;
  uint32_t calc = 0;
  bool crcOk = false;
};

/** 穷举兜底结果。 */
struct Located {
  std::vector<LocatedFrame> frames;
  size_t prefixBytes = 0;
  bool withCrc = false;
};

/** 在「前面可能带容器前缀」的一段 blob 里定位 UFCS 报文起点并逐帧切分；定位不出返回 nullopt。 */
std::optional<Located> ufcsLocateFrames(const uint8_t* blob, size_t len);

}}  // namespace pdscope::ufcs
