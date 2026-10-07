import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/document.dart';
import 'package:pdscope_app/core/engine.dart';
import 'package:pdscope_app/core/live_source.dart';
import 'package:pdscope_app/core/live_session.dart';
import 'package:pdscope_app/core/models.dart';
import 'package:pdscope_app/core/pcl_live_source.dart';
import 'package:pdscope_app/core/workspace.dart';
import 'package:pdscope_app/ui/app.dart';

import 'pcl_fixtures.dart';

void main() {
  late EngineClient engine;
  setUpAll(() async {
    engine = await EngineClient.instance();
  });

  testWidgets('真实 PCL 数据源：能力配置、报文/原始字节、未知电流、排空停止和关闭标签', (tester) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    final transport = MemoryPclTransport();
    final source = PclLiveSource(transport: transport);
    final workspace = Workspace(
      Future.value(engine),
      liveSourceFactory: () => source,
    );
    late CaptureDocument doc;
    await tester.runAsync(() async {
      doc = workspace.openLive();
      await doc.liveEnumerate();
      await doc.liveConnect(doc.liveDevices.single);
    });
    await tester.pumpWidget(PdScopeApp(workspace: workspace));
    await tester.pump();
    expect(find.text('开始采集'), findsOneWidget);
    expect(find.text('ADC 最短采样周期'), findsOneWidget);
    expect(find.text('数据通道'), findsOneWidget);
    expect(find.text('高速采样流'), findsNothing);
    expect(doc.liveDevice!.can('母线电流'), isFalse);
    expect(doc.liveOptions.current, isFalse);
    expect(transport.written, isEmpty);

    await tester.runAsync(() async {
      await doc.liveStart();
      await Future<void>.delayed(const Duration(milliseconds: 10));
    });
    await tester.pump();
    expect(
      doc.liveState,
      LiveState.capturing,
      reason:
          doc.liveMessage ??
          doc.live!.trace.map((entry) => entry.note).join('\n'),
    );
    expect(find.text('暂停'), findsNothing);
    await tester.runAsync(() async {
      final received = source.events
          .where((event) => event is LiveRowsEvent)
          .first;
      transport.push(dataFrame(transport.nextId++, 10, pdWire(0x0183)));
      await received;
      final measured = source.events
          .where((event) => event is LiveBusEvent)
          .first;
      transport.push(
        evtFrame(transport.nextId++, [
          [4, 23, 4, 900, 0xffff, 0],
        ]),
      );
      await measured;
      await Future<void>.delayed(const Duration(milliseconds: 300));
    });
    await tester.pump(const Duration(milliseconds: 300));
    await tester.pump();
    expect(doc.live!.rowCount, 1);
    expect(doc.rows.total, 1);
    expect(doc.stats!.crcUnknown, 0);
    expect(find.text('Accept'), findsWidgets);
    expect(find.text('丢包数'), findsOneWidget);
    expect(doc.bus!.ibus.single.isNaN, isTrue);

    await doc.selectRow(engine, 0, viewIndex: 0);
    await tester.pump();
    expect(doc.detail!.rawPayload, pdWire(0x0183));
    expect(find.text('PCL 原始解码字节（头 / 正文 / 线上 CRC）'), findsOneWidget);
    expect(find.text('未记录'), findsWidgets);
    expect(doc.detail!.row.ibus.isNaN, isTrue);
    final snapshot = jsonDecode(doc.live!.jsonText()) as Map;
    expect((snapshot['packets'] as List).single['rawPayload'], pdWire(0x0183));
    expect((snapshot['measurements'] as List).single['current'], isNull);

    transport.tailOnEnd = true;
    await tester.runAsync(() async {
      await doc.liveStop();
      await Future<void>.delayed(const Duration(milliseconds: 10));
    });
    await tester.pump();
    expect(doc.liveState, LiveState.stopped);
    expect(doc.live!.rowCount, 2);
    expect(find.text('采集配置'), findsOneWidget);
    await tester.tap(find.text('采集配置'));
    await tester.pump();
    expect(find.text('开始采集'), findsOneWidget);
    expect(doc.live!.rowCount, 2);
    transport.rejectConfig = true;
    await tester.runAsync(() async {
      await doc.liveStart();
      await Future<void>.delayed(const Duration(milliseconds: 10));
    });
    await tester.pump();
    expect(doc.live!.rowCount, 2, reason: '配置拒绝不能清除旧采集');
    transport.rejectConfig = false;
    transport.tailOnEnd = false;
    await tester.runAsync(() async {
      await doc.liveStart();
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final received = source.events
          .where((event) => event is LiveRowsEvent)
          .first;
      transport.push(dataFrame(transport.nextId++, 2, pdWire(0x0183)));
      await received;
      await Future<void>.delayed(const Duration(milliseconds: 300));
    });
    await tester.pump();
    expect(doc.liveState, LiveState.capturing);
    expect(doc.live!.rowCount, 1, reason: '确认新 START 后清除旧采集');
    expect(doc.live!.page(0, 1).single['index'], 0);
    expect(doc.detail, isNull);
    await tester.runAsync(() async {
      final closing = workspace.closeAt(workspace.docs.indexOf(doc));
      await workspace.closeAll();
      expect(transport.isOpen, isFalse, reason: '窗口关闭必须等待已移除标签的收尾');
      await closing;
    });
    await tester.pump();
    expect(transport.isOpen, isFalse);
    await tester.runAsync(transport.dispose);
    expect(tester.takeException(), isNull);
  });

  test('不规则测量沿用真实时间，未知值不补0；抽稀仍保留样本时刻', () {
    final series = BusSeries({
      'n': 3,
      'vbus': [5.0, 9.0, 9.1],
      'ibus': [double.nan, double.nan, double.nan],
      'times': [0.0, 0.023, 1.027],
      'step': 0,
    });
    expect(series.timeAt(1), 0.023);
    expect(series.nearestIndex(0.03), 1);
    expect(series.nearestIndex(1.0), 2);
    expect(series.ibus.first.isNaN, isTrue);
  });

  test('GoodCRC 时间轴配对核对 ID、SOP、CC、方向和 CRC', () async {
    final source = MockLiveSource();
    final session = LiveSession(source);
    Map<String, dynamic> row(
      int index,
      String type,
      String role, {
      int id = 2,
      int cc = 1,
    }) => {
      'index': index,
      'msgType': type,
      'role': role,
      'msgId': id,
      'channel': cc,
      'sop': 'SOP',
      'crc': 'ok',
      'timeMs': index.toDouble(),
      'kind': 'Control',
    };
    session.addRows([
      row(0, 'Accept', 'SRC'),
      row(1, 'GoodCRC', 'SNK', id: 3),
      row(2, 'Accept', 'SRC'),
      row(3, 'GoodCRC', 'SNK', cc: 2),
      row(4, 'Accept', 'SRC'),
      row(5, 'GoodCRC', 'SRC'),
      row(6, 'Accept', 'SRC'),
      row(7, 'GoodCRC', 'SNK'),
    ], {});
    expect(session.marks().flags.toList(), [0, 0, 0, 0, 0, 0, 2, 0]);
    await source.dispose();
  });
}
