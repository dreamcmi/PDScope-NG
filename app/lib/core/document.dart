// document.dart — 一份抓包 = 一个对象
//
// 这是 JS 基线「多份抓包 = 一文档一对象」那条约定在 Flutter 侧的落地点：
// 每份自带筛选、时间窗口、选中行、通道、时间轴档位、进度与错误。
// 全局偏好（行高、详情宽度、主题、曲线区高度）**不放这里** —— 那是「我怎么看」，
// 不是「这份数据是什么」，切标签不该变。
import 'package:flutter/foundation.dart';

import 'engine.dart';
import 'filters.dart';
import 'models.dart';

/// 标签状态（对应 JS 标签栏的状态圆点）。
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

/// 报文列表的分页缓存。列表按页取，翻到哪取到哪 —— 几万条也不把整份塞进内存。
class PagedRows {
  PagedRows(this.pageSize);

  final int pageSize;
  int total = 0;
  final Map<int, PacketRow> _cache = {};
  final Set<int> _inflight = {};

  PacketRow? at(int i) => _cache[i];

  bool isLoaded(int i) => _cache.containsKey(i);

  /// 已装进来的页号（去重后交给调用方批量请求）。
  List<int> missingPages(int from, int to) {
    final out = <int>[];
    if (total <= 0) return out;
    final last = to.clamp(0, total - 1);
    for (var p = from ~/ pageSize; p <= last ~/ pageSize; p++) {
      if (_inflight.contains(p)) continue;
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

  void clear() {
    _cache.clear();
    _inflight.clear();
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
  CaptureDocument({required this.id, required this.path, required this.displayName});

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
  PacketDetail? detail;
  bool detailLoading = false;

  BusSeries? bus;
  bool busLoading = false;

  WaveformRange? wave;
  bool waveLoading = false;

  DecodeProgress? progress;
  int? channel;

  TlMode tlMode = TlMode.main;

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
  /// 这层约束绕过去。语义上它对应基线的 `render()`。
  void touch() => notifyListeners();
  bool get hasPackets => rows.total > 0;
  String get protocol => meta?.protocol ?? 'USB PD';
  bool get isUfcs => protocol == 'UFCS';

  /// 设置筛选并让核心重建视图。
  Future<void> applyFilters(EngineClient engine) async {
    await engine.setFilter(id, filters.toJson());
    final n = await engine.viewCount(id);
    rows.setTotal(n);
    // 视图变了，选中行可能已经不在这份视图里；详情留着反而误导。
    notifyListeners();
  }

  /// 读一页并补进缓存。
  Future<void> loadPage(EngineClient engine, int page) async {
    if (rows.total <= 0) return;
    rows.markInFlight(page);
    final offset = page * rows.pageSize;
    final limit = rows.pageSize.clamp(0, rows.total - offset);
    if (limit <= 0) return;
    try {
      final raw = await engine.page(id, offset, limit);
      rows.put(page, raw.map(PacketRow.new).toList(), offset);
      notifyListeners();
    } catch (e) {
      rows.put(page, const [], offset);
      // 取页失败不改标签状态（会话还在），只在界面上把这一页留空。
      debugPrint('取第 $page 页失败：$e');
    }
  }

  Future<void> selectRow(EngineClient engine, int originalIndex) async {
    selected = originalIndex;
    detailLoading = true;
    notifyListeners();
    try {
      final j = await engine.detail(id, originalIndex);
      detail = j.isEmpty ? null : PacketDetail(j);
    } catch (e) {
      detail = null;
      error = '$e';
    } finally {
      detailLoading = false;
      notifyListeners();
    }
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
