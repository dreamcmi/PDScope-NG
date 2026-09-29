// sqlite_reader.h — 只读 SQLite 读取器（官方 amalgamation，静态链入）
//
// 用官方 amalgamation 而不是自写 B-tree 读取器：可读的库格式因此变多。
// 但**面向用户的语义与错误呈现不受实现影响**（例如 `pd_table` 不存 CRC 就必须报
// 「未记录」，不能因为重算对上了就报通过）。
//
// 用法：把整个库文件字节交给构造函数即可（内部用 sqlite3_deserialize 建内存只读库，
// 不落盘、不改动调用方的字节）。构造失败即抛 pdscope::Error。
#pragma once

#include "rowsource.h"

#include <memory>
#include <utility>

struct sqlite3;

namespace pdscope {

class SqliteReader : public RowSource {
public:
    /** @param bytes 整个数据库文件的内容（内部复制一份自持）。 */
    explicit SqliteReader(const Bytes& bytes);
    /**
     * 与调用方**共享**同一份字节。
     *
     * ⚠ `sqlite3_deserialize` 不复制缓冲区，它持有的就是我们给进去的那个指针；
     *   而 FFI 入口 `pdscope_open_bytes()` 里的 `Bytes` 是**函数局部变量**。
     *   只存 `const Bytes&` 的话，函数一返回缓冲区就没了 —— 之后每次读表都是
     *   use-after-free（这就是「同一份 .sqlite 用命令行读得出、走界面却崩」的根因）。
     */
    explicit SqliteReader(std::shared_ptr<const Bytes> bytes);
    ~SqliteReader() override;

    SqliteReader(const SqliteReader&) = delete;
    SqliteReader& operator=(const SqliteReader&) = delete;

    bool hasTable(const std::string& name) const override;
    uint64_t count(const std::string& name) const override;
    uint64_t forEachRow(const std::string& name,
                        const std::function<bool(const SqlRow&)>& cb) const override;

    bool isSqlite() const override { return true; }
    int pageSize() const override { return pageSize_; }
    int pageCount() const override { return pageCount_; }
    int textEncoding() const override { return textEncoding_; }
    int writeVersion() const override { return writeVersion_; }

    /** 表名列表（不含 sqlite_ 前缀的内部表）。 */
    std::vector<std::string> tableNames() const;

    /** 一句话摘要，供元数据/排错使用。 */
    std::string describe() const;

    /** 是不是 SQLite 文件（只看头 16 字节魔数，不解析）。 */
    static bool isSqlite(const Bytes& bytes);

private:
    // 声明顺序即初始化顺序：`bytes_` 绑定到 `*own_`，own_ 必须在前。
    std::shared_ptr<const Bytes> own_;
    const Bytes& bytes_;
    sqlite3* db_ = nullptr;
    int pageSize_ = 0;
    int pageCount_ = 0;
    int textEncoding_ = 1;
    int writeVersion_ = 1;
};

}  // namespace pdscope
