#include "util.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace pdscope {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (b < e && ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string toFixedStr(double v, int digits) {
    if (digits < 0) digits = 0;
    if (digits > 100) digits = 100;

    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";

    const bool neg = v < 0;
    const double a = std::fabs(v);

    // 展开到 digits+30 位。位数远多于需要，因此第 digits 位（哨兵位）是精确的，
    // 不会因「先舍到更少位」而二次舍入。
    char buf[512];
    const int prec = digits + 30;
    int n = std::snprintf(buf, sizeof(buf), "%.*f", prec, a);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        // 兜底（理论上到不了）：退回固定精度
        n = std::snprintf(buf, sizeof(buf), "%.*f", digits, a);
        return (neg ? "-" : "") + std::string(buf, static_cast<size_t>(n));
    }
    std::string s(buf, static_cast<size_t>(n));

    size_t dot = s.find('.');
    std::string ipart = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string fpart = (dot == std::string::npos) ? std::string() : s.substr(dot + 1);

    // 补齐到 digits 位（值本身可能小数位更少）
    while (static_cast<int>(fpart.size()) < digits) fpart.push_back('0');
    std::string keep = fpart.substr(0, static_cast<size_t>(digits));
    char guard = (static_cast<int>(fpart.size()) > digits) ? fpart[static_cast<size_t>(digits)] : '0';

    // 哨兵位 ≥ '5' 就进位：口径是「精确等于 .5」也要进位（远离零），
    // 所以这里不必再区分「后面是否还有非零位」。
    if (guard >= '5') {
        int i = static_cast<int>(keep.size()) - 1;
        bool carry = true;
        while (i >= 0 && carry) {
            if (keep[static_cast<size_t>(i)] == '9') { keep[static_cast<size_t>(i)] = '0'; --i; }
            else { keep[static_cast<size_t>(i)] = static_cast<char>(keep[static_cast<size_t>(i)] + 1); carry = false; }
        }
        if (carry) {
            // 小数部分全部进位 → 整数部分 +1
            int j = static_cast<int>(ipart.size()) - 1;
            while (j >= 0 && carry) {
                if (ipart[static_cast<size_t>(j)] == '9') { ipart[static_cast<size_t>(j)] = '0'; --j; }
                else { ipart[static_cast<size_t>(j)] = static_cast<char>(ipart[static_cast<size_t>(j)] + 1); carry = false; }
            }
            if (carry) ipart.insert(ipart.begin(), '1');
        }
    }

    std::string out = ipart;
    if (digits > 0) {
        out.push_back('.');
        out += keep;
    }
    // 结果恰为 0 且原本为负 → "-0.000"（保留符号位）
    if (neg) out.insert(out.begin(), '-');
    return out;
}

std::string numToStr(double v) {
    if (!std::isfinite(v)) {
        if (std::isnan(v)) return "NaN";
        return v > 0 ? "Infinity" : "-Infinity";
    }
    // 数学上是整数（且在可表示范围内）就当整数打印
    if (std::floor(v) == v && std::fabs(v) < 1e21) {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(v));
        return std::string(buf, static_cast<size_t>(r.ptr - buf));
    }
    // Math.round(v * 1000) / 1000 后再按最短往返表示打印
    double r = std::round(v * 1000.0) / 1000.0;
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), r);
    return std::string(buf, static_cast<size_t>(res.ptr - buf));
}

std::string hexU(uint64_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<size_t>(digits > 0 ? digits : 16));
    for (int i = (digits > 0 ? digits : 16) - 1; i >= 0; --i) {
        out.push_back(H[(v >> (4 * i)) & 0xF]);
    }
    return out;
}

std::string hexVar(uint64_t v) {
    if (v == 0) return "0";
    static const char* H = "0123456789ABCDEF";
    char buf[17];
    int n = 0;
    while (v != 0) { buf[n++] = H[v & 0xF]; v >>= 4; }
    std::string out;
    out.reserve(static_cast<size_t>(n));
    while (n > 0) out.push_back(buf[--n]);
    return out;
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char* H = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(H[p[i] >> 4]);
        out.push_back(H[p[i] & 0xF]);
    }
    return out;
}

std::string asciiOf(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 0) break;
        s.push_back((p[i] >= 0x20 && p[i] <= 0x7E) ? static_cast<char>(p[i]) : '.');
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

std::string fmtRate(double hz) {
    // ⚠ 必须走 toFixedStr —— 界面、CSV、CLI 三处显示同一个采样率时，
    // 数字要逐字符一致。用 printf 的 %.2f 会在「恰好 .5」这类值上给出不同的结果。
    if (hz >= 1e6) return toFixedStr(hz / 1e6, 2) + " MHz";
    if (hz >= 1e3) return toFixedStr(hz / 1e3, 2) + " kHz";
    return numToStr(std::round(hz)) + " Hz";
}

std::string csvClock(double ms) {
    if (!std::isfinite(ms)) ms = 0;
    const double s = ms / 1000.0;
    long long h = static_cast<long long>(std::floor(s / 3600.0));
    long long m = static_cast<long long>(std::floor(std::fmod(s, 3600.0) / 60.0));
    double sec = std::fmod(s, 60.0);
    if (sec < 0) sec += 60.0;

    std::string secStr = toFixedStr(sec, 3);
    while (secStr.size() < 6) secStr.insert(secStr.begin(), '0');

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%s", h, m, secStr.c_str());
    return buf;
}

std::string toValidUtf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t need = 0;
        if (c < 0x80) need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;

        bool ok = need > 0 && i + need <= n;
        if (ok && need > 1) {
            for (size_t k = 1; k < need; ++k) {
                if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            }
        }
        if (ok) {
            out.append(reinterpret_cast<const char*>(p + i), need);
            i += need;
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
        }
    }
    return out;
}

/* ── 文件路径（UTF-8 → 平台）──────────────────────────────────────── */

#if defined(_WIN32)

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          &out[0], n, nullptr, nullptr);
    return out;
}

std::FILE* openFileForRead(const std::string& path_utf8) {
    const std::wstring w = utf8ToWide(path_utf8);
    return w.empty() ? nullptr : ::_wfopen(w.c_str(), L"rb");
}

std::FILE* openFileForWrite(const std::string& path_utf8) {
    const std::wstring w = utf8ToWide(path_utf8);
    return w.empty() ? nullptr : ::_wfopen(w.c_str(), L"wb");
}

#else

std::FILE* openFileForRead(const std::string& path_utf8) {
    return std::fopen(path_utf8.c_str(), "rb");
}

std::FILE* openFileForWrite(const std::string& path_utf8) {
    return std::fopen(path_utf8.c_str(), "wb");
}

#endif

bool readWholeFile(const std::string& path_utf8, Bytes& out, std::string& why) {
    std::FILE* f = openFileForRead(path_utf8);
    if (!f) { why = "无法打开文件：" + path_utf8; return false; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    if (n < 0) { std::fclose(f); why = "无法获取文件大小：" + path_utf8; return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    const size_t got = n > 0 ? std::fread(out.data(), 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    if (got != static_cast<size_t>(n)) { why = "读文件不完整：" + path_utf8; return false; }
    return true;
}

bool writeWholeFile(const std::string& path_utf8, const std::string& text, std::string& why) {
    std::FILE* f = openFileForWrite(path_utf8);
    if (!f) { why = "建不了输出文件：" + path_utf8; return false; }
    const size_t wrote = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
    const bool okClose = std::fclose(f) == 0;
    if (wrote != text.size() || !okClose) { why = "写 " + path_utf8 + " 出错"; return false; }
    return true;
}

}  // namespace pdscope
