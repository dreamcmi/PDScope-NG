// test_sqlite.cpp — POWER-Z 的 .sqlite：用官方 SQLite 现场造库再解析
//
// 造库这一步走 `sqlite3_serialize`（不需要落盘），所以测试目录里不会多出临时文件。
// 断言的重点是**语义口径**而不是「能不能读」：CRC 未记录、时间戳毫秒映射、
// pd_table/ufcs_table 分流 —— 这三条是与 PDScope 基线对齐的地方。

#include "test.h"

#include "container/powerz.h"
#include "container/sqlite_reader.h"
#include "fixtures.h"
#include "packet.h"
#include "pd_frames.h"
#include "session.h"

#include <sqlite3.h>

using namespace pdscope;

namespace {

struct Db {
    sqlite3* h = nullptr;
    ~Db() { if (h) sqlite3_close(h); }
};

bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

void bindInsert(sqlite3* db, const char* sql, double t, double a, double b, const Bytes& blob) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_double(st, 1, t);
    sqlite3_bind_double(st, 2, a);
    sqlite3_bind_double(st, 3, b);
    sqlite3_bind_blob(st, 4, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void bindChart(sqlite3* db, const char* sql, double t, double v, double i, double cc1, double cc2) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_double(st, 1, t);
    sqlite3_bind_double(st, 2, v);
    sqlite3_bind_double(st, 3, i);
    sqlite3_bind_double(st, 4, cc1);
    sqlite3_bind_double(st, 5, cc2);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

Bytes serialize(sqlite3* db) {
    sqlite3_int64 n = 0;
    unsigned char* p = sqlite3_serialize(db, "main", &n, 0);
    Bytes out;
    if (p) {
        out.assign(p, p + n);
        sqlite3_free(p);
    }
    return out;
}

/** 造一份 POWER-Z 的 PD 导出：3 条报文 + 1 条插入事件 + 一小段 pd_chart。 */
Bytes makePdDb() {
    Db db;
    if (sqlite3_open(":memory:", &db.h) != SQLITE_OK) return {};
    exec(db.h, "CREATE TABLE pd_table(Time REAL, Vbus REAL, Ibus REAL, Raw BLOB);");
    exec(db.h, "CREATE TABLE pd_chart(Time REAL, VBUS REAL, IBUS REAL, CC1 REAL, CC2 REAL);");
    exec(db.h, "CREATE TABLE pd_table_key(K TEXT);");

    const char* ins = "INSERT INTO pd_table(Time,Vbus,Ibus,Raw) VALUES(?1,?2,?3,?4);";
    // 先来一条插入事件，再来三条 GoodCRC（报文头只变 msgId）
    bindInsert(db.h, ins, 0.000, 5.0, 0.0, fxframe::pdConnectBlob(0, 0x11));
    for (int i = 0; i < 3; ++i) {
        const uint16_t hdr = static_cast<uint16_t>((1 << 8) | (2 << 6) | 1 | ((i & 7) << 9));
        Bytes wire{static_cast<uint8_t>(hdr & 0xff), static_cast<uint8_t>(hdr >> 8)};
        bindInsert(db.h, ins, 0.1 * (i + 1), 5.0 + i, 1.0 + i,
                   fxframe::pdBlob(static_cast<uint32_t>(100 * (i + 1)), 0, wire));
    }

    const char* insChart = "INSERT INTO pd_chart(Time,VBUS,IBUS,CC1,CC2) VALUES(?1,?2,?3,?4,?5);";
    for (int i = 0; i < 3; ++i) {
        bindChart(db.h, insChart, 0.1 * (i + 1), 5.0 + i * 0.1, 1.0, 0.2, 0.3);
    }
    return serialize(db.h);
}

/** 造一份 POWER-Z 的 UFCS 导出：2 条报文（一正一反）+ 1 条状态事件。 */
Bytes makeUfcsDb() {
    Db db;
    if (sqlite3_open(":memory:", &db.h) != SQLITE_OK) return {};
    exec(db.h, "CREATE TABLE ufcs_table(Time REAL, Vbus REAL, Ibus REAL, Raw BLOB);");
    exec(db.h, "CREATE TABLE ufcs_chart(Time REAL, VBUS REAL, IBUS REAL, DP REAL, DM REAL);");

    const char* ins = "INSERT INTO ufcs_table(Time,Vbus,Ibus,Raw) VALUES(?1,?2,?3,?4);";
    const Bytes f1 = fxframe::withCrc(fxframe::ufcsControl(0b001, 1, 0b000001, 0x06));
    const Bytes f2 = fxframe::withCrc(fxframe::ufcsControl(0b010, 2, 0b000001, 0x01));
    bindInsert(db.h, ins, 0.05, 9.0, 2.0, fxframe::ufcsRecord(50, 0, 0, /*flag=*/0, f1));
    bindInsert(db.h, ins, 0.15, 9.0, 2.0, fxframe::ufcsRecord(150, 1, 1, /*flag=*/1, f2));
    bindInsert(db.h, ins, 0.25, 9.0, 2.0, fxframe::ufcsEvent(250, 0x02));
    return serialize(db.h);
}

}  // namespace

TEST(sqlite_magic_and_tables) {
    const Bytes db = makePdDb();
    CHECK(!db.empty());
    CHECK(SqliteReader::isSqlite(db));

    SqliteReader r(db);
    const std::vector<std::string> names = r.tableNames();
    CHECK(r.hasTable("pd_table"));
    CHECK(r.hasTable("pd_chart"));
    CHECK(!r.hasTable("ufcs_table"));
    CHECK_EQ(r.count("pd_table"), static_cast<uint64_t>(4));   // 1 事件 + 3 报文
    CHECK_EQ(r.count("pd_chart"), static_cast<uint64_t>(3));
    CHECK(r.pageSize() > 0);
    CHECK(r.pageCount() > 0);
    // 内部表 sqlite_* 不该出现在表名列里
    for (const std::string& n : names) CHECK(n.rfind("sqlite_", 0) != 0);
}

TEST(sqlite_sniff_routes_by_table_name) {
    auto pd = sniffPowerz(makePdDb());
    CHECK(pd.has_value());
    if (pd) CHECK(*pd == PowerzKind::Pd);

    auto ufcs = sniffPowerz(makeUfcsDb());
    CHECK(ufcs.has_value());
    if (ufcs) CHECK(*ufcs == PowerzKind::Ufcs);

    // 是个合法的 SQLite 库，但没有那两张表 ⇒ 不是 POWER-Z 导出
    Db db;
    CHECK(sqlite3_open(":memory:", &db.h) == SQLITE_OK);
    exec(db.h, "CREATE TABLE other(x INTEGER);");
    const Bytes other = serialize(db.h);
    CHECK(SqliteReader::isSqlite(other));
    CHECK(!sniffPowerz(other).has_value());
}

TEST(sqlite_rejects_corrupt_or_short_file) {
    CHECK(!SqliteReader::isSqlite(Bytes{1, 2, 3}));
    CHECK_THROWS(SqliteReader r(Bytes(64, 0x00)));
    Bytes magic(200, 0x00);
    std::memcpy(magic.data(), "SQLite format 3", 16);
    CHECK(SqliteReader::isSqlite(magic));
    CHECK_THROWS(SqliteReader r(magic));       // 头对但库是坏的
}

TEST(sqlite_pd_decode_keeps_crc_unrecorded) {
    // SqliteReader 自己持有字节（构造时复制一份，见 sqlite_reader.h）——
    // 早先它只存引用，于是 `pdscope_open_bytes()` 那种「传局部 Bytes」的调用会 use-after-free。
    // 现在把 makePdDb() 直接塞进构造函数也是安全的，这里保留具名变量只为可读性。
    const Bytes db = makePdDb();
    auto src = std::make_shared<SqliteReader>(db);
    const PowerzCapture cap(src, PowerzKind::Pd, db.size());

    const PowerzMeta& m = cap.meta();
    CHECK_EQ(m.kind, std::string("pd"));
    CHECK_EQ(m.protocol, std::string("USB PD"));
    CHECK_EQ(m.tableRows, static_cast<uint64_t>(4));
    CHECK_EQ(m.chartRows, static_cast<uint64_t>(3));
    CHECK(m.isSqlite);
    // 1 采样点 = 1 ms ⇒ 末条 chart 的 300 ms 就是 0.3 s
    CHECK_NEAR(m.durationSec, 0.3, 1e-9);
    CHECK_EQ(m.bus.size(), 3u);

    const PowerzResult res = cap.decode(nullptr, nullptr);
    CHECK_EQ(res.packets.size(), 3u);                     // 插入事件不产生报文
    CHECK_EQ(res.stats.packetCount, static_cast<uint64_t>(3));
    CHECK_EQ(res.stats.connectCount, static_cast<uint64_t>(1));
    CHECK_EQ(res.stats.crcUnknown, static_cast<uint64_t>(3));   // 全部「未记录」
    CHECK_EQ(res.stats.badCrc, static_cast<uint64_t>(0));       // 未记录 ≠ 失败
    for (const Packet& p : res.packets) {
        CHECK_EQ(p.msgType, std::string("GoodCRC"));
        CHECK_EQ(static_cast<int>(p.crcOk), static_cast<int>(CrcState::Unrecorded));
        CHECK(!p.crcRecorded);
    }
    // 时间戳按 1 采样点 = 1 ms 映射
    CHECK_EQ(res.packets[0].startSample, static_cast<uint64_t>(100));
    CHECK_NEAR(res.packets[0].timeMs, 100.0, 1e-9);
}

TEST(sqlite_ufcs_decode_uses_container_link) {
    const Bytes db = makeUfcsDb();
    auto src = std::make_shared<SqliteReader>(db);
    const PowerzCapture cap(src, PowerzKind::Ufcs, db.size());
    const PowerzMeta& m = cap.meta();
    CHECK_EQ(m.kind, std::string("ufcs"));
    CHECK_EQ(m.protocol, std::string("UFCS"));
    CHECK_EQ(m.tableRows, static_cast<uint64_t>(3));

    const PowerzResult res = cap.decode(nullptr, nullptr);
    CHECK_EQ(res.packets.size(), 2u);                 // 状态事件不计入报文数
    CHECK_EQ(res.stats.ufcsEvents, static_cast<uint64_t>(1));
    CHECK_EQ(res.stats.ufcsUnlocatedRows, static_cast<uint64_t>(0));
    CHECK_EQ(res.stats.ufcsFrames, static_cast<uint64_t>(2));
    CHECK_EQ(res.stats.ufcsDirInferred, static_cast<uint64_t>(0));   // 链路来自容器
    CHECK_EQ(res.stats.ufcsDirFromLine, static_cast<uint64_t>(2));
    if (res.packets.size() >= 2) {
        // 两条报文的链路不同 —— 如果 flag 读错位置（读到恒为 0xAA 的 Training），
        // 这里两条会一模一样，而且是「同一个值」而不是报错
        CHECK_NE(res.packets[0].role, res.packets[1].role);
        CHECK(!res.packets[0].roleInferred);
        CHECK(!res.packets[1].roleInferred);
        CHECK_EQ(static_cast<int>(res.packets[0].crcOk), static_cast<int>(CrcState::Ok));
    }
}

TEST(session_export_csv_filter_scope_matches_baseline) {
    // 命令行导出必须**不过筛选**（对齐 JS 的 `tools/cli.js --csv`：直接喂全部 packets），
    // 而界面「另存为」导出的是当前视图。这份夹具里三条全是 GoodCRC，而默认筛选
    // `hideGoodCrc = true`（与 JS UI 的 app.js 默认值一致）⇒ 视图为空、全量有三条。
    // 顺带钉住「视图可以为空但导出照样有内容」这条零报文路径。
    auto s = Session::openBytes(makePdDb(), "抓包.sqlite");
    s->decode(-1, 0, false);
    CHECK_EQ(s->packetCount(), static_cast<uint64_t>(3));

    const std::string viewCsv = s->exportCsv(0, /*bom=*/false, /*filtered=*/true);
    const std::string allCsv = s->exportCsv(0, /*bom=*/false, /*filtered=*/false);

    auto crlfCount = [](const std::string& t) {
        size_t n = 0;
        for (size_t i = 0; i + 1 < t.size(); ++i)
            if (t[i] == '\r' && t[i + 1] == '\n') ++n;
        return n;
    };
    CHECK_EQ(s->viewCount(), static_cast<uint64_t>(0));        // GoodCRC 全被默认筛选挡掉
    CHECK_EQ(crlfCount(viewCsv), static_cast<size_t>(0));      // 只剩表头
    CHECK_EQ(crlfCount(allCsv), static_cast<size_t>(3));       // 表头 + 3 条
    CHECK(allCsv.size() > viewCsv.size());
    // 空视图不是「没输出」：表头两边都要在
    CHECK(viewCsv.find("\"MsgType\"") != std::string::npos);
    CHECK(allCsv.find("\"MsgType\"") != std::string::npos);
    // 不过筛选的那份必须真的带上报文行
    CHECK(allCsv.find("\"GoodCRC\"") != std::string::npos);
    CHECK(viewCsv.find("\"GoodCRC\"") == std::string::npos);
}
