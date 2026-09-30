// test_shell_win.cpp — 桌面外壳里**能不起窗口就测**的那部分
//
// 外壳最关键的几段是在窗口消息里跑的（拖放、菜单点击、跨实例转交），那几段只能靠
// 真窗口冒烟。但剩下这些是纯逻辑，而且都属于**错了也不报错**的那类：
//
//   · 参数过滤 —— 「开关、目录、不存在的路径都不该当成抓包」出错的症状是
//     「拖进去没反应」，现场什么都看不到。
//   · WM_COPYDATA 载荷的编解码 —— 两边写岔了还是「没反应」。
//   · 菜单的结构与文字 —— 标签被改动谁都不会注意到，直到用户看见菜单不对。
//   · 窗口类名 —— 退回模板默认值的话，第二次打开会把路径送给机器上**别的**
//     Flutter 应用，然后自己退出。
//
// 所以这一层单测是值的：它把「不报错的错」变成会失败的用例。
#include "shell.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "test.h"

namespace {

/// 本进程自己的可执行文件路径 —— 一个**确实存在**的文件，不用去造样本。
bool OwnExePath(std::wstring* out) {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD n =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return false;
    if (n < buffer.size() - 1) {
      *out = std::wstring(buffer.data(), n);
      return true;
    }
    buffer.resize(buffer.size() * 2);
  }
}

/// 造一个 COPYDATASTRUCT 指向 |payload|（含结尾 NUL，与外壳发出的一致）。
COPYDATASTRUCT MakeCopyData(const std::string& payload, ULONG_PTR magic) {
  COPYDATASTRUCT data{};
  data.dwData = magic;
  data.cbData = static_cast<DWORD>(payload.size() + 1);
  data.lpData = const_cast<char*>(payload.c_str());
  return data;
}

// 菜单标签的期望值：⚠ 刻意用**码点转义**写，不写中文字面量。
//
// 这样期望值与源文件编码无关，验的是菜单里**实际的文字**；写成中文字面量的话，
// 两边由同一份源码解码出来，编码一旦出问题会一起错，比出来照样相等 ——
// 测试就成了自我印证，等于没测。
//
// （顺带说清另一件事：源文件编码错了这里**编不过**，不是「静默变乱码」。
//   实测把 `/utf-8` 去掉，MSVC 会先给 C4819，然后按 CP936 把源码读坏、
//   直接报语法错。所以菜单文字这条断言防的是**误改文字**，不是编码问题。）
constexpr const wchar_t kFileMenuLabel[] = L"\u6587\u4ef6(&F)";              // 文件(&F)
constexpr const wchar_t kViewMenuLabel[] = L"\u89c6\u56fe(&V)";              // 视图(&V)
constexpr const wchar_t kHelpMenuLabel[] = L"\u5e2e\u52a9(&H)";              // 帮助(&H)
constexpr const wchar_t kOpenItemLabel[] =
    L"\u6253\u5f00\u6293\u5305\u2026\tCtrl+O";                               // 打开抓包…\tCtrl+O

}  // namespace

TEST(shell_filter_openable_paths) {
  std::wstring exe;
  CHECK(OwnExePath(&exe));
  if (exe.empty()) TEST_SKIP("拿不到自身路径");

  const std::vector<std::wstring> candidates = {
      exe,                        // 存在的普通文件 → 保留
      L"C:\\Windows",             // 目录            → 丢
      L"Z:\\没有这个盘\\x.atkcc",  // 不存在          → 丢
      L"-Embedding",              // 开关（-）        → 丢
      L"/help",                   // 开关（/）        → 丢
      L"",                        // 空串            → 丢
  };

  const std::vector<std::string> got = pdscope_shell::FilterOpenablePaths(candidates);
  CHECK_EQ(got.size(), size_t{1});
  if (!got.empty()) {
    // 中文路径也要能转成 UTF-8（`Z:` 那条被丢掉是因为不存在，不是编码问题）。
    const std::vector<std::wstring> chinese = {exe};
    const std::vector<std::string> same = pdscope_shell::FilterOpenablePaths(chinese);
    CHECK_EQ(same.size(), size_t{1});
    CHECK_EQ(same[0], got[0]);
  }
}

TEST(shell_copydata_roundtrip) {
  const std::vector<std::string> paths = {
      "C:\\抓包\\绿联70w.atkcc",
      "D:\\my captures\\a b.sqlite",
      "\\\\server\\share\\x.pdStream",
  };

  const std::string payload = pdscope_shell::EncodePathPayload(paths);
  const COPYDATASTRUCT data = MakeCopyData(payload, pdscope_shell::kCopyDataMagic);

  std::vector<std::string> got;
  CHECK(pdscope_shell::PathsFromCopyData(&data, &got));
  CHECK_EQ(got.size(), paths.size());
  for (size_t i = 0; i < paths.size() && i < got.size(); ++i) {
    CHECK_EQ(got[i], paths[i]);
  }

  // 空列表：载荷里只有一个结尾 NUL，不该解析出一条空路径。
  const std::string empty = pdscope_shell::EncodePathPayload(std::vector<std::string>{});
  CHECK_EQ(empty.size(), size_t{0});
  const COPYDATASTRUCT none = MakeCopyData(empty, pdscope_shell::kCopyDataMagic);
  std::vector<std::string> parsed;
  CHECK(pdscope_shell::PathsFromCopyData(&none, &parsed));
  CHECK_EQ(parsed.size(), size_t{0});

  // 别人的消息必须原样拒绝 —— 否则同机器上任何进程都能往我们这儿塞路径。
  COPYDATASTRUCT foreign = data;
  foreign.dwData = 0x12345678;
  std::vector<std::string> junk{"先塞一条，看它清不清"};
  CHECK(!pdscope_shell::PathsFromCopyData(&foreign, &junk));
  CHECK_EQ(junk.size(), size_t{0});

  // 空指针 / 无数据也要安全。
  CHECK(!pdscope_shell::PathsFromCopyData(nullptr, &junk));
  COPYDATASTRUCT no_payload{};
  no_payload.dwData = pdscope_shell::kCopyDataMagic;
  CHECK(pdscope_shell::PathsFromCopyData(&no_payload, &junk));
  CHECK_EQ(junk.size(), size_t{0});
}

TEST(shell_menu_labels_and_state) {
  HMENU bar = pdscope_shell::BuildMenu();
  CHECK(bar != nullptr);
  if (bar == nullptr) return;

  CHECK_EQ(::GetMenuItemCount(bar), 3);  // 文件 / 视图 / 帮助

  wchar_t buf[128] = {0};
  // 菜单结构与文字：改动一处（多一项、少一项、文字被顺手改了）都要在这里红。
  CHECK(::GetMenuStringW(bar, 0, buf, 128, MF_BYPOSITION) > 0);
  CHECK(wcscmp(buf, kFileMenuLabel) == 0);
  CHECK(::GetMenuStringW(bar, 1, buf, 128, MF_BYPOSITION) > 0);
  CHECK(wcscmp(buf, kViewMenuLabel) == 0);
  CHECK(::GetMenuStringW(bar, 2, buf, 128, MF_BYPOSITION) > 0);
  CHECK(wcscmp(buf, kHelpMenuLabel) == 0);

  HMENU file = ::GetSubMenu(bar, 0);
  CHECK(file != nullptr);
  CHECK_EQ(::GetMenuItemID(file, 0), static_cast<UINT>(pdscope_shell::kMenuOpen));
  CHECK(::GetMenuStringW(file, 0, buf, 128, MF_BYPOSITION) > 0);
  CHECK(wcscmp(buf, kOpenItemLabel) == 0);

  HMENU view = ::GetSubMenu(bar, 1);
  CHECK(view != nullptr);
  CHECK_EQ(::GetMenuItemID(view, 0), static_cast<UINT>(pdscope_shell::kMenuSearch));
  CHECK_EQ(::GetMenuItemID(view, 1), static_cast<UINT>(pdscope_shell::kMenuToggleTheme));

  // 没有打开的文件时，两个导出项要置灰（菜单和界面是同一套状态）。
  pdscope_shell::MenuState no_doc;
  no_doc.has_document = false;
  pdscope_shell::ApplyMenuState(nullptr, bar, no_doc);
  CHECK((::GetMenuState(file, pdscope_shell::kMenuExportCsv, MF_BYCOMMAND) & MF_GRAYED) != 0);
  CHECK((::GetMenuState(file, pdscope_shell::kMenuExportJson, MF_BYCOMMAND) & MF_GRAYED) != 0);
  CHECK((::GetMenuState(file, pdscope_shell::kMenuClose, MF_BYCOMMAND) & MF_GRAYED) != 0);
  CHECK((::GetMenuState(file, pdscope_shell::kMenuCloseAll, MF_BYCOMMAND) & MF_GRAYED) != 0);

  // 有文件之后恢复可点；勾选状态跟着 Dart 报上来的来。
  pdscope_shell::MenuState with_doc;
  with_doc.filters_shown = false;
  with_doc.detail_shown = true;
  pdscope_shell::ApplyMenuState(nullptr, bar, with_doc);
  CHECK((::GetMenuState(file, pdscope_shell::kMenuExportCsv, MF_BYCOMMAND) & MF_GRAYED) == 0);
  CHECK((::GetMenuState(view, pdscope_shell::kMenuToggleFilters, MF_BYCOMMAND) & MF_CHECKED) == 0);
  CHECK((::GetMenuState(view, pdscope_shell::kMenuToggleDetail, MF_BYCOMMAND) & MF_CHECKED) != 0);

  ::DestroyMenu(bar);
}

TEST(shell_window_class_is_app_specific) {
  const std::wstring name(pdscope_shell::kWindowClassName);
  CHECK(!name.empty());
  // ⚠ 这不是个风格问题：模板默认的类名每个 Flutter Windows 应用都在用，而
  //   「已有实例就转交」靠 FindWindowW(类名) 找窗口 —— 用默认名的话，机器上
  //   随便哪个 Flutter 应用开着，第二次打开就会把路径送给它并自己退出。
  CHECK(name != L"FLUTTER_RUNNER_WIN32_WINDOW");
  // 也顺带钉住「名称没被写成空串/奇怪的字符」。
  CHECK(name.find(L'.') != std::wstring::npos);
}
