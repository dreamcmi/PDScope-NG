// pdo.h — 电源数据对象（PDO）与请求数据对象（RDO）解析
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

struct PdState;  // 完整定义见 decoder.h

/** lookupPdo 的结果（只暴露 RDO 反查所需字段）。 */
struct PdoLookupResult {
  std::string ref;
  std::string kind;  // PdoMeta.kind
  bool known;
};

/**
 * 解析一个 PDO 并写入详情，同时把它登记进状态（供后续 RDO 反查）。
 * @param role  'source' | 'sink'
 * @returns 概览摘要文本
 */
std::string pdoParse(PdState& st, pdscope::DetailEmitter& em, uint32_t pdo,
                     const std::string& role, int position, bool isEpr,
                     const std::string& revText);

/** 解析一个 RDO。表的选择依据「被请求的 PDO 类型」。 */
void rdoParse(PdState& st, pdscope::DetailEmitter& em, uint32_t rdo, bool isEpr);

/** 供外部（如 EPR_Request 的「被请求 PDO 副本」）复用的位置查询。 */
PdoLookupResult lookupPdo(PdState& st, const std::string& role, int pos);

}}  // namespace pdscope::pd
