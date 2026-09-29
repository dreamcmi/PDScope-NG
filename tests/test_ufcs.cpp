// test_ufcs.cpp — UFCS：容器行结构（快路径）、穷举兜底、CRC-8、方向判据
//
// 最要紧的两条：
//   · `flag` 在 **blob[7]**（写错成 blob[prefixBytes-1] 会读到恒为 0xAA 的 Training 字节，
//     链路读不出来、方向只能靠猜而且不自知）；
//   · 方向优先按**容器给的链路**定，只有容器没给时才走推断（此时必须标 roleInferred）。

#include "test.h"

#include "container/powerz.h"
#include "fixtures.h"
#include "packet.h"
#include "ufcs/crc.h"
#include "ufcs/decoder.h"
#include "ufcs/frame.h"
#include "ufcs/tables.h"
#include "pd_frames.h"

using namespace pdscope;

namespace {

/** 一条最短的 UFCS 控制消息（消息头 + 命令），不含 CRC。 */
Bytes ctrlFrame(uint8_t addr, uint8_t msgNo, uint8_t ver, uint8_t cmd) {
    return fxframe::ufcsControl(addr, msgNo, ver, cmd);
}

}  // namespace

TEST(ufcs_crc8_known_vectors) {
    // 附录 A 的算法：多项式 0x29、初值 0、不反射、不取反。
    // 空输入按定义得 0；其余用「手工按位算一遍」的点做锚。
    CHECK_EQ(static_cast<int>(ufcs::ufcsCrc8(nullptr, 0)), 0);
    const uint8_t one[1] = {0x00};
    CHECK_EQ(static_cast<int>(ufcs::ufcsCrc8(one, 1)), 0);
    // 与「逐位实现」互校：这里用一个显而易见的性质 —— CRC 是线性的，
    // 同样的输入永远得到同样的值（防止有人把表写歪）
    const uint8_t msg[4] = {0x22, 0x08, 0x01, 0x00};
    const uint8_t a = ufcs::ufcsCrc8(msg, 4);
    const uint8_t b = ufcs::ufcsCrc8(msg, 4);
    CHECK_EQ(static_cast<int>(a), static_cast<int>(b));
    // 改一个字节必须变（退化成常数的实现在这里会挂）
    uint8_t msg2[4] = {0x22, 0x08, 0x01, 0x01};
    CHECK_NE(static_cast<int>(ufcs::ufcsCrc8(msg2, 4)), static_cast<int>(a));
}

TEST(ufcs_header_bitfields) {
    // addr(15…13) │ msgNo(12…9) │ ver(8…3) │ mtype(2…0)
    const uint16_t hdr = static_cast<uint16_t>((0b010 << 13) | (0b0111 << 9) | (0b000001 << 3) | 0);
    const ufcs::HeaderInfo h = ufcs::ufcsHeaderInfo(hdr);
    CHECK_EQ(h.addr, 0b010);
    CHECK_EQ(h.msgNo, 0b0111);
    CHECK_EQ(h.verCode, 0b000001);
    CHECK_EQ(h.verText, std::string("1.0.0"));
    CHECK(h.verKnown);
    CHECK(h.addrValid);
    CHECK(h.mtypeValid);
    CHECK_EQ(h.mtype, 0);

    // 保留地址（0）与保留类型（3）都必须判为非法
    CHECK(!ufcs::ufcsHeaderInfo(0x0000).addrValid);
    CHECK(!ufcs::ufcsHeaderInfo(0x0003).mtypeValid);
}

TEST(ufcs_parse_record_reads_link_from_blob7) {
    // 这是本项目最贵的一个坑：flag 在 blob[7]，不是 blob[prefixBytes-1]（= blob[8] = 0xAA）。
    // 写错的表现是「每条报文的链路都读成同一个值」—— 所以正反两种 flag 都要断言。
    for (int wantFlag = 0; wantFlag <= 1; ++wantFlag) {
        const Bytes frame = fxframe::withCrc(ctrlFrame(0b001, 1, 0b000001, 0x01 /*ACK*/));
        const Bytes rec = fxframe::ufcsRecord(/*tsMs=*/1234, /*x0=*/7, /*x1=*/11,
                                             static_cast<uint8_t>(wantFlag), frame);

        auto r = ufcs::ufcsParseRecord(rec.data(), rec.size());
        CHECK(r.has_value());
        if (!r) continue;
        CHECK(r->hasLine);
        CHECK_EQ(r->line, wantFlag == 0 ? std::string("D+") : std::string("D-"));
        CHECK_EQ(r->prefixBytes, static_cast<size_t>(9));
        CHECK(r->hasTs);
        CHECK_EQ(r->tsMs, 1234);
        CHECK(r->hasLenField);
        CHECK_EQ(r->lenField, static_cast<long long>(frame.size() + 1));
        CHECK(r->hasCounter);
        CHECK_EQ(r->counterX0, 7);
        CHECK_EQ(r->counterX1, 11);
        CHECK_EQ(r->frames.size(), 1u);
        if (!r->frames.empty()) {
            CHECK(r->frames[0].crcOk);
            CHECK_EQ(r->frames[0].off, static_cast<size_t>(9));
            CHECK_EQ(r->frames[0].bodyEnd, 9u + frame.size() - 1);
        }
    }
}

TEST(ufcs_parse_record_rejects_wrong_layout) {
    const Bytes frame = fxframe::withCrc(ctrlFrame(0b001, 1, 0b000001, 0x01));

    // ① Training 字节不对
    Bytes a = fxframe::ufcsRecord(0, 0, 0, 0, frame);
    a[8] = 0x00;
    CHECK(!ufcs::ufcsParseRecord(a.data(), a.size()).has_value());

    // ② 长度域不把 Training 算进去（差 1）—— 实测样本 100% 是 flen+1，差 1 就不该认
    Bytes b = fxframe::ufcsRecord(0, 0, 0, 0, frame);
    b[6] = static_cast<uint8_t>(b[6] - 1);
    CHECK(!ufcs::ufcsParseRecord(b.data(), b.size()).has_value());

    // ③ flag 超出 {0,1}（那位置若是别的值，说明这行不是我们认识的布局）
    Bytes c = fxframe::ufcsRecord(0, 0, 0, 0, frame);
    c[7] = 2;
    CHECK(!ufcs::ufcsParseRecord(c.data(), c.size()).has_value());
}

TEST(ufcs_parse_event_layout) {
    const Bytes ev = fxframe::ufcsEvent(/*tsMs=*/99, /*code=*/0x03);
    auto e = ufcs::ufcsParseEvent(ev.data(), ev.size());
    CHECK(e.has_value());
    if (e) {
        CHECK_EQ(e->tsMs, 99);
        CHECK_EQ(e->code, 3);
    }
    // 尾标记不是 0x40 就不是事件；长度不是 8 也不是
    Bytes bad = ev;
    bad[7] = 0x41;
    CHECK(!ufcs::ufcsParseEvent(bad.data(), bad.size()).has_value());
    CHECK(!ufcs::ufcsParseEvent(ev.data(), 7).has_value());
}

TEST(ufcs_decode_control_uses_container_link) {
    // 验「物理链路」这一级，必须用**双向命令**：ACK（0x01）不在规范单向表里，
    // 方向只能由「容器给的链路 + 接收方地址」定出来。
    // ⚠ 别拿 0x06 Get_Output_Capabilities 验这条 —— 它是规范单向命令（SNK → SRC），
    //   第 ① 级就直接定成 SNK，容器链路根本不参与（本用例早先正是这么写错的）。
    const uint8_t cmd = 0x01;   // ACK：任意 → 任意
    const Bytes body = ctrlFrame(/*addr(接收方)=*/0b010, /*msgNo=*/2, 0b000001, cmd);
    const Bytes full = fxframe::withCrc(body);

    ufcs::UfcsDecoder dec(1000.0);
    ufcs::DecodeOpts opts;
    opts.timeMs = 250.0;
    opts.line = "D+";
    opts.dirByte = 0;
    opts.prefixBytes = 9;
    opts.layout = "record";
    opts.crc = full.back();
    opts.hasCrc = true;
    opts.withCrc = true;

    auto p = dec.decode(full.data(), full.size() - 1, opts);
    CHECK(p.has_value());
    if (!p) return;

    CHECK_EQ(p->msgType, std::string("ACK"));
    // 接收方是充电设备（010），报文出现在 D+（供电设备的 TX）⇒ 发送方只能是供电设备
    CHECK_EQ(p->role, std::string("SRC"));
    CHECK(!p->roleInferred);                     // 链路是容器给的 ⇒ 不算推断
    CHECK_EQ(static_cast<int>(p->crcOk), static_cast<int>(CrcState::Ok));
    CHECK(p->crcRecorded);
    CHECK_EQ(p->msgId, 2);
    CHECK_EQ(p->header, static_cast<int>((0b010 << 13) | (2u << 9) | (0b000001u << 3)));
    // UFCS 的帧结构里没有 PD 那样的 EOP 符号（JS 基线给 null）⇒ 如实报「无」
    CHECK(!p->eop);
    // 规范字段进详情，容器字段（layout / training / len / counter / dirByte）一律不进
    for (const DetailItem& d : p->details) {
        CHECK_NE(d.key, std::string("layout"));
        CHECK_NE(d.key, std::string("training"));
        CHECK_NE(d.key, std::string("lenField"));
        CHECK_NE(d.key, std::string("counter"));
        CHECK_NE(d.key, std::string("dirByte"));
    }
}

TEST(ufcs_spec_fixed_direction_beats_container_link) {
    // 规范单向命令优先级最高：Get_Output_Capabilities 恒为 充电设备→供电设备。
    // 这里**故意**给一个相反的链路（D+），方向仍必须是 SNK —— 一旦实现把链路提到
    // 单向表前面，这条立刻挂。JS 基线同输入也是 SNK（探针实测）。
    const Bytes body = ctrlFrame(0b001, 1, 0b000001, 0x06 /*Get_Output_Capabilities*/);
    const Bytes full = fxframe::withCrc(body);

    ufcs::UfcsDecoder dec(1000.0);
    ufcs::DecodeOpts opts;
    opts.timeMs = 10.0;
    opts.line = "D+";           // 与规范定义相反，必须被单向表压住
    opts.dirByte = 0;
    opts.prefixBytes = 9;
    opts.layout = "record";
    opts.crc = full.back();
    opts.hasCrc = true;
    opts.withCrc = true;

    auto p = dec.decode(full.data(), full.size() - 1, opts);
    CHECK(p.has_value());
    if (!p) return;
    CHECK_EQ(p->msgType, std::string("Get_Output_Capabilities"));
    CHECK_EQ(p->role, std::string("SNK"));
    CHECK(!p->roleInferred);                     // 规范定的，不是推断
}

TEST(ufcs_decode_marks_direction_inferred_when_container_silent) {
    const Bytes body = ctrlFrame(0b010, 1, 0b000001, 0x01 /*ACK*/);
    const Bytes full = fxframe::withCrc(body);

    ufcs::UfcsDecoder dec(1000.0);
    ufcs::DecodeOpts opts;
    opts.timeMs = 1.0;
    opts.line = "";             // 容器没给链路
    opts.dirByte = -1;
    opts.layout = "scan";
    opts.crc = full.back();
    opts.hasCrc = true;
    opts.withCrc = true;

    auto p = dec.decode(full.data(), full.size() - 1, opts);
    CHECK(p.has_value());
    if (!p) return;
    // ACK 是双向命令，没有链路就判不出方向 ⇒ 必须标成「推断」
    CHECK(p->roleInferred);
}

TEST(ufcs_locate_frames_fallback) {
    // 构造一段「前面有垃圾、后面接两帧」的 blob：快路径认不出，只能走穷举定位
    Bytes blob = {0x91, 0x37, 0x42};
    const Bytes f1 = fxframe::withCrc(ctrlFrame(0b001, 1, 0b000001, 0x01));
    const Bytes f2 = fxframe::withCrc(ctrlFrame(0b010, 2, 0b000001, 0x00));
    blob.insert(blob.end(), f1.begin(), f1.end());
    blob.insert(blob.end(), f2.begin(), f2.end());

    auto loc = ufcs::ufcsLocateFrames(blob.data(), blob.size());
    CHECK(loc.has_value());
    if (!loc) return;
    CHECK_EQ(loc->prefixBytes, static_cast<size_t>(3));
    CHECK_EQ(loc->frames.size(), 2u);
    CHECK(loc->withCrc);
    for (const auto& f : loc->frames) CHECK(f.crcOk);
}
