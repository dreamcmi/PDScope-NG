// main.cpp — 独立命令行：无图形环境下解析抓包并导出 CSV / JSON。
//
// 这是 PDScope-NG 的「核心与 CLI 先落地」那一半的一半：不依赖 Flutter、不依赖窗口系统，
// 在 CI 与服务器上都能跑。解析走的就是核心静态库（`pdscope_core`），
// 与 Dart FFI 经 C ABI 调用的是同一份实现，所以这里跑出来的结果就是界面里看到的结果。
//
// 用法（对齐 PDScope 的 `tools/cli.js` 与桌面版 `--csv`）：
//   pdscope-cli <抓包> [--csv [路径]] [--out 路径| -] [--channel N] [--limit N]
//                       [--bom|--no-bom] [--json] [--channels] [--rate HZ]
//   pdscope-cli --help | --version
//
// 退出码：0 成功 · 1 解析/导出失败 · 2 用法不对。

#include "csv.h"
#include "session.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

namespace {

using pdscope::Bytes;
using pdscope::jsToFixed;
using pdscope::Packet;
using pdscope::Session;
using pdscope::csvClock;
using pdscope::csvFileName;

/* ═══════════════════════ 输出通道 ═══════════════════════
 *
 * 一律按 UTF-8 写裸字节，从不经过控制台的代码页转换 —— 中文才不会变成乱码。
 * Windows 上额外把控制台输出代码页设成 UTF-8，这是屏幕显示的事，不影响落盘的字节。
 */

void initConsole() {
#if defined(_WIN32)
    // 控制台存在时才有意义；被重定向时调用也无害（返回 0）。
    ::SetConsoleOutputCP(65001);
    // ⚠ 同时把标准输出/错误切到**二进制**模式。CRT 默认的文本模式会把我们写出的每个
    //   '\n' 再补一个 '\r'，而 CSV 自身行尾已经是 CRLF ⇒ 落盘变成 `\r\r\n`
    //   （与 JS 基线逐字节比对时一眼可见，Excel 打开也会多出空行）。
    //   本程序全程按 UTF-8 裸字节写、不做代码页转换，二进制模式正是想要的语义。
    ::_setmode(::_fileno(stdout), _O_BINARY);
    ::_setmode(::_fileno(stderr), _O_BINARY);
#endif
}

void writeAll(std::FILE* f, const std::string& s) {
    if (s.empty()) return;
    std::fwrite(s.data(), 1, s.size(), f);
}

/** 给人看的字。CSV 占着标准输出时调用方要改用 `sayErr`，别把提示语混进管道。 */
void say(const std::string& line) { writeAll(stdout, line + "\n"); }

void sayErr(const std::string& line) {
    std::fflush(stdout);
    writeAll(stderr, line + "\n");
}

/**
 * 标准输出是不是被重定向到了**磁盘文件**。
 *
 * 只用来决定 CSV 要不要带 BOM：写文件带（Excel 认它），进管道不带（那三个字节只会碍事）。
 * Unix 上重定向与管道都是裸字节、没有代码页问题，这里保守地当管道 —— 宁可少一个 BOM。
 */
bool stdoutIsFile() {
#if defined(_WIN32)
    const HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    return ::GetFileType(h) == FILE_TYPE_DISK;
#else
    return false;
#endif
}

/* ═══════════════════════ 参数 ═══════════════════════ */

enum class Out { Auto, File, Stdout };

struct Args {
    std::string input;
    bool csv = false;
    Out out = Out::Auto;
    std::string outPath;
    int channel = -1;      // -1 = 自动挑（多通道 .atkcc）
    uint64_t limit = 0;    // 0 = 全部
    int bom = -1;          // -1 = 自动；0/1 = 强制
    double rate = 0;       // >0 = 强制采样率
    bool json = false;
    bool listChannels = false;
    bool help = false;
    bool version = false;
};

bool looksLikeCapture(const std::string& s) {
    const size_t dot = s.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= s.size()) return false;
    std::string ext = pdscope::lower(s.substr(dot + 1));
    return ext == "atkcc" || ext == "sqlite" || ext == "db" || ext == "bin"
        || ext == "zip" || ext == "pdstream";
}

std::string helpText() {
    return
        "PDScope-NG " + std::string(PDSCOPE_NG_VERSION) + " · USB PD / UFCS 抓包解析（命令行）\n"
        "\n"
        "用法：\n"
        "  pdscope-cli <抓包文件>                        打印报文表\n"
        "  pdscope-cli <抓包文件> --csv [输出路径| -]    导出 CSV\n"
        "  pdscope-cli <抓包文件> --json                 导出 JSON（含统计摘要）\n"
        "\n"
        "选项：\n"
        "  --csv [路径]      导出 CSV。省略路径 = 与输入同目录的 <名字>-ch<通道>.csv；\n"
        "                    `-` = 打到标准输出（此时提示语走标准错误）\n"
        "  --out <路径>      同 `--csv <路径>`\n"
        "  --channel <N>     指定通道（多通道 .atkcc 默认自动挑「像 CC 线」的那条）\n"
        "  --rate <HZ>       强制采样率，覆盖「文件声明 → 波形自检 → 兜底」三级策略\n"
        "  --limit <N>       只输出/导出前 N 条报文\n"
        "  --bom / --no-bom  强制带 / 不带 UTF-8 BOM（默认：写文件带、走管道不带）\n"
        "  --channels        只列出通道清单后退出\n"
        "  -h, --help        显示本帮助\n"
        "  -V, --version     显示版本\n"
        "\n"
        "抓包格式按**文件内容**自动分流，不看扩展名：\n"
        "  · 正点原子 ATK-C 的 .atkcc（CC 线原始电平采样 → BMC → 4B5B → PD 报文）\n"
        "  · POWER-Z 分析仪导出的 .sqlite（USB PD 或 UFCS，按表名分流）\n"
        "  · .pdStream（只有报文的记录流，不含 ADC 波形）\n"
        "\n"
        "CSV 一律是 UTF-8：写文件带 BOM（Excel / WPS 双击即正确），走管道不带。\n"
        "退出码：0 成功 · 1 导出失败（文件坏了 / 写不进去） · 2 用法不对\n";
}

std::string versionText() {
#if defined(_WIN32)
    return "PDScope-NG " PDSCOPE_NG_VERSION "（windows）";
#elif defined(__APPLE__)
    return "PDScope-NG " PDSCOPE_NG_VERSION "（macos）";
#else
    return "PDScope-NG " PDSCOPE_NG_VERSION "（linux）";
#endif
}

/** 解析命令行。返回 0 成功；非 0 表示用法错误（错误文本已写进 `msg`）。 */
int parseArgs(const std::vector<std::string>& argv, Args& a, std::string& msg) {
    std::string extra;   // 第二个位置参数：可能是输出路径
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (s == "-h" || s == "--help" || s == "-?") { a.help = true; return 0; }
        if (s == "-V" || s == "--version") { a.version = true; return 0; }
        if (s == "--bom") { a.bom = 1; continue; }
        if (s == "--no-bom") { a.bom = 0; continue; }
        if (s == "--json") { a.json = true; continue; }
        if (s == "--channels") { a.listChannels = true; continue; }

        if (s == "--csv") {
            a.csv = true;
            // `--csv 出.csv` 这种写法也认：紧跟其后、不以 `-` 开头、又**不像抓包文件**的词
            // 当输出路径。判「像不像抓包」是为了让 `--csv 抓包.atkcc`（开关写在前面）也能用 ——
            // 否则那份抓包会被当成输出名吃掉。
            if (i + 1 < argv.size()) {
                const std::string& n = argv[i + 1];
                if (n == "-") { a.out = Out::Stdout; ++i; }
                else if (!n.empty() && n[0] != '-' && !looksLikeCapture(n)) {
                    a.out = Out::File; a.outPath = n; ++i;
                }
            }
            continue;
        }
        if (s.rfind("--csv=", 0) == 0) {
            a.csv = true;
            a.outPath = s.substr(6);
            a.out = Out::File;
            continue;
        }
        if (s == "--out") {
            if (i + 1 >= argv.size()) { msg = "--out 后面要跟输出路径（`-` 表示标准输出）"; return 2; }
            a.outPath = argv[++i];
            a.out = (a.outPath == "-") ? Out::Stdout : Out::File;
            continue;
        }
        if (s.rfind("--out=", 0) == 0) {
            a.outPath = s.substr(6);
            a.out = (a.outPath == "-") ? Out::Stdout : Out::File;
            continue;
        }
        if (s == "--channel") {
            if (i + 1 >= argv.size()) { msg = "--channel 后面要跟通道号"; return 2; }
            try { a.channel = std::stoi(argv[++i]); }
            catch (...) { msg = "--channel 要的是通道号，收到的是「" + argv[i] + "」"; return 2; }
            continue;
        }
        if (s == "--limit") {
            if (i + 1 >= argv.size()) { msg = "--limit 后面要跟条数"; return 2; }
            try {
                const long long v = std::stoll(argv[++i]);
                if (v < 0) throw std::out_of_range("neg");
                a.limit = static_cast<uint64_t>(v);
            } catch (...) { msg = "--limit 要的是条数，收到的是「" + argv[i] + "」"; return 2; }
            continue;
        }
        if (s == "--rate") {
            if (i + 1 >= argv.size()) { msg = "--rate 后面要跟采样率（Hz）"; return 2; }
            try { a.rate = std::stod(argv[++i]); }
            catch (...) { msg = "--rate 要的是采样率，收到的是「" + argv[i] + "」"; return 2; }
            continue;
        }
        if (s.size() > 1 && s[0] == '-') {
            msg = "不认识的选项：" + s;
            return 2;
        }
        if (a.input.empty()) a.input = s;
        else if (extra.empty()) extra = s;
    }

    // 输出路径的第二个来源：第二个位置参数（前提是它不像抓包 —— 免得 `a.atkcc b.atkcc --csv`
    // 把第二份抓包当成输出文件覆盖掉）
    if (a.csv && a.out == Out::Auto && !extra.empty() && !looksLikeCapture(extra)) {
        a.out = Out::File;
        a.outPath = extra;
    }

    if (a.input.empty()) {
        msg = "要指定抓包文件。看用法请加 --help";
        return 2;
    }
    if (!a.csv && !a.json && !a.listChannels && a.out != Out::Auto) {
        msg = "「--out」只有在 --csv 或 --json 下才有意义";
        return 2;
    }
    // `--limit` 对「打印表格」也有意义（printTable 认它），所以这里只管 --bom；
    // 早先连 --limit 一起拦掉，和 --help 里写的「只输出/导出前 N 条报文」自相矛盾。
    if (!a.csv && !a.json && a.bom >= 0) {
        msg = "「--bom」只有在 --csv 或 --json 下才有意义";
        return 2;
    }
    return 0;
}

/* ═══════════════════════ 读文件 ═══════════════════════ */

bool readFile(const std::string& path, Bytes& out, std::string& msg) {
    // 与核心用同一份实现：路径一律 UTF-8，非 ASCII 在 Windows 上走宽字符 API。
    return pdscope::readWholeFile(path, out, msg);
}

std::string baseName(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

std::string dirName(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string(".") : path.substr(0, p);
}

/* ═══════════════════════ 表格输出 ═══════════════════════ */

std::string pad(const std::string& s, size_t w) {
    if (s.size() >= w) return s;
    return s + std::string(w - s.size(), ' ');
}

void printTable(const Session& s, uint64_t limit, bool verbose) {
    const std::vector<Packet>& packets = s.packets();
    const uint64_t n = (limit > 0 && limit < packets.size()) ? limit : packets.size();
    say("#     SOP     MsgType              ID  Dir    Elapsed         VBUS/IBUS          Data                                     Note");
    for (uint64_t i = 0; i < n; ++i) {
        const Packet& p = packets[static_cast<size_t>(i)];
        std::string line;
        line += pad(std::to_string(p.index), 5) + " ";
        line += pad(p.sop, 7) + " ";
        line += pad(p.msgType.empty() ? "" : p.msgType, 20) + " ";
        line += pad(p.hasMsgId ? std::to_string(p.msgId) : "", 3) + " ";
        line += pad(p.role, 6) + " ";
        line += pad(csvClock(p.timeMs), 15) + " ";
        line += pad(jsToFixed(p.vbus, 3) + "V/" + jsToFixed(p.ibus, 3) + "A", 18) + " ";
        line += pad(p.dataHex, 40) + " ";
        line += p.summary;
        say(line);
        if (verbose) {
            for (const auto& d : p.details) {
                if (d.key == "Object") { say("      ── " + d.value); continue; }
                say("        · " + d.key + ": " + d.value);
            }
            for (const auto& w : p.warnings) say("        ! " + w.longMsg);
        }
    }
}

/* ═══════════════════════ 摘要（人看的那几行）═══════════════════════ */

std::string sourceLabel(const Session& s) {
    // SourceKind 是命名空间级枚举，不在 Session 里
    switch (s.sourceKind()) {
        case pdscope::SourceKind::Atkcc:        return "ATK-C 原始采样";
        case pdscope::SourceKind::PowerzSqlite: return "POWER-Z 分析仪导出";
        case pdscope::SourceKind::PdStream:     return ".pdStream 报文流";
    }
    return "未知来源";
}

std::string fmtDuration(double sec) {
    char buf[64];
    if (sec >= 60.0) std::snprintf(buf, sizeof(buf), "%.2f min", sec / 60.0);
    else std::snprintf(buf, sizeof(buf), "%.3f s", sec);
    return buf;
}

std::string fmtSize(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    else std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

std::string progressBar(uint64_t done, uint64_t total, uint64_t packets) {
    if (total == 0) return "";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  · 解码 ch%d %llu/%llu 块 packets=%llu",
                  static_cast<int>(0), static_cast<unsigned long long>(done),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(packets));
    return buf;
}

}  // namespace

int runCli(std::vector<std::string> raw) {
    initConsole();

    Args a;
    std::string msg;
    const int perr = parseArgs(raw, a, msg);
    if (perr != 0) {
        sayErr("[PDScope-NG] " + msg);
        return 2;
    }
    if (a.help) { writeAll(stdout, helpText()); return 0; }
    if (a.version) { say(versionText()); return 0; }

    /* ── 读文件 ── */
    Bytes bytes;
    if (!readFile(a.input, bytes, msg)) {
        sayErr("[PDScope-NG] " + msg);
        return 1;
    }

    /* ── 打开会话（容器级元数据立刻可得，不解码）── */
    std::unique_ptr<Session> session;
    try {
        session = Session::openBytes(bytes, baseName(a.input));
    } catch (const pdscope::Error& e) {
        sayErr("[PDScope-NG] 打开失败：" + std::string(e.what()));
        return 1;
    }

    const pdscope::json meta = session->metadata();
    sayErr("[PDScope-NG] " + std::string(meta.value("title", "")) + "  " + baseName(a.input)
           + "  " + fmtSize(session->fileBytes()));

    /* ── --channels：只列通道 ── */
    if (a.listChannels) {
        if (meta.value("multiChannel", false) == false) {
            sayErr("  单通路（没有可分块的多通道概念）");
        }
        for (const auto& c : meta.at("channels")) {
            say("ch" + pad(std::to_string(c.value("channel", 0)), 2)
                + "  chunks=" + pad(std::to_string(c.value("chunks", 0)), 3)
                + "  bytes=" + std::to_string(c.value("totalBytes", static_cast<uint64_t>(0)))
                + "  samples=" + std::to_string(c.value("totalSamples", static_cast<uint64_t>(0))));
        }
        return 0;
    }

    /* ── 解码 ── */
    const int channel = (a.channel >= 0) ? a.channel : -1;
    if (a.channel >= 0 && !session->channelOrder().empty()) {
        bool ok = false;
        for (int ch : session->channelOrder()) if (ch == a.channel) ok = true;
        if (!ok) {
            sayErr("[PDScope-NG] 没有 ch" + std::to_string(a.channel) + " 这条通道");
            return 1;
        }
    }

    try {
        uint64_t lastBeat = 0;
        session->setProgressCallback([&](int phase, uint64_t done, uint64_t total, uint64_t pkts) {
            if (phase != 2) return;
            // 进度很密，只在整数百分比推进时打一行，免得终端刷屏
            const uint64_t pct = total ? (done * 100 / total) : 0;
            if (pct == lastBeat) return;
            lastBeat = pct;
            sayErr(progressBar(done, total, pkts));
        });
        session->decode(channel, a.rate, false);
    } catch (const pdscope::Error& e) {
        if (e.status() == pdscope::PDSCOPE_STATUS_CANCELLED) {
            sayErr("[PDScope-NG] 解码已取消");
            return 1;
        }
        sayErr("[PDScope-NG] 解码失败：" + std::string(e.what()));
        return 1;
    }

    const pdscope::DecodeStats& st = session->stats();
    sayErr("[decode] packets=" + std::to_string(st.packetCount)
           + "  badCrc=" + std::to_string(st.badCrc)
           + (st.crcUnknown ? "  crcUnknown=" + std::to_string(st.crcUnknown) : std::string())
           + "  dur=" + fmtDuration(st.durationSec));
    sayErr("[rate]   实际采用 " + pdscope::fmtRate(st.sampleRate)
           + "（" + st.sampleRateSource + "）");
    if (st.hasSampleRateNote) sayErr("[rate]   " + st.sampleRateNote);
    if (!st.unsupported.empty())
        sayErr("[warn]   " + st.unsupported + "（" + std::to_string(st.unsupportedMsgs) + " 条原始帧未做语义解析）");
    if (st.hasChannelPick && st.channelPick.noiseRejected > 0) {
        sayErr("[pick]   自动挑到 ch" + std::to_string(st.channelPick.picked)
               + "（排除了 " + std::to_string(st.channelPick.noiseRejected) + " 条噪声线"
               + (st.channelPick.allNoisy ? "，全部像噪声" : "") + "）");
    }

    /* ── 导出 / 打印 ── */
    if (a.json) {
        const std::string text = session->exportJson(a.limit);
        if (a.out == Out::Stdout || a.out == Out::File) {
            // `--json` 也认 --out：给了路径就落盘，`-` 就打到标准输出
            if (a.out == Out::Stdout) { writeAll(stdout, text); writeAll(stdout, "\n"); }
            else {
                std::string why;
                if (!pdscope::writeWholeFile(a.outPath, text + "\n", why)) {
                    sayErr("[PDScope-NG] " + why);
                    return 1;
                }
                sayErr("  输出    " + a.outPath);
            }
        } else {
            writeAll(stdout, text);
            writeAll(stdout, "\n");
        }
        return 0;
    }

    if (a.csv) {
        // BOM 策略：显式开关优先；否则落盘要、管道不要。
        bool wantBom;
        if (a.bom >= 0) wantBom = (a.bom == 1);
        else if (a.out == Out::Stdout) wantBom = stdoutIsFile();
        else wantBom = true;

        // 命令行导出**不过筛选**：与 JS 基线的 `tools/cli.js --csv`（直接喂全部 packets）
        // 逐字节对齐，也不会出现「表格里 88 条、导出来只有 44 行」这种自相矛盾。
        // 界面「另存为」走的是 exportCsv 的默认参数（导出当前视图）。
        const std::string csvText = session->exportCsv(a.limit, wantBom, /*filtered=*/false);
        // 导出的行数 = 全部报文条数，再被 --limit 截断
        const uint64_t rows = (a.limit > 0)
            ? std::min<uint64_t>(a.limit, session->packetCount())
            : session->packetCount();

        if (a.out == Out::Stdout) {
            writeAll(stdout, csvText);
            writeAll(stdout, "\n");
            std::fflush(stdout);
            sayErr("  输出    标准输出 · " + fmtSize(csvText.size()));
        } else {
            std::string path = a.outPath;
            if (a.out == Out::Auto) {
                path = dirName(a.input) + "/" + session->defaultCsvName();
            }
            std::string why;
            if (!pdscope::writeWholeFile(path, csvText, why)) {
                sayErr("[PDScope-NG] " + why);
                return 1;
            }
            sayErr("  输出    " + path + " · " + fmtSize(csvText.size()));
        }
        sayErr("  报文    " + std::to_string(st.packetCount) + " 条 → 导出 " + std::to_string(rows) + " 行");
        sayErr("  时长    " + fmtDuration(st.durationSec));
        return 0;
    }

    printTable(*session, a.limit, false);
    return 0;
}

/* ── 入口 ────────────────────────────────────────────────────────────
 *
 * Windows 必须用 `wmain`。MSVC 的 `main(argc, argv)` 给的是**进程 ANSI 代码页**
 * 编码的字节：中文路径在 `fopen`/`_wfopen` 之外一路"看着能用"，但只要换个调用
 * 场景（重定向、别的语言绑定）立刻现形。`wmain` 拿到的是原始 UTF-16，这里转成
 * UTF-8，正好对上核心的「路径一律 UTF-8」约定。
 */
#if defined(_WIN32)

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> raw;
    if (argc > 1) raw.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) raw.push_back(pdscope::wideToUtf8(argv[i]));
    return runCli(std::move(raw));
}

#else

int main(int argc, char** argv) {
    return runCli(std::vector<std::string>(argv + 1, argv + argc));
}

#endif
