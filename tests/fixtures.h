// fixtures.h — 测试用的合成样本构造器
//
// 为什么不直接读仓库外那份真实抓包：CI 上没有它们，而有样例才跑的测试等于没测。
// 所以这里按**格式规范**手工造出最小的合法容器（ZIP / SQLite / .pdStream / 协议帧），
// 让每条断言在任何机器上都能跑；真实样本另有一组「有就跑、没有就跳过」的补充测试。
//
// ⚠ 造样本时必须照**读取器**的约定来（例如 ZIP 走中央目录、UFCS 记录的长度域把
// Training 字节算进去）—— 造错了会得到「测试通过但真实文件打不开」，那还不如不测。

#pragma once

#include "util.h"

#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

namespace fx {

using pdscope::Bytes;

/* ══════════════════════ 小端写入 ══════════════════════ */

inline void putU8(Bytes& b, uint8_t v) { b.push_back(v); }
inline void putU16(Bytes& b, uint16_t v) { b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); }
inline void putU32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
inline void putU64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
inline void putBytes(Bytes& b, const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b.insert(b.end(), q, q + n);
}
inline void putStr(Bytes& b, const std::string& s) { putBytes(b, s.data(), s.size()); }

inline uint32_t zipCrc32(const Bytes& d) {
    return static_cast<uint32_t>(::crc32(0L, reinterpret_cast<const Bytef*>(d.data()),
                                         static_cast<uInt>(d.size())));
}

/** raw DEFLATE（负 windowBits，即 ZIP 用的那种，没有 zlib 头尾）。 */
inline Bytes deflateRaw(const Bytes& in, int level = 6) {
    z_stream zs{};
    if (::deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Bytes{};
    }
    Bytes out(in.size() + in.size() / 2 + 64);
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) { ::deflateEnd(&zs); return Bytes{}; }
    out.resize(zs.total_out);
    ::deflateEnd(&zs);
    return out;
}

/* ══════════════════════ ZIP ══════════════════════ */

struct ZipItem {
    std::string name;
    Bytes data;
    bool deflate = true;   // false = 纯存储（method 0）
};

/**
 * 造一个最小但完全合法的 ZIP（本地文件头 + 中央目录 + EOCD）。
 * 名字按 UTF-8 写、置 flag bit11，与 ATK-C 导出的那份一致。
 */
inline Bytes makeZip(const std::vector<ZipItem>& items) {
    Bytes out;
    struct Rec { uint64_t localOffset; uint32_t crc; uint64_t comp; uint64_t uncomp;
                 uint16_t method; std::string name; };
    std::vector<Rec> recs;
    recs.reserve(items.size());

    for (const ZipItem& it : items) {
        Bytes stored = it.deflate ? deflateRaw(it.data) : it.data;
        const uint16_t method = it.deflate ? 8 : 0;

        Rec r;
        r.localOffset = out.size();
        r.crc = zipCrc32(it.data);
        r.comp = stored.size();
        r.uncomp = it.data.size();
        r.method = method;
        r.name = it.name;

        putU32(out, 0x04034b50);
        putU16(out, 20);          // version needed
        putU16(out, 0x0800);      // flag: UTF-8 名字
        putU16(out, method);
        putU16(out, 0);           // mod time
        putU16(out, 0x21);        // mod date（1980-01-01，任意固定值）
        putU32(out, r.crc);
        putU32(out, static_cast<uint32_t>(r.comp));
        putU32(out, static_cast<uint32_t>(r.uncomp));
        putU16(out, static_cast<uint16_t>(it.name.size()));
        putU16(out, 0);           // extra len
        putStr(out, it.name);
        putBytes(out, stored.data(), stored.size());
        recs.push_back(std::move(r));
    }

    const uint64_t cdOffset = out.size();
    for (const Rec& r : recs) {
        putU32(out, 0x02014b50);
        putU16(out, 20);          // version made by
        putU16(out, 20);          // version needed
        putU16(out, 0x0800);      // flag
        putU16(out, r.method);
        putU16(out, 0);
        putU16(out, 0x21);
        putU32(out, r.crc);
        putU32(out, static_cast<uint32_t>(r.comp));
        putU32(out, static_cast<uint32_t>(r.uncomp));
        putU16(out, static_cast<uint16_t>(r.name.size()));
        putU16(out, 0);           // extra
        putU16(out, 0);           // comment
        putU16(out, 0);           // disk start
        putU16(out, 0);           // internal attrs
        putU32(out, 0);           // external attrs
        putU32(out, static_cast<uint32_t>(r.localOffset));
        putStr(out, r.name);
    }
    const uint64_t cdSize = out.size() - cdOffset;

    putU32(out, 0x06054b50);
    putU16(out, 0);
    putU16(out, 0);
    putU16(out, static_cast<uint16_t>(recs.size()));
    putU16(out, static_cast<uint16_t>(recs.size()));
    putU32(out, static_cast<uint32_t>(cdSize));
    putU32(out, static_cast<uint32_t>(cdOffset));
    putU16(out, 0);               // comment len
    return out;
}

/** 把字节当成「每采样点 1 bit、LSB 优先」的位图（就是 .atkcc 里 .bin 的内容）。 */
inline Bytes packBitsLsb(const std::vector<uint8_t>& bits) {
    Bytes out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) out[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
    return out;
}

/**
 * 造一份最小的 .atkcc（单通道）。
 * @param rawBits 该通道的原始位图字节（1 采样点 = 1 bit，LSB 优先）
 * @param sampleRateKhz channel.ini 里写的 SamplingFrequency（单位 kHz）
 * @param withNoiseChannel 额外塞一条全高（0xFF）的通道 —— 用于验多通道识别与噪声线判定
 */
inline Bytes makeAtkcc(const Bytes& rawBits, int sampleRateKhz = 2500,
                       bool withNoiseChannel = false) {
    std::string topIni = "SamplingFrequency=" + std::to_string(sampleRateKhz) + "\n";
    // 0/channel.ini：第 1 行通道组号，第 2 行总采样点数（**全局**计数，不是单通道的）
    std::string subIni = "0\n" + std::to_string(rawBits.size() * 8) + "\n";

    std::vector<ZipItem> items;
    items.push_back({"channel.ini", Bytes(topIni.begin(), topIni.end()), true});
    items.push_back({"0/channel.ini", Bytes(subIni.begin(), subIni.end()), true});
    items.push_back({"0/0-0.bin", rawBits, true});
    if (withNoiseChannel) {
        // 全高 = 一直空闲；真实噪声线是接近 50% 的混合字节，这里只验「多通道被识别到」
        items.push_back({"0/1-0.bin", Bytes(64, 0xFF), true});
    }
    return makeZip(items);
}

}  // namespace fx
