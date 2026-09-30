// document.dart — 一份抓包 = 一个对象
//
// 「一份抓包 = 一个对象」：每份自带筛选、时间窗口、选中行、通道、时间轴档位、
// 进度与错误。全局偏好（行高、详情宽度、主题、曲线区高度）**不放这里** ——
// 那是「我怎么看」，不是「这份数据是什么」，切标签不该变。
import 'dart:math' as math;

import 'package:flutter/foundation.dart';

import 'engine.dart';
import 'filters.dart';
import 'models.dart';

/// 标签状态（标签栏上的状态圆点）。
enum DocState {
  opening('打开中'),
  ready('待解码'),
  decoding('解码中'),
  done('已解码'),
  failed('打不开');

  const DocState(this.label);
  final String label;
}

/// 时间轴的两档模拟量视图。
enum TlMode { main, aux }

/// 每份文档的两个时间轴档位分别保存纵轴视图。
class TimelineYView {
  double zoom = 1;
  double pan = 0;
}

/// 报文列表的分页缓存。列表按页取，翻到哪取到哪 —— 几万条也不把整份塞进内存。
class PagedRows {
  PagedRows(this.pageSize);

  final int pageSize;
  int total = 0;
  final Map<int, PacketRow> _cache = {};
  final Set<int> _inflight = {};

  /// 取过但失败的页。**记住它，不再重试** ——
  /// 否则界面每次重画都会把这一页再请求一遍：失败一次就变成一直失败、一直请求。
  final Set<int> _failed = {};

  PacketRow? at(int i) => _cache[i];

  bool isLoaded(int i) => _cache.containsKey(i);

  /// 已装进来的页号（去重后交给调用方批量请求）。
  List<int> missingPages(int from, int to) {
    final out = <int>[];
    if (total <= 0) return out;
    final last = to.clamp(0, total - 1);
    for (var p = from ~/ pageSize; p <= last ~/ pageSize; p++) {
      if (_inflight.contains(p) || _failed.contains(p)) continue;
      final start = p * pageSize;
      final end = (start + pageSize).clamp(0, total);
      var complete = true;
      for (var i = start; i < end; i++) {
        if (!_cache.containsKey(i)) {
          complete = false;
          break;
        }
      }
      if (!complete) out.add(p);
    }
    return out;
  }

  void put(int page, List<PacketRow> rows, int offset) {
    for (var i = 0; i < rows.length; i++) {
      _cache[offset + i] = rows[i];
    }
    _inflight.remove(page);
  }

  void markInFlight(int page) => _inflight.add(page);

  /// 这一页取失败了。已经取过的行留着，但这一页不再重复请求。
  void markFailed(int page) {
    _failed.add(page);
    _inflight.remove(page);
  }

  void clear() {
    _cache.clear();
    _inflight.clear();
    _failed.clear();
  }

  void setTotal(int n) {
    if (n == total) return;
    total = n;
    // 视图变了（筛选 / 排序）⇒ 旧缓存的行号已经不指向同一批报文，必须整体作废。
    clear();
  }
}

/// 一份抓包的全部界面状态。
class CaptureDocument extends ChangeNotifier {
  CaptureDocument({
    required this.id,
    required this.path,
    required this.displayName,
  });

  /// 引擎侧的会话编号（工作 isolate 用它找 session）。
  final int id;
  final String path;
  String displayName;

  DocState state = DocState.opening;
  String? error;
  String? notice;

  CaptureMeta? meta;
  DecodeStats? stats;

  late FilterState filters = FilterState.defaults('pd');
  final PagedRows rows = PagedRows(200);

  /// 选中行的**原始报文序号**（不是视图行号 —— 排序后两者不同）。
  int? selected;
  int? selectedViewIndex;
  PacketDetail? detail;
  bool detailLoading = false;
  int _detailRequest = 0;

  BusSeries? bus;
  bool busLoading = false;

  /// 是否**已经取过一次**模拟量轨迹（不论取没取到）。
  ///
  /// 界面靠它区分「还在取」和「取过了，就是没有」——只判 `bus == null` 的话，
  /// 请求失败或返回空时**转圈永远停不下来**（数据永远到不了，条件永远成立）。
  bool busAttempted = false;

  WaveformRange? wave;
  bool waveLoading = false;

  DecodeProgress? progress;
  int? channel;

  TlMode tlMode = TlMode.main;
  final Map<TlMode, TimelineYView> timelineY = {
    for (final mode in TlMode.values) mode: TimelineYView(),
  };

  TimelineYView get timelineView => timelineY[tlMode]!;

  void resetTimelineViews() {
    for (final view in timelineY.values) {
      view.zoom = 1;
      view.pan = 0;
    }
  }

  /// 筛选控件需要「当前协议下所有出现过的报文类型 + 计数」。
  ///
  /// 来自核心的 `type_counts`，**统计全部报文、不受筛选影响** —— 否则勾掉一个类型，
  /// 它就从候选列表里消失了，再也勾不回来。
  Map<String, int> typeCounts = const {};

  /// 时间轴的报文标记（整份抓包，不跟表格筛选走）。
  PacketMarks marks = PacketMarks.empty;

  bool get decoded => state == DocState.done;
  bool get busy => state == DocState.opening || state == DocState.decoding;

  /// 界面改完筛选条件后叫一声重画。
  ///
  /// `notifyListeners()` 是 protected 的，界面层不该直接碰 —— 那会把「谁能改状态」
  /// 这层约束绕过去。
  void touch() => notifyListeners();
  bool get hasPackets => rows.total > 0;
  String get protocol => meta?.protocol ?? 'USB PD';
  bool get isUfcs => protocol == 'UFCS';

  /// 设置筛选并让核心重建视图。
  Future<void> applyFilters(EngineClient engine) async {
    await engine.setFilter(id, filters.toJson());
    final n = await engine.viewCount(id);
    // A sort or search change may keep the same row count. Cached positions are
    // still stale and must be fetched again from the new view.
    rows.clear();
    rows.setTotal(n);
    // 视图变了，选中行可能已经不在这份视图里。
    selected = null;
    selectedViewIndex = null;
    detail = null;
    _detailRequest++;
    detailLoading = false;
    notifyListeners();
  }

  /// 读一页并补进缓存。
  Future<void> loadPage(EngineClient engine, int page) async {
    if (rows.total <= 0) return;
    final offset = page * rows.pageSize;
    if (offset >= rows.total) return;
    final limit = rows.pageSize.clamp(0, rows.total - offset);
    if (limit <= 0) return;
    // ⚠ 这几行「不算数」的提前返回必须发生在 markInFlight **之前**：
    //   标了在飞却不发请求，这一页就永远是「在飞」状态，再也不会被请求。
    rows.markInFlight(page);
    try {
      final raw = await engine.page(id, offset, limit);
      rows.put(page, raw.map(PacketRow.new).toList(), offset);
      notifyListeners();
    } catch (e) {
      // 取页失败不改标签状态（会话还在），只把这一页记住、不再重试。
      rows.markFailed(page);
      debugPrint('取第 $page 页失败：$e');
      notifyListeners();
    }
  }

  Future<void> selectRow(
    EngineClient engine,
    int originalIndex, {
    int? viewIndex,
  }) async {
    final request = ++_detailRequest;
    selected = originalIndex;
    selectedViewIndex = viewIndex;
    detailLoading = true;
    notifyListeners();
    try {
      final j = await engine.detail(id, originalIndex);
      if (request != _detailRequest) return;
      detail = j.isEmpty ? null : PacketDetail(j);
    } catch (e) {
      if (request != _detailRequest) return;
      detail = null;
      error = '$e';
    } finally {
      if (request == _detailRequest) {
        detailLoading = false;
        notifyListeners();
      }
    }
  }

  /// 按当前筛选和排序后的可见行导航详情。
  Future<void> stepDetail(EngineClient engine, int delta) async {
    if (rows.total == 0) return;
    final current = selectedViewIndex;
    final target = ((current ?? -1) + delta).clamp(0, rows.total - 1).toInt();
    if (rows.at(target) == null) {
      await loadPage(engine, target ~/ rows.pageSize);
    }
    final row = rows.at(target);
    if (row != null) await selectRow(engine, row.index, viewIndex: target);
  }

  /// 在当前筛选视图中找时间最近的一条，供时间轴单击使用。
  Future<void> selectNearestAtTime(EngineClient engine, double seconds) async {
    var bestDistance = double.infinity;
    int? bestIndex;
    int? bestViewIndex;
    for (var offset = 0; offset < rows.total; offset += rows.pageSize) {
      final page = await engine.page(
        id,
        offset,
        math.min(rows.pageSize, rows.total - offset),
      );
      for (var i = 0; i < page.length; i++) {
        final row = PacketRow(page[i]);
        final distance = (row.timeMs / 1000 - seconds).abs();
        if (distance < bestDistance) {
          bestDistance = distance;
          bestIndex = row.index;
          bestViewIndex = offset + i;
        }
      }
    }
    if (bestIndex != null) {
      await selectRow(engine, bestIndex, viewIndex: bestViewIndex);
    }
  }

  /// 将时间筛选收紧到当前可见报文的最早和最晚采样点。
  Future<void> fitTimeToCurrentView(EngineClient engine) async {
    if (rows.total == 0) return;
    var lo = 0x7FFFFFFFFFFFFFFF;
    var hi = 0;
    for (var offset = 0; offset < rows.total; offset += rows.pageSize) {
      final page = await engine.page(
        id,
        offset,
        math.min(rows.pageSize, rows.total - offset),
      );
      for (final raw in page) {
        final row = PacketRow(raw);
        lo = math.min(lo, row.startSample);
        hi = math.max(hi, row.endSample);
      }
    }
    final total = stats?.totalSamples ?? meta?.totalSamples ?? 0;
    if (total <= 0 || hi < lo) return;
    filters.tFrom = (lo / total).clamp(0.0, 1.0);
    filters.tTo = (hi / total).clamp(0.0, 1.0);
    await applyFilters(engine);
  }

  Future<void> loadBus(EngineClient engine, {int targetPoints = 2400}) async {
    if (busLoading || bus != null) return;
    busLoading = true;
    notifyListeners();
    try {
      final j = await engine.busSeries(id, targetPoints: targetPoints);
      bus = j == null ? null : BusSeries(j);
    } catch (e) {
      debugPrint('取模拟量轨迹失败：$e');
      bus = null;
    } finally {
      busLoading = false;
      // 「收尾了」这件事必须单独记一笔：取到空和没取过，界面要给不同的说法。
      busAttempted = true;
      notifyListeners();
    }
  }

  Future<void> loadWaveform(
    EngineClient engine, {
    required int startSample,
    required int endSample,
    int maxPoints = 4000,
  }) async {
    if (waveLoading) return;
    waveLoading = true;
    try {
      final bytes = await engine.waveform(
        id,
        channel: channel ?? 0,
        startSample: startSample,
        endSample: endSample,
        maxPoints: maxPoints,
      );
      wave = WaveformRange.decode(bytes);
    } catch (e) {
      debugPrint('取波形失败：$e');
      wave = null;
    } finally {
      waveLoading = false;
      notifyListeners();
    }
  }

  /// 当前视图的时间跨度（秒）—— 刻度、刷选、悬停读数共用这一个口径。
  double get spanSec {
    final d = stats?.durationSec ?? meta?.durationSec ?? 0;
    return d > 0 ? d : 1;
  }
}
