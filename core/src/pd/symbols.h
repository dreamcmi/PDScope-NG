// symbols.h — USB PD 物理层符号表（BMC 4B5B 编码、K-code、有序集）
//
// 对应 JS 源：src/js/pd/symbols.js
// 位序约定：每字节内 bit0 时间最早（LSB 优先）。5 个比特拼一个符号，
// 因此符号里的 bit0 也是最早收到的位。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdscope { namespace pd {

/* ── K-code（控制符号）── */
constexpr uint8_t SYM_ERR    = 0x10;
constexpr uint8_t SYM_SYNC1  = 0x11;
constexpr uint8_t SYM_SYNC2  = 0x12;
constexpr uint8_t SYM_SYNC3  = 0x13;
constexpr uint8_t SYM_RST1   = 0x14;
constexpr uint8_t SYM_RST2   = 0x15;
constexpr uint8_t SYM_EOP    = 0x16;

/**
 * 5bit 原始码 → 4bit 数据 / K-code。
 * 下标 = 5 个采样位上读到的比特（bit0 最先收到），值 = 解码结果；
 * 0x10 = 非法编码（含 00000 / 11111 与未定义码），0x11…0x16 = K-code。
 */
extern const uint8_t DEC4B5B[32];

/** 符号名：[长名, 短名]，下标同 DEC4B5B 的取值域（0x00…0x16） */
extern const char* SYM_NAME[23][2];

/** 符号短名（下标越界返回 "??"）。 */
std::string symName(uint8_t s);

/** 有序集（Ordered Set）：4 个连续 K-code 组成的前导序列。 */
struct OrderedSet {
  uint8_t sequence[4];
  std::string name;
  std::string shortName;
  std::string peer;
  std::string link;
};

extern const std::vector<OrderedSet> SOP_ORDERED_SETS;

/** 纯序列数组（与 SOP_ORDERED_SETS 一一对应）。 */
extern const std::vector<std::vector<uint8_t>> SOP_SEQUENCES;

/** 序列（4 个符号值，按 JS 的 join() 形式拼接成的字符串）→ 有序集。 */
const OrderedSet* findOrderedSetByKey(const std::string& key);

/** 名称 → 有序集。 */
const OrderedSet* findOrderedSetByName(const std::string& name);

/** 用 4 个符号值构造 JS 同形的 key（如 "17,17,17,18"）。 */
std::string sopKey(const uint8_t sym[4]);

/** 只用 4 个符号里的「前 k 个」判定有序集（返回最佳匹配）。 */
struct OrderedSetMatch {
  const OrderedSet* set;
  int matched;
  int total;
};
OrderedSetMatch matchOrderedSet(const std::vector<uint8_t>& symbols);

}}  // namespace pdscope::pd
