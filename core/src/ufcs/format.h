// format.h — UFCS 载荷的位域取值与文本格式化
//
// 与 PD 侧最大的不同：UFCS 的**多字节字段是高字节在前**（规范反复强调
// 「发送的时候，先发送高字节」）。也就是载荷字节数组本身就是一份「大端位串」，
// payload[0] 是最高字节。
//
// 于是「取 bit msb…lsb」的定位是：
//     字节下标 = n - 1 - (bit >> 3)      （n = 载荷字节数）
//     位  下标 = bit & 7

#pragma once

#include <cstdint>
#include <string>
#include <cstddef>

namespace pdscope { namespace ufcs {

/** 位域：[msb..lsb] → 无符号整数（载荷按大端位串解读）。n = 载荷字节数。 */
uint32_t ufcsBits(const uint8_t* bytes, size_t n, int msb, int lsb);

/** 单个比特（0/1）。 */
int ufcsBit(const uint8_t* bytes, size_t n, int i);

/** 位域名：`B31-16` / `B8`（详情页字段标题）。 */
std::string ufcsRange(int msb, int lsb);

/** 整个载荷 → 十六进制串（按线上顺序，高字节在前，无分隔）。 */
std::string ufcsHex(const uint8_t* bytes, size_t n);

/** 大端读 16 位（越界返回 0）。 */
uint16_t ufcsU16BE(const uint8_t* bytes, size_t n, size_t off = 0);

/** 数字 → 文本（整数不带小数点，浮点最多 3 位）。 */
std::string ufcsNum(double v);

/** 布尔位 → `1 (…)/0 (…)`。 */
std::string ufcsFlag(bool on, const std::string& t = "是", const std::string& f = "否");

/** 保留域：非 0 时提示（规范要求接收端忽略，但非 0 常意味着版本差异）。 */
std::string ufcsReserved(const uint8_t* bytes, size_t n, int msb, int lsb);

/** 电压：单位 10 mV。 */
std::string ufcsVolt(int v10mv);
/** 电流：单位 10 mA。 */
std::string ufcsAmp(int v10ma);
/** 温度：值 - 50（0 表示无数据）。 */
std::string ufcsTemp(int raw);
/** 十六进制定长（大写、补零），如 0x00AB。 */
std::string ufcsHexNum(int v, int digits = 4);

/** 字节数组 → 可打印 ASCII（不可打印位用 `·`，遇 0 截断）。 */
std::string ufcsAscii(const uint8_t* bytes, size_t n);

/** 字节数组 → 空格分隔的 hex（界面「数据」列用）。 */
std::string ufcsHexSpaced(const uint8_t* bytes, size_t n);

}}  // namespace pdscope::ufcs
