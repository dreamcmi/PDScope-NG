// test_pd.cpp — PD 协议层：CRC、位流解码、CRC 三态、位序
//
// 这里不碰 BMC（那属于容器/流水线），直接给 `PdDecoder::decode` 喂位流 ——
// 位流由 `pd_frames.h` 用**解码器自己的表**逆编出来，所以两边不会各错一套还互相打勾。

#include "test.h"

#include "container/powerz.h"
#include "fixtures.h"
#include "packet.h"
#include "pd/crc.h"
#include "pd/decoder.h"
#include "pd/symbols.h"
#include "pd_frames.h"

#include <zlib.h>

using namespace pdscope;

namespace {

/** 用位流造一个 BMC 原始包（edges 只用于时间轴，这里给单调递增的假值）。 */
BmcRawPacket wrapBits(const std::vector<uint8_t>& bits, uint64_t startSample = 0) {
    BmcRawPacket raw;
    raw.bits = bits;
    uint64_t s = startSample;
    for (size_t i = 1; i < bits.size(); ++i) {
        if (bits[i] != bits[i - 1]) raw.edges.push_back(s);
        s += 4;
    }
    raw.startSample = startSample;
    raw.endSample = s;
    raw.bitrate = 600000;
    return raw;
}

/** PD 报文头：revCode(bit7…6) │ type(bit4…0，扩展时 bit15 置位) │ … */
uint16_t pdHeader(int revCode, int type, int nObjects, int msgId, int powerRole = 1,
                  int dataRole = 0) {
    return static_cast<uint16_t>(((nObjects & 7) << 12) | ((msgId & 7) << 9)
                                 | ((powerRole & 1) << 8) | ((dataRole & 1) << 5)
                                 | ((revCode & 3) << 6) | (type & 0x1F));
}

}  // namespace

TEST(pd_crc32_matches_zlib) {
    // 交叉核对：我们的 CRC 实现必须与随包内置的 zlib crc32 逐个字节一致。
    // 单看「标准向量 123456789 → CBF43926」只能证明算法对，证明不了调用点没写错。
    uint32_t s = 0xDEADBEEFu;
    std::vector<Bytes> cases;
    cases.push_back(Bytes{});
    cases.push_back(Bytes{0x00});
    cases.push_back(Bytes{0x81, 0x11});
    {
        Bytes b;
        for (int i = 0; i < 300; ++i) {
            s = s * 1103515245u + 12345u;
            b.push_back(static_cast<uint8_t>((s >> 16) & 0xff));
        }
        cases.push_back(b);
    }
    for (const Bytes& c : cases) {
        const uint32_t mine = pd::crc32(c.data(), c.size());
        const uint32_t z = static_cast<uint32_t>(
            ::crc32(0L, reinterpret_cast<const Bytef*>(c.data()), static_cast<uInt>(c.size())));
        CHECK_EQ(mine, z);
    }
    const std::string vec = "123456789";
    CHECK_EQ(pd::crc32(reinterpret_cast<const uint8_t*>(vec.data()), vec.size()), 0xCBF43926u);
}

TEST(pd_decodes_goodcrc_from_bitstream) {
    const uint16_t hdr = pdHeader(/*revCode=*/2, /*type=*/1, /*nObjects=*/0, /*msgId=*/3);
    BmcRawPacket raw = wrapBits(fxframe::pdPacketBits(hdr, {}));

    pd::PdDecoder dec(2500000.0);
    auto p = dec.decode(raw, 0);
    CHECK(p.has_value());
    if (!p) return;

    CHECK_EQ(p->sop, std::string("SOP"));
    CHECK_EQ(p->msgType, std::string("GoodCRC"));
    CHECK_EQ(p->msgKind, std::string("control"));
    CHECK_EQ(p->nObjects, 0);
    CHECK_EQ(p->msgId, 3);
    CHECK(p->hasMsgId);
    CHECK_EQ(p->header, static_cast<int>(hdr));
    CHECK(p->eop);                                    // 位流里带了 EOP 符号
    CHECK_EQ(static_cast<int>(p->crcOk), static_cast<int>(CrcState::Ok));
    CHECK(p->crcRecorded);
    CHECK_EQ(p->crc, p->crcCalc);
    CHECK(p->link == "port");
}

TEST(pd_decodes_source_capabilities_with_pdo) {
    // 一个固定 PDO：电压 5V、电流 3A（PDO 的位域见 pd/pdo.cpp）
    const uint32_t pdo = 0x0001912Cu;   // 5V/3A Fixed Supply（位域组合的代表值）
    const uint16_t hdr = pdHeader(2, 1, 1, 0);
    BmcRawPacket raw = wrapBits(fxframe::pdPacketBits(hdr, {pdo}));

    pd::PdDecoder dec(2500000.0);
    auto p = dec.decode(raw, 0);
    CHECK(p.has_value());
    if (!p) return;

    CHECK_EQ(p->msgType, std::string("Source_Cap"));   // 数据消息类型 1（PD 3.2 Table 6.5）
    CHECK_EQ(p->msgKind, std::string("data"));
    CHECK_EQ(p->nObjects, 1);
    CHECK_EQ(p->dataWords.size(), 1u);
    if (!p->dataWords.empty()) CHECK_EQ(p->dataWords[0], pdo);
    CHECK(p->msgTypeRaw == 1);
    CHECK_EQ(static_cast<int>(p->crcOk), static_cast<int>(CrcState::Ok));

    // 详情分组必须以 `Object` 哨兵开头（UI 侧靠它分节）
    bool sawObject = false;
    for (const DetailItem& d : p->details) {
        if (d.key == "Object") { sawObject = true; break; }
    }
    CHECK(sawObject);
}

TEST(pd_bad_crc_is_reported_as_bad_not_ok) {
    std::vector<uint8_t> bits = fxframe::pdPacketBits(pdHeader(2, 1, 0, 0), {});

    // 把 CRC 里的**一个符号**换成另一个合法符号（保证改动只落在 CRC 上、
    // 且换出来的仍是合法 4B5B 码 —— 免得变成「非法编码」而走成另一条分支）。
    // 符号序列：SOP(4) + 报文头(4) + CRC(8)，所以第 11 个符号是 CRC 的第 4 个。
    const size_t symIdx = 8 + 3;
    const size_t base = symIdx * 5;
    CHECK(bits.size() >= base + 5);
    uint8_t code = 0;
    for (int i = 0; i < 5; ++i) {
        if (bits[base + static_cast<size_t>(i)]) code |= static_cast<uint8_t>(1u << i);
    }
    const uint8_t value = pd::DEC4B5B[code];
    CHECK(value <= 0x0F);                                  // 4B5B 之前的都是数据符号
    const uint8_t newValue = (value == 0) ? 1 : static_cast<uint8_t>(value - 1);
    const uint8_t newCode = fxframe::codeOf(newValue);
    for (int i = 0; i < 5; ++i) {
        bits[base + static_cast<size_t>(i)] = static_cast<uint8_t>((newCode >> i) & 1);
    }

    BmcRawPacket raw = wrapBits(bits);
    pd::PdDecoder dec(2500000.0);
    auto p = dec.decode(raw, 0);
    CHECK(p.has_value());
    if (!p) return;
    // 报文头没动，所以类型还是 GoodCRC，只有 CRC 对不上
    CHECK_EQ(p->msgType, std::string("GoodCRC"));
    CHECK_EQ(static_cast<int>(p->crcOk), static_cast<int>(CrcState::Bad));
    CHECK_NE(p->crc, p->crcCalc);
    bool hasCrcWarn = false;
    for (const PacketWarning& w : p->warnings) {
        if (w.shortMsg == "CRC") hasCrcWarn = true;
    }
    CHECK(hasCrcWarn);
}

TEST(pd_decode_wire_marks_crc_unrecorded) {
    // POWER-Z 的 pd_table 不存 CRC：必须报「未记录」，绝不能因为重算对上了就写 OK
    const uint16_t hdr = pdHeader(2, 1, 0, 1);
    const uint8_t wire[2] = {static_cast<uint8_t>(hdr & 0xff),
                             static_cast<uint8_t>(hdr >> 8)};

    pd::PdDecoder dec(1000.0);
    auto p = dec.decodeWire(wire, 2, "SOP", /*timeMs=*/12.5, /*channel=*/0,
                            /*crcRecorded=*/false, /*sopByte=*/0, /*powerz=*/true);
    CHECK(p.has_value());
    if (!p) return;

    CHECK_EQ(p->msgType, std::string("GoodCRC"));
    CHECK_EQ(static_cast<int>(p->crcOk), static_cast<int>(CrcState::Unrecorded));
    CHECK(!p->crcRecorded);
    CHECK(p->powerz);
    CHECK(p->synthetic);
    CHECK_EQ(p->startSample, static_cast<uint64_t>(std::llround(12.5)));   // 1 采样点 = 1 ms
    CHECK_NEAR(p->timeMs, 12.5, 1e-9);
}

TEST(pd_symbol_table_is_invertible) {
    // 4B5B 的逆表如果有重复/缺口，造样本与解码就会同时错，所以把这条单独钉住
    std::vector<int> seen(0x17, -1);
    int dataCount = 0, kCount = 0;
    for (int c = 0; c < 32; ++c) {
        const uint8_t v = pd::DEC4B5B[c];
        if (v > 0x16) continue;                 // 0x10 = 非法编码
        if (v == pd::SYM_ERR) continue;
        CHECK_EQ(seen[v], -1);                  // 同一符号值只能有一个 5bit 码
        seen[v] = c;
        if (v >= pd::SYM_SYNC1 && v <= pd::SYM_EOP) ++kCount;
        else ++dataCount;
    }
    CHECK_EQ(dataCount, 16);                    // 16 个数据符号
    CHECK_EQ(kCount, 6);                        // 6 个 K-code（SYNC1..EOP）
}
