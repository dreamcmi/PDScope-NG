// crc.h — UFCS 协议层 CRC-8
//
// 依据 T/CCSA 393—2024 / T/TAF 083—2024 附录 A（规范性）：
//   多项式 X^8 + X^5 + X^3 + 1 → 0x29，初值 0x00，
//   逐字节异或后左移 8 次（MSB 优先，不反射，结果不取反）。
//
// 覆盖范围：消息头 + 消息主体（不含 CRC 自身，也不含物理层的 Training / 起止位）。

#pragma once

#include <cstdint>
#include <cstddef>

namespace pdscope { namespace ufcs {

/** CRC-8 多项式（X^8 + X^5 + X^3 + 1） */
constexpr uint8_t kCrc8Poly = 0x29;

/**
 * @param data 消息头 + 消息主体
 * @param len  只算前 len 字节
 * @returns 0…255
 */
uint8_t ufcsCrc8(const uint8_t* data, size_t len);

}}  // namespace pdscope::ufcs
