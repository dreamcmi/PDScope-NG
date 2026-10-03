// shell.h — 桌面外壳那一层（Windows 独有）
//
// 这一层只做四件**跟操作系统有关**的事，完全不碰解析：
//   1. 从命令行取抓包路径（文件关联 / 拖到图标上 / 直接 `PDScope.exe x.atkcc`）
//   2. 已经有实例在跑时，把路径转交给它，本进程直接退出
//   3. 让窗口接受文件拖放（客户区被 Flutter 的子窗口盖着，所以要连子窗口一起注册）
//   4. 中文菜单
//
// 交给 Dart 的**只有路径**：是不是抓包、是 ATK-C 还是 POWER-Z，仍然由核心按内容判定。
// 外壳不需要知道这次打开的是哪一种抓包。
#ifndef RUNNER_SHELL_H_
#define RUNNER_SHELL_H_

#include <windows.h>

// 顺序不能反：shellapi.h 需要 windows.h 先铺好（EXTERN_C 等）。
// ⚠ 必须显式包含 —— 核心工程带 `WIN32_LEAN_AND_MEAN`，那种情况下 windows.h
//   不会捎上 shellapi.h，HDROP / DragQueryFile 就成了未声明的标识符。
#include <shellapi.h>

#include <string>
#include <vector>

namespace pdscope_shell {

// 主窗口的类名，与 win32_window.cpp 里注册的保持一致。
// 转交路径时靠它找已经在跑的那个窗口。
extern const wchar_t kWindowClassName[];

// 窗口之间转交路径用的 WM_COPYDATA 标识（'PSP1'）。
constexpr ULONG_PTR kCopyDataMagic = 0x50535031;

// 菜单命令 id。从 1001 起，避开系统占用的低位区段。
enum MenuCommand {
  kMenuOpen = 1001,
  kMenuConnect,  // 连接设备（实时采集）
  kMenuClose,
  kMenuCloseAll,
  kMenuExportCsv,
  kMenuExportJson,
  kMenuSearch,
  kMenuDense,
  kMenuToggleTheme,
  kMenuToggleFilters,
  kMenuToggleDetail,
  kMenuResetLayout,
  kMenuAbout,
  kMenuExit,
};

// 菜单项灰/亮与勾选状态。由 Dart 侧报上来（外壳不猜界面状态）。
struct MenuState {
  bool has_document = true;
  bool filters_shown = true;
  bool detail_shown = true;
};

// 命令行的参数里，看起来是「本机存在的文件」的那些（UTF-8）。
// 开关参数（以 - 或 / 开头）一律跳过。
std::vector<std::string> PathsFromCommandLine();

// 从候选参数里挑出能打开的路径（UTF-8）。
//
// 单独拎出来是为了**能不起窗口地测**：`PathsFromCommandLine` 只是它的壳，
// 而「开关、目录、不存在的路径都不该当成抓包」这条规则出错的症状是
// 「拖进去没反应」，现场什么都看不到。
std::vector<std::string> FilterOpenablePaths(const std::vector<std::wstring>& candidates);

// 把路径列表编成 WM_COPYDATA 的载荷（LF 分隔、带结尾 NUL）。
// 与 `PathsFromCopyData` 严格成对 —— 两边写岔了同样是「没反应」，没有报错。
std::string EncodePathPayload(const std::vector<std::string>& paths);

// UTF-8 ↔ UTF-16。Win32 要的字符串都是宽字符，而 C ABI 与 Dart 之间一律 UTF-8。
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// 把路径转交给已在运行的实例；没有实例在跑时返回 false。
//
// ⚠ 用 WM_COPYDATA 而不是自己造一段共享内存：系统会把数据复制到接收进程，
//   发送方在这里**同步等待**（SendMessage 的语义），所以本进程退出时对方一定拿全了。
bool ForwardToRunningInstance(const std::vector<std::string>& paths);

// 解析 WM_COPYDATA 带来的路径列表；不是我们约定的格式时返回 false。
bool PathsFromCopyData(const COPYDATASTRUCT* data, std::vector<std::string>* out);

// 从一次拖放里取出所有路径（调用方负责 DragFinish）。
std::vector<std::string> PathsFromDrop(HDROP drop);

// 造菜单栏。返回的 HMENU 归调用方，窗口销毁时 DestroyMenu。
HMENU BuildMenu();

// 按 Dart 报上来的状态更新菜单项的启用与勾选（顺带重画菜单栏）。
void ApplyMenuState(HWND window, HMENU menu, const MenuState& state);

}  // namespace pdscope_shell

#endif  // RUNNER_SHELL_H_
