// ui_smoke_test.dart — 界面冒烟：把整个应用真的跑起来
//
// 为什么需要这一层：`core_integration_test.dart` 只测到数据层（FFI → 工作 isolate →
// 核心），界面装配错了、某个面板漏进树里、painter 里抛异常 —— 那些一条都测不出来。
// 而桌面窗口又没法在 CI 里开，所以用 widget test：`flutter_tester` 是个**真的 Dart VM**
// （FFI、isolate、dart:io 都在），整个 widget 树照常 build / layout / paint，
// 只是不出窗口。
//
// 这里刻意**不重复**数据层已经钉死的断言（CRC 口径、方向推断、CSV 行数）——
// 那些在 core_integration_test.dart 里，重复一遍只会让两处一起改起来更贵。
// 本文件只盯一件事：**数据确实被界面画出来了没有**。

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/document.dart';
import 'package:pdscope_app/core/engine.dart';
import 'package:pdscope_app/core/workspace.dart';
import 'package:pdscope_app/ui/app.dart';
import 'package:pdscope_app/ui/detail_panel.dart';
import 'package:pdscope_app/ui/filter_panel.dart';
import 'package:pdscope_app/ui/packet_table.dart';
import 'package:pdscope_app/ui/tab_strip.dart';
import 'package:pdscope_app/ui/timeline.dart';
import 'package:pdscope_app/ui/top_bar.dart';

/// 找一份本机样本；找不到返回 null（私有抓包不在仓库里，缺失是正常情况）。
String? _sample(String name) {
  for (final base in ['..', '.', '../..']) {
    final f = File('$base${Platform.pathSeparator}$name');
    if (f.existsSync()) return f.absolute.path;
  }
  return null;
}

/// 真实时间下的等待。**必须**在 `tester.runAsync` 里调 —— 外面是假时钟，
/// 解码这类真异步永远推进不了。
Future<void> _waitUntil(bool Function() ready, {Duration timeout = const Duration(seconds: 90)}) async {
  final deadline = DateTime.now().add(timeout);
  while (!ready()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('等待超时（${timeout.inSeconds}s）');
    }
    await Future<void>.delayed(const Duration(milliseconds: 25));
  }
}

/// 建一个足够大的画布：默认 800×600 装不下「筛选 + 表格 + 时间轴 + 详情」，
/// 布局一旦溢出测试就变成在验别的东西了。
Future<Workspace> _pumpApp(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1600, 1000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.reset);

  final ws = Workspace(Future.value(engine));
  await tester.pumpWidget(PdScopeApp(workspace: ws));
  await tester.pump();
  return ws;
}

/// 多泵几帧（不引 pumpAndSettle：解码中会有持续的动画，settle 容易等不到头）。
Future<void> _frames(WidgetTester tester, {int n = 4}) async {
  for (var i = 0; i < n; i++) {
    await tester.pump(const Duration(milliseconds: 16));
  }
}

/// ⚠ 引擎必须在 `setUpAll` 里建，**不能**在 `testWidgets` 体内建：
/// `EngineClient.instance()` 要 `Isolate.spawn`，那是真异步；而 widget test 的
/// 测试体内跑的是**假时钟**（要 `runAsync` 才放行真异步）。在里面建的话 isolate
/// 永远起不来，`Workspace.ready` 也就永远不返回 —— 症状是文档一路停在
/// `opening`、解码永不开始，而且超时报错指向的是一行看起来毫无问题的 `await`。
late EngineClient engine;

void main() {
  setUpAll(() async {
    engine = await EngineClient.instance();
  });

  testWidgets('空工作区：顶栏在、标签栏不出现（没有标签），中间给的是能开文件的空态', (tester) async {
    await _pumpApp(tester);

    expect(find.byType(TopBar), findsOneWidget);
    // 一份数据都没有时不该有标签栏，也不该出现六块里的数据面板
    expect(find.byType(TabStrip), findsNothing);
    expect(find.byType(PacketTable), findsNothing);
    expect(find.byType(Timeline), findsNothing);
    expect(find.text('打开文件'), findsOneWidget);
  });

  testWidgets('真实样本：六块齐活，表格有行、时间轴有标记、详情分得出组', (tester) async {
    final path = _sample('山泽60w-ip18pro.sqlite');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    final ws = await _pumpApp(tester);
    late CaptureDocument doc;

    await tester.runAsync(() async {
      doc = await ws.openFile(path);
      await _waitUntil(() => doc.state == DocState.done || doc.state == DocState.failed);
    });

    expect(doc.state, DocState.done, reason: '样本应当能解开（失败：${doc.error}）');
    expect(doc.rows.total, greaterThan(0), reason: '筛选后的视图里应当有行');

    // 让表格看到「已解码」并去取第一页（取页是真的异步）
    await _frames(tester);
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 400)));
    await _frames(tester);

    // ── 六块齐活 ──
    expect(find.byType(FilterPanel), findsOneWidget);
    expect(find.byType(PacketTable), findsOneWidget);
    expect(find.byType(Timeline), findsOneWidget);
    expect(find.byType(DetailPanel), findsOneWidget);
    expect(tester.getSize(find.byType(Timeline)).height, greaterThan(0),
        reason: '时间轴要有实际高度，否则等于没画');

    // ── 行真的进了表格 ──
    expect(doc.rows.at(0), isNotNull, reason: '第一页没取回来，表格会是空的');
    expect(find.text(doc.rows.at(0)!.msgType), findsWidgets,
        reason: '第一行的报文类型应当出现在表格里');

    // ── 时间轴的报文标记到位（整份抓包，不跟表格筛选走）──
    expect(doc.marks.n, greaterThan(0));
    expect(doc.marks.n, doc.stats!.packetCount,
        reason: '标记条数应当等于报文总数（时间轴画的是全量）');

    // ── 选中一行，详情面板要真的分得出组 ──
    final engine = await ws.ready;
    await tester.runAsync(() async {
      // 用视图第 0 行对应的**原始序号**去取详情
      await doc.selectRow(engine, 0);
    });
    await _frames(tester);

    expect(doc.detail, isNotNull);
    expect(doc.detail!.grouped(), isNotEmpty, reason: '详情应当切成若干分组');
  });

  testWidgets('坏文件只让那一个标签变红，不牵连旁边那份好的', (tester) async {
    final good = _sample('山泽60w-ip18pro.sqlite');
    if (good == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    // 一份内容不是任何抓包格式的文件（认内容不认扩展名，所以故意还给个像样的后缀）
    final dir = Directory.systemTemp.createTempSync('pdscope-ui-');
    addTearDown(() {
      if (dir.existsSync()) dir.deleteSync(recursive: true);
    });
    final bad = File('${dir.path}${Platform.pathSeparator}坏掉的抓包.sqlite')
      ..writeAsBytesSync(List<int>.generate(4096, (i) => (i * 37) & 0xff));

    final ws = await _pumpApp(tester);
    await tester.runAsync(() async {
      await ws.openFiles([good, bad.path]);
      await _waitUntil(() => ws.docs.every((d) => d.state == DocState.done || d.state == DocState.failed));
    });
    await _frames(tester);

    expect(ws.docs.length, 2);
    expect(ws.docs[0].state, DocState.done, reason: '好的那份不该被带坏');
    expect(ws.docs[1].state, DocState.failed, reason: '坏的那份应当只红自己');
    expect(ws.docs[1].error, isNotNull);
    // 两个标签都还在（不是「坏文件把整份工作区搞崩」）
    expect(find.byType(TabStrip), findsOneWidget);

    await tester.runAsync(() => ws.closeAll());
    await _frames(tester);
  });
}
