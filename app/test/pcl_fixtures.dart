import 'dart:async';
import 'dart:typed_data';

import 'package:pdscope_app/core/pcl_protocol.dart';
import 'package:pdscope_app/core/pcl_transport.dart';

/// CH32X035 当前固件能力的 HELLO，测试不打开任何真实端点。
Uint8List helloFrame({int error = 0, int featureCaps = 1 | 2 | 16 | 32 | 64}) {
  final body = Uint8List(33);
  final view = ByteData.sublistView(body);
  body[0] = 1;
  view.setUint16(1, 0x314f, Endian.little);
  body[3] = 1;
  body[5] = 1;
  view.setUint32(6, featureCaps, Endian.little);
  view.setUint32(10, 0x7f, Endian.little);
  view.setUint32(18, 1000, Endian.little);
  view.setUint16(22, 384, Endian.little);
  view.setUint16(24, 268, Endian.little);
  view.setUint16(26, 2048, Endian.little);
  body[30] = error;
  view.setUint16(31, 5, Endian.little);
  return PclWire.encode(PclWire.hello, body);
}

/// 从传输字节获得已校验测试帧。
PclFrame frameOf(Uint8List wire) =>
    PclStreamParser().add(wire, nowMs: 0).single;

/// 构造独立或普通 EVT；每个 item 顺序为 code/ticks/flags/a/b/c。
Uint8List evtFrame(int id, List<List<int>> items, {int loss = 0}) {
  final body = Uint8List(5 + 12 * items.length);
  final view = ByteData.sublistView(body);
  view.setUint32(0, id, Endian.little);
  body[4] = loss;
  for (var i = 0; i < items.length; i++) {
    final item = items[i];
    final offset = 5 + i * 12;
    body[offset] = item[0];
    view.setUint32(offset + 1, item[1], Endian.little);
    body[offset + 5] = item[2];
    for (var field = 3; field < 6; field++) {
      view.setUint16(offset + 6 + (field - 3) * 2, item[field], Endian.little);
    }
  }
  return PclWire.encode(PclWire.evt, body);
}

/// 构造单条 PD DATA，保留完整包及接收 flags。
Uint8List dataFrame(
  int id,
  int ticks,
  List<int> payload, {
  int flags = 0,
  int sop = 0,
  int cc = 1,
  int loss = 0,
}) {
  final body = Uint8List(15 + payload.length);
  final view = ByteData.sublistView(body);
  view.setUint32(0, id, Endian.little);
  body[4] = loss;
  body[5] = 1;
  body[6] = sop;
  body[7] = cc;
  body[8] = flags;
  view.setUint32(9, ticks, Endian.little);
  view.setUint16(13, payload.length, Endian.little);
  body.setRange(15, body.length, payload);
  return PclWire.encode(PclWire.data, body);
}

/// 独立生成 PD wire CRC；正文与 CRC 按接收顺序组成完整 payload。
Uint8List pdWire(int header, [List<int> words = const []]) {
  final bytes = Uint8List(2 + words.length * 4 + 4);
  final view = ByteData.sublistView(bytes);
  view.setUint16(0, header, Endian.little);
  for (var i = 0; i < words.length; i++) {
    view.setUint32(2 + i * 4, words[i], Endian.little);
  }
  var crc = 0xffffffff;
  for (final byte in bytes.take(bytes.length - 4)) {
    crc ^= byte;
    for (var bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
    }
  }
  view.setUint32(bytes.length - 4, crc ^ 0xffffffff, Endian.little);
  return bytes;
}

/// 有序内存端点，模拟协议对端；只用于验证上位机收发逻辑。
class MemoryPclTransport implements PclTransport {
  final controller = StreamController<Uint8List>.broadcast();
  final written = <Uint8List>[];
  bool isOpen = false;
  bool sendHelloOnOpen = true;
  bool sendStart = true;
  bool matchStart = true;
  bool corruptAfterStart = false;
  bool sendStop = true;
  bool tailOnEnd = false;
  bool rejectConfig = false;
  bool running = false;
  int features = 1 | 2 | 16 | 32 | 64;
  int nextId = 1;
  Uint8List? cachedStop;
  final endpoint = const PclEndpoint(
    id: 'memory',
    name: '内存 PCL',
    transport: PclTransportKind.uart,
  );

  @override
  Stream<Uint8List> get incoming => controller.stream;
  @override
  Future<List<PclEndpoint>> enumerate() async => [endpoint];
  @override
  Future<void> open(PclEndpoint endpoint, {int baudRate = 921600}) async {
    isOpen = true;
    if (sendHelloOnOpen) push(helloFrame(featureCaps: features), split: true);
  }

  /// 任意字节块均按传输层复制，模拟 DMA 缓冲可立即被复用。
  void push(Uint8List bytes, {bool split = false}) {
    if (split) {
      for (final byte in bytes) {
        controller.add(Uint8List.fromList([byte]));
      }
    } else {
      controller.add(Uint8List.fromList(bytes));
    }
  }

  @override
  Future<void> write(Uint8List bytes) async {
    if (!isOpen) throw StateError('端点未打开');
    written.add(Uint8List.fromList(bytes));
    final frame = frameOf(bytes);
    if (frame.type == PclWire.cfg) {
      if (rejectConfig) {
        push(helloFrame(error: 7, featureCaps: features));
        return;
      }
      running = true;
      nextId = 1;
      cachedStop = null;
      if (sendStart) {
        final view = ByteData.sublistView(frame.body);
        final start = evtFrame(0, [
          [
            1,
            0,
            frame.body[0],
            (matchStart ? frame.body[1] : frame.body[1] ^ 1) |
                (frame.body[6] << 8),
            view.getUint16(4, Endian.little),
            view.getUint16(2, Endian.little),
          ],
        ]);
        if (corruptAfterStart) {
          final bad = dataFrame(nextId++, 1, pdWire(0x0183))..last ^= 1;
          push(Uint8List.fromList([...start, ...bad]));
        } else {
          push(start);
        }
      }
    } else if (frame.type == PclWire.end) {
      if (running && sendStop) {
        if (tailOnEnd) push(dataFrame(nextId++, 75, pdWire(0x0183)));
        cachedStop = evtFrame(nextId++, [
          [2, 80, 0, 0, 0, 0],
        ]);
        push(cachedStop!);
      } else if (!running && cachedStop != null) {
        push(cachedStop!);
      }
      running = false;
      if (sendStop) push(helloFrame(featureCaps: features));
    }
  }

  @override
  Future<void> close() async {
    isOpen = false;
  }

  /// 测试结束释放实例，不受串口硬件影响。
  Future<void> dispose() async {
    await close();
    await controller.close();
  }
}
