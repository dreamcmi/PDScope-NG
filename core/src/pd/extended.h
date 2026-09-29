// extended.h — 扩展消息数据块解析（PD 3.2 Chapter 6.5）
//
// 对应 JS 源：src/js/pd/extended.js

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "util.h"
#include "decoder.h"   // 引入 PdState / PdCtx / ExtCarry

namespace pdscope { namespace pd {

/** 扩展消息的数据块（分块场景下由调用方给出本包覆盖的字节区间 off）。 */
struct ExtBlock {
  std::vector<uint8_t> bytes;          // 本包承载的字节（payload slice）
  int off = 0;                         // 在「整块扩展数据」里的绝对偏移
  int dataSize = 0;
  bool chunked = false;
  int chunkNum = 0;
  bool reqChunk = false;
  std::optional<ExtCarry> carry;       // _carry（上一分块留下的尾巴）
  int group = -1;                      // _group（详情分组游标）
  std::optional<ExtCarry> tail;        // _tail（输出：留给下一分块的尾巴）
};

/** 解析一个扩展消息数据块，写入详情；返回概览片段。 */
std::string extendedParse(PdState& st, pdscope::DetailEmitter& em, int t,
                          ExtBlock& bk, const PdCtx& ctx);

}}  // namespace pdscope::pd
