// test.h — 极简测试框架（不引第三方依赖，保证 CI 上零安装）
//
// 只用「注册 + 跑」两件事。故意不做参数化、不做 mock：
// 这些测试要验的是**解析口径**（字节怎么读、CRC 三态、CSV 逐字节），
// 拿真字节喂进去、把结果打出来比对，比任何框架都直接。

#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

/** 记一次失败（不抛异常：一个用例里的多条断言都要跑完，别在第一条就断掉）。 */
void reportFailure(const std::string& what, const char* file, int line);

/** 标记当前用例为「跳过」（缺少样例文件等环境原因，不是失败）。 */
void markSkipped(const std::string& why);

/** 跑全部（或名字含 `filter` 的那些）。返回失败用例数。 */
int runAll(const char* filter);

/** 把值变成可读文本。uint8_t 默认会被当字符打出来，所以单独接一下。 */
template <class T>
std::string dbg(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string dbg(uint8_t v) { return std::to_string(static_cast<int>(v)); }
inline std::string dbg(bool v) { return v ? "true" : "false"; }
inline std::string dbg(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}

template <class A, class B>
void checkEq(const A& a, const B& b, const char* ae, const char* be, const char* file, int line) {
    if (!(a == b)) {
        reportFailure(std::string("期望 ") + ae + " == " + be + "，实际 "
                          + dbg(a) + "  vs  " + dbg(b), file, line);
    }
}

template <class A, class B>
void checkNe(const A& a, const B& b, const char* ae, const char* be, const char* file, int line) {
    if (a == b) {
        reportFailure(std::string("期望 ") + ae + " != " + be + "，但两者都是 " + dbg(a),
                      file, line);
    }
}

void checkNear(double a, double b, double eps, const char* ae, const char* be,
               const char* file, int line);

}  // namespace test

#define TEST(name)                                                       \
    static void pdscope_test_##name();                                   \
    static test::Registrar pdscope_reg_##name(#name, pdscope_test_##name); \
    static void pdscope_test_##name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) test::reportFailure(std::string("不成立：") + #cond, __FILE__, __LINE__); \
    } while (0)

#define CHECK_EQ(a, b) test::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NE(a, b) test::checkNe((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) test::checkNear((a), (b), (eps), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr)                                               \
    do {                                                                 \
        bool pdscope_threw = false;                                      \
        try { expr; } catch (...) { pdscope_threw = true; }              \
        if (!pdscope_threw)                                              \
            test::reportFailure(std::string("期望抛错，但没有：") + #expr, __FILE__, __LINE__); \
    } while (0)

/** 用例内提前返回并记一次跳过。 */
#define TEST_SKIP(why)                                                   \
    do {                                                                 \
        test::markSkipped(why);                                          \
        return;                                                          \
    } while (0)
