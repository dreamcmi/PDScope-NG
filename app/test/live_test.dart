// live_test.dart — 实时采集：文档接线、入口唯一性、CD 快照、底部区双模式
//
// 这一组测试**不碰 C 核心**：实时那一路的数据全在界面进程里（LiveSession），
// 所以它能脱离样本文件、脱离解码，直接验「点下去会发生什么」。
//
// 为什么要在这层测：真机上的抓包流程没法在 CI 里跑（要有分析仪），
// 但「入口是否唯一」「导出的 CSV 是不是那份口径」「模式开关切不切得动」
// 这些恰恰是最容易在改动中悄悄坏掉、而人手工点又容易漏掉的地方。
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/document.dart';
import 'package:pdscope_app/core/engine.dart';
import 'package:pdscope_app/core/filters.dart';
import 'package:pdscope_app/core/live_session.dart';
import 'package:pdscope_app/core/live_source.dart';
import 'package:pdscope_app/core/workspace.dart';
import 'package:pdscope_app/ui/app.dart';
import 'package:pdscope_app/ui/trace_panel.dart';

/// ⚠ 与 ui_smoke_test 同样的理由：引擎必须在 `setUpAll` 里建。
/// `EngineClient.instance()` 要 `Isolate.spawn`，那是真异步；而 testWidgets
/// 体内跑的是假时钟，在里面建 isolate 永远起不来。
late EngineClient engine;

Future<Workspace> _pumpApp(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1600, 1000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.reset);
  final ws = Workspace(
    Future.value(engine),
    liveSourceFactory: MockLiveSource.new,
  );
  await tester.pumpWidget(PdScopeApp(workspace: ws));
  await tester.pump();
  return ws;
}

/// 收尾：关掉全部标签。
///
/// 不只是为了让测试干净 —— 它走的正是**用户关标签那条真实路径**：
/// `Workspace.closeAt` 对实时文档会先 stop（补发 Disconnect）再 disconnect，
/// 最后 dispose，两组定时器都在那里被取消。所以这一步同时也在验
/// 「关标签必须把设备从抓包态里放出来」这条规矩。
///
/// ⚠ 不能用 `addTearDown` 代替：框架的「还有定时器没停」检查发生在测试体结束时，
///   而 addTearDown 在那之后才跑 —— 那时候停已经来不及了。
Future<void> _finish(WidgetTester tester, Workspace ws) async {
  await ws.closeAll();
  await tester.pump();
}

Future<void> _frames(WidgetTester tester, {int n = 4, int ms = 16}) async {
  for (var i = 0; i < n; i++) {
    await tester.pump(Duration(milliseconds: ms));
  }
}

/// 把实时文档推到「采集中」：查找设备 → 连接 → 开始采集。
Future<void> _driveToCapturing(WidgetTester tester, Workspace ws) async {
  ws.openLive();
  await tester.pump();

  await tester.tap(find.text('查找设备'));
  await tester.pump(const Duration(milliseconds: 900)); // 枚举的 800 ms
  await _frames(tester);

  await tester.tap(find.text('连接').first);
  await tester.pump(const Duration(milliseconds: 600)); // 连接的 420 ms
  await _frames(tester);

  await tester.tap(find.text('开始采集'));
  await _frames(tester, n: 12, ms: 100); // 让模拟源的节拍跑一会儿
}

void main() {
  setUpAll(() async {
    engine = await EngineClient.instance();
  });

  group('入口', () {
    testWidgets('三个入口都落到 openLive：**实时标签只允许一个**', (tester) async {
      final ws = await _pumpApp(tester);

      ws.openLive();
      await tester.pump();
      expect(ws.docs.length, 1);
      expect(ws.docs.first.isLive, isTrue);
      expect(ws.active!.isLive, isTrue);

      // 再点一次「连接设备」不该多出一个标签：一台设备只能有一个会话，
      // 两个实时标签会互相抢设备。
      ws.openLive();
      ws.openLive();
      await tester.pump();
      expect(ws.docs.length, 1);
    });

    testWidgets('实时标签与文件标签可以共存，且互不干扰', (tester) async {
      final ws = await _pumpApp(tester);
      final live = ws.openLive();
      await tester.pump();
      final file = CaptureDocument(
        id: 999,
        path: r'x\y.atkcc',
        displayName: 'y.atkcc',
      );
      ws.docs.add(file);
      await tester.pump();

      expect(ws.docs.length, 2);
      // 实时文档没有引擎会话，isLive 只认它自己那个标记
      expect(live.isLive, isTrue);
      expect(file.isLive, isFalse);
    });
  });

  group('连接与采集', () {
    testWidgets('未采集时中列是连接面板，采集开始后交回表格', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await tester.pump();

      // 未连接：连接面板的空态
      expect(find.text('这里还没有设备'), findsOneWidget);
      expect(doc.livePreCapture, isTrue);
      expect(find.text('开始采集'), findsNothing);

      await tester.tap(find.text('查找设备'));
      await tester.pump(const Duration(milliseconds: 900));
      await _frames(tester);

      // 已发现：两台设备（同一台机器的两个接口，能力不同）
      expect(find.text('连接'), findsNWidgets(2));
      expect(doc.liveState, LiveState.found);
      // 能力不可用时**必须写出原因**，不能只置灰
      expect(find.textContaining('不可用'), findsWidgets);

      await tester.tap(find.text('连接').first);
      await tester.pump(const Duration(milliseconds: 600));
      await _frames(tester);

      // 待机：采集配置出来，开始采集可用
      expect(doc.liveState, LiveState.ready);
      expect(find.text('开始采集'), findsOneWidget);

      await tester.tap(find.text('开始采集'));
      await _frames(tester, n: 12, ms: 100);

      expect(doc.liveState, LiveState.capturing);
      expect(doc.livePreCapture, isFalse);
      // 模拟源约 8 条/秒，1.2 秒后至少该有报文了
      expect(doc.live!.rowCount, greaterThan(0));
      expect(doc.rows.total, greaterThan(0));
      await _finish(tester, ws);
    });

    testWidgets('采集中：状态条在、丢包读数在（0 也要显示）', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      expect(doc.showLiveBar, isTrue);
      expect(find.text('采集中'), findsWidgets);
      // 丢包**永远显示**：它一旦非 0 会被误读成「设备没发」
      expect(find.text('丢包 0'), findsOneWidget);
      // 主按钮是「停止」，不是「暂停」—— 任何时刻只有一个填充主按钮
      expect(find.text('停止'), findsOneWidget);
      expect(find.text('导出 CSV'), findsOneWidget);

      await doc.liveStop();
      await _frames(tester);
      expect(doc.liveState, LiveState.stopped);
      // 停止后数据仍然完整可用（原则④）
      expect(doc.live!.rowCount, greaterThan(0));
    });

    testWidgets('暂停：数据不再进来，但已采的还在', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      final before = doc.live!.rowCount;
      await tester.tap(find.text('暂停'));
      await _frames(tester);
      expect(doc.liveState, LiveState.paused);
      await _frames(tester, n: 10, ms: 100);
      // 暂停期间不再追加
      expect(doc.live!.rowCount, before);
      await _finish(tester, ws);
    });
  });

  group('底部区双模式', () {
    testWidgets('时间轴头部里的「通讯日志」能切过去，也能切回来', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      expect(doc.showTrace, isFalse);
      expect(find.byType(TracePanel), findsNothing);

      // 时间轴头部里的那一个（不是通讯日志面板里的那一个）
      await tester.tap(find.text('通讯日志'));
      await _frames(tester);

      expect(doc.showTrace, isTrue);
      expect(find.byType(TracePanel), findsOneWidget);
      expect(find.textContaining('环形保留最近'), findsOneWidget);

      // 切回去
      await tester.tap(find.text('模拟量轨迹'));
      await _frames(tester);
      expect(doc.showTrace, isFalse);
      expect(find.byType(TracePanel), findsNothing);
      await _finish(tester, ws);
    });

    testWidgets('文件抓包没有通讯日志：模式开关不出现', (tester) async {
      final ws = await _pumpApp(tester);
      expect(find.text('通讯日志'), findsNothing);
      expect(find.text('模拟量轨迹'), findsNothing);
      expect(ws.docs, isEmpty);
    });
  });

  group('停车与掉线', () {
    testWidgets('主动停车：横幅说明原因、已采数据保留、不给「重连」按钮', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      final before = doc.live!.rowCount;
      expect(before, greaterThan(0));

      doc.liveDrillPark();
      await _frames(tester);

      expect(doc.liveState, LiveState.parked);
      // 停车会**先把那条看不懂的报文落下来再停** —— 它恰恰是排障时最需要看的，
      // 所以条数应该比停车前多 1，而不是被丢掉。
      final kept = doc.live!.rowCount;
      expect(kept, before + 1);
      // 整页横幅：说清「已主动停止」+ 原因
      expect(find.text('已主动停止采集并断开设备'), findsOneWidget);
      expect(find.textContaining('无法解释的响应'), findsOneWidget);
      // 已采数据仍然可用（原则④）
      expect(find.textContaining('已采到的 $kept 条报文仍然完整可用'), findsOneWidget);
      // 停车后可用的动作：看日志、断开设备、导快照
      expect(find.text('断开设备'), findsOneWidget);
      expect(find.text('查看通讯日志'), findsOneWidget);
      expect(find.text('导出 CSV'), findsOneWidget);
      // 原则③：不自动重连，也不给「重连」按钮 —— 要不要重来由人决定。
      // ⚠ 断言的是「没有这个**按钮**」，不是「没有这两个字」：
      //   横幅里那句「不会自动重连」正是在说明这件事，它必须留着。
      expect(find.widgetWithText(OutlinedButton, '重连'), findsNothing);
      expect(find.widgetWithText(FilledButton, '重连'), findsNothing);
      // 停车后不该还留着「停止」「暂停」这种「正在跑」的操作
      expect(find.text('停止'), findsNothing);
      expect(find.text('暂停'), findsNothing);

      // 横幅的收尾那句要能读到「要不要重来由你决定」
      expect(find.textContaining('不会自动重连'), findsOneWidget);
      await _finish(tester, ws);
    });

    testWidgets('设备掉线：横幅给出原因与「重新查找」，已采的仍在', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      doc.liveDrillDropout();
      await _frames(tester);

      expect(doc.liveState, LiveState.disconnected);
      // 状态条上是状态名，横幅补的是**后果**，两者不重复
      expect(find.text('设备已断开'), findsOneWidget); // 状态条
      expect(find.text('设备已断开，采集已停止'), findsOneWidget); // 横幅
      expect(find.text('重新查找'), findsWidgets);
      expect(doc.live!.rowCount, greaterThan(0));
      await _finish(tester, ws);
    });

    testWidgets('停车后点「断开设备」回到未连接，连接面板回来', (tester) async {
      final ws = await _pumpApp(tester);
      final doc = ws.openLive();
      await _driveToCapturing(tester, ws);

      doc.liveDrillPark();
      await _frames(tester);
      await tester.tap(find.text('断开设备'));
      await _frames(tester);

      expect(doc.liveState, LiveState.idle);
      expect(doc.livePreCapture, isTrue);
      expect(find.text('这里还没有设备'), findsOneWidget);
      await _finish(tester, ws);
    });
  });

  group('CSV 快照', () {
    test('列名与核心 csv.cpp 一致（13 列，PD 用 Objects）', () {
      final s = LiveSession(MockLiveSource());
      expect(LiveSession.csvHeader, [
        '#',
        'SOP',
        'MsgType',
        'ID',
        'Direction',
        'Objects',
        'Elapsed',
        'Time(ms)',
        'VBUS(V)',
        'IBUS(A)',
        'Data',
        'CRC',
        'Note',
      ]);
      // 一行都没有时只有表头，且**没有尾换行**（与核心一致）
      final t = s.csvText(filtered: false);
      expect(t.split('\r\n').length, 1);
      expect(t.endsWith('"Note"'), isTrue);
    });

    test('CRC 未记录必须留空 —— 不能替设备的数据背书', () {
      final s = LiveSession(MockLiveSource());
      s.addRows([
        {
          'index': 1,
          'sop': 'SOP',
          'msgType': 'Request',
          'role': 'SNK',
          'msgId': 3,
          'objects': 1,
          'timeMs': 12.5,
          'elapsed': '0.013 s',
          'startSample': 12,
          'endSample': 200,
          'vbus': 11.98,
          'ibus': 1.8422,
          'dataHex': '2C 91 12 00',
          'crc': 'none',
          'summary': 'RDO → PDO#3',
          'warn': 0,
          'channel': 0,
          'seq': 1,
          'kind': 'Data',
          'msgKind': 'data',
          'durationUs': 188.0,
        },
      ], {});
      final rows = s.csvText(filtered: false).split('\r\n');
      expect(rows.length, 2);
      final cells = rows[1].split(',');
      // 逐字段加引号：13 列
      expect(cells.length, 13);
      expect(cells[5], '"1"'); // Objects
      expect(cells[8], '"11.98"'); // VBUS 两位
      expect(cells[9], '"1.842"'); // IBUS 三位
      expect(cells[11], '""'); // CRC 留空 —— 关键
    });

    test('视图口径受筛选影响：filtered=true 只导当前视图', () {
      final s = LiveSession(MockLiveSource());
      Map<String, dynamic> row(int i, String type) => {
        'index': i,
        'sop': 'SOP',
        'msgType': type,
        'role': i.isEven ? 'SRC' : 'SNK',
        'msgId': 0,
        'objects': 0,
        'timeMs': i * 1.0,
        'elapsed': '0.000 s',
        'startSample': i,
        'endSample': i + 1,
        'vbus': 5.0,
        'ibus': 0.0,
        'dataHex': '',
        'crc': 'none',
        'summary': '',
        'warn': 0,
        'channel': 0,
        'seq': i,
        'kind': 'Control',
        'msgKind': 'control',
        'durationUs': 1.0,
      };
      s.addRows([row(1, 'GoodCRC'), row(2, 'Request')], {});

      final f = FilterState.defaults('pd');
      s.rebuildView(f);
      // 默认隐藏 GoodCRC
      expect(s.viewCount, 1);
      expect(s.csvText(filtered: true).split('\r\n').length, 2);
      // 不过滤就是两条
      expect(s.csvText(filtered: false).split('\r\n').length, 3);
    });
  });
}
