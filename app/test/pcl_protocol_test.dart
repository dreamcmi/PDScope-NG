import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/pcl_protocol.dart';
import 'package:pdscope_app/core/pcl_transport.dart';

import 'pcl_fixtures.dart';

void main() {
  final hello = PclHello.decode(frameOf(helloFrame()), PclTransportKind.uart);
  const config = PclConfig(opts: 2, samplePeriodMs: 50);

  test('CCITT-FALSE 固定向量、任意拆分、粘包和 BODY 含 SYNC', () {
    expect(PclWire.crc16(ascii.encode('123456789')), 0x29b1);
    final wire = PclWire.encode(99, [0xa5, 0x5a, 0, 1]);
    for (var split = 0; split <= wire.length; split++) {
      final parser = PclStreamParser();
      final frames = [
        ...parser.add(Uint8List.sublistView(wire, 0, split), nowMs: 0),
        ...parser.add(Uint8List.sublistView(wire, split), nowMs: 1),
      ];
      expect(frames.single.body, [0xa5, 0x5a, 0, 1]);
    }
    final parser = PclStreamParser();
    final burst = Uint8List.fromList(
      List.generate(2000, (_) => wire).expand((frame) => frame).toList(),
    );
    expect(parser.add(burst, nowMs: 0), hasLength(2000));
    expect(parser.bufferedBytes, lessThanOrEqualTo(519));
  });

  test('坏 CRC、非法长度、半帧超时从候选下一字节恢复', () {
    final good = helloFrame();
    final bad = Uint8List.fromList(good)..last ^= 1;
    final parser = PclStreamParser();
    expect(
      parser.add(
        Uint8List.fromList([0xa5, 0x5a, 1, 0xff, 0xff, ...bad, ...good]),
        nowMs: 0,
      ),
      hasLength(1),
    );
    expect(parser.invalidFrames, 2);
    parser.add(Uint8List.fromList([0xa5, 0x5a, 1, 0xff, 1, ...good]), nowMs: 1);
    expect(parser.expire(1000), isEmpty);
    expect(parser.expire(1001).single.type, PclWire.hello);
  });

  test('HID 有效长度与固定 Report ID 校验', () {
    expect(
      PclWire.hidBytes(
        Uint8List.fromList([7, 2, 0xa5, 0x5a, 0, 0]),
        reportId: 7,
      ),
      [0xa5, 0x5a],
    );
    expect(
      () => PclWire.hidBytes(Uint8List.fromList([2, 1])),
      throwsA(isA<PclProtocolError>()),
    );
    expect(
      () => PclWire.hidBytes(Uint8List.fromList([1, 2, 9])),
      throwsA(isA<PclProtocolError>()),
    );
  });

  test('HELLO 能力、当前接口、布局和上限交叉约束', () {
    expect(hello.timebaseHz, 1000);
    expect(hello.maxPdBytes, 268);
    expect(hello.has(PclFeature.current), isFalse);
    expect(
      () => PclHello.decode(frameOf(helloFrame()), PclTransportKind.hid),
      throwsA(isA<PclProtocolError>()),
    );
    for (final offset in [0, 1, 5, 9, 13, 17, 21, 23, 25, 29, 30, 32]) {
      final body = Uint8List.fromList(frameOf(helloFrame()).body);
      body[offset] = 0xff;
      expect(
        () => PclHello.decode(
          frameOf(PclWire.encode(2, body)),
          PclTransportKind.uart,
        ),
        throwsA(isA<PclProtocolError>()),
        reason: 'offset=$offset',
      );
    }
  });

  test('CFG 原子验证、UART 超时0、通道能力与 START 全字段匹配', () {
    config.validate(hello, PclTransportKind.uart);
    expect(frameOf(config.encode()).body, [1, 2, 0, 0, 50, 0, 0, 0]);
    expect(
      () => const PclConfig(
        opts: 4,
        samplePeriodMs: 50,
      ).validate(hello, PclTransportKind.uart),
      throwsA(isA<PclProtocolError>()),
    );
    expect(
      () => const PclConfig(
        channelSelect: 3,
      ).validate(hello, PclTransportKind.uart),
      throwsA(isA<PclProtocolError>()),
    );
    expect(config.matches(const PclEvent(1, 0, 1, 2, 50, 0)), isTrue);
    expect(config.matches(const PclEvent(1, 1, 1, 2, 50, 0)), isFalse);
    expect(config.matches(const PclEvent(1, 0, 1, 2, 51, 0)), isFalse);
  });

  test('DATA 整帧验证，完整 PD / Reset / 截短与未知记录尾部', () {
    final bytes = pdWire(0x0183);
    final data = PclData.decode(frameOf(dataFrame(1, 4, bytes)), hello, config);
    expect(data.records.single.bytes, bytes);
    expect(
      PclData.decode(
        frameOf(dataFrame(2, 5, [], sop: 3, flags: 2)),
        hello,
        config,
      ).records.single.bytes,
      isEmpty,
    );
    expect(
      PclData.decode(
        frameOf(dataFrame(3, 6, [0x81, 0x11], flags: 6)),
        hello,
        config,
      ).records.single.flags,
      6,
    );
    final body = Uint8List.fromList([
      ...frameOf(dataFrame(1, 4, bytes)).body,
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
    ]);
    expect(
      () => PclData.decode(frameOf(PclWire.encode(1, body)), hello, config),
      throwsA(isA<PclProtocolError>()),
    );
    expect(
      () => PclData.decode(
        frameOf(dataFrame(1, 4, bytes, flags: 3)),
        hello,
        config,
      ),
      throwsA(isA<PclProtocolError>()),
    );
  });

  test('EVT 全帧验证、1mA 电流和 FFFF 未请求测量值', () {
    final event = PclEvents.decode(
      frameOf(
        evtFrame(2, [
          [4, 50, 4, 500, 0xffff, 0],
        ]),
      ),
      hello,
      config,
    ).items.single;
    expect(event.a / 100, 5.0);
    expect(event.b, 0xffff);
    expect(
      () => PclEvents.decode(
        frameOf(
          evtFrame(2, [
            [4, 50, 4, 500, 0, 0],
          ]),
        ),
        hello,
        config,
      ),
      throwsA(isA<PclProtocolError>()),
    );
    expect(
      () => PclEvents.decode(
        frameOf(
          evtFrame(2, [
            [4, 50, 4, 500, 0xffff, 0],
            [99, 60, 0, 0, 0, 0],
          ]),
        ),
        hello,
        config,
      ),
      throwsA(isA<PclProtocolError>()),
    );
    expect(
      () => PclEvents.decode(
        frameOf(
          evtFrame(0, [
            [1, 1, 1, 2, 50, 0],
          ]),
        ),
        hello,
        config,
      ),
      throwsA(isA<PclProtocolError>()),
    );
  });

  test('u32 时间回绕、小幅乱序、长静默歧义和新 START 归零', () {
    final clock = PclTickClock(1000);
    expect(clock.extend(0x70000000, nowMs: 1).extendedTicks, 0x70000000);
    expect(clock.extend(0xd0000000, nowMs: 2).extendedTicks, 0xd0000000);
    expect(clock.extend(0xfffffffe, nowMs: 2).extendedTicks, 0xfffffffe);
    expect(clock.extend(2, nowMs: 3).extendedTicks, 0x100000002);
    expect(clock.extend(0xffffffff, nowMs: 4).extendedTicks, 0xffffffff);
    expect(clock.anchor, 0x100000002);
    expect(clock.extend(3, nowMs: 0x80000000 + 4).uncertain, isTrue);
    expect(PclTickClock(1000).extend(0, nowMs: 0).extendedTicks, 0);
  });
}
