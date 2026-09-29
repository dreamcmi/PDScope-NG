// data.h — 数据消息里除 PDO/RDO/VDM 之外的固定格式对象
//
// 对应 JS 源：src/js/pd/data.js

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "packet.h"   // DetailEmitter 在这里（util.h 只给基础件）
#include "util.h"

namespace pdscope { namespace pd {

std::string bistParse(pdscope::DetailEmitter& em, uint32_t data, int idx, const std::string& revText);
std::string batteryStatusParse(pdscope::DetailEmitter& em, uint32_t data);
std::string alertParse(pdscope::DetailEmitter& em, uint32_t data, int idx);
std::string enterUsbParse(pdscope::DetailEmitter& em, uint32_t data);
std::string sourceInfoParse(pdscope::DetailEmitter& em, uint32_t data, int idx);
std::string revisionParse(pdscope::DetailEmitter& em, uint32_t data);
std::string eprModeParse(pdscope::DetailEmitter& em, uint32_t data, int idx);
std::string countryCodeParse(pdscope::DetailEmitter& em, uint32_t data);
std::string manufacturerString(pdscope::DetailEmitter& em, const std::vector<uint8_t>& bytes);

}}  // namespace pdscope::pd
