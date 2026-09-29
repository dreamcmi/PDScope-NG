// main.dart — 入口
//
// 启动顺序：先把引擎（工作 isolate + 动态库）建起来、再外壳接上，最后交给界面。
// 引擎建不起来（找不到 pdscope.dll / 路径不对）时**不能**一路裸崩 ——
// 那时给一张能读的说明页，比一个空白窗口有用。
import 'package:flutter/material.dart';

import 'core/engine.dart';
import 'core/ffi.dart';
import 'core/shell.dart';
import 'core/workspace.dart';
import 'ui/app.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    final engine = await EngineClient.instance();
    final ws = Workspace(Future.value(engine));

    // 接上桌面外壳：拖放、文件关联、命令行参数、原生菜单都从这条通道进来。
    //
    // ⚠ 必须在 runApp 之前 —— 用「打开方式」启动时，外壳在窗口刚建好就把路径
    //   交过来了，它等的就是这一句 `ready`。
    // ⚠ 启动参数**只由外壳送进来**（和拖放走同一个入口）。这里再自己读一遍命令行
    //   的话，用文件关联打开会**同时开出两个标签** —— 同一份文件。
    await ShellBridge.attach(ws);

    runApp(PdScopeApp(workspace: ws));
  } on PdscopeException catch (e) {
    runApp(_EngineFailureApp(message: e.message));
  } catch (e) {
    runApp(_EngineFailureApp(message: '$e'));
  }
}

/// 引擎没起来时的兜底页：把原因和该去哪儿找库写清楚。
class _EngineFailureApp extends StatelessWidget {
  const _EngineFailureApp({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        backgroundColor: const Color(0xFFEEF1F5),
        body: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 760),
            child: Padding(
              padding: const EdgeInsets.all(28),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    '核心动态库没有加载成功',
                    style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
                  ),
                  const SizedBox(height: 12),
                  Container(
                    width: double.infinity,
                    padding: const EdgeInsets.all(14),
                    decoration: BoxDecoration(
                      color: const Color(0xFFFFFFFF),
                      border: Border.all(color: const Color(0xFFE0E5EB)),
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: SelectableText(
                      message,
                      style: const TextStyle(fontSize: 12.5, height: 1.6),
                    ),
                  ),
                  const SizedBox(height: 12),
                  const Text(
                    '开发时先构建核心：cmake -S . -B build && cmake --build build\n'
                    '产物在 build/out/，界面会自动去那里找。',
                    style: TextStyle(fontSize: 12, color: Color(0xFF5B6572)),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
