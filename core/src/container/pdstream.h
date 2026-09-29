// pdstream.h — POWER-Z 的 `.pdStream` 容器
//
// 同一份抓包，POWER-Z 可以导出两种文件：
//   · `.sqlite`   三张表（含 pd_chart 的 ADC 波形）
//   · `.pdStream` **只有 pd_table 那部分**：二进制记录首尾相接，无文件头/索引/校验和
//
// 所以这**不是第三种协议**：里面的报文与 `.sqlite` 的 `pd_table.Raw` 逐字节相同，
// 语义解析走 `pd::PdDecoder`。本文件只负责把「表格」
// 从二进制流里读出来，再交给 PowerzCapture 现成的解码流程。
// 唯一实质差别：**没有 ADC 波形**，于是时间轴没有曲线可画。
#pragma once

#include "powerz.h"

#include <memory>
#include <utility>

namespace pdscope {

/** 每条记录的定长开销：4 字节长度 + 3 个 f64。 */
constexpr uint64_t kPdStreamRecFixed = 28;
/** payload 合法上限（实测最大 32 字节，留足余量但挡住乱数据）。 */
constexpr uint64_t kPdStreamMaxPayload = 4096;

struct PdStreamRecord {
    double time = 0;
    double vbus = 0;
    double ibus = 0;
    std::vector<uint8_t> raw;
};

struct PdStreamParsed {
    std::vector<PdStreamRecord> records;
    uint64_t bytes = 0;
    uint64_t payloadBytes = 0;
    uint64_t bytesConsumed = 0;
};

/** 把 `.pdStream` 读成记录数组；任何一步对不上就抛错并说明卡在哪个偏移。 */
PdStreamParsed readPdStream(const Bytes& bytes);

/** 只看结构判断是不是 `.pdStream`（不抛错、不复制字节）。 */
bool sniffPdStream(const Bytes& bytes);

/** 把 `{time, vbus, ibus, raw}` 行写成 `.pdStream` 字节（写入前按 Time 排序）。 */
Bytes writePdStream(const std::vector<PdStreamRecord>& rows);

/**
 * 把记录流包装成只读虚拟表，形状对齐 `SqliteReader` 的那几个方法 ——
 * 于是 PowerzCapture 的构造与解码流程一行都不用改。
 */
class PdStreamTable : public RowSource {
public:
    // ⚠ 必须**按值持有**解析结果：`openPdStream()` 里的 `parsed` 是局部变量，
    //   表对象活到 `PowerzCapture::decode()` 之后，存引用会变成悬垂引用
    //   （曾因此让 pdstream 一组测试整体段错误）。
    explicit PdStreamTable(PdStreamParsed parsed) : parsed_(std::move(parsed)) {}

    bool hasTable(const std::string& name) const override { return name == "pd_table"; }
    uint64_t count(const std::string& name) const override {
        return hasTable(name) ? parsed_.records.size() : 0;
    }
    uint64_t forEachRow(const std::string& name,
                        const std::function<bool(const SqlRow&)>& cb) const override;

    bool isSqlite() const override { return false; }

private:
    PdStreamParsed parsed_;
};

/** 打开 `.pdStream`，返回与 `.sqlite` 同形的抓包对象（kind=Pd）。 */
std::unique_ptr<PowerzCapture> openPdStream(const Bytes& bytes);

}  // namespace pdscope
