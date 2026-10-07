// models.dart — C 核心 JSON 契约的 Dart 侧值对象
//
// 字段名**逐一对齐** core/src/session.cpp 里的 packetListItemJson / packetDetailJsonOf /
// metadata / decodeStatsJson / busSeriesJson。改核心的 JSON 就要同步改这里。
import 'dart:typed_data';

double _d(dynamic v, [double fallback = 0]) {
  if (v is num) return v.toDouble();
  if (v is String) return double.tryParse(v) ?? fallback;
  return fallback;
}

int _i(dynamic v, [int fallback = 0]) => v is num ? v.toInt() : fallback;
String _s(dynamic v, [String fallback = '']) => v is String ? v : fallback;
bool _b(dynamic v, [bool fallback = false]) => v is bool ? v : fallback;

/* ────────────────────────── 列表行 ────────────────────────── */

/// 报文表的一行（`pdscope_query_page` 的元素）。
class PacketRow {
  PacketRow(Map<String, dynamic> j)
    : index = _i(j['index']),
      seq = _i(j['seq']),
      channel = _i(j['channel']),
      sop = _s(j['sop']),
      msgType = _s(j['msgType']),
      role = _s(j['role']),
      kind = _s(j['kind']),
      msgKind = _s(j['msgKind']),
      msgId = j['msgId'] == null ? null : _i(j['msgId']),
      objects = j['objects'] == null ? null : _i(j['objects']),
      bytes = j['bytes'] == null ? null : _i(j['bytes']),
      timeMs = _d(j['timeMs'], double.nan),
      timeUncertain = _b(j['timeUncertain']),
      elapsed = _s(j['elapsed']),
      startSample = _i(j['startSample']),
      endSample = _i(j['endSample']),
      durationUs = _d(j['durationUs'], double.nan),
      vbus = _d(j['vbus'], double.nan),
      ibus = _d(j['ibus'], double.nan),
      dataHex = _s(j['dataHex']),
      crc = _s(j['crc']),
      summary = _s(j['summary']),
      warn = _i(j['warn']),
      ackOf = j['ackOf'] == null ? null : _i(j['ackOf']),
      ackType = j['ackType'] == null ? null : _s(j['ackType']);

  final int index;
  final int seq;
  final int channel;
  final String sop;
  final String msgType;
  final String role;
  final String kind; // Control / Data / Extended / VDM / Error / Custom
  final String msgKind;
  final int? msgId;
  final int? objects; // PD：数据对象个数
  final int? bytes; // UFCS：数据字节数
  final double timeMs;
  final bool timeUncertain;
  final String elapsed;
  final int startSample;
  final int endSample;
  final double durationUs;
  final double vbus;
  final double ibus;
  final String dataHex;
  final String crc; // ok | bad | none
  final String summary;
  final int warn;
  final int? ackOf;
  final String? ackType;

  bool get isBadCrc => crc == 'bad';

  /// 「Obj」列：PD 写对象数，UFCS 写字节数。
  String get objText =>
      objects != null ? '$objects' : (bytes != null ? '$bytes' : '');
}

/* ────────────────────────── 详情 ────────────────────────── */

/// 详情面板的一行。`key == 'Object'` 是分组标题哨兵。
class DetailItem {
  DetailItem(this.key, this.value);
  final String key;
  final String value;

  bool get isGroupTitle => key == 'Object' || key == '对象';
}

class PacketWarning {
  PacketWarning(this.long, this.short);
  final String long;
  final String short;
}

/// `pdscope_packet_detail` 的完整结果。
class PacketDetail {
  PacketDetail(Map<String, dynamic> j)
    : row = PacketRow(j),
      text = _s(j['text']),
      header = j['header'] == null ? null : _i(j['header']),
      extHeader = j['extHeader'] == null ? null : _i(j['extHeader']),
      rev = j['rev'] == null ? null : _i(j['rev']),
      revText = j['revText'] == null ? null : _s(j['revText']),
      msgTypeRaw = j['msgTypeRaw'] == null ? null : _i(j['msgTypeRaw']),
      link = _s(j['link']),
      category = _s(j['category']),
      eop = _b(j['eop']),
      synthetic = _b(j['synthetic']),
      roleInferred = _b(j['roleInferred']),
      crcValue = j['crcValue'] == null ? null : _i(j['crcValue']),
      crcCalc = _i(j['crcCalc']),
      details = (j['details'] as List? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map((e) => DetailItem(_s(e['key']), _s(e['value'])))
          .toList(),
      warnings = (j['warnings'] as List? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map((e) => PacketWarning(_s(e['long']), _s(e['short'])))
          .toList(),
      dataWords = (j['dataWords'] as List? ?? const [])
          .map((e) => _i(e))
          .toList(),
      dataBytes = (j['dataBytes'] as List? ?? const [])
          .map((e) => _i(e))
          .toList(),
      rawPayload = (j['rawPayload'] as List? ?? const [])
          .map((e) => _i(e))
          .toList(),
      pdFlags = j['pdFlags'] == null ? null : _i(j['pdFlags']),
      elapsedTicks = j['elapsedTicks'] == null ? null : _i(j['elapsedTicks']);

  final PacketRow row;

  /// PCL 头、正文和原始线上 CRC；截短时为真实连续前缀。
  final List<int> rawPayload;
  final int? pdFlags;
  final int? elapsedTicks;
  final String text;
  final int? header;
  final int? extHeader;
  final int? rev;
  final String? revText;
  final int? msgTypeRaw;
  final String link;
  final String category;
  final bool eop;
  final bool synthetic; // 分析仪逻辑字节（不是从波形解出来的）
  final bool roleInferred;
  final int? crcValue;
  final int crcCalc;
  final List<DetailItem> details;
  final List<PacketWarning> warnings;
  final List<int> dataWords;
  final List<int> dataBytes;

  /// 按 `Object` 哨兵切成分组。
  List<DetailGroup> grouped() {
    final out = <DetailGroup>[];
    DetailGroup? cur;
    for (final d in details) {
      if (d.isGroupTitle) {
        cur = DetailGroup(d.value);
        out.add(cur);
      } else {
        cur ??= () {
          final g = DetailGroup('');
          out.add(g);
          return g;
        }();
        cur.items.add(d);
      }
    }
    return out;
  }
}

class DetailGroup {
  DetailGroup(this.title);
  final String title;
  final List<DetailItem> items = [];
}

/* ────────────────────────── 元数据 ────────────────────────── */

class ChannelInfo {
  ChannelInfo(Map<String, dynamic> j)
    : channel = _i(j['channel']),
      label = _s(j['label']),
      folder = j['folder'] == null ? null : _s(j['folder']),
      totalSamples = _i(j['totalSamples']),
      totalBytes = _i(j['totalBytes']),
      chunks = _i(j['chunks']),
      effectiveSampleLimit = _i(j['effectiveSampleLimit']);

  final int channel;
  final String label;
  final String? folder;
  final int totalSamples;
  final int totalBytes;
  final int chunks;
  final int effectiveSampleLimit;
}

/// 容器级元数据（`pdscope_metadata`）。打开即可取，不触发报文解码。
class CaptureMeta {
  CaptureMeta(Map<String, dynamic> j)
    : name = _s(j['name']),
      source = _s(j['source']),
      container = _s(j['container']),
      protocol = _s(j['protocol']),
      fileBytes = _i(j['fileBytes']),
      decoded = _b(j['decoded']),
      kind = j['kind'] == null ? null : _s(j['kind']),
      title = _s(j['title']),
      sampleRate = _d(j['sampleRate']),
      sampleRateSource = _s(j['sampleRateSource'], 'default'),
      sampleRateKey = j['sampleRateKey'] == null
          ? null
          : _s(j['sampleRateKey']),
      sampleRateRaw = j['sampleRateRaw'] == null
          ? null
          : _s(j['sampleRateRaw']),
      samplingFrequencyRaw = j['samplingFrequencyRaw'] == null
          ? null
          : _s(j['samplingFrequencyRaw']),
      sampleRateNote = j['sampleRateNote'] == null
          ? null
          : _s(j['sampleRateNote']),
      totalSamples = _i(j['totalSamples']),
      durationSec = _d(j['durationSec']),
      entryCount = _i(j['entryCount']),
      multiChannel = _b(j['multiChannel']),
      busLabels = (j['busLabels'] as List? ?? const [])
          .map((e) => _s(e))
          .toList(),
      hasBus = _b(j['hasBus']),
      busPoints = _i(j['busPoints']),
      unsupported = j['unsupported'] == null ? null : _s(j['unsupported']),
      tableRows = _i(j['tableRows']),
      chartRows = _i(j['chartRows']),
      channels = (j['channels'] as List? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map(ChannelInfo.new)
          .toList();

  final String name;
  final String source; // atkcc | powerz
  final String container; // atkcc | sqlite | pdstream | ufcsstream
  final String protocol; // USB PD | UFCS
  final int fileBytes;
  final bool decoded;
  final String? kind; // pd | ufcs（仅 powerz）
  final String title;
  final double sampleRate;
  final String
  sampleRateSource; // declared | measured | default | override | powerz
  final String? sampleRateKey;
  final String? sampleRateRaw;
  final String? samplingFrequencyRaw;
  final String? sampleRateNote;
  final int totalSamples;
  final double durationSec;
  final int entryCount;
  final bool multiChannel;
  final List<String> busLabels;
  final bool hasBus;
  final int busPoints;
  final String? unsupported;
  final int tableRows;
  final int chartRows;
  final List<ChannelInfo> channels;

  bool get isUfcs => protocol == 'UFCS';
  bool get isPdStream => container == 'pdstream';
  bool get isUfcsStream => container == 'ufcsstream';
  bool get isPowerz => source == 'powerz';
}

/* ────────────────────────── 统计 ────────────────────────── */

class DecodeStats {
  DecodeStats(Map<String, dynamic> j)
    : channel = _i(j['channel']),
      source = _s(j['source']),
      kind = _s(j['kind']),
      protocol = _s(j['protocol']),
      unsupported = j['unsupported'] == null ? null : _s(j['unsupported']),
      totalSamples = _i(j['totalSamples']),
      durationSec = _d(j['durationSec']),
      sampleRate = _d(j['sampleRate']),
      sampleRateSource = _s(j['sampleRateSource']),
      sampleRateNote = j['sampleRateNote'] == null
          ? null
          : _s(j['sampleRateNote']),
      sampleRateDeclared = _d(j['sampleRateDeclared']),
      sampleRateMeasured = j['sampleRateMeasured'] == null
          ? null
          : _d(j['sampleRateMeasured']),
      edges = _i(j['edges']),
      trimmedBytes = _i(j['trimmedBytes']),
      packetCount = _i(j['packetCount']),
      badCrc = _i(j['badCrc']),
      crcUnknown = _i(j['crcUnknown']),
      warnings = _i(j['warnings']),
      badWire = _i(j['badWire']),
      truncatedRows = _i(j['truncatedRows']),
      connectCount = _i(j['connectCount']),
      disconnectCount = _i(j['disconnectCount']),
      ufcsEvents = _i(j['ufcsEvents']),
      unsupportedMsgs = _i(j['unsupportedMsgs']),
      ufcsFrames = _i(j['ufcsFrames']),
      ufcsUnlocatedRows = _i(j['ufcsUnlocatedRows']),
      ufcsDirFromLine = _i(j['ufcsDirFromLine']),
      ufcsDirInferred = _i(j['ufcsDirInferred']),
      tableRows = _i(j['tableRows']),
      chartRows = _i(j['chartRows']),
      ufcsEventCodes = (j['ufcsEventCodes'] as List? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map((e) => (code: _i(e['code']), n: _i(e['n'])))
          .toList(),
      channelPick = j['channelPick'] == null
          ? null
          : Map<String, dynamic>.from(j['channelPick'] as Map);

  final int channel;
  final String source;
  final String kind;
  final String protocol;
  final String? unsupported;
  final int totalSamples;
  final double durationSec;
  final double sampleRate;
  final String sampleRateSource;
  final String? sampleRateNote;
  final double sampleRateDeclared;
  final double? sampleRateMeasured;
  final int edges;
  final int trimmedBytes;
  final int packetCount;
  final int badCrc;
  final int crcUnknown;
  final int warnings;
  final int badWire;
  final int truncatedRows;
  final int connectCount;
  final int disconnectCount;
  final int ufcsEvents;
  final int unsupportedMsgs;
  final int ufcsFrames;
  final int ufcsUnlocatedRows;
  final int ufcsDirFromLine;
  final int ufcsDirInferred;
  final int tableRows;
  final int chartRows;
  final List<({int code, int n})> ufcsEventCodes;
  final Map<String, dynamic>? channelPick;

  /// 采样率来源的中文标注（与 `core/src/csv.cpp` 同一套说法）。
  String get sourceTag {
    switch (sampleRateSource) {
      case 'declared':
        return '文件声明';
      case 'measured':
        return '波形实测';
      case 'default':
        return '默认值';
      case 'override':
        return '手动指定';
      case 'powerz':
        return '分析仪时间戳';
      case 'live':
        return '设备时间戳';
      default:
        return sampleRateSource.isEmpty ? '—' : sampleRateSource;
    }
  }
}

/* ────────────────────────── 时间轴数据 ────────────────────────── */

/// `pdscope_bus_series` 的抽稀结果。
class BusSeries {
  BusSeries(Map<String, dynamic> j)
    : t0 = _d(j['t0']),
      step = _d(j['step']),
      n = _i(j['n']),
      sampleRate = _d(j['sampleRate']),
      hasAux = _b(j['hasAux']),
      vmax = _d(j['vmax']),
      imax = _d(j['imax']),
      camax = _d(j['camax']),
      cbmax = _d(j['cbmax']),
      labels = (j['labels'] as List? ?? const []).map((e) => _s(e)).toList(),
      vbus = _fl(j['vbus']),
      ibus = _fl(j['ibus']),
      ca = j['ca'] == null ? null : _fl(j['ca']),
      cb = j['cb'] == null ? null : _fl(j['cb']),
      times = j['times'] == null ? null : _fl(j['times']);

  static Float64List _fl(dynamic v) {
    if (v is List) {
      final out = Float64List(v.length);
      for (var i = 0; i < v.length; i++) {
        out[i] = _d(v[i]);
      }
      return out;
    }
    return Float64List(0);
  }

  final double t0; // 起点（秒）
  final double step; // 相邻点间隔（秒）
  final int n; // 点数
  final double sampleRate;
  final bool hasAux; // 有第三、四路（CC1/CC2 或 DP/DM）
  final double vmax;
  final double imax;
  final double camax;
  final double cbmax;
  final List<String> labels;
  final Float64List vbus;
  final Float64List ibus;
  final Float64List? ca;
  final Float64List? cb;

  /// 实时不规则测量的原始相对秒，文件均匀序列仍使用 t0/step。
  final Float64List? times;

  bool get isEmpty => n == 0 || vbus.isEmpty;

  /// 第 i 个点的时间（秒）。
  double timeAt(int i) =>
      times != null && i < times!.length ? times![i] : t0 + step * i;

  /// 选取最接近光标时间的实际样本，不伪造固定采样步长。
  int nearestIndex(double seconds) {
    if (n == 0) return 0;
    if (times == null && step > 0) {
      return ((seconds - t0) / step).round().clamp(0, n - 1);
    }
    var best = 0;
    var distance = double.infinity;
    for (var i = 0; i < n; i++) {
      final delta = (timeAt(i) - seconds).abs();
      if (delta < distance) {
        distance = delta;
        best = i;
      }
    }
    return best;
  }
}

/// 原始电平包络（`pdscope_waveform_range` 的二进制布局）。
class WaveformRange {
  WaveformRange(this.n, this.bucket, this.startSample, this.hi, this.lo);

  static WaveformRange decode(Uint8List bytes) {
    if (bytes.length < 20) {
      return WaveformRange(0, 0, 0, Float32List(0), Float32List(0));
    }
    final bd = ByteData.sublistView(bytes);
    final n = bd.getUint32(0, Endian.little);
    final bucket = bd.getUint64(4, Endian.little);
    final start = bd.getUint64(12, Endian.little);
    final need = 20 + n * 4 * 2;
    if (bytes.length < need) {
      return WaveformRange(0, 0, 0, Float32List(0), Float32List(0));
    }
    final hi = Float32List(n);
    final lo = Float32List(n);
    for (var i = 0; i < n; i++) {
      hi[i] = bd.getFloat32(20 + i * 4, Endian.little);
      lo[i] = bd.getFloat32(20 + n * 4 + i * 4, Endian.little);
    }
    return WaveformRange(n, bucket, start, hi, lo);
  }

  final int n;
  final int bucket; // 每个桶覆盖的采样点数
  final int startSample;
  final Float32List hi;
  final Float32List lo;

  bool get isEmpty => n == 0;
}

/* ────────────────────────── 时间轴标记 ────────────────────────── */

/// 类别编号 → 类别名。
///
/// ⚠ 这张表是 **C ABI 的契约**：核心的 `kindCodeOf()`（core/src/session.cpp）按同样
/// 的顺序编号。加类别要在两边同时改，改错了时间轴的颜色会静默错位。
const List<String> kKindNames = [
  'Control',
  'Data',
  'Extended',
  'VDM',
  'Error',
  'Custom',
];

String kindNameFromCode(int code) =>
    (code >= 0 && code < kKindNames.length) ? kKindNames[code] : 'Control';

/// 时间轴的报文标记（`pdscope_packet_marks` 的二进制布局）。
class PacketMarks {
  PacketMarks(this.n, this.ts, this.kind, this.flags);

  /// 空结果（没解码、或没有报文）。
  static final PacketMarks empty = PacketMarks(
    0,
    Float64List(0),
    Uint8List(0),
    Uint8List(0),
  );

  static PacketMarks decode(Uint8List bytes) {
    if (bytes.length < 4) return empty;
    final bd = ByteData.sublistView(bytes);
    final n = bd.getUint32(0, Endian.little);
    if (n == 0) return empty;
    final need = 4 + n * 10;
    if (bytes.length < need) return empty;

    final ts = Float64List(n);
    for (var i = 0; i < n; i++) {
      ts[i] = bd.getFloat64(4 + i * 8, Endian.little);
    }
    final kindOff = 4 + n * 8;
    final kind = Uint8List.fromList(bytes.sublist(kindOff, kindOff + n));
    final flags = Uint8List.fromList(
      bytes.sublist(kindOff + n, kindOff + n * 2),
    );
    return PacketMarks(n, ts, kind, flags);
  }

  final int n;

  /// 每条报文的时间（秒）。
  final Float64List ts;

  /// 类别编号（见 [kKindNames]）。
  final Uint8List kind;

  /// bit0 = CRC 校验未通过；bit1 = 是某个配对的确认对象。
  final Uint8List flags;

  bool get isEmpty => n == 0;

  bool isBadCrc(int i) => (flags[i] & 1) != 0;
  bool isPaired(int i) => (flags[i] & 2) != 0;

  String kindNameAt(int i) => kindNameFromCode(kind[i]);
}
