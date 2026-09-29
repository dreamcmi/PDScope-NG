#include "sqlite_reader.h"

#include <sqlite3.h>

#include <algorithm>

namespace pdscope {
namespace {

const char* kMagic = "SQLite format 3";
constexpr size_t kMagicLen = 16;

std::string quoteIdent(const std::string& name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

}  // namespace

bool SqliteReader::isSqlite(const Bytes& bytes) {
    if (bytes.size() < kMagicLen) return false;
    return std::memcmp(bytes.data(), kMagic, kMagicLen) == 0;
}

// 复制一份再反序列化：给「调用方自己还留着 bytes」的场景用（测试、命令行）。
SqliteReader::SqliteReader(const Bytes& bytes)
    : SqliteReader(std::make_shared<const Bytes>(bytes)) {}

SqliteReader::SqliteReader(std::shared_ptr<const Bytes> bytes)
    : own_(std::move(bytes)), bytes_(*own_) {
    if (bytes_.size() < 100) {
        failFormat("文件过小，不是 SQLite 数据库（" + std::to_string(bytes_.size()) + " 字节）");
    }
    if (!isSqlite(bytes_)) {
        failFormat("不是 SQLite 数据库（文件头魔数不匹配）");
    }

    int rc = sqlite3_open_v2(":memory:", &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                             nullptr);
    if (rc != SQLITE_OK || !db_) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "无法创建内存数据库";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        fail(PDSCOPE_STATUS_INTERNAL, "打开内存 SQLite 失败：" + msg);
    }

    // 只读反序列化：不自建页缓存副本，也不改动调用方的字节。
    // ⚠ 缓冲区在整个连接生命周期内必须保持有效，而 SQLite **不会**自己复制它 ——
    //   所以缓冲区由本对象的 `own_` 持有（而不是依赖调用方）。
    sqlite3_deserialize(db_, "main",
                        reinterpret_cast<unsigned char*>(const_cast<uint8_t*>(bytes_.data())),
                        static_cast<sqlite3_int64>(bytes_.size()),
                        static_cast<sqlite3_int64>(bytes_.size()),
                        SQLITE_DESERIALIZE_READONLY);

    // 立刻验证库确实可读：损坏的库在这里就报出来，而不是等到查询时给半截结果
    sqlite3_stmt* st = nullptr;
    rc = sqlite3_prepare_v2(db_, "PRAGMA schema_version;", -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_); db_ = nullptr;
        failFormat("SQLite 库无法读取（可能已损坏）：" + msg);
    }
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        const std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_); db_ = nullptr;
        failFormat("SQLite 库无法读取（可能已损坏）：" + msg);
    }

    auto pragmaInt = [&](const char* sql, int fallback) -> int {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) return fallback;
        int v = fallback;
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return v;
    };
    pageSize_ = pragmaInt("PRAGMA page_size;", 0);
    pageCount_ = pragmaInt("PRAGMA page_count;", 0);
    textEncoding_ = pragmaInt("PRAGMA encoding;", 1);   // 1=UTF-8 2=UTF-16le 3=UTF-16be
    if (pageSize_ <= 0 && bytes_.size() >= 18) {
        int ps = (bytes_[16] << 8) | bytes_[17];
        pageSize_ = (ps == 1) ? 65536 : ps;
    }
    if (pageCount_ <= 0 && pageSize_ > 0) {
        pageCount_ = static_cast<int>(bytes_.size() / static_cast<size_t>(pageSize_));
    }
    if (bytes_.size() >= 20) writeVersion_ = bytes_[18];
}

SqliteReader::~SqliteReader() {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

std::vector<std::string> SqliteReader::tableNames() const {
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' ORDER BY rowid;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        if (t) out.emplace_back(reinterpret_cast<const char*>(t));
    }
    sqlite3_finalize(st);
    return out;
}

bool SqliteReader::hasTable(const std::string& name) const {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    const bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

uint64_t SqliteReader::count(const std::string& name) const {
    if (!hasTable(name)) return 0;
    const std::string sql = "SELECT COUNT(*) FROM " + quoteIdent(name) + ";";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return 0;
    uint64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return n;
}

uint64_t SqliteReader::forEachRow(const std::string& name,
                                  const std::function<bool(const SqlRow&)>& cb) const {
    if (!hasTable(name)) return 0;
    const std::string sql = "SELECT * FROM " + quoteIdent(name) + ";";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db_);
        failFormat("读取表 " + name + " 失败：" + msg);
    }

    const int nCol = sqlite3_column_count(st);
    SqlRow row;
    row.reserve(static_cast<size_t>(nCol));
    uint64_t n = 0;

    for (;;) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            const std::string msg = sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            failFormat("读取表 " + name + " 时出错：" + msg);
        }
        row.clear();
        for (int c = 0; c < nCol; ++c) {
            SqlValue v;
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER:
                    v.kind = SqlValue::Kind::Int;
                    v.i = sqlite3_column_int64(st, c);
                    v.d = static_cast<double>(v.i);
                    break;
                case SQLITE_FLOAT:
                    v.kind = SqlValue::Kind::Double;
                    v.d = sqlite3_column_double(st, c);
                    break;
                case SQLITE_TEXT: {
                    v.kind = SqlValue::Kind::Text;
                    const unsigned char* t = sqlite3_column_text(st, c);
                    const int len = sqlite3_column_bytes(st, c);
                    if (t && len > 0) v.s.assign(reinterpret_cast<const char*>(t),
                                                 static_cast<size_t>(len));
                    break;
                }
                case SQLITE_BLOB: {
                    v.kind = SqlValue::Kind::Blob;
                    const void* p = sqlite3_column_blob(st, c);
                    const int len = sqlite3_column_bytes(st, c);
                    if (p && len > 0) {
                        const uint8_t* b = static_cast<const uint8_t*>(p);
                        v.b.assign(b, b + len);
                    }
                    break;
                }
                default:
                    v.kind = SqlValue::Kind::Null;
                    break;
            }
            row.push_back(std::move(v));
        }
        n++;
        if (cb && !cb(row)) break;
    }
    sqlite3_finalize(st);
    return n;
}

std::string SqliteReader::describe() const {
    const std::vector<std::string> t = tableNames();
    std::string joined;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i) joined += ", ";
        joined += t[i];
    }
    return std::to_string(pageSize_) + " B/页 · " + std::to_string(pageCount_)
         + " 页 · 文本编码 " + std::to_string(textEncoding_) + " · 表 [" + joined + "]";
}

}  // namespace pdscope
