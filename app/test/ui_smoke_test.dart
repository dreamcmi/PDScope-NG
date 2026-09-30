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
import 'sample_fixture.dart';

/// 找一份本机样本；找不到返回 null（私有抓包不在仓库里，缺失是正常情况）。
String? _sample(String name) {
  return sampleFixture(name);
}

/// 等一个条件成立，然后多泵几帧让界面把结果画出来。
///
/// ⚠ **必须交替「真等待」和 `pump()`**，两件事各管一半，缺一半就永远等不到：
///   · 界面里发起的请求（取一页报文、取模拟量轨迹）是在**假时钟的 zone** 里 await 的，
///     它的续体要 `pump()` 才会跑；
///   · 而这些请求真正完成要靠工作 isolate 回消息，那要**真实时间**。
///   只真等待 ⇒ 续体永远不跑（现象是「干等 90 秒毫无动静」）；
///   只 pump ⇒ isolate 永远回不来。
///   条件满足后再多泵几帧也很要紧：数据到了还要走一轮「通知 → 重建」，
///   界面这时才真的把行 / 曲线画出来 —— 少这一轮，断言就会看到一张空表。
Future<void> _waitFor(
  WidgetTester tester,
  bool Function() ready, {
  Duration timeout = const Duration(seconds: 60),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (!ready()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('等待超时（${timeout.inSeconds}s）');
    }
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 20)),
    );
    await tester.pump(const Duration(milliseconds: 16));
  }
  await _frames(tester, n: 3);
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
    });
    await _waitFor(tester, () => doc.state == DocState.done || doc.state == DocState.failed);

    expect(doc.state, DocState.done, reason: '样本应当能解开（失败：${doc.error}）');
    expect(doc.rows.total, greaterThan(0), reason: '筛选后的视图里应当有行');

    // 让表格看到「已解码」并去取第一页，再等第一页真的画出来。
    await _waitFor(tester, () => doc.rows.at(0) != null);

    // ── 六块齐活 ──
    expect(find.byType(FilterPanel), findsOneWidget);
    expect(find.byType(PacketTable), findsOneWidget);
    expect(find.byType(Timeline), findsOneWidget);
    expect(find.byType(DetailPanel), findsOneWidget);
    expect(tester.getSize(find.byType(Timeline)).height, greaterThan(0),
        reason: '时间轴要有实际高度，否则等于没画');

    // ── 行真的进了表格 ──
    //
    // ⚠ 断言必须**限定在 PacketTable 里**。先前这里是不限作用域的
    //   `find.text(msgType)`,而报文类型名同时也出现在**左栏「报文类型」候选列表**里——
    //   于是表格一行都没画，这条断言照样通过：它测的是筛选栏，不是表格。
    //   加作用域这件事本身就是这次「表格不显示」能溜过测试的原因。
    expect(doc.rows.at(0), isNotNull, reason: '第一页没取回来，表格会是空的');
    final firstRow = doc.rows.at(0)!;
    final table = find.byType(PacketTable);
    expect(
      find.descendant(of: table, matching: find.text(firstRow.msgType)),
      findsWidgets,
      reason: '第一行的报文类型应当出现在**表格**里（在表格外面出现不算）',
    );
    // 再钉一条只有表格才有的列：「时间」列的文本。它在别处都不出现。
    expect(
      find.descendant(of: table, matching: find.text(firstRow.elapsed)),
      findsWidgets,
      reason: '时间列是表格独有的：它画出来了，才说明行真的渲染了',
    );

    // ── 底部模拟量区：不能停在转圈上 ──
    //
    // ⚠ 先前这里一条断言都没有，所以「VBUS / IBUS 一直转圈」能一路溜过去：
    //   请求其实早就回来了，只是**没人重画**（对文档的通知没人听）。
    //   所以要等它收尾，再断言界面上没有转圈 ——「取回来了但没画」和「根本没取到」
    //   在这里被区分开。
    await _waitFor(tester, () => doc.busAttempted);
    expect(doc.busAttempted, isTrue, reason: '模拟量请求应当收尾（成功或失败都算）');
    if (doc.meta?.hasBus == true) {
      expect(doc.bus, isNotNull, reason: '这份样本声明有模拟量，应当取得到轨迹');
      expect(
        find.descendant(of: find.byType(Timeline), matching: find.byType(CircularProgressIndicator)),
        findsNothing,
        reason: '轨迹已经取回来了，底部不该还在转圈',
      );
    }

    // ── 左栏开关不许越界压字 ──
    //
    // ⚠ Material 的 Switch 有固定的固有尺寸；硬塞进小 `SizedBox` 之后它**仍按自己的
    //   尺寸绘制**，越界压住左边的标签，而且不抛异常、不报溢出 —— 只有人眼看得出来。
    //   所以这几行必须用自绘的小号开关。下面两条把它钉住：用的是什么、有多大。
    final filters = find.byType(FilterPanel);
    expect(
      find.descendant(of: filters, matching: find.byType(Switch)),
      findsNothing,
      reason: '筛选栏不许用 Material Switch：压小了会越界压字，而且不报任何错',
    );
    final switches = find.descendant(of: filters, matching: find.byType(MiniSwitch));
    expect(switches, findsNWidgets(4), reason: '四个快捷筛选各一个小号开关');
    for (var i = 0; i < 4; i++) {
      expect(
        tester.getSize(switches.at(i)),
        MiniSwitch.size,
        reason: '开关就是这么大，不是「被压小」——尺寸变了说明它不再是自绘的那个',
      );
    }

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
    await _waitFor(tester, () => doc.detail != null);

    expect(doc.detail, isNotNull);
    expect(doc.detail!.grouped(), isNotEmpty, reason: '详情应当切成若干分组');
  });

  testWidgets('ATK-C 样本：表格首帧就有行，底部不悬在转圈上', (tester) async {
    // 这份就是「表格空白、VBUS/IBUS 一直转圈」被报上来的那个场景（ATK-C 电平采样）。
    // 它和上面 POWER-Z 那份走的容器不同，所以单列一条。
    final path = _sample('酷泰科10u线-ip18pro.atkcc');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    final ws = await _pumpApp(tester);
    late CaptureDocument doc;
    await tester.runAsync(() async {
      doc = await ws.openFile(path);
    });
    await _waitFor(tester, () => doc.state == DocState.done || doc.state == DocState.failed);
    expect(doc.state, DocState.done, reason: '样本应当能解开（失败：${doc.error}）');

    // 表格：**不滚任何东西**，第一行就该画在表格里。
    await _waitFor(tester, () => doc.rows.at(0) != null);
    final firstRow = doc.rows.at(0);
    expect(firstRow, isNotNull, reason: '第一页没取回来，表格会是空的');
    expect(
      find.descendant(of: find.byType(PacketTable), matching: find.text(firstRow!.elapsed)),
      findsWidgets,
      reason: '首帧就该画出数据行 ——「滚一下才出来」等于数据到了但没人重画',
    );

    // 模拟量：收尾之后不能再转圈。
    await _waitFor(tester, () => doc.busAttempted);
    expect(doc.busAttempted, isTrue);
    expect(
      find.descendant(of: find.byType(Timeline), matching: find.byType(CircularProgressIndicator)),
      findsNothing,
      reason: '请求已收尾，底部不该还在转圈',
    );
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
    });
    await _waitFor(
      tester,
      () => ws.docs.every((d) => d.state != DocState.opening && d.state != DocState.decoding),
    );

    expect(ws.docs.length, 2);
    expect(ws.docs[0].state, DocState.ready, reason: '未激活的好文件应保持待解码');
    expect(ws.docs[1].state, DocState.failed, reason: '坏的那份应当只红自己');
    expect(ws.docs[1].error, isNotNull);
    // 两个标签都还在（不是「坏文件把整份工作区搞崩」）
    expect(find.byType(TabStrip), findsOneWidget);

    await tester.runAsync(() => ws.closeAll());
    await _frames(tester);
  });
}
