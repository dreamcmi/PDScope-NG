#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include <utility>

#include "flutter_window.h"
#include "shell.h"
#include "utils.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  // 命令行里带的抓包文件（文件关联、把文件拖到图标上、直接敲路径）。
  //
  // 「用 PDScope 打开」第二次发生时，用户要的是**同一个窗口多一个标签**，
  // 不是再开一个进程 —— 所以先找已经在跑的那个实例，把路径交给它就退出。
  std::vector<std::string> open_paths = pdscope_shell::PathsFromCommandLine();
  if (pdscope_shell::ForwardToRunningInstance(open_paths)) {
    ::CoUninitialize();
    return EXIT_SUCCESS;
  }

  FlutterWindow window(project);
  // 命令行带来的路径先寄存在窗口里：这时候 Dart 侧的通道还没建立，
  // 直接发会丢。窗口在收到 Dart 的 ready 之后一起交出去。
  window.SetPendingOpenPaths(std::move(open_paths));

  Win32Window::Point origin(10, 10);
  Win32Window::Size size(1280, 720);
  if (!window.Create(L"PDScope", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
