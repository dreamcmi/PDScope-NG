// pd_frames.h — 测试用的 PD / UFCS 帧构造器
//
// 用**逆表**把符号重新编码回比特流：`DEC4B5B` 是「5bit → 符号值」，
// 反过来查一遍就得到「符号值 → 5bit」。这样造出来的帧走的是解码器自己的表，
// 不会出现「测试和实现各用一份表、一起错」的情况。
//
// ⚠ 造样本时必须照**读取器/解码器**的约定：
//   · 符号内 bit0 最早收到；
//   · 每个字节先低半字节后高半字节；
//   · EOP 是位流的一部分，不能省（省了会带一条「缺少 EOP」告警）；
//   · UFCS 的容器记录长度域把 Training 字节算进去（`len == flen + 1`）。

#pragma once

#include "util.h"

#include "pd/crc.h"
#include "pd/symbols.h"
#include "ufcs/crc.h"
#include "ufcs/frame.h"
#include "ufcs/tables.h"

#include <vector>

namespace fxframe {

using pdscope::Bytes;

/* ══════════════════════ PD ══════════════════════ */

/** 符号值 → 5bit 采样码（DEC4B5B 的逆）。找不到返回 0。 */
inline uint8_t codeOf(uint8_t value) {
    for (int c = 0; c < 32; ++c) {
        if (pdscope::pd::DEC4B5B[c] == value) return static_cast<uint8_t>(c);
    }
    return 0;
}

/** 把一个符号（5bit，LSB 最先）推进位流。 */
inline void pushSymbol(std::vector<uint8_t>& bits, uint8_t value) {
    const uint8_t code = codeOf(value);
    for (int k = 0; k < 5; ++k) bits.push_back(static_cast<uint8_t>((code >> k) & 1));
}

inline void pushByte(std::vector<uint8_t>& bits, uint8_t b) {
    pushSymbol(bits, static_cast<uint8_t>(b & 0x0F));
    pushSymbol(bits, static_cast<uint8_t>((b >> 4) & 0x0F));
}

/** SOP 有序集（用符号表里的第一组，即 SOP）。 */
inline void pushSop(std::vector<uint8_t>& bits) {
    for (uint8_t s : pdscope::pd::SOP_SEQUENCES[0]) pushSymbol(bits, s);
}

/** 一条完整 PD 报文的**位流**（含 SOP、CRC、EOP），可直接喂 PdDecoder::decode。 */
inline std::vector<uint8_t> pdPacketBits(uint16_t header,
                                        const std::vector<uint32_t>& objects) {
    std::vector<uint8_t> bits;
    pushSop(bits);
    pushByte(bits, static_cast<uint8_t>(header & 0xff));
    pushByte(bits, static_cast<uint8_t>((header >> 8) & 0xff));
    for (uint32_t w : objects) {
        for (int i = 0; i < 4; ++i) pushByte(bits, static_cast<uint8_t>((w >> (8 * i)) & 0xff));
    }
    // CRC-32 覆盖「报文头 + 数据对象」，各按小端字节顺序进
    Bytes covered;
    covered.push_back(static_cast<uint8_t>(header & 0xff));
    covered.push_back(static_cast<uint8_t>((header >> 8) & 0xff));
    for (uint32_t w : objects) {
        for (int i = 0; i < 4; ++i) covered.push_back(static_cast<uint8_t>((w >> (8 * i)) & 0xff));
    }
    const uint32_t crc = pdscope::pd::crc32(covered.data(), covered.size());
    for (int i = 0; i < 4; ++i) pushByte(bits, static_cast<uint8_t>((crc >> (8 * i)) & 0xff));
    pushSymbol(bits, pdscope::pd::SYM_EOP);
    return bits;
}

/* ══════════════════════ BMC 波形 ══════════════════════ */

/**
 * 把位流编成 BMC 采样序列（每采样点 1 个 0/1）。
 *
 * 规则（与解码器 `BmcDecoder` 的判据互逆）：
 *   · 每个位边界必翻转；
 *   · 位值 1 在正中再翻转一次 ⇒ 两个 width 长的游程；
 *   · 位值 0 不翻转 ⇒ 一个 2×width 长的游程。
 * 于是 width 就是「1 UI 等于多少采样点」。
 */
inline std::vector<uint8_t> bmcSamples(const std::vector<uint8_t>& bits, int width = 4,
                                       int padBefore = 40, int padAfter = 80) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() * 2 * static_cast<size_t>(width) + padBefore + padAfter);
    out.insert(out.end(), static_cast<size_t>(padBefore), 0);   // 起始空闲（低）

    uint8_t level = 0;
    auto emit = [&](int n) { out.insert(out.end(), static_cast<size_t>(n), level); };
    for (uint8_t b : bits) {
        level ^= 1;                 // 位边界必翻转
        emit(width);                // 前半个位
        if (b) level ^= 1;          // 位 1 在正中再翻转一次
        emit(width);                // 后半个位：位 0 与前半段同电平，自然合成一个 2×width 游程
    }
    out.insert(out.end(), static_cast<size_t>(padAfter), 1);    // 收尾空闲（高，且非 0 免得被裁）
    return out;
}

/* ══════════════════════ POWER-Z 的 Raw blob ══════════════════════ */

/**
 * PD 的报文 blob：`marker │ ts(4B LE) │ sopByte │ wire`
 * 其中 `marker = 0x80 | (wireLen + 5)`（低 6 位 = 记录总长 - 1）。
 * ⚠ `wire` 不含 CRC / SOP / EOP —— 与 `PdDecoder::decodeWire` 的入参同形。
 */
inline Bytes pdBlob(uint32_t tsMs, uint8_t sopByte, const Bytes& wire) {
    Bytes b;
    b.push_back(static_cast<uint8_t>(0x80 | (wire.size() + 5)));
    b.push_back(static_cast<uint8_t>(tsMs & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 24) & 0xff));
    b.push_back(sopByte);
    b.insert(b.end(), wire.begin(), wire.end());
    return b;
}

/** PD 的插拔事件 blob：`0x45 │ ts(3B LE) │ 0x00 │ code`（0x11=接入 / 0x12=断开）。 */
inline Bytes pdConnectBlob(uint32_t tsMs, uint8_t code) {
    Bytes b;
    b.push_back(0x45);
    b.push_back(static_cast<uint8_t>(tsMs & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 16) & 0xff));
    b.push_back(0x00);
    b.push_back(code);
    return b;
}

/* ══════════════════════ UFCS ══════════════════════ */

/**
 * 造一条 UFCS 控制消息的字节（消息头 + 命令），不含 CRC。
 * @param addr  接收方设备地址（1=供电设备 SRC，2=充电设备 SNK，3=线缆）
 * @param msgNo 消息编号
 * @param ver   协议版本编号（查 kVersion，例如 0b000001 = 1.0.0）
 * @param cmd   控制命令（kCtrlCmd 的键）
 */
inline Bytes ufcsControl(uint8_t addr, uint8_t msgNo, uint8_t ver, uint8_t cmd) {
    const uint16_t hdr = static_cast<uint16_t>(((addr & 0x7) << 13) | ((msgNo & 0xF) << 9)
                                              | ((ver & 0x3F) << 3) | 0u);
    Bytes b;
    b.push_back(static_cast<uint8_t>(hdr >> 8));   // 消息头高字节先发
    b.push_back(static_cast<uint8_t>(hdr & 0xff));
    b.push_back(cmd);
    return b;
}

/** 追加 CRC-8，得到完整帧（含 CRC）。 */
inline Bytes withCrc(const Bytes& body) {
    Bytes b = body;
    b.push_back(pdscope::ufcs::ufcsCrc8(body.data(), body.size()));
    return b;
}

/**
 * 按分析仪的容器布局造一行 Raw：
 *   ts(4B LE) │ x0 │ x1 │ len │ flag │ 0xAA │ 帧（含 CRC）
 * `len = 帧长 + 1`（把 Training 字节算进去）。
 * @param flag 物理链路：0 → D+（供电设备侧发），1 → D-（充电设备侧发）
 */
inline Bytes ufcsRecord(uint32_t tsMs, uint8_t x0, uint8_t x1, uint8_t flag, const Bytes& frame) {
    Bytes b;
    b.push_back(static_cast<uint8_t>(tsMs & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 24) & 0xff));
    b.push_back(x0);
    b.push_back(x1);
    b.push_back(static_cast<uint8_t>(frame.size() + 1));   // lenField
    b.push_back(flag);
    b.push_back(pdscope::ufcs::kUfcsTraining);
    b.insert(b.end(), frame.begin(), frame.end());
    return b;
}

/** 状态事件记录（8 字节，以 0x40 结尾）。 */
inline Bytes ufcsEvent(uint32_t tsMs, uint8_t code) {
    Bytes b;
    b.push_back(static_cast<uint8_t>(tsMs & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((tsMs >> 24) & 0xff));
    b.push_back(code);
    b.push_back(0);
    b.push_back(0);
    b.push_back(pdscope::ufcs::kUfcsEventTail);
    return b;
}

}  // namespace fxframe
