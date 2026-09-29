// shell.cpp — 桌面外壳的实现（Windows 独有）
//
// ⚠ 这个文件**不依赖 Flutter，也不依赖 runner 里的其它文件**（只用 Win32 + 标准库）。
//   理由是它要能被 `pdscope-tests` 独立编进去跑单测：外壳里最要命的是几段
//   「错了也不报错」的逻辑（参数过滤、WM_COPYDATA 编解码、中文菜单标签），
//   而这些恰好是能不起窗口就验的。真正在窗口消息里来回的部分见 doc/desktop.md。
#include "shell.h"

#include <string>
#include <vector>

namespace pdscope_shell {

// 窗口类名。**必须与模板默认值不一样**。
//
// 模板给的是 `FLUTTER_RUNNER_WIN32_WINDOW` —— 那个名字**每个 Flutter Windows 应用
// 都在用**。而「已经有实例在跑就把文件转交给它」是靠 FindWindowW(类名) 找窗口的，
// 用模板默认名的话，机器上随便哪个 Flutter 应用开着，PDScope 就会把路径送给**它**，
// 然后自己默默退出：用户看到的是「PDScope 没打开，别的程序也没反应」。
//
// win32_window.cpp 用的是同一份定义（include 本头文件），不另写字面量。
const wchar_t kWindowClassName[] = L"PDScope.MainWindow";

namespace {

// 菜单栏里各下拉菜单的位置（BuildMenu 里按这个顺序 append）。
enum SubMenu { kSubFile = 0, kSubView = 1, kSubHelp = 2 };

// 一份路径：既不能是目录（拖进来一个文件夹不该当成抓包），也不能不存在。
bool IsOpenableFile(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 命令行参数里像开关的那些（`-Embedding`、`/help`）不当作路径。
bool LooksLikeSwitch(const std::wstring& arg) {
  return !arg.empty() && (arg[0] == L'-' || arg[0] == L'/');
}

}  // namespace

// 这两个转换自己写一份，而不是用 runner 的 `utils.h`：那个文件拉着 Flutter 引擎
// （`FlutterDesktopResyncOutputStreams`），带上它这个文件就没法单独编进单测了。
std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};

  const int target = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           utf8.c_str(),
                                           static_cast<int>(utf8.size()),
                                           nullptr, 0);
  if (target <= 0) return {};

  std::wstring wide(static_cast<size_t>(target), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            utf8.c_str(),
                                            static_cast<int>(utf8.size()),
                                            wide.data(), target);
  if (written == 0) return {};
  return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return {};

  const int target = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           wide.c_str(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (target <= 0) return {};

  std::string utf8(static_cast<size_t>(target), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            wide.c_str(),
                                            static_cast<int>(wide.size()),
                                            utf8.data(), target, nullptr, nullptr);
  if (written == 0) return {};
  return utf8;
}

std::vector<std::string> FilterOpenablePaths(
    const std::vector<std::wstring>& candidates) {
  std::vector<std::string> out;
  for (const auto& arg : candidates) {
    if (LooksLikeSwitch(arg)) continue;
    if (!IsOpenableFile(arg)) continue;
    out.push_back(WideToUtf8(arg));
  }
  return out;
}

std::vector<std::string> PathsFromCommandLine() {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv == nullptr) return {};

  std::vector<std::wstring> candidates;
  // argv[0] 是可执行文件自己，跳过。
  for (int i = 1; i < argc; ++i) {
    candidates.emplace_back(argv[i]);
  }
  ::LocalFree(argv);

  return FilterOpenablePaths(candidates);
}

std::string EncodePathPayload(const std::vector<std::string>& paths) {
  std::string payload;
  for (const auto& p : paths) {
    payload += p;
    // Windows 的路径里不可能出现换行，拿它当分隔符不会歧义。
    payload.push_back('\n');
  }
  return payload;
}

bool ForwardToRunningInstance(const std::vector<std::string>& paths) {
  if (paths.empty()) return false;

  HWND target = ::FindWindowW(kWindowClassName, nullptr);
  if (target == nullptr) return false;

  const std::string payload = EncodePathPayload(paths);

  COPYDATASTRUCT data{};
  data.dwData = kCopyDataMagic;
  // 带上结尾的 NUL：接收方不需要再靠长度算边界。
  data.cbData = static_cast<DWORD>(payload.size() + 1);
  data.lpData = const_cast<char*>(payload.c_str());

  // 同步送达（系统会把数据复制到接收进程），返回时对方已经拿到全部路径。
  ::SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
  // 顺带把已有窗口提到前面 —— 用户刚做了「用 PDScope 打开」，焦点应该跟过去。
  ::SetForegroundWindow(target);
  return true;
}

bool PathsFromCopyData(const COPYDATASTRUCT* data, std::vector<std::string>* out) {
  out->clear();
  if (data == nullptr || data->dwData != kCopyDataMagic) return false;
  if (data->lpData == nullptr || data->cbData == 0) return true;  // 空列表也是我们的消息

  const auto* text = static_cast<const char*>(data->lpData);
  const size_t size = static_cast<size_t>(data->cbData);
  size_t begin = 0;
  for (size_t i = 0; i < size; ++i) {
    if (text[i] != '\n') continue;
    if (i > begin) out->emplace_back(text + begin, i - begin);
    begin = i + 1;
  }
  // 结尾那个 NUL 不算一条路径。
  if (size > begin && text[begin] != '\0') {
    out->emplace_back(text + begin, size - begin);
  }
  return true;
}

std::vector<std::string> PathsFromDrop(HDROP drop) {
  std::vector<std::wstring> candidates;
  if (drop == nullptr) return {};

  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
    if (len == 0) continue;
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    if (::DragQueryFileW(drop, i, buffer.data(), len + 1) == 0) continue;
    buffer.resize(len);
    candidates.push_back(std::move(buffer));
  }

  // 过滤规则与命令行那条路完全一致（目录、不存在的都丢）。
  return FilterOpenablePaths(candidates);
}

HMENU BuildMenu() {
  HMENU bar = ::CreateMenu();
  if (bar == nullptr) return nullptr;

  HMENU file = ::CreatePopupMenu();
  ::AppendMenuW(file, MF_STRING, kMenuOpen, L"打开抓包…\tCtrl+O");
  ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(file, MF_STRING, kMenuExportCsv, L"导出 CSV（当前筛选结果）");
  ::AppendMenuW(file, MF_STRING, kMenuExportJson, L"导出 JSON（全部报文）");
  ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(file, MF_STRING, kMenuExit, L"退出");
  ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");

  HMENU view = ::CreatePopupMenu();
  ::AppendMenuW(view, MF_STRING, kMenuToggleTheme, L"切换明暗主题\tT");
  ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(view, MF_STRING, kMenuToggleFilters, L"显示筛选栏");
  ::AppendMenuW(view, MF_STRING, kMenuToggleDetail, L"显示详情面板");
  ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(view, MF_STRING, kMenuResetLayout, L"重置布局");
  ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图(&V)");

  HMENU help = ::CreatePopupMenu();
  ::AppendMenuW(help, MF_STRING, kMenuAbout, L"关于 PDScope");
  ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");

  return bar;
}

void ApplyMenuState(HWND window, HMENU menu, const MenuState& state) {
  if (menu == nullptr) return;

  HMENU file = ::GetSubMenu(menu, kSubFile);
  if (file != nullptr) {
    const UINT how = MF_BYCOMMAND | (state.has_document ? MF_ENABLED : MF_GRAYED);
    ::EnableMenuItem(file, kMenuExportCsv, how);
    ::EnableMenuItem(file, kMenuExportJson, how);
  }

  HMENU view = ::GetSubMenu(menu, kSubView);
  if (view != nullptr) {
    ::CheckMenuItem(view, kMenuToggleFilters,
                    MF_BYCOMMAND |
                        (state.filters_shown ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(view, kMenuToggleDetail,
                    MF_BYCOMMAND |
                        (state.detail_shown ? MF_CHECKED : MF_UNCHECKED));
  }

  // 没挂到窗口上的菜单（单测里就是这样）不需要重画。
  if (window != nullptr) ::DrawMenuBar(window);
}

}  // namespace pdscope_shell
