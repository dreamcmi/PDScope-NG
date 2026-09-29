#include "flutter_window.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <optional>
#include <utility>

#include "flutter/generated_plugin_registrant.h"
#include "shell.h"

namespace {

// 挂在 Flutter 子窗口上的属性名：从子窗口的消息过程找回宿主对象。
constexpr const wchar_t kOwnerProp[] = L"PDScopeFlutterWindowOwner";

// 通道名。Dart 侧同名，两边只靠它对齐。
constexpr const char kShellChannel[] = "pdscope/shell";

bool ReadBool(const flutter::EncodableMap& map, const char* key, bool fallback) {
  const auto it = map.find(flutter::EncodableValue(std::string(key)));
  if (it == map.end()) return fallback;
  const auto* value = std::get_if<bool>(&it->second);
  return value == nullptr ? fallback : *value;
}

std::string ReadString(const flutter::EncodableMap& map, const char* key,
                       const std::string& fallback) {
  const auto it = map.find(flutter::EncodableValue(std::string(key)));
  if (it == map.end()) return fallback;
  const auto* value = std::get_if<std::string>(&it->second);
  return value == nullptr ? fallback : *value;
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

void FlutterWindow::SetPendingOpenPaths(std::vector<std::string> paths) {
  for (auto& p : paths) {
    pending_paths_.push_back(std::move(p));
  }
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  InstallChannelHandler();

  // 中文菜单栏。命令 id 见 shell.h，点击后经通道转给 Dart 处理 ——
  // 外壳不自己实现「打开文件」这类动作，否则界面和菜单会各有一套行为。
  menu_ = pdscope_shell::BuildMenu();
  if (menu_ != nullptr) {
    ::SetMenu(GetHandle(), menu_);
  }

  // 拖放要注册**两个**窗口：拖到客户区时系统把 WM_DROPFILES 发给光标底下的
  // Flutter 子窗口（顶层窗口收不到），拖到标题栏/边框才发给顶层窗口。
  ::DragAcceptFiles(GetHandle(), TRUE);
  view_window_ = flutter_controller_->view()->GetNativeWindow();
  if (view_window_ != nullptr) {
    ::DragAcceptFiles(view_window_, TRUE);
    // 先把宿主挂上去，再换消息过程 —— 反过来会让早到的消息找不到宿主。
    ::SetPropW(view_window_, kOwnerProp, this);
    view_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        view_window_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&FlutterWindow::ViewProc)));
  }

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  // 把子窗口的消息过程还回去，别留下一个指向将要销毁对象的钩子。
  if (view_window_ != nullptr && ::IsWindow(view_window_)) {
    ::RemovePropW(view_window_, kOwnerProp);
    if (view_proc_ != nullptr) {
      ::SetWindowLongPtrW(view_window_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(view_proc_));
    }
  }
  view_window_ = nullptr;
  view_proc_ = nullptr;

  // 通道的处理器挂在引擎的 messenger 上，必须先摘掉再放引擎 ——
  // 反过来的话，引擎收尾期间的一次回调就会打到半死的对象上。
  if (channel_) {
    channel_->SetMethodCallHandler(nullptr);
    channel_.reset();
  }

  if (menu_ != nullptr) {
    ::DestroyMenu(menu_);
    menu_ = nullptr;
  }

  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

void FlutterWindow::InstallChannelHandler() {
  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), kShellChannel,
      &flutter::StandardMethodCodec::GetInstance());

  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) {
        const std::string& method = call.method_name();

        // Dart 说「通道接好了，可以送文件了」。
        if (method == "ready") {
          dart_ready_ = true;
          FlushPendingPaths();
          result->Success();
          return;
        }

        // Dart 报上来的界面状态：窗口标题、菜单项的灰/亮与勾选。
        if (method == "shellState") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (args != nullptr) {
            pdscope_shell::MenuState state;
            state.has_document = ReadBool(*args, "hasDoc", state.has_document);
            state.filters_shown = ReadBool(*args, "filters", state.filters_shown);
            state.detail_shown = ReadBool(*args, "detail", state.detail_shown);
            pdscope_shell::ApplyMenuState(GetHandle(), menu_, state);

            // 标题带上当前抓包：任务栏与 Alt+Tab 里才看得出开着哪一份。
            const std::string title = ReadString(*args, "title", std::string());
            if (!title.empty()) {
              ::SetWindowTextW(GetHandle(), pdscope_shell::Utf8ToWide(title).c_str());
            }
          }
          result->Success();
          return;
        }

        result->NotImplemented();
      });
}

LRESULT
FlutterWindow::ViewProc(HWND window, UINT const message, WPARAM const wparam,
                        LPARAM const lparam) noexcept {
  auto* self = reinterpret_cast<FlutterWindow*>(::GetPropW(window, kOwnerProp));

  if (message == WM_DROPFILES) {
    if (self != nullptr) {
      self->HandleDrop(reinterpret_cast<HDROP>(wparam));
    } else {
      // 宿主已经走了（窗口正在销毁）—— 至少把系统分配的那块内存还回去。
      ::DragFinish(reinterpret_cast<HDROP>(wparam));
    }
    return 0;
  }

  if (self != nullptr && self->view_proc_ != nullptr) {
    return ::CallWindowProcW(self->view_proc_, window, message, wparam, lparam);
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // 外壳自己的消息先处理：这三个跟 Flutter 处理的那批（键鼠、尺寸、DPI）不相交。
  switch (message) {
    case WM_DROPFILES:
      HandleDrop(reinterpret_cast<HDROP>(wparam));
      return 0;

    case WM_COPYDATA: {
      std::vector<std::string> paths;
      if (pdscope_shell::PathsFromCopyData(
              reinterpret_cast<const COPYDATASTRUCT*>(lparam), &paths)) {
        SendPaths(paths);
        return TRUE;
      }
      break;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id >= pdscope_shell::kMenuOpen && id <= pdscope_shell::kMenuExit) {
        HandleMenuCommand(id);
        return 0;
      }
      break;
    }

    default:
      break;
  }

  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                     lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      if (flutter_controller_) {
        flutter_controller_->engine()->ReloadSystemFonts();
      }
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

void FlutterWindow::HandleDrop(HDROP drop) {
  SendPaths(pdscope_shell::PathsFromDrop(drop));
  // DragQueryFile 拿到的这块内存归系统分配，不管用不用得上都要还。
  ::DragFinish(drop);
}

void FlutterWindow::HandleMenuCommand(int id) {
  if (id == pdscope_shell::kMenuExit) {
    ::PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
    return;
  }

  const char* command = nullptr;
  switch (id) {
    case pdscope_shell::kMenuOpen:
      command = "openFile";
      break;
    case pdscope_shell::kMenuExportCsv:
      command = "exportCsv";
      break;
    case pdscope_shell::kMenuExportJson:
      command = "exportJson";
      break;
    case pdscope_shell::kMenuToggleTheme:
      command = "toggleTheme";
      break;
    case pdscope_shell::kMenuToggleFilters:
      command = "toggleFilters";
      break;
    case pdscope_shell::kMenuToggleDetail:
      command = "toggleDetail";
      break;
    case pdscope_shell::kMenuResetLayout:
      command = "resetLayout";
      break;
    case pdscope_shell::kMenuAbout:
      command = "about";
      break;
    default:
      return;
  }

  if (!channel_) return;
  channel_->InvokeMethod(
      "command", std::make_unique<flutter::EncodableValue>(std::string(command)));
}

void FlutterWindow::SendPaths(const std::vector<std::string>& paths) {
  if (paths.empty()) return;

  // Dart 还没准备好：**攒着，不能丢**。命令行打开时它一定还没准备好。
  if (!dart_ready_ || !channel_) {
    for (const auto& p : paths) {
      pending_paths_.push_back(p);
    }
    return;
  }

  flutter::EncodableList list;
  list.reserve(paths.size());
  for (const auto& p : paths) {
    list.emplace_back(p);
  }
  channel_->InvokeMethod("openFiles",
                         std::make_unique<flutter::EncodableValue>(list));
}

void FlutterWindow::FlushPendingPaths() {
  if (pending_paths_.empty()) return;
  std::vector<std::string> queued;
  queued.swap(pending_paths_);
  SendPaths(queued);
}
