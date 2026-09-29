// test_main.cpp — 测试运行器
//
// 运行方式：
//   pdscope-tests            跑全部
//   pdscope-tests csv        只跑名字里含 "csv" 的用例
//
// 退出码 = 失败用例数（0 = 全通过）。跳过不算失败，但会打出来 —— 用例被跳过时
// 它的断言一条都没验证过，把「跳过」说成「通过」是自检最容易骗自己的地方。

#include "test.h"

#include <cstring>

namespace test {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

namespace {
int g_failures = 0;       // 当前用例的失败数
int g_skips = 0;          // 当前用例的跳过数
const char* g_current = "";
}  // namespace

void reportFailure(const std::string& what, const char* file, int line) {
    ++g_failures;
    std::cout << "    ✗ " << what << "\n        at " << file << ":" << line << "\n";
}

void markSkipped(const std::string& why) { ++g_skips; std::cout << "    ~ 跳过：" << why << "\n"; }

void checkNear(double a, double b, double eps, const char* ae, const char* be,
               const char* file, int line) {
    const double d = (a > b) ? (a - b) : (b - a);
    if (!(d <= eps)) {
        reportFailure(std::string("期望 ") + ae + " ≈ " + be + "（容差 " + dbg(eps) + "），实际 "
                          + dbg(a) + " vs " + dbg(b) + "，相差 " + dbg(d), file, line);
    }
}

int runAll(const char* filter) {
    // 逐行刷出：崩溃时缓冲里的字会全丢，而崩溃现场正是最需要看的东西
    std::cout << std::unitbuf;

    int failedCases = 0;
    int ran = 0;
    int skippedCases = 0;
    std::vector<std::string> failedNames;

    for (const Case& c : registry()) {
        if (filter && *filter && c.name.find(filter) == std::string::npos) continue;
        ++ran;
        g_failures = 0;
        g_skips = 0;
        g_current = c.name.c_str();
        std::cout << "  · " << c.name << "\n";
        try {
            c.fn();
        } catch (const std::exception& e) {
            reportFailure(std::string("未捕获的异常：") + e.what(), __FILE__, __LINE__);
        } catch (...) {
            reportFailure("未捕获的未知异常", __FILE__, __LINE__);
        }
        if (g_failures > 0) {
            ++failedCases;
            failedNames.push_back(c.name);
        } else if (g_skips > 0) {
            ++skippedCases;
        }
    }

    std::cout << "\n";
    if (failedCases == 0) {
        std::cout << "通过：" << ran << " 个用例，0 失败";
        if (skippedCases > 0) std::cout << "，" << skippedCases << " 个用例有跳过项";
        std::cout << "\n";
        // ⚠ 有跳过就明确说出来：跳过意味着那些断言根本没跑，别让「全绿」看起来像全覆盖。
        if (skippedCases > 0) {
            std::cout << "⚠ 有 " << skippedCases << " 个用例跳过了部分检查（多为缺样例文件）\n";
        }
        return 0;
    }
    std::cout << "失败：" << failedCases << " / " << ran << " 个用例\n";
    for (const std::string& n : failedNames) std::cout << "  ✗ " << n << "\n";
    return failedCases;
}

}  // namespace test

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    return test::runAll(filter);
}
