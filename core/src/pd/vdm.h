// vdm.h — Vendor Defined Message（VDM）解析
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "packet.h"   // DetailEmitter
#include "util.h"

namespace pdscope { namespace pd {

/** 跨报文解码状态，完整定义在 decoder.h —— 这里只用引用，前向声明即可（避免头文件互含）。 */
struct PdState;

struct PdState;  // 完整定义在 decoder.h
struct PdCtx;    // 完整定义在 decoder.h

/** 解析一条完整的 VDM（含 VDM Header 在内的全部数据对象）。 */
std::string vdmParse(PdState& st, pdscope::DetailEmitter& em,
                     const std::vector<uint32_t>& vdos, const PdCtx& ctx);

/** 标准 VDM 命令名（含 SVID 自定义命令的写法）。 */
std::string vdmCommandName(int cmd);

}}  // namespace pdscope::pd
