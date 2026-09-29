// svid.h — Standard/Vendor ID 名称表
//
#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <optional>

namespace pdscope { namespace pd {

/** 标准 ID（PD 3.2 Table 6.33 明确列出的两个）。 */
extern const std::map<int, std::string> STANDARD_SVID;

/** 常见厂商 ID（USB-IF VID 分配，非穷举）。 */
extern const std::map<int, std::string> VENDOR_VID;

/** SVID → 名称（未知名返回 null → 用空 optional 表达）。 */
std::optional<std::string> svidName(uint16_t svid);

/** SVID → 简短标签（附带十六进制原值）。 */
std::string svidText(uint16_t svid);

/** 该 SVID 是否为本规范定义的标准 ID。 */
inline bool isStandardSvid(uint16_t svid) {
  return (svid & 0xFFFF) == 0xFF00 || (svid & 0xFFFF) == 0xFF01;
}

constexpr uint16_t SVID_PD = 0xFF00;
constexpr uint16_t SVID_DPTC = 0xFF01;

}}  // namespace pdscope::pd
