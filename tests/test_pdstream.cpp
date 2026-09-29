// test_pdstream.cpp — .pdStream 容器：结构自证、往返、无 ADC 波形
//
// `.pdStream` 与 `.sqlite` 的 pd_table 逐字节同构，唯一的实质差别是**没有 ADC 波形**。
// 所以这里要断言的不是「能不能解出报文」（那由 test_pd.cpp 覆盖），
// 而是「时间轴由报文撑起来」+「模拟量确实是空的」这两条语义。

#include "test.h"

#include "container/powerz.h"
#include "container/pdstream.h"
#include "fixtures.h"
#include "packet.h"
#include "pd_frames.h"

#include <cmath>

using namespace pdscope;

namespace {

Bytes wireOf(uint16_t header) {
    return Bytes{static_cast<uint8_t>(header & 0xff), static_cast<uint8_t>(header >> 8)};
}

std::vector<PdStreamRecord> sampleRows() {
    std::vector<PdStreamRecord> rows;
    for (int i = 0; i < 4; ++i) {
        PdStreamRecord r;
        r.time = 0.25 * (i + 1);          // 秒
        r.vbus = 5.0 + i;
        r.ibus = 1.0 + 0.5 * i;
        // 报文头：revCode=2、type=1、nObjects=0 ⇒ GoodCRC，msgId 依次变化
        const uint16_t hdr = static_cast<uint16_t>((1 << 8) | (2 << 6) | 1 | ((i & 7) << 9));
        r.raw = fxframe::pdBlob(static_cast<uint32_t>(250 * (i + 1)), 0, wireOf(hdr));
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace

TEST(pdstream_sniff_rejects_junk) {
    CHECK(!sniffPdStream(Bytes(200, 0x00)));
    CHECK(!sniffPdStream(Bytes{}));
    Bytes junk(200, 0xAB);
    CHECK(!sniffPdStream(junk));
}

TEST(pdstream_write_read_roundtrip) {
    const std::vector<PdStreamRecord> rows = sampleRows();
    const Bytes stream = writePdStream(rows);

    CHECK(sniffPdStream(stream));

    const PdStreamParsed parsed = readPdStream(stream);
    CHECK_EQ(parsed.records.size(), rows.size());
    CHECK_EQ(parsed.bytesConsumed, static_cast<uint64_t>(stream.size()));
    // payloadBytes 累计的是每条记录的 raw 长度（`pdBlob` 含 marker/ts/sop，
    // 比裸 wire 长 5 字节），所以按真实 raw 求和，别拿 wireOf() 的长度硬套。
    uint64_t expectPayload = 0;
    for (const PdStreamRecord& r : rows) expectPayload += r.raw.size();
    CHECK_EQ(parsed.payloadBytes, expectPayload);
    for (size_t i = 0; i < rows.size(); ++i) {
        CHECK_NEAR(parsed.records[i].time, rows[i].time, 1e-12);
        CHECK_NEAR(parsed.records[i].vbus, rows[i].vbus, 1e-12);
        CHECK_NEAR(parsed.records[i].ibus, rows[i].ibus, 1e-12);
        CHECK(parsed.records[i].raw == rows[i].raw);
    }
}

TEST(pdstream_write_sorts_by_time) {
    std::vector<PdStreamRecord> rows = sampleRows();
    std::swap(rows[0], rows[3]);          // 故意打乱
    const Bytes stream = writePdStream(rows);
    const PdStreamParsed parsed = readPdStream(stream);
    CHECK_EQ(parsed.records.size(), 4u);
    for (size_t i = 1; i < parsed.records.size(); ++i) {
        CHECK(parsed.records[i].time >= parsed.records[i - 1].time);
    }
}

TEST(pdstream_rejects_truncated_record) {
    Bytes stream = writePdStream(sampleRows());
    // 声称的 payload 长度比文件剩余空间还大 —— 报错必须指到偏移，别静默读出脏数据
    stream[3] = 0x7F;
    CHECK_THROWS(readPdStream(stream));
    CHECK(!sniffPdStream(stream));
}

TEST(pdstream_rejects_time_going_backwards) {
    std::vector<PdStreamRecord> rows = sampleRows();
    const Bytes stream = writePdStream(rows);
    // 直接把第二条记录的 Time 改成比第一条小（Time 在 payload 之后 8 字节）
    const uint32_t len0 = static_cast<uint32_t>(rows[0].raw.size());
    const uint64_t rec1 = 4 + len0 + 24;
    const uint64_t at = rec1 + 4 + rows[1].raw.size();
    Bytes patched = stream;
    const double bad = 0.0;
    uint64_t bits;
    std::memcpy(&bits, &bad, 8);
    for (int i = 0; i < 8; ++i) {
        patched[at + static_cast<uint64_t>(i)] = static_cast<uint8_t>((bits >> (8 * (7 - i))) & 0xFF);
    }
    CHECK_THROWS(readPdStream(patched));
}

TEST(pdstream_decodes_packets_without_adc) {
    const Bytes stream = writePdStream(sampleRows());
    auto cap = openPdStream(stream);
    CHECK(cap != nullptr);
    if (!cap) return;

    const PowerzMeta& m = cap->meta();
    CHECK_EQ(m.kind, std::string("pd"));
    CHECK_EQ(m.protocol, std::string("USB PD"));
    CHECK_EQ(m.title, std::string("POWER-Z · .pdStream"));
    CHECK(!m.isSqlite);
    CHECK_EQ(m.pageSize, 0);
    // 没有 ADC 波形
    CHECK(m.bus.empty());
    CHECK(m.busLabelA.empty());
    CHECK(m.busLabelB.empty());
    CHECK(m.sampleRateNote.find("没有 ADC 波形") != std::string::npos);
    // 时间轴由末条报文撑起来：1.00 s
    CHECK_NEAR(m.durationSec, 1.0, 1e-9);

    const PowerzResult res = cap->decode(nullptr, nullptr);
    CHECK_EQ(res.packets.size(), 4u);
    CHECK_EQ(res.stats.packetCount, static_cast<uint64_t>(4));
    for (const Packet& p : res.packets) {
        CHECK_EQ(p.msgType, std::string("GoodCRC"));
        CHECK(p.powerz);
        // pd_table / .pdStream 都不存 CRC ⇒ 只能报「未记录」
        CHECK_EQ(static_cast<int>(p.crcOk), static_cast<int>(CrcState::Unrecorded));
        CHECK(!p.crcRecorded);
        CHECK_NEAR(p.ibus, 0.0, 1e-12);       // 没有 ADC ⇒ VBUS/IBUS 保持 0，不能凭空写
        CHECK_NEAR(p.vbus, 0.0, 1e-12);
    }
    // 时间戳按「1 采样点 = 1 ms」映射
    CHECK_NEAR(res.packets[0].timeMs, 250.0, 1e-9);
    CHECK_EQ(res.packets[0].startSample, static_cast<uint64_t>(250));
}
