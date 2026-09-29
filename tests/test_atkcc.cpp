// test_atkcc.cpp — .atkcc 容器 + 采样率三级策略 + 端到端解码
//
// 端到端这条最有价值：合成位流 → BMC 波形 → ZIP → decodeChannel → 报文。
// 它一次串起了「容器分块 / 位序 / BMC 阈值 / 4B5B / PD 语义 / 统计口径」，
// 任何一环改了而没同步，这里都会红。

#include "test.h"

#include "container/atkcc.h"
#include "fixtures.h"
#include "packet.h"
#include "pd_frames.h"
#include "pipeline.h"

#include <cmath>

using namespace pdscope;

namespace {

CancelFn neverCancel() { return []() { return false; }; }

/** 确定性的伪随机比特流（不引入 <random> 的平台差异）。 */
std::vector<uint8_t> pseudoBits(size_t n, uint32_t seed = 0x2468ACE0u) {
    std::vector<uint8_t> bits;
    bits.reserve(n);
    uint32_t s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        bits.push_back(static_cast<uint8_t>((s >> 18) & 1));
    }
    return bits;
}

}  // namespace

TEST(atkcc_parse_sample_rate_units) {
    // 声明值单位是 kHz（SamplingFrequency=2500 ⇒ 2.5 MHz）
    SampleRateInfo a = parseSampleRate("SamplingFrequency=2500\n");
    CHECK_EQ(a.source, std::string("declared"));
    CHECK_NEAR(a.hz, 2500000.0, 1.0);

    // 带单位
    SampleRateInfo b = parseSampleRate("SamplingFrequency = 2500 kHz\n");
    CHECK_EQ(b.source, std::string("declared"));
    CHECK_NEAR(b.hz, 2500000.0, 1.0);

    SampleRateInfo c = parseSampleRate("sample_rate=2.5MHz\n");
    CHECK_EQ(c.source, std::string("declared"));
    CHECK_NEAR(c.hz, 2500000.0, 1.0);

    // 裸的大数按 Hz 理解（否则 2500000 会被乘成 2.5 GHz）
    SampleRateInfo d = parseSampleRate("SampleRate=2500000\n");
    CHECK_EQ(d.source, std::string("declared"));
    CHECK_NEAR(d.hz, 2500000.0, 1.0);

    // 不是采样率的键不能认
    CHECK_EQ(parseSampleRate("Channel=0\n").source, std::string("default"));
    CHECK_EQ(parseSampleRate("").source, std::string("default"));

    // 「数量级离谱」的判据是 **[10 kHz, 1 GHz] 区间**：
    //   · 裸数字按 kHz 理解 ⇒ `SampleRate=12` 就是 12 kHz，**落在区间内**，照样认
    //     （别按直觉把它改成 default —— 12 kHz 本身是合法采样率）；
    //   · 显式单位、或越出区间的才算离谱。
    CHECK_EQ(parseSampleRate("SampleRate=12\n").source, std::string("declared"));
    CHECK_NEAR(parseSampleRate("SampleRate=12\n").hz, 12000.0, 1.0);
    CHECK_EQ(parseSampleRate("SampleRate=12 Hz\n").source, std::string("default"));   // 12 Hz
    CHECK_EQ(parseSampleRate("SampleRate=5 kHz\n").source, std::string("default"));   // 5 kHz
    CHECK_EQ(parseSampleRate("SampleRate=9\n").source, std::string("default"));       // 9 kHz
    CHECK_EQ(parseSampleRate("SampleRate=0\n").source, std::string("default"));
    CHECK_EQ(parseSampleRate("SampleRate=999999999999\n").source, std::string("default")); // > 1 GHz
    CHECK_EQ(parseSampleRate("SampleRate=10\n").source, std::string("declared"));     // 10 kHz 边界内
    // 没声明的走兜底 2.5 MHz
    CHECK_NEAR(parseSampleRate("nothing here").hz, kDefaultSampleRate, 1.0);
}

TEST(atkcc_container_meta_and_chunks) {
    const Bytes raw = fx::packBitsLsb(pseudoBits(2048));
    const Bytes zip = fx::makeAtkcc(raw, /*sampleRateKhz=*/2500, /*withNoiseChannel=*/true);

    AtkccCapture cap = AtkccCapture::open(zip);
    const AtkccMeta& m = cap.meta();
    CHECK_EQ(m.sampleRateSource, std::string("declared"));
    CHECK_NEAR(m.sampleRate, 2500000.0, 1.0);
    CHECK_EQ(m.channelOrder.size(), 2u);
    CHECK(m.channelMap.count(0) == 1);
    CHECK(m.channelMap.count(1) == 1);
    CHECK_EQ(m.channelMap.at(0).chunks.size(), 1u);
    CHECK_EQ(m.channelMap.at(0).chunks[0].name, std::string("0/0-0.bin"));

    Bytes got;
    CHECK(cap.readChunk(0, 0, got));
    CHECK(got == raw);
    CHECK(!cap.readChunk(0, 5, got));      // 越界块

    Bytes noise;
    CHECK(cap.readChunk(1, 0, noise));
    CHECK_EQ(noise.size(), static_cast<size_t>(64));
    CHECK_EQ(noise[0], static_cast<uint8_t>(0xFF));

    // 全高的那条线：活动度为 0、空闲占比 1
    const ChannelActivity act = cap.scanActivity(1, 4);
    CHECK_EQ(act.channel, 1);
    CHECK_EQ(act.activity, static_cast<uint64_t>(0));
    CHECK_NEAR(act.idleRatio, 1.0, 1e-9);
}

TEST(atkcc_trim_trailing_zeros) {
    Bytes b = {0x12, 0x00, 0x00, 0x00};
    CHECK_EQ(AtkccCapture::trimTrailingZeros(b), static_cast<size_t>(1));
    Bytes all = {0x00, 0x00};
    CHECK_EQ(AtkccCapture::trimTrailingZeros(all), static_cast<size_t>(0));
    Bytes none = {0x01, 0x02};
    CHECK_EQ(AtkccCapture::trimTrailingZeros(none), static_cast<size_t>(2));
}

TEST(atkcc_rate_declared_wins_within_tolerance) {
    // 这条是防回归的核心：波形反推值稳定比声明值低约 4%（600 kHz 是标称值），
    // 差在 ±25% 以内就必须信**文件声明** —— 改成「波形说了算」会让所有时标偏移 4%。
    const std::vector<uint8_t> bits = pseudoBits(40000);
    const Bytes raw = fx::packBitsLsb(fxframe::bmcSamples(bits, /*width=*/4, 0, 0));

    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(raw, 2400));
    const RateResolution r = resolveSampleRate(cap, 0, neverCancel());
    CHECK(r.hasMeasured);
    CHECK_NEAR(r.measured, 2400000.0, 60000.0);        // 4 采样点/UI × 600 kHz
    CHECK_EQ(r.source, std::string("declared"));        // 声明与实测一致 ⇒ 用声明
    CHECK_NEAR(r.rate, 2400000.0, 1.0);
    CHECK(!r.hasNote);
}

TEST(atkcc_rate_measured_wins_when_declared_is_way_off) {
    const std::vector<uint8_t> bits = pseudoBits(40000, 0x13579BDFu);
    const Bytes raw = fx::packBitsLsb(fxframe::bmcSamples(bits, /*width=*/4, 0, 0));

    // 文件声明 1 MHz，波形节拍却是 2.4 MHz（差 140%）⇒ 必须按实测解码并说明原因
    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(raw, 1000));
    const RateResolution r = resolveSampleRate(cap, 0, neverCancel());
    CHECK_EQ(r.source, std::string("measured"));
    CHECK_NEAR(r.rate, 2400000.0, 60000.0);
    CHECK(r.hasNote);
}

TEST(atkcc_rate_falls_back_to_declared_when_waveform_is_flat) {
    // 通道全是空闲：波形认不出来 ⇒ 保持声明值，且不该报「实测」
    const Bytes raw(2048, 0xFF);
    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(raw, 2500));
    const RateResolution r = resolveSampleRate(cap, 0, neverCancel());
    CHECK(!r.hasMeasured);
    CHECK_EQ(r.source, std::string("declared"));
    CHECK_NEAR(r.rate, 2500000.0, 1.0);
}

TEST(atkcc_rate_override_beats_everything) {
    const Bytes raw(2048, 0xFF);
    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(raw, 2500));
    const RateResolution r = resolveSampleRate(cap, 0, neverCancel(), /*sampleRateOverride=*/1234567.0);
    CHECK_EQ(r.source, std::string("override"));
    CHECK_NEAR(r.rate, 1234567.0, 1e-6);
    CHECK(r.hasNote);
}

TEST(atkcc_end_to_end_decodes_a_data_message) {
    // 7 个数据对象 ⇒ 位流够长（>200 个游程），采样率自检也能真正跑起来
    std::vector<uint32_t> pdos;
    for (int i = 0; i < 7; ++i) pdos.push_back(0x0001912Cu + static_cast<uint32_t>(i));
    const uint16_t hdr = static_cast<uint16_t>((7 << 12) | (1 << 8) | (2 << 6) | 1);
    const std::vector<uint8_t> bits = fxframe::pdPacketBits(hdr, pdos);

    const Bytes samples = fx::packBitsLsb(fxframe::bmcSamples(bits, /*width=*/4));
    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(samples, 2400));

    ChannelDecodeOpts opts;
    opts.shouldStop = neverCancel();
    const DecodeOutcome out = decodeChannel(cap, 0, opts);

    CHECK_EQ(out.packets.size(), 1u);
    if (out.packets.empty()) return;
    const Packet& p = out.packets[0];
    CHECK_EQ(p.sop, std::string("SOP"));
    CHECK_EQ(p.msgType, std::string("Source_Cap"));   // 数据消息类型 1（PD 3.2 Table 6.5）
    CHECK_EQ(p.nObjects, 7);
    CHECK(p.eop);
    CHECK_EQ(static_cast<int>(p.crcOk), static_cast<int>(CrcState::Ok));
    CHECK_EQ(out.stats.packetCount, static_cast<uint64_t>(1));
    CHECK_EQ(out.stats.badCrc, static_cast<uint64_t>(0));
    CHECK_EQ(out.stats.sampleRateSource, std::string("declared"));
    CHECK(!p.synthetic);                       // 这份报文是从波形解出来的，不是铺出来的
    CHECK(out.stats.edges > 0);
}

TEST(atkcc_end_to_end_flat_channel_yields_zero_packets) {
    // 零报文是一条正经路径，不是错误 —— 统计要如实给出，不能靠「有没有报文」判断成败
    const Bytes raw(4096, 0xFF);
    AtkccCapture cap = AtkccCapture::open(fx::makeAtkcc(raw, 2400));

    ChannelDecodeOpts opts;
    opts.shouldStop = neverCancel();
    const DecodeOutcome out = decodeChannel(cap, 0, opts);
    CHECK_EQ(out.packets.size(), 0u);
    CHECK_EQ(out.stats.packetCount, static_cast<uint64_t>(0));
    CHECK(!out.cancelled);
}
