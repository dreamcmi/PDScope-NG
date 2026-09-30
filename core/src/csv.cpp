#include "csv.h"

#include <cmath>

namespace pdscope {
namespace {

/** CSV 字段转义：整体加引号 + 内部引号翻倍。 */
std::string csvCell(const std::string& c) {
    std::string out;
    out.reserve(c.size() + 2);
    out.push_back('"');
    for (char ch : c) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

/** `(Number.isFinite(v) ? v : 0).toFixed(digits)` */
std::string numFixed(double v, int digits) {
    if (!std::isfinite(v)) v = 0;
    return toFixedStr(v, digits);
}

/** 导出时该如实说的事。只写会影响「这份 CSV 怎么读」的事实，不堆统计数字。 */
std::vector<std::string> csvNotes(const ExportDoc& doc, uint64_t total, uint64_t rows) {
    std::vector<std::string> out;
    const DecodeStats& st = doc.stats ? *doc.stats : DecodeStats{};

    std::string srcTag;
    if (st.sampleRateSource == "declared") srcTag = "文件声明";
    else if (st.sampleRateSource == "measured") srcTag = "波形实测";
    else if (st.sampleRateSource == "default") srcTag = "默认值";
    else if (st.sampleRateSource == "override") srcTag = "手动指定";
    else if (st.sampleRateSource == "powerz") srcTag = "分析仪时间戳";
    else srcTag = st.sampleRateSource;

    if (st.hasSampleRateNote) out.push_back(st.sampleRateNote);
    else if (!srcTag.empty()) {
        out.push_back("采样率按「" + srcTag + "」取用：" + fmtRate(doc.rate) + "，时标换算以它为准");
    }

    if (st.hasChannelPick) {
        const ChannelPick& p = st.channelPick;
        if (p.allNoisy) {
            out.push_back("多通道文件里没找到像 CC 线的通道，仍选了活动度最高的 ch"
                          + std::to_string(p.picked) + " —— 这份 CSV 很可能没有报文");
        } else {
            std::string s = "多通道文件自动选了 ch" + std::to_string(p.picked);
            if (p.noiseRejected > 0) {
                s += "（跳过 " + std::to_string(p.noiseRejected) + " 条浮空/噪声线）";
            }
            out.push_back(s);
        }
    }
    if (!st.unsupported.empty()) {
        out.push_back(st.unsupported + "（" + std::to_string(st.unsupportedMsgs)
                      + " 条原始帧未做语义解析）");
    }
    if (st.ufcsUnlocatedRows) {
        out.push_back(std::to_string(st.ufcsUnlocatedRows)
                      + " 行没能认出 UFCS 报文，已跳过（不影响其余报文）");
    }
    if (st.badCrc) {
        out.push_back(std::to_string(st.badCrc) + " 条报文 CRC 校验失败（CRC 列标 BAD）");
    }
    // 「未记录」与「通过」是两件事：POWER-Z 的 PD 报文压根不存 CRC，不能替它写 OK
    if (st.crcUnknown) {
        out.push_back(std::to_string(st.crcUnknown)
                      + " 条报文的 CRC 未记录（分析仪不存，CRC 列留空 —— 不代表通过）");
    }
    if (st.badWire) {
        out.push_back(std::to_string(st.badWire)
                      + " 条报文的物理层字节与容器记录不符（拆帧自检未通过）");
    }
    if (st.truncatedRows) {
        out.push_back(std::to_string(st.truncatedRows) + " 行事件被截断，拼不回完整报文");
    }
    if (rows < total) {
        out.push_back("只导了前 " + std::to_string(rows) + " 条（该文件共 "
                      + std::to_string(total) + " 条）");
    }
    return out;
}

}  // namespace

std::vector<std::string> csvHead(const std::string& protocol) {
    const bool ufcs = (protocol == "UFCS");
    return {"#", "SOP", "MsgType", "ID", "Direction",
            ufcs ? "Bytes" : "Objects",
            "Elapsed", "Time(ms)", "VBUS(V)", "IBUS(A)", "Data", "CRC", "Note"};
}

std::vector<std::string> csvRow(const Packet& p, const std::string& protocol) {
    const bool ufcs = (protocol == "UFCS");
    std::vector<std::string> row;

    row.push_back(std::to_string(p.index));
    row.push_back(p.sop);
    row.push_back(p.msgType);
    row.push_back(p.hasMsgId ? std::to_string(p.msgId) : std::string());
    row.push_back(p.role);
    row.push_back(ufcs ? std::to_string(p.dataLen) : std::to_string(p.nObjects));
    row.push_back(csvClock(p.timeMs));
    row.push_back(std::isfinite(p.timeMs) ? toFixedStr(p.timeMs, 4) : std::string());
    row.push_back(numFixed(p.vbus, 4));
    row.push_back(numFixed(p.ibus, 4));
    row.push_back(p.dataHex);

    // CRC 的真实口径有三种：通过 / 校验错 / **没记录**（POWER-Z 的 PD 报文就是这种）。
    // 第三种留空而不是写 OK —— 不能替分析仪的数据背书。
    if (p.crcOk == CrcState::Unrecorded) row.push_back(std::string());
    else row.push_back(p.crcOk == CrcState::Ok ? "OK" : "BAD");

    row.push_back(p.summary);
    return row;
}

std::string csvText(const std::vector<const Packet*>& packets, const std::string& protocol,
                    bool bom) {
    const std::vector<std::string> head = csvHead(protocol);
    std::string out;
    if (bom) out += kCsvBom;

    for (size_t i = 0; i < head.size(); ++i) {
        if (i) out += ',';
        out += csvCell(head[i]);
    }
    for (const Packet* p : packets) {
        out += kCsvEol;
        const std::vector<std::string> row = csvRow(*p, protocol);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) out += ',';
            out += csvCell(row[i]);
        }
    }
    return out;
}

std::string csvBase(const std::string& fileName) {
    std::string s = fileName.empty() ? std::string("pdscope") : fileName;
    const std::string l = lower(s);
    // ⚠ 这三个扩展名要与「按内容分流」支持的三类来源一一对应 ——
    // 少一个 `.pdstream` 就会让默认导出名变成 `抓包.pdstream-ch0.csv`。
    for (const char* ext : {".atkcc", ".sqlite", ".db", ".pdstream", ".ufcsstream"}) {
        const size_t n = std::strlen(ext);
        if (l.size() >= n && l.compare(l.size() - n, n, ext) == 0) {
            return s.substr(0, s.size() - n);
        }
    }
    return s;
}

std::string csvFileName(const std::string& fileName, int channel) {
    return csvBase(fileName) + "-ch" + std::to_string(channel) + ".csv";
}

// fmtRate 只有一处实现（util.cpp）—— 界面 / CSV / CLI 显示同一个采样率时必须逐字符一致。

CsvExportResult csvExport(const std::vector<const Packet*>& rows, const ExportDoc& doc,
                          uint64_t limit, bool bom) {
    CsvExportResult r;
    const uint64_t n = (limit > 0 && limit < rows.size()) ? limit : rows.size();
    std::vector<const Packet*> slice(rows.begin(), rows.begin() + static_cast<ptrdiff_t>(n));

    r.csv = csvText(slice, doc.protocol, bom);
    r.fileName = csvFileName(doc.fileName, doc.channel);
    r.channel = doc.channel;
    r.protocol = doc.protocol;
    r.source = doc.source;
    r.fileSize = doc.fileSize;
    r.packets = doc.totalPackets ? doc.totalPackets : rows.size();
    r.rows = n;
    r.decodeMs = doc.decodeMs;
    r.durationSec = doc.stats ? doc.stats->durationSec : 0;
    r.sampleRate = doc.rate;
    r.sampleRateSource = doc.stats ? doc.stats->sampleRateSource : std::string();
    r.sampleRateText = fmtRate(doc.rate);
    r.notes = csvNotes(doc, r.packets, r.rows);
    return r;
}

}  // namespace pdscope
