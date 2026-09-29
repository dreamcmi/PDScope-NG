// test_abi.cpp — C ABI 的契约测试
//
// 这个文件的存在理由很直接：**边界上的约定没人测过**。
// 在它之前，`pdscope-tests` 只链核心静态库、只调 C++ 内部接口，`abi.cpp` 压根没编进来
// ——于是两个 bug 一路活到了 Dart 侧才被揪出来，而且都是「内部接口看着完全正常」的那种：
//
//   ① 路径编码：`pdscope.h` 写着参数是 `path_utf8`，实现却在 Windows 上用 `fopen`
//      按**进程 ANSI 代码页**解释它 ⇒ 中文路径必然打不开。命令行下"一直能用"纯属巧合
//      （MSVC 的 `main(argc, argv)` 给的就是 ANSI 字节），骗过了所有手工验证。
//
//   ② `pdscope_view_count` 恒为 0：视图是懒重建的，而它直接读了 `view_.size()`；
//      同一个会话上 `pdscope_query_page` 却会顺手重建、照常返回一行行数据。
//      界面于是变成「列表里有行，条数写 0 条」。
//
// 两件事的共性：**只有跨过 ABI 才显形，且内部接口的测试怎么加都测不到**。
// 所以这个文件一律直接调 `pdscope_*`，不碰 `Session`。

#include "test.h"

#include <pdscope/pdscope.h>

#include "container/pdstream.h"
#include "fixtures.h"
#include "pd_frames.h"
#include "util.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace pdscope;

namespace {

namespace fs = std::filesystem;

/**
 * 系统临时目录下的一个带中文的目录。
 *
 * ⚠ 必须用 `fs::u8path` 而不是直接拿窄字符串造 `fs::path`：本工程带 `/utf-8` 编译，
 *   源码里的字面量是 UTF-8 字节，而 `fs::path` 的窄字符构造在 Windows 上按 ANSI
 *   解释 —— 直接传就会造出一个乱码名的目录，测试"通过"得莫名其妙。
 */
fs::path scratchDir() {
    const fs::path dir = fs::temp_directory_path() / fs::u8path("pdscope ABI 测试");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void dropDir(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);  // 尽力而为：留个临时目录不该让测试变红
}

/** `fs::path` → 交给 C ABI 的 UTF-8 文本（C++17 下 u8string() 返回 std::string）。 */
std::string utf8Of(const fs::path& p) { return p.u8string(); }

/** 4 条 GoodCRC 的 .pdStream（默认筛选会把它们全屏蔽掉，正好用来验视图）。 */
Bytes makePdStreamFixture() {
    std::vector<PdStreamRecord> rows;
    for (int i = 0; i < 4; ++i) {
        PdStreamRecord r;
        r.time = 0.25 * (i + 1);  // 秒
        r.vbus = 5.0 + i;
        r.ibus = 1.0;
        // revCode=2、type=1、nObjects=0 ⇒ GoodCRC
        const uint16_t hdr = static_cast<uint16_t>((1 << 8) | (2 << 6) | 1 | ((i & 7) << 9));
        const Bytes wire{static_cast<uint8_t>(hdr & 0xff), static_cast<uint8_t>(hdr >> 8)};
        r.raw = fxframe::pdBlob(static_cast<uint32_t>(250 * (i + 1)), 0, wire);
        rows.push_back(std::move(r));
    }
    return writePdStream(rows);
}

/** 打开失败时把库给的错误文本取出来并释放。 */
std::string takeErr(char* err) {
    if (!err) return std::string();
    const std::string s = err;
    pdscope_str_free(err);
    return s;
}

struct Ring {
    pdscope_session* s = nullptr;
    ~Ring() { if (s) pdscope_close(s); }
};

}  // namespace

/* ══════════════════ 路径：契约写的是 UTF-8 ══════════════════ */

TEST(abi_path_utf8_read_write_roundtrip) {
    const fs::path dir = scratchDir();
    const fs::path file = dir / fs::u8path("我的抓包 60W.sqlite");

    const std::string text = "PDScope-NG 路径往返\nsecond line\n";
    std::string why;
    CHECK(writeWholeFile(utf8Of(file), text, why));
    if (!why.empty()) test::reportFailure("写文件不该报错，实际：" + why, __FILE__, __LINE__);

    // 真的落在磁盘上（而不是某个被 ANSI 转义过的别的名字）
    CHECK(fs::exists(file));

    Bytes got;
    why.clear();
    CHECK(readWholeFile(utf8Of(file), got, why));
    CHECK_EQ(std::string(got.begin(), got.end()), text);

    dropDir(dir);
}

TEST(abi_open_file_accepts_utf8_path) {
    const fs::path dir = scratchDir();
    const fs::path file = dir / fs::u8path("安可 60W.atkcc");

    // 一份最小的合法 .atkcc：只要容器能被按内容认出来即可（解析细节由 test_atkcc 覆盖）
    const Bytes atkcc = fx::makeAtkcc(fx::packBitsLsb(std::vector<uint8_t>(256, 1)), 2500);
    std::string why;
    CHECK(writeWholeFile(utf8Of(file), std::string(atkcc.begin(), atkcc.end()), why));

    char* err = nullptr;
    Ring ring;
    ring.s = pdscope_open_file(utf8Of(file).c_str(), &err);

    // 中文路径打不开时这里会拿到 nullptr —— 就是那个 bug 的现场
    CHECK(ring.s != nullptr);
    const std::string e = takeErr(err);
    if (!e.empty()) test::reportFailure("打开中文路径失败：" + e, __FILE__, __LINE__);

    if (ring.s) {
        pdscope_buf meta{};
        CHECK_EQ(pdscope_metadata(ring.s, &meta), PDSCOPE_OK);
        const std::string json(meta.data, meta.data + meta.len);
        CHECK(json.find("\"atkcc\"") != std::string::npos);
        // 文件名里的中文要原样透出（显示名来自路径，别在这儿丢）
        CHECK(json.find("安可 60W.atkcc") != std::string::npos);
        pdscope_buf_free(&meta);
    }

    dropDir(dir);
}

TEST(abi_open_file_missing_reports_readable_error) {
    const fs::path dir = scratchDir();
    const fs::path file = dir / fs::u8path("并不存在的抓包.atkcc");

    char* err = nullptr;
    Ring ring;
    ring.s = pdscope_open_file(utf8Of(file).c_str(), &err);

    CHECK(ring.s == nullptr);
    CHECK(err != nullptr);
    const std::string msg = takeErr(err);
    // 报错要带上路径，而且**路径本身不能被转义坏**：用户得能照着它核对文件名
    CHECK(msg.find(utf8Of(file)) != std::string::npos);

    dropDir(dir);
}

#if defined(_WIN32)

TEST(abi_path_utf8_wide_roundtrip) {
    // 三级转换的机制本身：UTF-8 → UTF-16 → UTF-8 必须逐字节回到原样
    const std::string s = "C:\\用户\\张三\\我的抓包 60W.atkcc";
    CHECK_EQ(wideToUtf8(utf8ToWide(s)), s);

    // 空串不许崩，也不许编出个假的宽字符串
    CHECK(utf8ToWide("").empty());
    CHECK(wideToUtf8(L"").empty());
}

#endif

/* ══════════════════ 视图条数：必须和分页看到的是同一个视图 ══════════════════ */

TEST(abi_view_count_agrees_with_query_page) {
    const Bytes stream = makePdStreamFixture();

    char* err = nullptr;
    Ring ring;
    ring.s = pdscope_open_bytes(stream.data(), stream.size(), "夹具.pdStream", &err);
    const std::string e = takeErr(err);
    CHECK(ring.s != nullptr);
    if (!ring.s) {
        test::reportFailure("打不开夹具：" + e, __FILE__, __LINE__);
        return;
    }

    pdscope_buf stats{};
    CHECK_EQ(pdscope_decode(ring.s, nullptr, &stats), PDSCOPE_OK);
    pdscope_buf_free(&stats);

    // 夹具是 4 条 GoodCRC，而默认筛选 `hideGoodCrc = true` ⇒ 视图为空。
    // ⚠ 这一条**在 bug 存在时同样会通过**（直接读空 vector 的长度也是 0），
    //   所以它只是把默认语义钉住，真正抓 bug 的是下面那一段。
    uint64_t emptyView = 12345;
    CHECK_EQ(pdscope_view_count(ring.s, &emptyView), PDSCOPE_OK);
    CHECK_EQ(emptyView, static_cast<uint64_t>(0));

    // 关掉屏蔽：视图应当有 4 条。曾经的实现这里仍然返回 0（视图没重建），
    // 而同一个会话上 query_page 却能取出 4 行 —— 界面就是被这个不一致搞坏的。
    char* ferr = nullptr;
    CHECK_EQ(pdscope_set_filter(ring.s, "{\"hideGoodCrc\":false}", &ferr), PDSCOPE_OK);
    const std::string fe = takeErr(ferr);
    if (!fe.empty()) test::reportFailure("设筛选失败：" + fe, __FILE__, __LINE__);

    uint64_t n = 0;
    CHECK_EQ(pdscope_view_count(ring.s, &n), PDSCOPE_OK);
    CHECK_EQ(n, static_cast<uint64_t>(4));

    // 两个接口必须指向同一个视图：把分页一页页取空，行数要正好等于报出来的条数。
    uint64_t rows = 0;
    for (uint64_t off = 0;; off += 2) {
        pdscope_buf page{};
        CHECK_EQ(pdscope_query_page(ring.s, off, 2, &page), PDSCOPE_OK);
        const json j = json::parse(std::string(page.data, page.data + page.len));
        pdscope_buf_free(&page);
        const size_t got = j.size();
        rows += got;
        if (got < 2) break;
    }
    CHECK_EQ(rows, n);
}

TEST(abi_view_count_is_stable_across_repeated_reads) {
    // 顺带钉住「读条数」是幂等的：它会顺带重建视图，但不该把状态改坏。
    const Bytes stream = makePdStreamFixture();
    char* err = nullptr;
    Ring ring;
    ring.s = pdscope_open_bytes(stream.data(), stream.size(), "夹具.pdStream", &err);
    takeErr(err);
    if (!ring.s) {
        test::reportFailure("打不开夹具", __FILE__, __LINE__);
        return;
    }
    pdscope_buf stats{};
    CHECK_EQ(pdscope_decode(ring.s, nullptr, &stats), PDSCOPE_OK);
    pdscope_buf_free(&stats);

    // 解码后**不设任何筛选**，直接连读三次
    uint64_t a = 0, b = 0, c = 0;
    CHECK_EQ(pdscope_view_count(ring.s, &a), PDSCOPE_OK);
    CHECK_EQ(pdscope_view_count(ring.s, &b), PDSCOPE_OK);
    CHECK_EQ(pdscope_view_count(ring.s, &c), PDSCOPE_OK);
    CHECK_EQ(a, b);
    CHECK_EQ(b, c);
}
