// live_session.dart — 实时采集：已采数据的内存持有者与查询入口
//
// 为什么要有这一层：文件抓包的「取一页 / 取详情 / 取统计」都是问核心（engine），
// 而实时采集的数据目前在**界面进程里**（协议层未定）。这里实现同一组方法
// （page / detail / typeCounts / marks / busSeries / csv），
// 让 CaptureDocument 只改「问谁」，不改「怎么用」—— 界面因此不必分两套。
//
// ⚠ 两处刻意的镜像，接入真协议时要一并处理（不是遗漏，是当前的取舍）：
//   · [rebuildView] 是 core/src/filters.cpp 的 Dart 侧镜像。真协议接入后应改成
//     把实时行喂进核心的 Session，让筛选只剩一份实现。
//   · [csvText] 是 core/src/csv.cpp 的镜像。列名 / CRLF / 逐字段加引号
//     **逐条对齐**，否则「采集中导出的文件」与「停止后导出的文件」会长得不一样。
import 'dart:math' as math;
import 'dart:typed_data';

import 'filters.dart';
import 'live_source.dart';
import 'models.dart';

/// 内存里最多留多少条报文。
///
/// 实时采集可能跑很久，必须有上界。裁掉的是**最旧的**，并把它记进
/// [LiveStats.trimmed] 显示出来 —— 悄悄丢历史是不可接受的（原则①）。
const int kLiveRowCap = 20000;

class LiveSession {
  LiveSession(this.source);

  final LiveSource source;

  /// 设备信息（状态条与设备卡用）。
  String deviceName = '';
  String transport = '';
  String transportNote = '';

  final LiveStats stats = LiveStats();

  /// 环形通讯日志（最近 [kLiveTraceMax] 条）。
  final List<LiveTraceEntry> trace = [];

  final List<Map<String, dynamic>> _rows = [];
  final Map<int, Map<String, dynamic>> _details = {};

  /// `_rows[0]` 对应的原始序号。裁掉历史后它会往后走 ——
  /// 详情是按**原始序号**取的，所以必须留着这个基准去换算位置。
  int _base = 0;

  final List<double> _bt = [];
  final List<double> _bv = [];
  final List<double> _bi = [];

  /// 当前视图（`_rows` 的下标，已经过滤 + 排序）。
  List<int> _view = [];

  int get rowCount => _rows.length;
  int get viewCount => _view.length;
  bool get isEmpty => _rows.isEmpty;
  double get durationSec => stats.durationSec;

  /* ── 追加 ─────────────────────────────────────────────────── */

  void addRows(List<Map<String, dynamic>> rows, Map<int, Map<String, dynamic>> details) {
    if (rows.isEmpty) return;
    _rows.addAll(rows);
    _details.addAll(details);
    stats.packets = _rows.length + stats.trimmed;
    // 超出上限就从头裁，并把裁掉的条数如实记下来
    final over = _rows.length - kLiveRowCap;
    if (over > 0) {
      _rows.removeRange(0, over);
      for (var i = _base; i < _base + over; i++) {
        _details.remove(i);
      }
      _base += over;
      stats.trimmed += over;
    }
  }

  void addBus(List<double> t, List<double> vbus, List<double> ibus) {
    for (var i = 0; i < t.length; i++) {
      _bt.add(t[i]);
      _bv.add(vbus[i]);
      _bi.add(ibus[i]);
    }
    stats.samples = _bt.length;
  }

  void addTrace(LiveTraceEntry e) {
    trace.add(e);
    if (trace.length > kLiveTraceMax) trace.removeRange(0, trace.length - kLiveTraceMax);
  }

  void clear() {
    _rows.clear();
    _details.clear();
    _bt.clear();
    _bv.clear();
    _bi.clear();
    _view = [];
    _base = 0;
    trace.clear();
    stats.reset();
  }

  /* ── 视图：core/src/filters.cpp 的 Dart 侧镜像 ─────────────── */

  /// 筛选顺序与核心保持一致：
  /// roles → sops → cats → types → hideGoodCrc → onlyBad → onlyPower → onlyEnter
  /// → 时间区间 → 全文 q → ViewMode。
  void rebuildView(FilterState f) {
    final total = durationSec;
    final out = <int>[];
    for (var i = 0; i < _rows.length; i++) {
      final r = _rows[i];
      if (!_passes(r, f, total)) continue;
      out.add(i);
    }
    _sortView(out, f);
    _view = out;
  }

  bool _passes(Map<String, dynamic> r, FilterState f, double total) {
    String s(String k) => (r[k] ?? '') as String;
    int n(String k) => (r[k] as num?)?.toInt() ?? 0;

    if (!f.roles.contains(s('role'))) return false;
    if (!f.sops.contains(s('sop'))) return false;
    if (!f.cats.contains(s('kind'))) return false;
    if (f.types.isNotEmpty && !f.types.contains(s('msgType'))) return false;

    // 「隐藏 GoodCRC」默认是开的：真实抓包里它占一多半，不隐藏没法看。
    if (f.hideGoodCrc && s('msgType') == 'GoodCRC') return false;

    final isBad = s('crc') == 'bad' || n('warn') > 0;
    if (f.onlyBad && !isBad) return false;
    if (f.onlyPower && !_kPowerTypes.contains(s('msgType'))) return false;
    if (f.onlyEnter && !s('msgType').contains('Mode')) return false;

    if (f.hasTimeWindow && total > 0) {
      final frac = ((r['startSample'] as num?)?.toDouble() ?? 0) / (total * 1000);
      if (frac < f.tFrom || frac > f.tTo) return false;
    }

    // 全文：与核心一样，在类型 + 摘要 + 数据 + 链路 + 方向里找，不区分大小写
    if (f.q.trim().isNotEmpty) {
      final q = f.q.trim().toLowerCase();
      final hay = '${s('msgType')} ${s('summary')} ${s('dataHex')} ${s('sop')} ${s('role')}'
          .toLowerCase();
      if (!hay.contains(q)) return false;
    }

    switch (f.viewMode) {
      case ViewMode.neg:
        if (!_kPowerTypes.contains(s('msgType'))) return false;
      case ViewMode.err:
        if (!isBad) return false;
      case ViewMode.all:
        break;
    }
    return true;
  }

  void _sortView(List<int> view, FilterState f) {
    int cmp(int a, int b) {
      final ra = _rows[a], rb = _rows[b];
      int c;
      switch (f.sortKey) {
        case SortKey.msgType:
          c = ((ra['msgType'] ?? '') as String).compareTo((rb['msgType'] ?? '') as String);
        case SortKey.sop:
          c = ((ra['sop'] ?? '') as String).compareTo((rb['sop'] ?? '') as String);
        case SortKey.role:
          c = ((ra['role'] ?? '') as String).compareTo((rb['role'] ?? '') as String);
        case SortKey.kind:
          c = ((ra['kind'] ?? '') as String).compareTo((rb['kind'] ?? '') as String);
        case SortKey.summary:
          c = ((ra['summary'] ?? '') as String).compareTo((rb['summary'] ?? '') as String);
        case SortKey.msgId:
          c = ((ra['msgId'] as num?)?.toInt() ?? -1)
              .compareTo((rb['msgId'] as num?)?.toInt() ?? -1);
        case SortKey.nObjects:
          c = ((ra['objects'] as num?)?.toInt() ?? -1)
              .compareTo((rb['objects'] as num?)?.toInt() ?? -1);
        case SortKey.durationUs:
          c = ((ra['durationUs'] as num?)?.toDouble() ?? 0)
              .compareTo((rb['durationUs'] as num?)?.toDouble() ?? 0);
        case SortKey.crcOk:
          c = _crcRank(ra).compareTo(_crcRank(rb));
        case SortKey.startSample:
        case SortKey.timeMs:
          c = ((ra['timeMs'] as num?)?.toDouble() ?? 0)
              .compareTo((rb['timeMs'] as num?)?.toDouble() ?? 0);
        default:
          c = ((ra['index'] as num?)?.toInt() ?? 0)
              .compareTo((rb['index'] as num?)?.toInt() ?? 0);
      }
      // 同值时按原始序号定序，保证排序是稳定的（核心用 std::stable_sort）
      if (c == 0) {
        c = ((ra['index'] as num?)?.toInt() ?? 0)
            .compareTo((rb['index'] as num?)?.toInt() ?? 0);
      }
      return f.sortAsc ? c : -c;
    }

    view.sort(cmp);
  }

  static int _crcRank(Map<String, dynamic> r) {
    switch ((r['crc'] ?? '') as String) {
      case 'ok':
        return 1;
      case 'bad':
        return -1;
      default:
        return 0; // 未记录
    }
  }

  static const Set<String> _kPowerTypes = {
    'Source_Capabilities',
    'Sink_Capabilities',
    'Request',
    'Accept',
    'Reject',
    'PS_RDY',
    'GotoMin',
    'PPS_Status',
    'EPR_Request',
    'EPR_Mode',
    'EPR_Source_Capabilities',
    'EPR_Sink_Capabilities',
  };

  /* ── 查询（与 EngineClient 同名同形）──────────────────────── */

  List<Map<String, dynamic>> page(int offset, int limit) {
    if (offset >= _view.length) return const [];
    final end = math.min(offset + limit, _view.length);
    return [
      for (var i = offset; i < end; i++) _rows[_view[i]],
    ];
  }

  /// 按**原始序号**取详情。
  Map<String, dynamic>? detail(int originalIndex) => _details[originalIndex];

  /// 报文类型 → 条数。**统计全部报文、不受筛选影响** ——
  /// 否则勾掉一个类型它就从候选里消失、再也勾不回来。
  Map<String, int> typeCounts() {
    final m = <String, int>{};
    for (final r in _rows) {
      final t = (r['msgType'] ?? '') as String;
      if (t.isEmpty) continue;
      m[t] = (m[t] ?? 0) + 1;
    }
    return m;
  }

  /// 时间轴标记（全部报文，不跟筛选走）—— 布局见 pdscope.h 的 packet_marks。
  PacketMarks marks() {
    final n = _rows.length;
    if (n == 0) return PacketMarks.empty;
    final ts = Float64List(n);
    final kind = Uint8List(n);
    final flags = Uint8List(n);
    // 配对的确认对象：紧随其后的那条 GoodCRC 确认的就是它
    for (var i = 0; i < n; i++) {
      final r = _rows[i];
      ts[i] = ((r['timeMs'] as num?)?.toDouble() ?? 0) / 1000;
      kind[i] = _kindCode((r['kind'] ?? '') as String);
      var f = 0;
      if ((r['crc'] ?? '') == 'bad') f |= 1;
      if (i + 1 < n && (_rows[i + 1]['msgType'] ?? '') == 'GoodCRC') f |= 2;
      flags[i] = f;
    }
    return PacketMarks(n, ts, kind, flags);
  }

  static int _kindCode(String kind) {
    final i = kKindNames.indexOf(kind);
    return i < 0 ? 0 : i;
  }

  /// 模拟量抽稀序列（布局与 `pdscope_bus_series` 同形）。
  Map<String, dynamic> busSeriesJson({int targetPoints = 2400}) {
    final n = _bt.length;
    if (n == 0) {
      return {
        't0': 0.0,
        'step': 0.0,
        'n': 0,
        'sampleRate': 0.0,
        'hasAux': false,
        'vmax': 0.0,
        'imax': 0.0,
        'camax': 0.0,
        'cbmax': 0.0,
        'labels': const <String>[],
        'vbus': const <double>[],
        'ibus': const <double>[],
        'ca': null,
        'cb': null,
      };
    }
    final step = math.max(1, (n / targetPoints).ceil());
    final outV = <double>[];
    final outI = <double>[];
    final outT = <double>[];
    var vmax = 0.0, imax = 0.0;
    for (var i = 0; i < n; i += step) {
      outT.add(_bt[i]);
      outV.add(_bv[i]);
      outI.add(_bi[i]);
      vmax = math.max(vmax, _bv[i]);
      imax = math.max(imax, _bi[i]);
    }
    return {
      't0': outT.isEmpty ? 0.0 : outT.first,
      'step': step * ((_bt.length > 1) ? (_bt[1] - _bt[0]) : 0.125),
      'n': outV.length,
      'sampleRate': 0.0,
      'hasAux': false,
      'vmax': vmax,
      'imax': imax,
      'camax': 0.0,
      'cbmax': 0.0,
      'labels': const ['VBUS', 'IBUS'],
      'vbus': outV,
      'ibus': outI,
      'ca': null,
      'cb': null,
    };
  }

  /* ── CSV：core/src/csv.cpp 的镜像 ─────────────────────────── */

  /// 与核心完全相同的 13 列。PD 与 UFCS 只差第 6 列（Objects / Bytes）。
  static const List<String> csvHeader = [
    '#', 'SOP', 'MsgType', 'ID', 'Direction',
    'Objects', 'Elapsed', 'Time(ms)', 'VBUS(V)', 'IBUS(A)', 'Data', 'CRC', 'Note',
  ];

  String csvText({required bool filtered, int limit = 0}) {
    final idx = filtered ? _view : List<int>.generate(_rows.length, (i) => i);
    final n = (limit > 0 && limit < idx.length) ? limit : idx.length;
    final b = StringBuffer();
    b.write(csvHeader.map(_q).join(','));
    for (var k = 0; k < n; k++) {
      final r = _rows[idx[k]];
      String s(String key) => ((r[key] ?? '') as Object).toString();
      final cells = <String>[
        s('index'),
        s('sop'),
        s('msgType'),
        r['msgId'] == null ? '' : s('msgId'),
        s('role'),
        r['objects'] == null ? (r['bytes'] == null ? '' : s('bytes')) : s('objects'),
        s('elapsed'),
        ((r['timeMs'] as num?)?.toDouble() ?? 0).toStringAsFixed(3),
        ((r['vbus'] as num?)?.toDouble() ?? 0).toStringAsFixed(2),
        ((r['ibus'] as num?)?.toDouble() ?? 0).toStringAsFixed(3),
        s('dataHex'),
        _crcCell((r['crc'] ?? '') as String),
        s('summary'),
      ];
      // ⚠ 行尾 \r\n 写在**每行之前** —— 与核心一致（表头后不换行、末行不补尾换行）。
      b.write('\r\n');
      b.write(cells.map(_q).join(','));
    }
    return b.toString();
  }

  /// CRC 三态里「未记录」必须留空 —— 不能替设备的数据背书。
  static String _crcCell(String crc) => switch (crc) {
    'ok' => 'OK',
    'bad' => 'BAD',
    _ => '',
  };

  /// 逐字段加引号，内部的引号翻倍（RFC 4180，与核心同一套）。
  static String _q(String v) => '"${v.replaceAll('"', '""')}"';
}
