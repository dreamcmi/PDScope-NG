// csv.h — 「报文表 → CSV」的唯一实现
//
// 三个出口共用它，必须字字一样，否则「界面里导出来是好的、命令行导出来少一列」
// 这种差异只有用户会发现：
//   界面「另存为」→ CSV ／ 命令行 `--csv` ／ `pdscope-cli`
//
// 格式约定：
//   · 每个字段都加双引号，内部的双引号翻倍（RFC 4180）；
//   · 行尾 `\r\n`，**结尾不带换行**；
//   · 时间既给 `hh:mm:ss.mmm` 也给裸毫秒，两列都在；
//   · BOM 默认写在最前面（Excel 靠它才认 UTF-8）。
#pragma once

#include "decode_stats.h"
#include "packet.h"

namespace pdscope {

constexpr const char* kCsvBom = "\xEF\xBB\xBF";
constexpr const char* kCsvEol = "\r\n";

/** 列头。`protocol == "UFCS"` 时第 6 列由「数据对象个数」变成「数据字节数」。 */
std::vector<std::string> csvHead(const std::string& protocol);

/** 一条报文 → 一行单元格（顺序与 csvHead 严格对应）。 */
std::vector<std::string> csvRow(const Packet& p, const std::string& protocol);

/** 完整 CSV 文本（含 BOM 与 CRLF）。 */
std::string csvText(const std::vector<const Packet*>& packets, const std::string& protocol,
                    bool bom);

/** 导出用的文件名主干：去掉抓包扩展名（`.atkcc` / `.sqlite` / `.db` / `.pdstream`）。 */
std::string csvBase(const std::string& fileName);

/** 导出文件名：`<主干>-ch<通道>.csv`。带通道号是有意的 —— 换个通道再导不该覆盖。 */
std::string csvFileName(const std::string& fileName, int channel);

/** 采样率按量级选单位。 */
std::string fmtRate(double hz);

/** 导出用的上下文。 */
struct ExportDoc {
    std::string fileName;
    int channel = 0;
    std::string protocol = "USB PD";
    std::string source = "atkcc";
    double rate = 0;
    uint64_t fileSize = 0;
    uint64_t decodeMs = 0;
    const DecodeStats* stats = nullptr;
    uint64_t totalPackets = 0;
};

struct CsvExportResult {
    std::string csv;                 // 可直接落盘的完整文本（含 BOM）
    std::string fileName;            // 建议的导出名
    int channel = 0;
    std::string protocol;
    std::string source;
    uint64_t fileSize = 0;
    uint64_t packets = 0;
    uint64_t rows = 0;
    uint64_t decodeMs = 0;
    double durationSec = 0;
    double sampleRate = 0;
    std::string sampleRateSource;
    std::string sampleRateText;
    std::vector<std::string> notes;  // 「该如实告诉用户」的话
};

/**
 * 「一份抓包的解码结果」→ 导出成品（CSV 文本 + 摘要 + 该说的话）。
 *
 * @param rows   要导出的报文（界面传当前视图，命令行传全部报文）
 * @param limit  >0 时只导前 N 行
 */
CsvExportResult csvExport(const std::vector<const Packet*>& rows, const ExportDoc& doc,
                          uint64_t limit, bool bom);

}  // namespace pdscope
