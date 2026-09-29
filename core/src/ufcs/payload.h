// payload.h — UFCS 各类消息的载荷逐字段解析
//
// 规范第 8 章把消息分成三类，本文件负责其中「消息主体」那一层的语义：
//   • 控制消息（8.2.3）   主体只有 1 字节命令，无载荷 —— 由 decoder 直接判定，不进来；
//   • 数据消息（8.2.4）   命令(1) + 数据长度(1) + 数据(N)
//   • 自定义消息（8.2.5） 厂家识别码(2) + 数据长度(1) + 数据(N)
//
// 所有多字节字段按规范「先发送高字节」的约定解读，位域定位见 format.h#ufcsBits。
//
// 注：ufcsDataPayload 的第五个参数 ctxRevText 对应 JS 版 ctx.revText（协议版本文本），
// 当前语义下解析不依赖它，故接收后仅占位；ufcsCustomPayload 的 p 指向「厂家识别码」字段
// （即整段自定义消息体去掉 2 字节消息头之后），n 为其后字节数 —— 这样可保持与契约一致的签名
// 且不丢失 vid。

#pragma once

#include "util.h"
#include "packet.h"
#include <cstdint>
#include <cstddef>
#include <string>

namespace pdscope { namespace ufcs {

/** 一次载荷解析的结果（摘要进入详情面板的「解析详情」列）。 */
struct PayloadResult {
  std::string summary;
};

/**
 * 数据消息载荷解析。
 * @param em   详情发射器
 * @param cmd  命令编号
 * @param p    数据区指针（消息头 + 命令 + 长度 之后）
 * @param n    数据区字节数
 * @param ctxRevText 协议版本文本（占位，当前未使用）
 */
PayloadResult ufcsDataPayload(pdscope::DetailEmitter& em, int cmd, const uint8_t* p, size_t n,
                              const std::string& ctxRevText);

/**
 * 厂家自定义消息解析。p 指向「厂家识别码」字段，n 为其后字节数。
 */
PayloadResult ufcsCustomPayload(pdscope::DetailEmitter& em, const uint8_t* p, size_t n);

/** 逐字节列出（自定义消息 / 未知命令的诊断用），返回空格分隔的 hex 串（最多前 48 字节）。 */
std::string ufcsDumpBytes(const uint8_t* p, size_t n);

}}  // namespace pdscope::ufcs
