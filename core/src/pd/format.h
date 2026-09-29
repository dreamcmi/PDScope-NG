// format.h — 位域取值与文本格式化的小工具（PD 库共用）
//
// 所有顶层名字都带 `pd` 前缀。

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

#include "util.h"

namespace pdscope { namespace pd {

/** 位域：[msb..lsb] → 无符号整数（v 视为 32bit 无符号）。 */
inline uint32_t pdField(uint32_t v, int msb, int lsb) {
  int n = msb - lsb + 1;
  uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
  return (v >> lsb) & mask;
}

/** 位域：单个比特 → 0/1。 */
inline uint32_t pdBit(uint32_t v, int i) { return (v >> i) & 1u; }

/** 位域名：`B31-24` / `B5`。 */
inline std::string pdRange(int msb, int lsb) {
  if (msb == lsb) return "B" + std::to_string(msb);
  return "B" + std::to_string(msb) + "-" + std::to_string(lsb);
}

/** 位域二进制串：`0b010`。 */
std::string pdBin(uint32_t v, int msb, int lsb);

/** 32/16 位十六进制（大写补零）。 */
inline std::string pdHex(uint32_t v, int digits = 8) {
  return pdscope::hexU(v, digits);
}

/**
 * **不补零**的大写十六进制。
 *
 * ⚠ 别拿 `pdHex(v, 1)` / `hexU(v, 1)` 顶替：`hexU` 是**定宽**的，digits=1 只留最低
 *   一个 nibble ⇒ `0x1AB` 会静默变成 `0xB`、`Reserved [B12-0]` 写 0x1234 只剩 `0x4`。
 *   凡是「Reserved / 任意宽度值」都要走这个。
 */
inline std::string pdHexVar(uint32_t v) { return pdscope::hexVar(v); }

/** 数字 → 字符串（整数不带小数点，浮点最多 3 位、去掉多余的 0）。 */
inline std::string pdNum(double v) { return pdscope::numToStr(v); }

/**
 * 布尔位 → `1 (Supported)` / `0 (Not supported)` 风格的文本。
 *
 * ⚠ 参数类型必须是**一个**整型：写成 `pdFlag(bool,…)` + `pdFlag(int,…)` 两个重载时，
 * 传 `uint32_t`（`pdBit()` 的返回类型就是这个）两个重载都要做一次标准转换，
 * 于是变成「调用不明确」而编不过。
 */
inline std::string pdFlag(uint32_t on, const std::string& t = "Yes",
                          const std::string& f = "No") {
  return (on != 0) ? ("1 (" + t + ")") : ("0 (" + f + ")");
}

/** Reserved 字段：值非 0 时提示。 */
std::string pdReserved(uint32_t v, int msb, int lsb);

/** 查表，未命中给默认文案。 */
std::string pdLookup(const std::map<int, std::string>& table, int key,
                     const std::string& fallback = "Reserved");

/** 32bit → 4 字节（LSB 在前，与线上传输顺序一致）。 */
std::vector<uint8_t> pdBytes4(uint32_t v);

/** 字节数组 → 32bit（LSB 在前，最多 4 字节）。 */
uint32_t pdBytesToU32(const std::vector<uint8_t>& bytes);

/** 字节数组 → 可打印 ASCII（不可打印位用 `·`，遇 0 截断）。 */
std::string pdAscii(const std::vector<uint8_t>& bytes);

/** 两个 8bit 字符（Alpha-2 国家码 / 通用 ASCII 对）→ 字符串。 */
std::string pdCharPair(uint8_t lo, uint8_t hi);

/** 百分比 / 电压 / 电流 的带单位文本。 */
inline std::string pdVolt(double v) { return pdNum(v) + " V"; }
inline std::string pdAmp(double v) { return pdNum(v) + " A"; }
inline std::string pdWatt(double v) { return pdNum(v) + " W"; }

}}  // namespace pdscope::pd
