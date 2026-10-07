#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/encodable_value.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "shell.h"
#include "win32_window.h"

// 一个把 Flutter 视图装进去、并接上桌面外壳能力的窗口。
//
// 与模板的差别只在「外壳那一层」：拖放、命令行/文件关联、别的实例转交进来的路径、
// 以及中文菜单。解析和界面仍然全在 Dart + 核心动态库里。
class FlutterWindow : public Win32Window {
public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject &project);
  virtual ~FlutterWindow();

  // 引擎起来前就收到的路径（命令行，或另一个实例转交过来）。
  //
  // ⚠ 不能在这儿直接塞给 Dart：命令行打开时 Dart 侧**一定**还没建立通道，
  //   这时候发出去的消息会石沉大海。先存着，等 Dart 自己说 ready 再一起发。
  void SetPendingOpenPaths(std::vector<std::string> paths);

protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

private:
  // Flutter 视图那个子窗口的消息过程。
  //
  // ⚠ 为什么必须挂这一级：客户区被 Flutter 的子窗口整个盖住，文件拖进来时
  //   系统把 WM_DROPFILES **发给光标底下的那个子窗口**，顶层窗口根本收不到。
  //   只给顶层窗口 DragAcceptFiles 的写法看着对，实际一次也收不到。
  static LRESULT CALLBACK ViewProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) noexcept;

  void InstallChannelHandler();
  void HandleDrop(HDROP drop);
  void HandleMenuCommand(int id);
  void SendPaths(const std::vector<std::string> &paths);
  void FlushPendingPaths();

  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // 与 Dart 通信的那条通道（pdscope/shell）。
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;

  // 原生菜单栏。
  HMENU menu_ = nullptr;

  // Flutter 视图的子窗口句柄，以及它原本的消息过程。
  HWND view_window_ = nullptr;
  WNDPROC view_proc_ = nullptr;

  // Dart 侧还没说 ready 时先攒在这里的路径。
  std::vector<std::string> pending_paths_;
  bool dart_ready_ = false;
  /** @brief 已向 Dart 请求收尾，重复关闭消息不重复提交请求。 */
  bool close_pending_ = false;
  /** @brief Dart 已完成设备收尾，此后允许销毁窗口。 */
  bool close_allowed_ = false;
};

#endif // RUNNER_FLUTTER_WINDOW_H_
