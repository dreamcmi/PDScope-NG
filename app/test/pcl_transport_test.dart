import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/pcl_transport.dart';

/// 仅用于契约测试的内存传输，不访问真实设备。
class _FakeTransport implements PclTransport {
  final StreamController<Uint8List> _controller =
      StreamController<Uint8List>.broadcast();
  bool isOpen = false;
  final List<Uint8List> written = <Uint8List>[];

  @override
  Stream<Uint8List> get incoming => _controller.stream;

  @override
  Future<List<PclEndpoint>> enumerate() async => const <PclEndpoint>[
    PclEndpoint(
      id: 'fake-pcl-0',
      name: '测试设备',
      transport: PclTransportKind.uart,
      description: '内存模拟端点',
    ),
  ];

  @override
  Future<void> open(PclEndpoint endpoint, {int baudRate = 921600}) async {
    isOpen = true;
  }

  @override
  Future<void> write(Uint8List bytes) async {
    if (!isOpen) throw StateError('未连接');
    written.add(Uint8List.fromList(bytes));
  }

  @override
  Future<void> close() async {
    isOpen = false;
    await _controller.close();
  }
}

void main() {
  test('通用传输契约可注入内存 fake，端点类型和字节方向清晰', () async {
    final transport = _FakeTransport();
    final endpoints = await transport.enumerate();
    expect(endpoints, hasLength(1));
    expect(endpoints.single.transport, PclTransportKind.uart);
    expect(endpoints.single.id, 'fake-pcl-0');

    await transport.open(endpoints.single);
    final received = expectLater(
      transport.incoming,
      emits(Uint8List.fromList(<int>[0x50, 0x44, 0x31])),
    );
    transport._controller.add(Uint8List.fromList(<int>[0x50, 0x44, 0x31]));

    final command = Uint8List.fromList(<int>[0x01, 0x02]);
    await transport.write(command);
    command[0] = 0xFF;
    expect(transport.written.single, <int>[0x01, 0x02]);
    await received;
    await transport.close();
    expect(transport.isOpen, isFalse);
  });
}
