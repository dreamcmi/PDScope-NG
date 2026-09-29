// inflate.h — raw DEFLATE（zlib 静态链入）
//
// .atkcc 的每个分块都是 raw deflate（不含 zlib 头）。这里只做解压，
// 并强制一个解压上限：捕获文件来自外部，必须防「压缩炸弹」。
#pragma once

#include "util.h"

namespace pdscope {

/** 单个条目解压后的字节上限（256 MiB）。正常样本每块固定 1 MiB。 */
constexpr uint64_t kMaxInflateBytes = 256ull * 1024 * 1024;

/**
 * 解压 raw deflate 数据。
 * @param data          压缩数据
 * @param len           压缩数据长度
 * @param expectedSize  预期解压大小；>0 时会校验实际结果是否相符
 * @param what          出错信息里用的条目名（便于定位）
 */
Bytes inflateRaw(const uint8_t* data, size_t len, uint64_t expectedSize,
                 const std::string& what);

}  // namespace pdscope
