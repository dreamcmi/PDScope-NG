// rowsource.h — 「事件行来源」抽象
//
// 为什么需要它：POWER-Z 可以把同一份抓包导出成两种容器
//   · `.sqlite`   —— 三张表（pd_chart 波形 / pd_table 事件 / pd_table_key 密钥）
//   · `.pdStream` —— 只有 pd_table 那部分的二进制记录流，没有 ADC 波形
// 而**报文的解码流程完全一样**。把「取行」抽成接口之后，解码、事件拆分、
// CRC 口径、统计全都只有一份实现（对应 JS 侧 core/pdstream.js 的 PdStreamTable）。
#pragma once

#include "util.h"

#include <functional>

namespace pdscope {

/** 一个列值（对齐 SQLite 的动态类型）。 */
struct SqlValue {
    enum class Kind { Null, Int, Double, Text, Blob } kind = Kind::Null;
    int64_t i = 0;
    double d = 0;
    std::string s;
    Bytes b;

    bool isNull() const { return kind == Kind::Null; }
    bool isBlob() const { return kind == Kind::Blob; }

    /** 数值化：Blob / Text / Null 一律给 0（与 JS 的 `Number(x) || 0` 同口径）。 */
    double asDouble() const {
        switch (kind) {
            case Kind::Int: return static_cast<double>(i);
            case Kind::Double: return d;
            case Kind::Text: return 0.0;
            default: return 0.0;
        }
    }

    /** 字节视图；非 Blob 返回空。 */
    const Bytes& bytes() const { return b; }
};

using SqlRow = std::vector<SqlValue>;

/** 只读的行来源。`pd_table` / `ufcs_table` 这两个方法名即全部接触面。 */
class RowSource {
public:
    virtual ~RowSource() = default;

    virtual bool hasTable(const std::string& name) const = 0;
    virtual uint64_t count(const std::string& name) const = 0;

    /** 顺序遍历全表。cb 返回 false 表示提前停止。返回已交付的行数。 */
    virtual uint64_t forEachRow(const std::string& name,
                               const std::function<bool(const SqlRow&)>& cb) const = 0;

    /* 容器级信息（仅用于元数据展示；.pdStream 一律给 0/false） */
    virtual bool isSqlite() const { return false; }
    virtual int pageSize() const { return 0; }
    virtual int pageCount() const { return 0; }
    virtual int textEncoding() const { return 0; }
    virtual int writeVersion() const { return 0; }
};

}  // namespace pdscope
