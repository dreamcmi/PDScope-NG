import 'dart:typed_data';

import 'pcl_transport.dart';

/// PCL v1、schema 0x314F 的帧类型与硬上限。
abstract final class PclWire {
  static const data = 1;
  static const hello = 2;
  static const cfg = 3;
  static const end = 4;
  static const evt = 5;
  static const maxBody = 512;

  /// 计算不反射、初值 FFFF 的 CRC-16/CCITT-FALSE。
  static int crc16(List<int> bytes, [int start = 0, int? end]) {
    var crc = 0xffff;
    for (var i = start; i < (end ?? bytes.length); i++) {
      crc ^= bytes[i] << 8;
      for (var bit = 0; bit < 8; bit++) {
        crc = ((crc << 1) ^ ((crc & 0x8000) != 0 ? 0x1021 : 0)) & 0xffff;
      }
    }
    return crc;
  }

  /// 组装完整字节流帧；不向传输层泄漏可复用的内部缓冲。
  static Uint8List encode(int type, List<int> body) {
    if (type < 0 || type > 255 || body.length > maxBody) {
      throw const PclProtocolError('帧类型或长度超出 PCL 范围');
    }
    final out = Uint8List(body.length + 7);
    final view = ByteData.sublistView(out);
    out[0] = 0xa5;
    out[1] = 0x5a;
    out[2] = type;
    view.setUint16(3, body.length, Endian.little);
    out.setRange(5, 5 + body.length, body);
    view.setUint16(
      5 + body.length,
      crc16(out, 2, 5 + body.length),
      Endian.little,
    );
    return out;
  }

  /// 提取单个 HID 报告的有效字节；Report ID 由接口层固定。
  static Uint8List hidBytes(Uint8List report, {int? reportId}) {
    final offset = reportId == null ? 0 : 1;
    if (report.length <= offset ||
        (reportId != null && report[0] != reportId)) {
      throw const PclProtocolError('HID 报告缺少有效长度或 Report ID 不符');
    }
    final size = report[offset];
    if (size > report.length - offset - 1 ||
        report.skip(offset + 1 + size).any((value) => value != 0)) {
      throw const PclProtocolError('HID 有效长度或填充非法');
    }
    return Uint8List.fromList(report.sublist(offset + 1, offset + 1 + size));
  }
}

/// 校验或语义验证失败；调用者应整帧拒绝。
class PclProtocolError implements Exception {
  const PclProtocolError(this.message);
  final String message;
  @override
  String toString() => message;
}

/// 已通过长度和 CRC16 校验的帧，所有字节均独立持有。
class PclFrame {
  PclFrame(
    this.type,
    this.body,
    this.bytes, [
    this.receivedAtMs = 0,
    this.parserErrorsBefore = 0,
  ]);
  final int type;
  final Uint8List body;
  final Uint8List bytes;

  /// 最后字节的本地到达时间，仅用于检测计时歧义与半帧超时。
  final int receivedAtMs;

  /// 此帧完成之前已观察到的字节流校验错误数。
  final int parserErrorsBefore;
}

/// 有界字节流重组器，不将读取边界当作帧边界。
///
/// 长度或 CRC 错误从候选 SYNC 下一字节恢复。CRC 正确的语义非法帧
/// 由上层原子拒绝，不能重新解析 BODY 中的 SYNC。
class PclStreamParser {
  final List<int> _pending = [];
  int _lastInputMs = 0;
  int invalidFrames = 0;
  int maxBodyLength = PclWire.maxBody;

  /// 当前缓存的未完整候选字节数，始终不超过 519。
  int get bufferedBytes => _pending.length;

  /// 串行输入任意大小的读取块，并返回其中全部完整帧。
  List<PclFrame> add(Uint8List bytes, {required int nowMs}) {
    final frames = expire(nowMs);
    var cursor = 0;
    while (cursor < bytes.length) {
      final size = (PclWire.maxBody + 7 - _pending.length).clamp(
        0,
        bytes.length - cursor,
      );
      _pending.addAll(bytes.sublist(cursor, cursor + size));
      cursor += size;
      _lastInputMs = nowMs;
      _drain(frames);
    }
    return frames;
  }

  /// 半帧连续 1000 ms 无新字节时放弃候选；不停止采集。
  List<PclFrame> expire(int nowMs) {
    final frames = <PclFrame>[];
    if (_pending.isNotEmpty && nowMs - _lastInputMs >= 1000) {
      _pending.removeAt(0);
      invalidFrames++;
      _drain(frames);
      _lastInputMs = nowMs;
    }
    return frames;
  }

  /// 新连接清空候选及协商上限。
  void reset() {
    _pending.clear();
    invalidFrames = 0;
    maxBodyLength = PclWire.maxBody;
    _lastInputMs = 0;
  }

  void _drain(List<PclFrame> frames) {
    while (_pending.length >= 2) {
      var sync = -1;
      for (var i = 0; i + 1 < _pending.length; i++) {
        if (_pending[i] == 0xa5 && _pending[i + 1] == 0x5a) {
          sync = i;
          break;
        }
      }
      if (sync < 0) {
        final keep = _pending.last == 0xa5;
        _pending.clear();
        if (keep) _pending.add(0xa5);
        return;
      }
      if (sync > 0) _pending.removeRange(0, sync);
      if (_pending.length < 5) return;
      final length = _pending[3] | (_pending[4] << 8);
      if (length > maxBodyLength || length > PclWire.maxBody) {
        invalidFrames++;
        _pending.removeAt(0);
        continue;
      }
      final total = length + 7;
      if (_pending.length < total) return;
      final crc = _pending[total - 2] | (_pending[total - 1] << 8);
      if (crc != PclWire.crc16(_pending, 2, total - 2)) {
        invalidFrames++;
        _pending.removeAt(0);
        continue;
      }
      frames.add(
        PclFrame(
          _pending[2],
          Uint8List.fromList(_pending.sublist(5, total - 2)),
          Uint8List.fromList(_pending.sublist(0, total)),
          _lastInputMs,
          invalidFrames,
        ),
      );
      _pending.removeRange(0, total);
    }
  }
}

/// HELLO 功能能力位，与下位机保持一致。
abstract final class PclFeature {
  static const pd = 1;
  static const voltage = 2;
  static const current = 4;
  static const dpdm = 8;
  static const goodCrcFilter = 16;
  static const extendedPayload = 32;
  static const ccSelect = 64;
  static const dualCc = 128;
}

/// 已完整验证的设备声明，不通过枚举推断能力。
class PclHello {
  PclHello._(ByteData body)
    : firmwareMajor = body.getUint8(3),
      firmwareMinor = body.getUint8(4),
      transports = body.getUint8(5),
      featureCaps = body.getUint32(6, Endian.little),
      pdCaps = body.getUint32(10, Endian.little),
      dpdmCaps = body.getUint32(14, Endian.little),
      timebaseHz = body.getUint32(18, Endian.little),
      maxFrame = body.getUint16(22, Endian.little),
      maxPdBytes = body.getUint16(24, Endian.little),
      ringBytes = body.getUint16(26, Endian.little),
      maxDpdmPayload = body.getUint16(28, Endian.little),
      cfgError = body.getUint8(30),
      minSamplePeriodMs = body.getUint16(31, Endian.little);

  final int firmwareMajor;
  final int firmwareMinor;
  final int transports;
  final int featureCaps;
  final int pdCaps;
  final int dpdmCaps;
  final int timebaseHz;
  final int maxFrame;
  final int maxPdBytes;
  final int ringBytes;
  final int maxDpdmPayload;
  final int cfgError;
  final int minSamplePeriodMs;

  /// 检查功能位是否已由设备明确声明。
  bool has(int feature) => (featureCaps & feature) != 0;

  /// 解析固定布局、位域与能力交叉约束，并核对当前接口。
  static PclHello decode(PclFrame frame, PclTransportKind transport) {
    _require(
      frame.type == PclWire.hello && frame.body.length == 33,
      'HELLO 长度非法',
    );
    final data = ByteData.sublistView(frame.body);
    _require(
      data.getUint8(0) == 1 && data.getUint16(1, Endian.little) == 0x314f,
      'PCL 版本或 schema 不匹配',
    );
    final hello = PclHello._(data);
    final bit = switch (transport) {
      PclTransportKind.uart => 1,
      PclTransportKind.hid => 2,
      PclTransportKind.bulk => 4,
      PclTransportKind.cdc => 8,
    };
    _require(
      hello.transports > 0 &&
          hello.transports <= 15 &&
          (hello.transports & bit) != 0,
      'HELLO 未声明当前传输接口',
    );
    _require(
      hello.featureCaps <= 0xff && hello.pdCaps <= 0x1ff && hello.dpdmCaps <= 1,
      'HELLO 能力保留位非零',
    );
    _require(
      hello.timebaseHz >= 1000 &&
          hello.timebaseHz <= 1000000 &&
          hello.maxFrame >= 128 &&
          hello.maxFrame <= 512 &&
          hello.cfgError <= 10,
      'HELLO 时基、帧上限或错误码非法',
    );
    final pd = hello.has(PclFeature.pd);
    final dpdm = hello.has(PclFeature.dpdm);
    _require(
      pd
          ? hello.maxPdBytes >= 34 && hello.maxPdBytes <= 268
          : hello.maxPdBytes == 0 &&
                (hello.pdCaps & 0x7f) == 0 &&
                (hello.featureCaps & 0xf0) == 0,
      'HELLO PD 能力与字节上限不一致',
    );
    _require(
      dpdm
          ? hello.dpdmCaps == 1 &&
                hello.maxDpdmPayload >= 1 &&
                hello.maxDpdmPayload <= 128
          : hello.dpdmCaps == 0 && hello.maxDpdmPayload == 0,
      'HELLO DPDM 能力不一致',
    );
    _require(
      !hello.has(PclFeature.extendedPayload) ||
          (pd && hello.maxPdBytes == 268 && hello.maxFrame >= 283),
      'HELLO 扩展 PD 能力不一致',
    );
    _require(
      !hello.has(PclFeature.goodCrcFilter) || (hello.pdCaps & 0x40) != 0,
      'GoodCRC 过滤缺少 CRC 检查能力',
    );
    final largest = hello.maxPdBytes > hello.maxDpdmPayload
        ? hello.maxPdBytes
        : hello.maxDpdmPayload;
    _require(
      largest == 0 ||
          (15 + largest <= hello.maxFrame && 10 + largest <= hello.ringBytes),
      'HELLO 帧或记录队列容量不足',
    );
    final analog = (hello.featureCaps & 6) != 0;
    _require(
      analog
          ? hello.minSamplePeriodMs >= 1 &&
                hello.minSamplePeriodMs <= 1000 &&
                hello.minSamplePeriodMs * hello.timebaseHz >= 1000
          : hello.minSamplePeriodMs == 0,
      'HELLO 测量周期不一致',
    );
    return hello;
  }
}

/// 原子 CFG 参数；序列化后与 START 逐字段核对。
class PclConfig {
  const PclConfig({
    this.mode = 1,
    this.opts = 0,
    this.hostTimeoutMs = 0,
    this.samplePeriodMs = 0,
    this.channelSelect = 0,
  });
  final int mode;
  final int opts;
  final int hostTimeoutMs;
  final int samplePeriodMs;
  final int channelSelect;

  /// 检查设备能力，拒绝隐式忽略选项或降级。
  void validate(PclHello hello, PclTransportKind transport) {
    _require(mode >= 1 && mode <= 3 && opts >= 0 && opts <= 7, 'CFG 数据源或选项非法');
    _require((mode & 1) == 0 || hello.has(PclFeature.pd), '设备不支持 PD');
    _require((mode & 2) == 0 || hello.has(PclFeature.dpdm), '设备不支持 DPDM');
    _require(
      (opts & 1) == 0 ||
          ((mode & 1) != 0 && hello.has(PclFeature.goodCrcFilter)),
      '设备不支持 GoodCRC 过滤',
    );
    _require((opts & 2) == 0 || hello.has(PclFeature.voltage), '设备不支持电压测量');
    _require((opts & 4) == 0 || hello.has(PclFeature.current), '设备不支持电流测量');
    _require(
      (opts & 6) == 0
          ? samplePeriodMs == 0
          : samplePeriodMs >= hello.minSamplePeriodMs && samplePeriodMs <= 1000,
      'ADC 周期超出范围',
    );
    _require(
      transport == PclTransportKind.uart
          ? hostTimeoutMs == 0
          : hostTimeoutMs >= 100 && hostTimeoutMs <= 60000,
      '主机超时不适用于当前接口',
    );
    _require(
      channelSelect >= 0 &&
          channelSelect <= 3 &&
          (mode != 3 || channelSelect == 0 || channelSelect == 3),
      '数据通道组合非法',
    );
    _require(
      (mode & 1) == 0 ||
          channelSelect == 0 ||
          (channelSelect == 3
              ? hello.has(PclFeature.dualCc)
              : hello.has(PclFeature.ccSelect)),
      '设备不支持所选 CC 通道',
    );
  }

  /// 组装 CFG，保留位固定为零。
  Uint8List encode() {
    final body = Uint8List(8);
    final data = ByteData.sublistView(body);
    body[0] = mode;
    body[1] = opts;
    data.setUint16(2, hostTimeoutMs, Endian.little);
    data.setUint16(4, samplePeriodMs, Endian.little);
    body[6] = channelSelect;
    return PclWire.encode(PclWire.cfg, body);
  }

  /// START 必须回显全部已发送配置。
  bool matches(PclEvent event) =>
      event.code == 1 &&
      event.ticks == 0 &&
      event.flags == mode &&
      event.a == (opts | (channelSelect << 8)) &&
      event.b == samplePeriodMs &&
      event.c == hostTimeoutMs;
}

/// DATA 中的一条完整、独立持有原始字节的记录。
class PclRecord {
  PclRecord(
    this.type,
    this.kind,
    this.channel,
    this.flags,
    this.ticks,
    this.bytes,
  );
  final int type;

  /// PD 为 SOP 类型，DPDM 为 link。
  final int kind;

  /// PD 为 CC，DPDM 为 proto。
  final int channel;
  final int flags;
  final int ticks;
  final Uint8List bytes;
}

/// 已原子验证的 DATA 帧，非法尾部不会提交合法前缀。
class PclData {
  PclData._(this.id, this.lossFlags, this.records);
  final int id;
  final int lossFlags;
  final List<PclRecord> records;

  /// 按 HELLO 能力和本次 CFG 验证全部记录。
  static PclData decode(PclFrame frame, PclHello hello, PclConfig config) {
    _require(
      frame.type == PclWire.data && frame.body.length >= 15,
      'DATA 长度非法',
    );
    final body = frame.body;
    final data = ByteData.sublistView(body);
    final records = <PclRecord>[];
    _require(body[4] <= 0x1f, 'DATA 损失保留位非零');
    var offset = 5;
    while (offset < body.length) {
      _require(offset + 10 <= body.length, 'DATA 记录头不完整');
      final type = body[offset];
      final kind = body[offset + 1];
      final channel = body[offset + 2];
      final flags = body[offset + 3];
      final length = data.getUint16(offset + 8, Endian.little);
      _require(offset + 10 + length <= body.length, 'DATA 记录跨帧');
      if (type == 1) {
        _require(
          (config.mode & 1) != 0 &&
              kind <= 5 &&
              channel <= 2 &&
              flags <= 15 &&
              (flags & 3) != 3 &&
              length <= hello.maxPdBytes,
          'PD 记录字段非法',
        );
        _require(kind == 5 || (hello.pdCaps & (1 << kind)) != 0, '设备上报未声明 SOP');
        if (kind == 3 || kind == 4) {
          _require(length == 0 && flags == 2, 'Reset 记录必须无 payload 且 CRC 未知');
        } else {
          _require(length >= ((flags & 12) != 0 ? 2 : 6), 'PD payload 长度非法');
          _require((flags & 9) == 0 || (hello.pdCaps & 0x20) != 0, '坏包字节未声明');
          _require(
            (flags & 2) != 0 || (hello.pdCaps & 0x40) != 0,
            '明确 CRC 状态未声明',
          );
        }
      } else if (type == 2) {
        _require(
          (config.mode & 2) != 0 &&
              kind <= 2 &&
              channel == 1 &&
              flags <= 1 &&
              length >= 1 &&
              length <= hello.maxDpdmPayload &&
              hello.dpdmCaps == 1,
          'DPDM 记录字段非法',
        );
      } else {
        throw const PclProtocolError('DATA 记录类型未定义');
      }
      records.add(
        PclRecord(
          type,
          kind,
          channel,
          flags,
          data.getUint32(offset + 4, Endian.little),
          Uint8List.fromList(body.sublist(offset + 10, offset + 10 + length)),
        ),
      );
      offset += 10 + length;
    }
    return PclData._(data.getUint32(0, Endian.little), body[4], records);
  }
}

/// EVT 中的单项事件，时间保留设备原始 u32 值。
class PclEvent {
  const PclEvent(this.code, this.ticks, this.flags, this.a, this.b, this.c);
  final int code;
  final int ticks;
  final int flags;
  final int a;
  final int b;
  final int c;
}

/// 已原子验证的单向设备 EVT。
class PclEvents {
  PclEvents._(this.id, this.lossFlags, this.items);
  final int id;
  final int lossFlags;
  final List<PclEvent> items;

  /// 验证事件长度、保留位、独立 START/STOP/ERROR 与测量配置。
  static PclEvents decode(PclFrame frame, PclHello hello, PclConfig? config) {
    _require(
      frame.type == PclWire.evt &&
          frame.body.length >= 17 &&
          (frame.body.length - 5) % 12 == 0,
      'EVT 长度非法',
    );
    final body = frame.body;
    final data = ByteData.sublistView(body);
    _require(body[4] <= 0x1f, 'EVT 损失保留位非零');
    final events = <PclEvent>[];
    for (var offset = 5; offset < body.length; offset += 12) {
      final event = PclEvent(
        body[offset],
        data.getUint32(offset + 1, Endian.little),
        body[offset + 5],
        data.getUint16(offset + 6, Endian.little),
        data.getUint16(offset + 8, Endian.little),
        data.getUint16(offset + 10, Endian.little),
      );
      _require(event.code >= 1 && event.code <= 5, 'EVT 事件码未定义');
      if (event.code <= 3) {
        _require(body.length == 17, 'START/STOP/ERROR 必须独立成帧');
      }
      switch (event.code) {
        case 1:
          _require(
            data.getUint32(0, Endian.little) == 0 && event.ticks == 0,
            'START 编号或时间未归零',
          );
          final echoed = PclConfig(
            mode: event.flags,
            opts: event.a & 0xff,
            channelSelect: event.a >> 8,
            samplePeriodMs: event.b,
            hostTimeoutMs: event.c,
          );
          // 接口超时在源层匹配本次 CFG；这里检查其余布局和能力。
          echoed.validate(
            hello,
            event.c == 0 ? PclTransportKind.uart : PclTransportKind.bulk,
          );
        case 2:
          _require(
            event.flags <= 2 && event.a <= 10 && event.b == 0 && event.c == 0,
            'STOP 字段非法',
          );
        case 3:
          _require(
            event.flags <= 5 &&
                event.a >= 1 &&
                event.a <= 10 &&
                event.b == 0 &&
                event.c == 0,
            'ERROR 字段非法',
          );
        case 4:
          _require(
            config != null &&
                (config.opts & 6) != 0 &&
                event.flags >= 1 &&
                event.flags <= 15 &&
                event.c == 0,
            'MEASUREMENT 字段或配置非法',
          );
          _require(
            ((config!.opts & 2) != 0 || event.a == 0xffff) &&
                ((config.opts & 4) != 0 || event.b == 0xffff),
            '未请求通道必须为 FFFF',
          );
        case 5:
          _require(
            event.flags <= 7 &&
                event.a <= 15 &&
                event.b == 0 &&
                event.c == 0 &&
                ((event.a >> 1) & 3) != 3,
            'LINK_STATE 字段非法',
          );
          _require(
            ((event.flags & 1) != 0 || (event.a & 1) == 0) &&
                ((event.flags & 2) != 0 || (event.a & 6) == 0) &&
                ((event.flags & 4) != 0 || (event.a & 8) == 0),
            'LINK_STATE 无效字段非零',
          );
          _require(
            ((event.flags & 3) == 0 || (hello.pdCaps & 0x80) != 0) &&
                ((event.flags & 4) == 0 || (hello.pdCaps & 0x100) != 0),
            '链接观测能力未声明',
          );
      }
      events.add(event);
    }
    return PclEvents._(data.getUint32(0, Endian.little), body[4], events);
  }
}

/// 设备时间扩展结果；歧义后不提供猜测的连续时间。
class PclTimestamp {
  const PclTimestamp(this.rawTicks, this.extendedTicks);
  final int rawTicks;
  final int? extendedTicks;
  bool get uncertain => extendedTicks == null;
}

/// 相对最大时间锚点扩展 u32，允许 DATA/EVT 小幅乱序。
class PclTickClock {
  PclTickClock(this.timebaseHz, {int nowMs = 0}) : _lastHostMs = nowMs;
  final int timebaseHz;
  int _anchor = 0;
  int _lastHostMs;
  bool _uncertain = false;
  int get anchor => _anchor;
  bool get uncertain => _uncertain;

  /// 本地时间仅检测超过半个回绕周期的静默，不替代设备计时。
  PclTimestamp extend(int rawTicks, {required int nowMs}) {
    if (nowMs - _lastHostMs >= 0x80000000 * 1000 / timebaseHz) {
      _uncertain = true;
    }
    if (nowMs > _lastHostMs) _lastHostMs = nowMs;
    var delta = (rawTicks - (_anchor & 0xffffffff)) & 0xffffffff;
    if (delta >= 0x80000000) delta -= 0x100000000;
    final extended = _anchor + delta;
    if (extended < 0) _uncertain = true;
    if (_uncertain) return PclTimestamp(rawTicks, null);
    if (extended > _anchor) _anchor = extended;
    return PclTimestamp(rawTicks, extended);
  }
}

void _require(bool condition, String message) {
  if (!condition) throw PclProtocolError(message);
}
