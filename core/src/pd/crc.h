// crc.h — USB PD 报文尾部用的 CRC-32（与 zlib 同多项式 0xEDB88320，反射算法）
//
// 对应 JS 源：src/js/pd/crc.js
// 计算范围：Message Header（2 字节，小端）+ 全部 Data Object（各 4 字节，小端）。

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace pdscope { namespace pd {

/** CRC-32（反射算法，多项式 0xEDB88320），结果 32bit 无符号。 */
uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32(const std::vector<uint8_t>& data);

}}  // namespace pdscope::pd
