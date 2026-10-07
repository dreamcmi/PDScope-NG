import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/live_source.dart';
import 'package:pdscope_app/core/pcl_live_source.dart';
import 'package:pdscope_app/core/pcl_pd_decoder.dart';
import 'package:pdscope_app/core/pcl_protocol.dart';

import 'pcl_fixtures.dart';

Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 40));

void main() {
  late MemoryPclTransport transport;
  late PclLiveSource source;
  late List<LiveEvent> events;
  late StreamSubscription<LiveEvent> subscription;

  setUp(() {
    transport = MemoryPclTransport();
    source = PclLiveSource(
      transport: transport,
      responseTimeout: const Duration(milliseconds: 250),
    );
    events = [];
    subscription = source.events.listen(events.add);
  });
  tearDown(() async {
    await source.dispose();
    await subscription.cancel();
    await transport.dispose();
  });

  Future<void> connect() async {
    await source.connect((await source.enumerate()).single);
  }

  test('被动 HELLO、CFG/START、后台 PD 解码、测量未知和 END 尾部排空', () async {
    await connect();
    expect(transport.written, isEmpty);
    expect(source.connectedDevice!.can('PD 报文'), isTrue);
    expect(source.connectedDevice!.can('母线电流'), isFalse);
    await source.start(const LiveStartOptions());
    expect(frameOf(transport.written.single).body, [1, 2, 0, 0, 50, 0, 0, 0]);
    final gotRow = source.events.where((event) => event is LiveRowsEvent).first;
    transport.push(
      dataFrame(transport.nextId++, 10, pdWire(0x0183)),
      split: true,
    );
    final rows =
        await gotRow.timeout(const Duration(seconds: 3)) as LiveRowsEvent;
    expect(rows.rows.single['msgType'], 'Accept');
    expect(rows.rows.single['crc'], 'ok');
    expect(rows.details[0]!['rawPayload'], pdWire(0x0183));
    final gotBus = source.events.where((event) => event is LiveBusEvent).first;
    transport.push(
      evtFrame(transport.nextId++, [
        [4, 23, 4, 900, 0xffff, 0],
      ]),
    );
    final bus = await gotBus as LiveBusEvent;
    expect(bus.t, [0.023]);
    expect(bus.vbus, [9.0]);
    expect(bus.ibus.single.isNaN, isTrue);
    transport.tailOnEnd = true;
    await source.stop();
    await settle();
    expect(
      events.whereType<LiveRowsEvent>().expand((event) => event.rows),
      hasLength(2),
    );
    expect(events.whereType<LiveStateEvent>().last.state, LiveState.stopped);
    expect(events.whereType<LiveStatsEvent>().last.durationSec, 0.08);
    expect(frameOf(transport.written.last).body, [1]);
    expect(source.supportsPause, isFalse);
  });

  test('非法尾部不提交前缀、CRC 错误恢复、粘滞损失不反复计数', () async {
    await connect();
    await source.start(const LiveStartOptions());
    final body = [
      ...frameOf(dataFrame(1, 10, pdWire(0x0183))).body,
      99,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
    ];
    transport.push(PclWire.encode(1, body));
    final bad = dataFrame(2, 11, pdWire(0x0183))..last ^= 1;
    transport.push(bad);
    final gotRow = source.events.where((event) => event is LiveRowsEvent).first;
    transport.push(dataFrame(3, 12, pdWire(0x0183), loss: 1));
    await gotRow;
    transport.push(dataFrame(4, 13, pdWire(0x0183), loss: 1));
    await settle();
    final stats = events.whereType<LiveStatsEvent>().last;
    expect(stats.sequenceGaps, 2);
    expect(stats.invalidFrames, 2);
    expect(stats.lossReports, 1);
    expect(stats.lossCountKnown, isFalse);
    expect(
      events.whereType<LiveRowsEvent>().expand((event) => event.rows),
      hasLength(2),
    );
    transport.nextId = 5;
  });

  test('START 与坏 CRC 同次到达，重置统计仍保留后续校验错误', () async {
    transport.corruptAfterStart = true;
    await connect();
    await source.start(const LiveStartOptions());
    final gotRow = source.events.where((event) => event is LiveRowsEvent).first;
    transport.push(dataFrame(transport.nextId++, 2, pdWire(0x0183)));
    await gotRow;
    await settle();
    final stats = events.whereType<LiveStatsEvent>().last;
    expect(stats.invalidFrames, 1);
    expect(stats.sequenceGaps, 1);
  });

  test('电流按 1 mA 换算，真实0与FFFF未知分别保留', () async {
    transport.features |= PclFeature.current;
    await connect();
    await source.start(const LiveStartOptions(current: true));
    final gotBus = source.events
        .where((event) => event is LiveBusEvent)
        .take(3)
        .toList();
    transport.push(
      evtFrame(transport.nextId++, [
        [4, 10, 12, 900, 1234, 0],
        [4, 11, 12, 900, 0, 0],
        [4, 12, 12, 900, 0xffff, 0],
      ]),
    );
    final currents = (await gotBus)
        .cast<LiveBusEvent>()
        .expand((bus) => bus.ibus)
        .toList();
    expect(currents.take(2), [1.234, 0.0]);
    expect(currents.last.isNaN, isTrue);
  });

  test('重复 STOP 幂等、新采集从0编号、设备刻度归零', () async {
    await connect();
    await source.start(const LiveStartOptions());
    final gotRow = source.events.where((event) => event is LiveRowsEvent).first;
    transport.push(dataFrame(transport.nextId++, 15, pdWire(0x0183)));
    await gotRow;
    await source.stop();
    final oldStop = transport.cachedStop!;
    transport.push(oldStop);
    await settle();
    expect(
      events.whereType<LiveStateEvent>().where(
        (event) => event.state == LiveState.stopped,
      ),
      hasLength(1),
    );
    await source.start(const LiveStartOptions());
    final gotFresh = source.events
        .where((event) => event is LiveRowsEvent)
        .first;
    transport.push(dataFrame(transport.nextId++, 2, pdWire(0x0183)));
    final fresh = await gotFresh as LiveRowsEvent;
    expect(fresh.rows.single['index'], 0);
    expect(fresh.rows.single['timeMs'], 2.0);
  });

  test('START 不匹配或丢失时使用 END 恢复，绝不自动续采', () async {
    await connect();
    transport.matchStart = false;
    await expectLater(
      source.start(const LiveStartOptions()),
      throwsA(isA<LiveSourceError>()),
    );
    expect(transport.written.map((bytes) => frameOf(bytes).type), [3, 4]);
    expect(
      events.whereType<LiveStateEvent>().any(
        (event) => event.state == LiveState.capturing,
      ),
      isFalse,
    );
    expect(transport.running, isFalse);
    transport.matchStart = true;
    await source.start(const LiveStartOptions());
  });

  test('未收到 STOP 不能声称排空，超时关闭并保留诊断', () async {
    await connect();
    await source.start(const LiveStartOptions());
    transport.sendStop = false;
    await expectLater(source.stop(), throwsA(isA<LiveSourceError>()));
    await settle();
    expect(transport.isOpen, isFalse);
    expect(events.whereType<LiveStateEvent>().last.state, LiveState.parked);
    expect(events.whereType<LiveStateEvent>().last.message, contains('结束未确认'));
    expect(
      transport.written.where((wire) => frameOf(wire).type == 4),
      hasLength(2),
    );
  });

  test('运行中意外 HELLO 中断旧采集并保留记录，不发新 CFG', () async {
    await connect();
    await source.start(const LiveStartOptions());
    transport.push(helloFrame());
    await settle();
    expect(events.whereType<LiveStateEvent>().last.state, LiveState.stopped);
    expect(events.whereType<LiveStateEvent>().last.message, contains('结束未确认'));
    expect(transport.written, hasLength(1));
  });

  test('未配置端点的只读监听全程0写入，之后正常连接', () async {
    final device = (await source.enumerate()).single;
    transport.sendHelloOnOpen = false;
    final listening = source.listenDevice(device, 30);
    await Future<void>.delayed(const Duration(milliseconds: 5));
    transport.push(dataFrame(123, 42, pdWire(0x0183)));
    expect(await listening, 1);
    expect(transport.written, isEmpty);
    expect(transport.isOpen, isFalse);
    transport.sendHelloOnOpen = true;
    await source.connect(device);
  });

  test('打开仍采集的端点时 END(0) 恢复，等待 HELLO 后才就绪', () async {
    final device = (await source.enumerate()).single;
    transport.sendHelloOnOpen = false;
    final connecting = source.connect(device);
    await Future<void>.delayed(const Duration(milliseconds: 5));
    transport.push(dataFrame(88, 500, pdWire(0x0183)));
    await connecting;
    expect(frameOf(transport.written.single).body, [0]);
    expect(events.whereType<LiveRowsEvent>(), isEmpty);
  });

  test('后台解码器关闭幂等，Reset 和异常前缀不伪造 CRC', () async {
    final decoder = PclNativePdDecoder();
    await decoder.reset();
    final result = await decoder.decodeBatch([
      PclDecodeInput(
        bytes: Uint8List(0),
        sop: 3,
        cc: 1,
        flags: 2,
        elapsedTicks: 1,
        timebaseHz: 1000,
        index: 0,
      ),
      PclDecodeInput(
        bytes: Uint8List.fromList([0x81, 0x11, 0xa5, 0x5a]),
        sop: 0,
        cc: 1,
        flags: 6,
        elapsedTicks: 2,
        timebaseHz: 1000,
        index: 1,
      ),
    ]);
    expect(result.first['row']['msgType'], 'Hard Reset');
    expect(result.last['detail']['crcValue'], isNull);
    expect(result.last['row']['durationUs'], isNull);
    await decoder.close();
    await decoder.close();
  });
}
