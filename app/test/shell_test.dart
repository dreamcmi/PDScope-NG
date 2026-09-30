// shell_test.dart — 桌面外壳那条路的检查
//
// 外壳（Windows 原生窗口）送到界面的**只有路径**，落到 `Workspace.openFiles`
// 这一个入口。拖放本身没法在无窗口的测试里真的发生，但**「外壳 → 界面」这一段**
// 可以：直接用平台消息把 `openFiles` / `command` 打进 Dart 侧，看界面怎么接。
//
// 这里盯三件事（其余的口径在 core_integration_test 与 ui_smoke_test 里）：
//   ① 混着一批拖进来时三种来源**都**被识别 —— 特别是 `.pdStream`：
//      混着拖进来时 `.pdStream` 最容易漏，专门盯一条
//   ② 坏文件只让**它自己**那个标签变红
//   ③ 原生菜单的命令与界面上的按钮是**同一套动作**（不是各写一份）
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/document.dart';
import 'package:pdscope_app/core/engine.dart';
import 'package:pdscope_app/core/prefs.dart';
import 'package:pdscope_app/core/shell.dart';
import 'package:pdscope_app/core/workspace.dart';
import 'sample_fixture.dart';
import 'package:pdscope_app/ui/app.dart';

const MethodChannel _shellChannel = MethodChannel('pdscope/shell');
const StandardMethodCodec _codec = StandardMethodCodec();

/// Dart 发给外壳的调用（`ready` / `shellState`）都记在这儿。
final List<MethodCall> _toShell = [];

void _installShellPlatform() {
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(_shellChannel, (call) async {
    _toShell.add(call);
    return null;
  });
}

/// 模拟外壳反过来推一条调用进来：拖放/文件关联 → `openFiles`，菜单 → `command`。
Future<void> _fromShell(String method, Object? arguments) async {
  final data = _codec.encodeMethodCall(MethodCall(method, arguments));
  await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .handlePlatformMessage(_shellChannel.name, data, (_) {});
}

/// 找一份本机样本；找不到返回 null（私有抓包不在仓库里，缺失是正常情况）。
String? _sample(String name) {
  return sampleFixture(name);
}

/// `.pdStream` 样本在老工程那边（合成的那份只有 8 条报文，稳定又小）。
String? _pdStreamSample() {
  const relative = [
    'artifacts/_pdstream_synth.pdStream',
    'rawdata/DJIPOWER_VIVOX300U_PPS.pdStream',
  ];
  for (final base in ['../../PDScope', '../PDScope', './PDScope']) {
    for (final rel in relative) {
      final f = File('$base/$rel');
      if (f.existsSync()) return f.absolute.path;
    }
  }
  return null;
}

/// 外壳最后一次收到的窗口标题（外壳就是拿它 SetWindowTextW 的）。
String? _reportedTitle() {
  for (final call in _toShell.reversed) {
    if (call.method != 'shellState') continue;
    final args = call.arguments;
    if (args is Map && args['title'] is String) return args['title'] as String;
  }
  return null;
}

/// 真实时间下的等待（假时钟推进不了解码这类真异步）。
Future<void> _waitUntil(
  bool Function() ready, {
  Duration timeout = const Duration(seconds: 90),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (!ready()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('等待超时（${timeout.inSeconds}s）');
    }
    await Future<void>.delayed(const Duration(milliseconds: 25));
  }
}

Future<void> _pumpApp(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1600, 1000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(PdScopeApp(workspace: workspace));
  await tester.pump();
}

late EngineClient engine;
late Workspace workspace;

void main() {
  // 通道那一套要 binding；而且 `ShellBridge` 的调用正是走 binding 的 messenger，
  // 不初始化的话它发出去的消息到不了我们装的「外壳」。
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    engine = await EngineClient.instance();
    workspace = Workspace(Future.value(engine));
    _installShellPlatform();
    await ShellBridge.attach(workspace);
  });

  test('接线时确实向外壳报了 ready —— 否则外壳攒的路径永远送不出来', () {
    expect(
      _toShell.map((c) => c.method),
      contains('ready'),
      reason: '没有 ready，命令行/文件关联送来时会一直卡在外壳的待发队列里',
    );
  });

  test('混合拖放：.atkcc / .sqlite(PD) / .sqlite(UFCS) / .pdStream 四种都认出来', () async {
    final atkcc = _sample('安可60w-ip18pro.atkcc');
    final pd = _sample('山泽60w-ip18pro.sqlite');
    final ufcs = _sample('ufcs_vivo_x300u.sqlite');
    final pdstream = _pdStreamSample();

    final paths = <String>[?atkcc, ?pd, ?ufcs, ?pdstream];
    if (paths.length < 3 || pdstream == null) {
      markTestSkipped('本机样本不足（私有抓包不在仓库里），跳过');
      return;
    }

    final before = workspace.docs.length;
    // ⚠ 走的是外壳那条路：平台消息 → 通道 → openFiles。不是直接调 ws.openFiles。
    await _fromShell('openFiles', paths);

    await _waitUntil(
      () =>
          workspace.docs.length == before + paths.length &&
          workspace.docs
              .skip(before)
              .every((d) => d.state != DocState.opening && d.state != DocState.decoding) &&
          workspace.docs.last.state == DocState.done,
    );

    final opened = workspace.docs.sublist(before);
    expect(opened.length, paths.length, reason: '一次拖四份就应当开四个标签');

    final failed = opened.where((d) => d.state == DocState.failed).toList();
    expect(
      failed.map((d) => '${d.displayName}: ${d.error}').toList(),
      isEmpty,
      reason: '这四份都应当认得出来',
    );

    // 按**内容**分流，不看扩展名。
    expect(
      opened.every((d) => d.meta != null),
      isTrue,
      reason: '四份都应当有元数据（打不开的话上面那条就已经挂了）',
    );
    expect(opened[0].meta!.container, 'atkcc');
    expect(opened[1].meta!.container, 'sqlite');
    expect(opened[2].meta!.container, 'sqlite');
    expect(
      opened.map((d) => d.meta!.container),
      contains('pdstream'),
      reason: '.pdStream 必须能认出来',
    );

    // 「认出来」还不够：没有 ADC 的 .pdStream 也要真的解出报文（只是没有波形）。
    final streamDoc = opened.firstWhere((d) => d.meta!.container == 'pdstream');
    expect(streamDoc.isUfcs, isFalse);
    expect(streamDoc.stats!.packetCount, greaterThan(0), reason: '.pdStream 也要解出报文');
    expect(
      streamDoc.stats!.crcUnknown,
      streamDoc.stats!.packetCount,
      reason: '这类容器不存 CRC ⇒ 只能记「未记录」，不是「通过」',
    );

    // 顺带确认 UFCS 那份走的是 UFCS 语义，不是 PD。
    final ufcsDoc = opened.firstWhere((d) => d.meta!.isUfcs);
    expect(ufcsDoc.meta!.protocol, 'UFCS');
  });

  test('拖进来的坏文件只让它自己那个标签变红', () async {
    final good = _sample('山泽60w-ip18pro.sqlite');
    if (good == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    // 内容不是任何抓包格式，但故意给个像样的后缀（判定看内容不看扩展名）。
    final dir = Directory.systemTemp.createTempSync('pdscope-shell-');
    addTearDown(() {
      if (dir.existsSync()) dir.deleteSync(recursive: true);
    });
    final bad = File('${dir.path}${Platform.pathSeparator}坏掉的抓包.sqlite')
      ..writeAsBytesSync(List<int>.generate(4096, (i) => (i * 37) & 0xff));

    final before = workspace.docs.length;
    await _fromShell('openFiles', [good, bad.path]);

    await _waitUntil(
      () =>
          workspace.docs.length == before + 2 &&
          workspace.docs
              .skip(before)
              .every((d) => d.state != DocState.opening && d.state != DocState.decoding),
    );

    final opened = workspace.docs.sublist(before);
    expect(opened[0].state, DocState.ready, reason: '未激活的好文件应保持待解码');
    expect(opened[1].state, DocState.failed, reason: '坏的那份只红自己');
    expect(opened[1].error, isNotNull);
  });

  testWidgets('原生菜单的命令与界面按钮是同一套动作', (tester) async {
    await _pumpApp(tester);
    final prefs = workspace.prefs;

    // 主题
    final darkBefore = prefs.isDark;
    await _fromShell('command', 'toggleTheme');
    await tester.pump();
    expect(prefs.isDark, !darkBefore);

    // 筛选栏 / 详情面板的显示开关（菜单上是勾选项，勾的是「显示」）
    final filtersBefore = prefs.filtersCollapsed;
    await _fromShell('command', 'toggleFilters');
    await tester.pump();
    expect(prefs.filtersCollapsed, !filtersBefore);

    final detailBefore = prefs.detailCollapsed;
    await _fromShell('command', 'toggleDetail');
    await tester.pump();
    expect(prefs.detailCollapsed, !detailBefore);

    // 重置布局：收起的两栏都展开、尺寸回默认
    prefs
      ..filtersCollapsed = false
      ..detailCollapsed = true
      ..detailW = 700
      ..tlH = 500;
    await _fromShell('command', 'resetLayout');
    await tester.pump();
    expect(prefs.detailCollapsed, isFalse);
    expect(prefs.filtersCollapsed, isFalse);
    expect(prefs.detailW, kDetailWidthDefault);
    expect(prefs.tlH, kTimelineHeightDefault);
    expect(prefs.rowH, kRowHeightNormal);

    await _fromShell('command', 'toggleDense');
    await tester.pump();
    expect(prefs.compact, isTrue);
    await _fromShell('command', 'toggleDense');
    await tester.pump();
    expect(prefs.compact, isFalse);

    // 没认出来的命令要静默忽略（新外壳配旧界面时不该炸）
    await _fromShell('command', '下一个版本才有的命令');
    await tester.pump();
  });

  testWidgets('界面状态会报给外壳：菜单项的灰/勾选不靠外壳猜', (tester) async {
    _toShell.clear();
    await _pumpApp(tester);
    await tester.pump();

    final state = _toShell.where((c) => c.method == 'shellState').toList();
    expect(state, isNotEmpty, reason: '外壳看不见 widget 树，状态只能由这边报');
    final args = state.last.arguments as Map;
    expect(args['hasDoc'], isA<bool>());
    expect(args['filters'], isA<bool>());
    expect(args['detail'], isA<bool>());
    expect(args['title'], isA<String>());
  });

  testWidgets('当前抓包会写进窗口标题 —— 任务栏里看得出开着哪一份', (tester) async {
    final path = _sample('苹果40w-ip18pro.atkcc');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    _toShell.clear();
    await _pumpApp(tester);
    final before = workspace.docs.length;

    await tester.runAsync(() async {
      await _fromShell('openFiles', [path]);
      // 外壳推来的这条是异步的；等到这份成为当前文档、且状态报出去了为止。
      await _waitUntil(() => workspace.docs.length == before + 1);
      await _waitUntil(() => _reportedTitle() != null, timeout: const Duration(seconds: 20));
    });

    // 外壳拿标题的方式就是 SetWindowTextW；这一条同时也是「转交进来的路径
    // 真的被打开了」的**外部可观测证据** —— 不靠人眼看窗口也能验。
    expect(_reportedTitle(), 'PDScope — ${File(path).uri.pathSegments.last}');

    await tester.runAsync(() async {
      await _fromShell('command', 'closeCurrent');
      await _waitUntil(() => workspace.docs.length == before);
      await _fromShell('command', 'closeAll');
      await _waitUntil(() => workspace.docs.isEmpty);
    });
  });
}
