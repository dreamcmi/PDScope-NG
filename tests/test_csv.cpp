// test_csv.cpp — CSV 与数字格式化：这是**三个出口共用**的那一份实现，
// 所以这里订的是契约而不是实现细节：列名、转义、行尾、BOM、CRC 三态怎么写。
//
// `toFixedStr` 那一组用例把进位方向钉死（含负数与「远离零」）—— 界面、CSV、CLI
// 三个出口共用这一份实现，数字必须逐字符一致，否则同一份抓包换个出口就对不上。

#include "test.h"

#include "csv.h"
#include "fixtures.h"
#include "packet.h"
#include "util.h"

#include <cmath>

using namespace pdscope;

namespace {

Packet mkPacket() {
    Packet p;
    p.index = 7;
    p.sop = "SOP";
    p.msgType = "Source_Capabilities";
    p.hasMsgId = true;
    p.msgId = 2;
    p.role = "SRC";
    p.nObjects = 3;
    p.dataLen = 11;
    p.timeMs = 1234.5678;
    p.vbus = 5.0123;
    p.ibus = -0.0625;             // 负值，验「远离零」的取整方向
    p.dataHex = "2C 91 01 00";
    p.crcOk = CrcState::Ok;
    p.summary = "含\"引号\"与,逗号";
    return p;
}

/** CSV 一行的单元格数（按我们自己的转义规则数 —— 引号内不切分）。 */
size_t countCells(const std::string& line) {
    size_t n = 1;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') { ++i; continue; }
            inQuotes = !inQuotes;
        } else if (line[i] == ',' && !inQuotes) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(csv_to_fixed_semantics) {
    // 实测口径（Node 的 Number.prototype.toFixed）：先取绝对值，
    // 按第 digits+1 位十进制数字 ≥5 就进位 —— 包括精确的 .5。
    CHECK_EQ(toFixedStr(0.0625, 3), std::string("0.063"));
    CHECK_EQ(toFixedStr(-0.0625, 3), std::string("-0.063"));
    CHECK_EQ(toFixedStr(1.005, 2), std::string("1.00"));     // 二进制里小于 1.005，够不着 .5
    CHECK_EQ(toFixedStr(0.0, 3), std::string("0.000"));
    CHECK_EQ(toFixedStr(-0.0, 3), std::string("0.000"));     // -0 不该出现负号
    CHECK_EQ(toFixedStr(1.5, 0), std::string("2"));
    CHECK_EQ(toFixedStr(2.5, 0), std::string("3"));
    CHECK_EQ(toFixedStr(-2.5, 0), std::string("-3"));
    CHECK_EQ(toFixedStr(1.2345, 3), std::string("1.234"));   // 二进制里实为 1.23449999…，够不着 .5
    CHECK_EQ(toFixedStr(9.9995, 3), std::string("9.999"));   // 同理实为 9.99949999…
    // 上面两条是**照着 Node 实测**钉的，别按数学预期改成 1.235 / 10.000：
    // 十进制短写 1.2345 落到 double 上并不等于 1.2345（exact=1.2344999999999999307）。
    // 二次舍入陷阱：展开到 3 位时 0.014999 会被先舍成 0.015 从而误进位
    CHECK_EQ(toFixedStr(0.014999, 2), std::string("0.01"));
    CHECK_EQ(toFixedStr(5.0, 4), std::string("5.0000"));
}

TEST(csv_clock_format) {
    CHECK_EQ(csvClock(0.0), std::string("00:00:00.000"));
    CHECK_EQ(csvClock(500.0), std::string("00:00:00.500"));
    CHECK_EQ(csvClock(3661123.0), std::string("01:01:01.123"));
    CHECK_EQ(csvClock(3600000.0), std::string("01:00:00.000"));
    // 小时不截断（长抓包）
    CHECK_EQ(csvClock(39600000.0), std::string("11:00:00.000"));
}

TEST(csv_heads_differ_only_in_column_six) {
    const std::vector<std::string> pd = csvHead("USB PD");
    const std::vector<std::string> ufcs = csvHead("UFCS");
    CHECK_EQ(pd.size(), ufcs.size());
    CHECK_EQ(pd.size(), static_cast<size_t>(13));
    CHECK_EQ(pd[5], std::string("Objects"));
    CHECK_EQ(ufcs[5], std::string("Bytes"));
    for (size_t i = 0; i < pd.size(); ++i) {
        if (i == 5) continue;
        CHECK_EQ(pd[i], ufcs[i]);
    }
    CHECK_EQ(pd[0], std::string("#"));
    CHECK_EQ(pd[12], std::string("Note"));
}

TEST(csv_escaping_and_layout) {
    const Packet p = mkPacket();
    const std::vector<const Packet*> rows{&p};

    const std::string csv = csvText(rows, "USB PD", /*bom=*/false);
    // 表头 + 1 行数据，中间用 CRLF 分隔，**结尾不带换行**
    const size_t firstEol = csv.find("\r\n");
    CHECK(firstEol != std::string::npos);
    CHECK(csv.find("\r\n", firstEol + 2) == std::string::npos);
    CHECK(csv.back() != '\n');

    const std::string head = csv.substr(0, firstEol);
    CHECK_EQ(countCells(head), static_cast<size_t>(13));
    CHECK(head[0] == '"');                                  // 每个字段都带引号
    CHECK(head.find("\"#\",\"SOP\"") == 0);

    // 内部引号翻倍（RFC 4180）
    CHECK(csv.find("\"含\"\"引号\"\"与,逗号\"") != std::string::npos);
    const std::string line = csv.substr(firstEol + 2);
    CHECK_EQ(countCells(line), static_cast<size_t>(13));

    // CRC 通过写 OK
    CHECK(line.find("\"OK\"") != std::string::npos);
}

TEST(csv_crc_unrecorded_is_blank_not_ok) {
    // 这条是「不许替分析仪的数据背书」的落点：未记录必须留空
    Packet p = mkPacket();
    p.crcOk = CrcState::Unrecorded;
    const std::vector<const Packet*> rows{&p};
    const std::string csv = csvText(rows, "USB PD", false);
    CHECK(csv.find("\"OK\"") == std::string::npos);
    CHECK(csv.find("\"BAD\"") == std::string::npos);

    p.crcOk = CrcState::Bad;
    const std::string csv2 = csvText(std::vector<const Packet*>{&p}, "USB PD", false);
    CHECK(csv2.find("\"BAD\"") != std::string::npos);
}

TEST(csv_bom_prefix_is_exactly_three_bytes) {
    const Packet p = mkPacket();
    const std::string withBom = csvText(std::vector<const Packet*>{&p}, "USB PD", true);
    CHECK_EQ(withBom.size() >= 3, true);
    CHECK_EQ(static_cast<int>(static_cast<uint8_t>(withBom[0])), 0xEF);
    CHECK_EQ(static_cast<int>(static_cast<uint8_t>(withBom[1])), 0xBB);
    CHECK_EQ(static_cast<int>(static_cast<uint8_t>(withBom[2])), 0xBF);
    const std::string noBom = csvText(std::vector<const Packet*>{&p}, "USB PD", false);
    CHECK_EQ(withBom.size(), noBom.size() + 3);
    CHECK_EQ(noBom.compare(0, 3, "\"#\""), 0);
}

TEST(csv_ufcs_sixth_column_is_bytes) {
    Packet p = mkPacket();
    p.dataLen = 11;
    const std::string ufcs = csvText(std::vector<const Packet*>{&p}, "UFCS", false);
    CHECK(ufcs.find("\"Bytes\"") != std::string::npos);
    CHECK(ufcs.find("\"Objects\"") == std::string::npos);
    // 第 6 个单元格必须是 dataLen（=11），不是 nObjects（=3）
    const std::string line = ufcs.substr(ufcs.find("\r\n") + 2);
    size_t pos = 0;
    for (int i = 0; i < 5; ++i) pos = line.find("\",\"", pos) + 3;
    CHECK_EQ(line.substr(pos, 2), std::string("11"));
}

TEST(csv_file_name_rules) {
    CHECK_EQ(csvBase("抓包.atkcc"), std::string("抓包"));
    CHECK_EQ(csvBase("a.SQLITE"), std::string("a"));        // 大小写不敏感
    CHECK_EQ(csvBase("b.db"), std::string("b"));
    CHECK_EQ(csvBase("c.pdstream"), std::string("c"));      // .pdStream 也要去掉
    CHECK_EQ(csvBase("d.pdStream"), std::string("d"));
    CHECK_EQ(csvBase("noext"), std::string("noext"));
    CHECK_EQ(csvBase(""), std::string("pdscope"));

    CHECK_EQ(csvFileName("抓包.atkcc", 0), std::string("抓包-ch0.csv"));
    CHECK_EQ(csvFileName("抓包.atkcc", 3), std::string("抓包-ch3.csv"));
}

TEST(csv_fmt_rate_units) {
    CHECK_EQ(fmtRate(2500000.0), std::string("2.50 MHz"));
    CHECK_EQ(fmtRate(2400000.0), std::string("2.40 MHz"));
    CHECK_EQ(fmtRate(1000.0), std::string("1.00 kHz"));
    CHECK_EQ(fmtRate(600.0), std::string("600 Hz"));
}

TEST(csv_export_doc_reports_what_matters) {
    std::vector<Packet> packets;
    for (int i = 0; i < 5; ++i) {
        Packet p = mkPacket();
        p.index = static_cast<uint64_t>(i);
        packets.push_back(p);
    }
    std::vector<const Packet*> rows;
    for (const Packet& p : packets) rows.push_back(&p);

    DecodeStats st;
    st.packetCount = 5;
    st.crcUnknown = 5;
    st.sampleRate = 1000.0;
    st.sampleRateSource = "powerz";
    st.durationSec = 1.5;

    ExportDoc doc;
    doc.fileName = "抓包.sqlite";
    doc.channel = 0;
    doc.protocol = "USB PD";
    doc.source = "powerz";
    doc.rate = 1000.0;
    doc.stats = &st;
    doc.totalPackets = 5;

    const CsvExportResult r = csvExport(rows, doc, /*limit=*/2, /*bom=*/true);
    CHECK_EQ(r.rows, static_cast<uint64_t>(2));
    CHECK_EQ(r.packets, static_cast<uint64_t>(5));
    CHECK_EQ(r.fileName, std::string("抓包-ch0.csv"));
    CHECK_EQ(r.sampleRateText, std::string("1.00 kHz"));
    CHECK_EQ(r.csv.compare(0, 3, "\xEF\xBB\xBF"), 0);

    // 只有 2 行数据（+ 1 行表头）
    size_t eols = 0;
    for (size_t i = 0; i + 1 < r.csv.size(); ++i) {
        if (r.csv[i] == '\r' && r.csv[i + 1] == '\n') ++eols;
    }
    CHECK_EQ(eols, static_cast<size_t>(2));

    // 该说的话必须说：CRC 未记录 + 只导了前 2 条
    bool saidUnrecorded = false, saidLimited = false;
    for (const std::string& n : r.notes) {
        if (n.find("未记录") != std::string::npos) saidUnrecorded = true;
        if (n.find("只导了前 2 条") != std::string::npos) saidLimited = true;
    }
    CHECK(saidUnrecorded);
    CHECK(saidLimited);
}

TEST(csv_hex_var_is_unpadded) {
    // **不补零**，0 输出 "0"。
    // 这条是专门防「拿 hexU(v,1) 顶替」的：hexU 是定宽截断，digits=1 只留最低一个
    // nibble ⇒ 0x1AB 静默变 "B"。曾因此让多条 Reserved / 私有载荷 字段显示错误。
    CHECK_EQ(hexVar(0), std::string("0"));
    CHECK_EQ(hexVar(5), std::string("5"));
    CHECK_EQ(hexVar(0x1AB), std::string("1AB"));
    CHECK_EQ(hexVar(0xFFFF), std::string("FFFF"));
    CHECK_EQ(hexVar(0x12345678), std::string("12345678"));
    // 与定宽版对照，固定住两者的分工
    CHECK_EQ(hexU(0x1AB, 1), std::string("B"));      // 定宽：只剩一位
    CHECK_EQ(hexU(0x1AB, 4), std::string("01AB"));   // 定宽：左侧补零
}
