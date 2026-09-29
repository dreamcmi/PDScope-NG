// shell.dart — 桌面外壳的接触面（拖放 / 文件关联 / 原生菜单）
//
// 外壳只做四件**跟操作系统有关**的事：接文件拖放、收命令行与文件关联送来的路径、
// 把「已经在跑的那个窗口」接上第二次打开、以及挂中文菜单。
// 它交给 Dart 的**只有路径** —— 是不是抓包、是 ATK-C 还是 POWER-Z，
// 仍然由核心按内容判定。
//
// ⚠ 没有外壳的平台（macOS/Linux 尚未实现、测试环境）**没有**这一层，
//   所以通道没人应答必须降级成「什么都没发生」，不能抛到界面上。
import 'dart:io';

import 'package:flutter/services.dart';

import 'workspace.dart';

/// 外壳命令的**唯一派发点**。
///
/// 界面自己订阅它，而不是让每个控件各自去挂通道 —— 菜单项、快捷键、
/// 以后的其它入口都从这里走，行为不会走岔。
class ShellCommands {
  final _queue = <String>[];
  final _listeners = <void Function(String)>[];

  /// 来了一条命令。**还没有订阅者时先存着** —— 窗口刚起来的瞬间就可能来一条。
  void add(String command) {
    if (_listeners.isEmpty) {
      _queue.add(command);
      return;
    }
    for (final listener in List.of(_listeners)) {
      listener(command);
    }
  }

  /// 订阅命令；返回取消订阅的函数。
  void Function() listen(void Function(String) onCommand) {
    _listeners.add(onCommand);
    if (_queue.isEmpty) return () => _listeners.remove(onCommand);

    final queued = List.of(_queue);
    _queue.clear();
    for (final command in queued) {
      onCommand(command);
    }
    return () => _listeners.remove(onCommand);
  }
}

class ShellBridge {
  ShellBridge._();

  /// 与 Windows 外壳约定的通道名，和 `runner/flutter_window.cpp` 里的一致。
  static const MethodChannel _channel = MethodChannel('pdscope/shell');

  static final ShellCommands commands = ShellCommands();

  static bool _attached = false;

  /// 接上外壳。**要在 runApp 之前调**：外壳那边（命令行参数、另一个实例
  /// 转交过来的路径）可能已经攒着等这一句 `ready` 了。
  static Future<void> attach(Workspace workspace) async {
    if (_attached) return;
    _attached = true;

    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'openFiles':
          // 拖放 / 文件关联 / 命令行都落到这里，走 `openFiles` 这一个入口
          // （每份都是新标签，坏文件只红自己那个标签）。
          final paths = (call.arguments as List).cast<String>();
          await workspace.openFiles(paths);
          return null;
        case 'command':
          commands.add(call.arguments as String);
          return null;
      }
      return null;
    });

    if (!Platform.isWindows) return;
    await _invoke('ready');
  }

  /// 把界面状态报给外壳：窗口标题、菜单项的灰/亮与勾选。
  ///
  /// 外壳**不猜**界面状态（它看不见 Flutter 的 widget 树），所以这件事必须由这边说。
  /// 标题带上当前抓包是有用的：任务栏和 Alt+Tab 里能看出开着哪一份。
  static Future<void> reportState({
    required bool hasDocument,
    required bool filtersShown,
    required bool detailShown,
    required String title,
  }) async {
    if (!Platform.isWindows || !_attached) return;
    await _invoke('shellState', {
      'hasDoc': hasDocument,
      'filters': filtersShown,
      'detail': detailShown,
      'title': title,
    });
  }

  /// 通道没人应答是**正常情况**（没有外壳的平台、测试环境）。
  static Future<void> _invoke(String method, [Object? arguments]) async {
    try {
      await _channel.invokeMethod<void>(method, arguments);
    } on MissingPluginException {
      return;
    } on PlatformException {
      return;
    }
  }
}
