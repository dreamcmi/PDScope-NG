// util.h — 核心内部共用的基础件：错误类型、字节读取、字符串、JSON 别名。
//
// 本工程不使用 C++ 异常跨 ABI；但**核心内部**用异常传达解析失败，
// 由 abi.cpp 统一兜住并翻译成状态码 + 错误文本（见 pdscope::Error）。
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace pdscope {

using json = nlohmann::json;

// 字节缓冲的统一点：允许共享、允许切片
using Bytes = std::vector<uint8_t>;

/**
 * 解析/IO 失败。`status` 直接对应 C ABI 的 pdscope_status。
 * 消息面向人，会被原样交给调用方（含偏移、原因）。
 */
class Error : public std::runtime_error {
public:
    Error(int32_t status, const std::string& msg)
        : std::runtime_error(msg), status_(status) {}
    int32_t status() const noexcept { return status_; }
private:
    int32_t status_;
};

// 避免 util.h 反向依赖公共头：这里重复声明状态值，abi.cpp 有静态断言盯着两者一致。
// ⚠ 必须排在下面的 fail()/failFormat() **前面** —— 它们在函数体里用到这些枚举值，
//   而枚举在 C++ 里不是前向可用的。
enum : int32_t {
    PDSCOPE_STATUS_OK = 0,
    PDSCOPE_STATUS_ARGUMENT = -1,
    PDSCOPE_STATUS_IO = -2,
    PDSCOPE_STATUS_FORMAT = -3,
    PDSCOPE_STATUS_UNSUPPORTED = -4,
    PDSCOPE_STATUS_CANCELLED = -5,
    PDSCOPE_STATUS_STATE = -6,
    PDSCOPE_STATUS_MEMORY = -7,
    PDSCOPE_STATUS_INTERNAL = -8,
};

[[noreturn]] inline void fail(int32_t status, const std::string& msg) {
    throw Error(status, msg);
}

[[noreturn]] inline void failFormat(const std::string& msg) {
    throw Error(PDSCOPE_STATUS_FORMAT, msg);
}

/* ── 小端 / 大端读取 ──────────────────────────────────────────────── */

inline uint16_t rdU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rdU24LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16);
}
inline uint32_t rdU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rdU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint16_t rdU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t rdU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint64_t rdU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline double rdF64BE(const uint8_t* p) {
    uint64_t bits = rdU64BE(p);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

/* ── 有界切片 ─────────────────────────────────────────────────────── */

/** 取 [off, off+len)；越界即失败（所有文件偏移都必须先校验再读）。 */
inline const uint8_t* slice(const Bytes& b, size_t off, size_t len) {
    if (off > b.size() || len > b.size() - off) {
        failFormat("读取越界：偏移 " + std::to_string(off) + " 长度 " + std::to_string(len)
                   + " 超出文件大小 " + std::to_string(b.size()));
    }
    return b.data() + off;
}

/* ── 字符串 ───────────────────────────────────────────────────────── */

/** 去掉行尾 \r，并去掉首尾空白（ASCII）。 */
std::string trim(const std::string& s);

/** 按 \n 切行（同时兼容 \r\n）。 */
std::vector<std::string> splitLines(const std::string& text);

/** 小写化（ASCII）。 */
std::string lower(std::string s);

/**
 * 定点小数打印：按第 digits+1 位十进制数字 ≥5 就进位，方向是「远离零」。
 *
 * 口径（回归用例见 tests/test_csv.cpp）：
 *   (0.0625, 3)  → "0.063"     ← 精确的 .5 也要进位
 *   (-0.0625, 3) → "-0.063"    ← 方向是「远离零」，不是「取较大整数」
 *   (1.005, 2)   → "1.00"      ← 二进制里它其实小于 1.005，够不着 .5
 * 也就是「按第 digits+1 位十进制数字 ≥5 就进位、且先取绝对值」。
 *
 * 实现上先让 printf 展开到 digits+30 位再判定，避免只展开一位时发生
 * **二次舍入**（例如 0.014999 在 2 位下会被先舍成 0.015 从而误进位）。
 */
std::string toFixedStr(double v, int digits);

/** 数字 → 字符串，整数不带小数点、浮点最多 3 位。 */
std::string numToStr(double v);
inline std::string numToStr(int64_t v) { return std::to_string(v); }
inline std::string numToStr(uint64_t v) { return std::to_string(v); }

/** 定宽十六进制（大写、补零）。 */
std::string hexU(uint64_t v, int digits);

/**
 * **不补零**的大写十六进制（0 输出 `0`）。
 * ⚠ 别用 `hexU(v, 1)` 代替：`hexU` 是按 digits 定宽截断的，`0x1AB` 会只剩 `0xB`。
 */
std::string hexVar(uint64_t v);

/** 字节 → 连续大写十六进制（无分隔）。 */
std::string hexOf(const uint8_t* p, size_t n);

/** 字节 → 可打印 ASCII（不可打印位用 `.`，遇 0 截断，与 pdAscii 一致。 */
std::string asciiOf(const uint8_t* p, size_t n);

/** 百分比 / 速率等给界面看的短文本：`2.50 MHz` / `1.00 kHz` / `600 Hz`。 */
std::string fmtRate(double hz);

/** 自毫秒起算的 `hh:mm:ss.mmm`（小时不截断）。 */
std::string csvClock(double ms);

/* ── UTF-8 校验（把非法字节换成 U+FFFD 之外的原样保留，用于 JSON 安全）── */

/** 把任意字节串变成合法 UTF-8（非法序列替换为 U+FFFD）。 */
std::string toValidUtf8(const uint8_t* p, size_t n);

/* ── 文件路径：一律 UTF-8，由这里收口 ────────────────────────────────
 *
 * ABI 收的路径是 UTF-8（`pdscope.h` 写着 `path_utf8`），CLI 的 argv 也在入口
 * 统一转成 UTF-8。但 Windows 的 `fopen` / `_wfopen` 是按**进程 ANSI 代码页**
 * 解释窄字符串的 —— 直接喂 UTF-8 路径，只要路径里有一个汉字就必然打不开
 * （`C:\Users\张三\我的抓包.atkcc` 这种，桌面版选文件时到处都是）。
 * 所以在 Windows 上先转 UTF-16 再走宽字符 API，其余平台原样用窄字符。
 *
 * ⚠ 不要为了「省事」改回 `fopen(path.c_str(), ...)`。命令行下曾经看着能用，
 *   只是因为 MSVC 的 `main(argc, argv)` 拿到的本来就是 ANSI 字节（那本身是
 *   另一个 bug），并不代表 ABI 的 UTF-8 约定被满足了 —— DLL 的调用方
 *   （Flutter/Dart、其他语言绑定）传的一定是真 UTF-8。
 */

/** 以只读打开（路径 UTF-8）。失败返回 nullptr。 */
std::FILE* openFileForRead(const std::string& path_utf8);

/** 以「覆盖写」打开（路径 UTF-8）。失败返回 nullptr。 */
std::FILE* openFileForWrite(const std::string& path_utf8);

/** 读整个文件。失败时把面向人的原因写进 `why`。 */
bool readWholeFile(const std::string& path_utf8, Bytes& out, std::string& why);

/** 覆盖写整个文本。失败时把面向人的原因写进 `why`。 */
bool writeWholeFile(const std::string& path_utf8, const std::string& text, std::string& why);

#if defined(_WIN32)
/** UTF-8 → UTF-16。Windows 专有；无法转换时返回空串。 */
std::wstring utf8ToWide(const std::string& s);

/** UTF-16 → UTF-8。Windows 专有；无法转换时返回空串。
 *  用途：把 `wmain` 拿到的宽字符 argv 还原成 UTF-8，好兑现 ABI 的路径约定。 */
std::string wideToUtf8(const std::wstring& s);
#endif

}  // namespace pdscope
