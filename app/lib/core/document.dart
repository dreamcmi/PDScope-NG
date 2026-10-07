// document.dart — 一份抓包 = 一个对象
//
// 「一份抓包 = 一个对象」：每份自带筛选、时间窗口、选中行、通道、时间轴档位、
// 进度与错误。全局偏好（行高、详情宽度、主题、曲线区高度）**不放这里** ——
// 那是「我怎么看」，不是「这份数据是什么」，切标签不该变。
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';

import 'engine.dart';
import 'filters.dart';
import 'live_session.dart';
import 'live_source.dart';
import 'pcl_live_source.dart';
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

  /// 实时采集专用：视图**只往后长**，旧行的位置不动。
  ///
  /// 这时绝不能走 [setTotal] —— 它每次都 `clear()`，而实时流的条数每 250 ms 就变一次，
  /// 结果是刚取回来的页立刻作废、表格每秒重取好几轮，滚动位置也保不住。
  ///
  /// @param appendOnly 仅当排序是「按原始序号升序」时才成立；换了排序键，新行可能
  ///        插到中间，那就只能老实作废重取。
  void growTo(int n, {required bool appendOnly}) {
    if (n == total) return;
    if (appendOnly && n > total) {
      total = n;
      return;
    }
    setTotal(n);
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

  /* ══════════════════ 实时采集 ══════════════════════════════════
     实时文档与文件文档**共用这一个类**：数据来源不同（LiveSession vs engine），
     但「筛选 / 分页 / 详情 / 时间轴 / 导出」这套用法完全一样。
     下面凡是问 engine 的地方都先看 [isLive] —— 这就是全部的分叉，
     分叉点只有五处：applyFilters / loadPage / selectRow / 两个遍历取页 / loadBus。
     ══════════════════════════════════════════════════════════════ */

  LiveSource? liveSource;

  /// 正在等待枚举、HELLO、START 或 STOP；防止重复提交控制命令。
  bool liveBusy = false;

  /// 停止后允许重新打开配置面板，已采数据继续保留。
  bool liveShowConfig = false;
  bool _clearOnStart = false;
  Completer<void>? _livePending;
  int _lastLiveTrim = 0;
  LiveSession? live;
  LiveState liveState = LiveState.idle;

  /// 停车 / 掉线 / 可恢复错误的说明文字。
  String? liveMessage;

  List<LiveDevice> liveDevices = const [];
  LiveDevice? liveDevice;

  /// 采集配置（连接面板上那几个勾与节拍）。
  LiveStartOptions liveOptions = const LiveStartOptions();

  /// 零写监听收到的报告条数。
  int liveListenReports = 0;

  /// 底部区当前显示什么：模拟量轨迹 ⇄ 通讯日志（设计第 9 节的双模式）。
  bool showTrace = false;

  /// 采集中界面的刷新节拍。
  ///
  /// 数据事件是**每条都到**的（约 8 条/秒），但界面没必要每条都重建视图 ——
  /// 250 ms 一次既跟得上眼睛，又不会把筛选与时间轴重算成瓶颈。
  static const Duration kLiveRefresh = Duration(milliseconds: 250);
  Timer? _liveTimer;
  StreamSubscription<LiveEvent>? _liveSub;

  bool get isLive => live != null;
  bool get liveCapturing => liveState == LiveState.capturing;
  bool get livePaused => liveState == LiveState.paused;

  /// 还没进入采集阶段 —— 这时中列显示**连接面板**而不是报文表。
  ///
  /// 一旦开始采集（或停车、掉线），中列就交回给表格：那时用户最想看的是数据，
  /// 而不是连接界面。要回到连接界面得先明确「断开设备」（原则③：不自动重连）。
  bool get livePreCapture =>
      isLive &&
      (liveShowConfig ||
          liveState == LiveState.idle ||
          liveState == LiveState.scanning ||
          liveState == LiveState.found ||
          liveState == LiveState.connecting ||
          (liveState == LiveState.ready && live!.isEmpty) ||
          liveState == LiveState.listening ||
          (liveState == LiveState.recoverableError && live!.isEmpty));

  /// 状态条是否显示：进入采集阶段之后才出现（未连接时它没有东西可报告）。
  bool get showLiveBar => isLive && !livePreCapture;

  /// 视图是否「只往后长」。只有按原始序号升序时成立（见 [PagedRows.growTo]）。
  bool get _liveAppendOnly =>
      filters.sortKey == SortKey.index && filters.sortAsc;

  /// 挂上一个设备源，把这份文档变成实时文档。
  void attachLive(LiveSource source) {
    liveSource = source;
    live = LiveSession(source)
      ..deviceName = ''
      ..transport = '';
    displayName = '实时采集';
    state = DocState.done; // 实时文档没有「解码」这一步，直接就是可用状态
    filters = FilterState.defaults('pd');
    meta = CaptureMeta({
      'name': displayName,
      'source': 'live',
      'container': 'live',
      'protocol': 'USB PD',
      'fileBytes': 0,
      'decoded': true,
      'title': '实时采集',
      // 设备给的就是毫秒时间戳：1 采样点 = 1 ms，与 POWER-Z 路径同一口径。
      'sampleRate': 1000.0,
      'sampleRateSource': 'live',
      'totalSamples': 0,
      'durationSec': 0.0,
      'hasBus': true,
      'busLabels': const ['VBUS', 'IBUS'],
      'multiChannel': false,
      'entryCount': 0,
    });
    stats = DecodeStats({
      'source': 'live',
      'kind': 'live',
      'protocol': 'USB PD',
      'sampleRate': 1000.0,
      'sampleRateSource': 'live',
      'packetCount': 0,
      'crcUnknown': 0,
      'durationSec': 0.0,
    });
    _liveSub = source.events.listen(_onLiveEvent);
    notifyListeners();
  }

  void _onLiveEvent(LiveEvent e) {
    final l = live;
    if (l == null) return;
    switch (e) {
      case LiveRowsEvent():
        l.addRows(e.rows, e.details);
      case LiveBusEvent():
        l.addBus(e.t, e.vbus, e.ibus);
      case LiveTraceEvent():
        l.addTrace(e.entry);
        if (showTrace) notifyListeners();
      case LiveStatsEvent():
        // 设备侧计数由源报上来；报文数与采样数由 addRows / addBus 累计，
        // 两者口径分开，不会互相覆盖。
        final st = l.stats;
        st.durationSec = e.durationSec;
        st.commands = e.commands;
        st.ioTimeouts = e.ioTimeouts;
        st.rejects = e.rejects;
        st.dropped = e.dropped;
        st.lossReports = e.lossReports;
        st.sequenceGaps = e.sequenceGaps;
        st.invalidFrames = e.invalidFrames;
        st.lossCountKnown = e.lossCountKnown;
        st.timeUncertain = e.timeUncertain;
      case LiveDeviceEvent():
        liveDevice = e.device;
        l.deviceName = e.device.name;
        l.transport = e.device.transport;
        l.transportNote = e.device.transportNote;
        if (liveSource is PclLiveSource && !liveCapturing) {
          final o = liveOptions;
          final minimum =
              (liveSource as PclLiveSource).hello!.minSamplePeriodMs;
          liveOptions = LiveStartOptions(
            pd: true,
            analog: o.analog,
            pollMs: o.pollMs < minimum ? minimum : o.pollMs,
            voltage: o.voltage && e.device.can('母线电压'),
            current: o.current && e.device.can('母线电流'),
            goodCrcFilter: o.goodCrcFilter && e.device.can('GoodCRC 过滤'),
            channelSelect:
                (o.channelSelect == 3
                    ? e.device.can('双 CC 接收')
                    : o.channelSelect == 0 || e.device.can('CC 选择'))
                ? o.channelSelect
                : 0,
          );
        }
        notifyListeners();
      case LiveStateEvent():
        if (e.state == LiveState.capturing && _clearOnStart) {
          liveShowConfig = false;
          l.clear();
          rows.clear();
          rows.setTotal(0);
          _lastLiveTrim = 0;
          _clearOnStart = false;
          selected = null;
          selectedViewIndex = null;
          detail = null;
          _detailRequest++;
          detailLoading = false;
        }
        liveState = e.state;
        if (e.message != null) liveMessage = e.message;
        // 「表格在跟随」这件事要让用户看得见 —— 否则他会以为界面卡住了
        // （明明在采集，列表却不动）。停止/停车时这条提示要收掉。
        notice = switch (e.state) {
          LiveState.capturing => '采集中 · 表格在底部时自动跟随最新报文；往上翻会停止跟随。',
          LiveState.paused => '已暂停 · 数据不再进来，已采的仍然可以筛选与导出。',
          _ => null,
        };
        // 停车 / 掉线时立刻收尾：停掉刷新节拍，并把最后一批数据落进视图。
        if (e.state == LiveState.parked ||
            e.state == LiveState.disconnected ||
            e.state == LiveState.stopped) {
          _stopLiveRefresh();
          _refreshLive(force: true);
        } else if (e.state == LiveState.capturing) {
          _startLiveRefresh();
        }
        notifyListeners();
    }
  }

  void _startLiveRefresh() {
    _liveTimer ??= Timer.periodic(kLiveRefresh, (_) => _refreshLive());
  }

  void _stopLiveRefresh() {
    _liveTimer?.cancel();
    _liveTimer = null;
  }

  /// 把 LiveSession 里的增量反映到界面状态上。
  void _refreshLive({bool force = false}) {
    final l = live;
    if (l == null) return;
    l.rebuildView(filters);

    // 历史被裁掉时视图起点会前移，缓存必须整体作废（只在超过上限后偶发一次）。
    final trimmedNow = l.stats.trimmed != _lastLiveTrim;
    _lastLiveTrim = l.stats.trimmed;
    if (trimmedNow) {
      rows.clear();
      rows.setTotal(l.viewCount);
    } else {
      rows.growTo(l.viewCount, appendOnly: _liveAppendOnly);
    }

    typeCounts = l.typeCounts();
    marks = l.marks();
    bus = BusSeries(l.busSeriesJson(targetPoints: 2400));
    busAttempted = true;

    final st = l.stats;
    stats = DecodeStats({
      'source': 'live',
      'kind': 'live',
      'protocol': 'USB PD',
      'sampleRate': 1000.0,
      'sampleRateSource': 'live',
      'totalSamples': st.samples,
      'durationSec': st.durationSec,
      'packetCount': st.packets,
      'crcUnknown': l.crcUnknown,
      'badCrc': l.badCrc,
      'trimmedBytes': st.trimmed,
    });
    meta = CaptureMeta({
      'name': displayName,
      'source': 'live',
      'container': 'live',
      'protocol': 'USB PD',
      'fileBytes': 0,
      'decoded': true,
      'title': liveDevice == null ? '实时采集' : '实时采集 · ${liveDevice!.name}',
      'sampleRate': 1000.0,
      'sampleRateSource': 'live',
      'sampleRateRaw': '1 点 = 1 ms',
      'totalSamples': st.samples,
      'durationSec': st.durationSec,
      'hasBus': st.samples > 0,
      'busLabels': const ['VBUS', 'IBUS'],
      'multiChannel': false,
      'entryCount': st.packets,
      'unsupported': st.trimmed > 0 ? '已裁掉最旧的 ${st.trimmed} 条（内存上限）' : null,
    });
    notifyListeners();
  }

  /* ── 实时采集的操作 ─────────────────────────────────────── */

  Future<void> liveEnumerate() async {
    final s = liveSource;
    if (s == null) return;
    await _liveAction(() async {
      liveDevices = await s.enumerate();
      liveState = liveDevices.isEmpty ? LiveState.idle : LiveState.found;
    });
  }

  Future<void> liveConnect(LiveDevice d) async {
    final s = liveSource;
    if (s == null) return;
    liveDevice = d;
    await _liveAction(() async {
      await s.connect(d);
      final connected = liveDevice ?? d;
      live!.deviceName = connected.name;
      live!.transport = connected.transport;
      live!.transportNote = connected.transportNote;
    });
  }

  Future<void> liveListen(int windowMs) async {
    final s = liveSource;
    if (s == null) return;
    await _liveAction(() async {
      liveListenReports = await s.listen(windowMs);
    });
  }

  /// 尚未确认 HELLO 的端点也可打开只读监听，全程不发送命令。
  Future<void> liveListenDevice(LiveDevice device, int windowMs) async {
    final source = liveSource;
    if (source is! PclLiveSource) return;
    liveDevice = device;
    await _liveAction(() async {
      liveListenReports = await source.listenDevice(device, windowMs);
    });
  }

  Future<void> liveStart() async {
    if (liveBusy) return;
    final s = liveSource;
    if (s == null) return;
    _clearOnStart = true;
    await _liveAction(() async {
      try {
        await s.start(liveOptions);
      } catch (_) {
        _clearOnStart = false;
        rethrow;
      }
    });
  }

  void livePause() {
    liveSource?.pause();
    _stopLiveRefresh();
  }

  void liveResume() {
    liveSource?.resume();
    _startLiveRefresh();
  }

  /// 停止采集。**收尾仍要发一次 Disconnect** —— 设备靠它结束会话、
  /// 屏幕从「抓包中」恢复回来。这条最该发出去。
  Future<void> liveStop() async {
    await _liveAction(() async {
      await liveSource?.stop();
      _stopLiveRefresh();
      _refreshLive(force: true);
    });
  }

  Future<void> liveDisconnect() async {
    await _liveAction(() async {
      await liveSource?.disconnect();
      _stopLiveRefresh();
      liveDevice = null;
      liveDevices = const [];
      liveState = LiveState.idle;
      liveShowConfig = false;
    });
  }

  Future<void> _liveAction(Future<void> Function() action) async {
    if (liveBusy) return;
    liveBusy = true;
    final pending = Completer<void>();
    _livePending = pending;
    liveMessage = null;
    notifyListeners();
    try {
      await action();
    } catch (error) {
      liveMessage = error.toString();
      notice = liveMessage;
    } finally {
      liveBusy = false;
      pending.complete();
      _livePending = null;
      notifyListeners();
    }
  }

  /// 关闭标签前等待本地控制操作结束，再同步停止和释放硬件。
  Future<void> closeLive() async {
    await _livePending?.future;
    try {
      await liveSource?.disconnect();
    } finally {
      await liveSource?.dispose();
    }
  }

  /// 演练入口（仅模拟设备源有）：把「主动停车 / 设备掉线」这两个
  /// 在真机上很难随时复现的状态做出来，供界面审查。
  void liveDrillPark() {
    final s = liveSource;
    if (s is MockLiveSource) s.drillPark();
  }

  void liveDrillDropout() {
    final s = liveSource;
    if (s is MockLiveSource) s.drillDropout();
  }

  /// 导出当前快照为 CSV。
  ///
  /// 采集中导出的是**到此刻为止的快照**，不是完整抓包 —— 所以文件名要带时间戳、
  /// 导出后要说明「截至 T 秒」（设计第 14 节）。
  String liveCsvText() => live!.csvText(filtered: true);

  String liveCsvName() {
    final t = DateTime.now();
    String two(int v) => v.toString().padLeft(2, '0');
    final stamp =
        '${t.year}${two(t.month)}${two(t.day)}-${two(t.hour)}${two(t.minute)}';
    final dev = liveDevice?.name.replaceAll(RegExp(r'[^\w\-]+'), '-') ?? '设备';
    return '实时采集-$dev-$stamp.csv';
  }

  @override
  void dispose() {
    _liveTimer?.cancel();
    _liveSub?.cancel();
    unawaited(liveSource?.dispose());
    super.dispose();
  }

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
    // 实时文档：筛选在 Dart 侧做（见 LiveSession.rebuildView 的说明），
    // 不进核心、也不用等队列。
    if (isLive) {
      live!.rebuildView(filters);
      rows.clear();
      rows.setTotal(live!.viewCount);
      typeCounts = live!.typeCounts();
      marks = live!.marks();
      selected = null;
      selectedViewIndex = null;
      detail = null;
      _detailRequest++;
      detailLoading = false;
      notifyListeners();
      return;
    }
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
      final raw = isLive
          ? live!.page(offset, limit)
          : await engine.page(id, offset, limit);
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
      final j = isLive
          ? (live!.detail(originalIndex) ?? const <String, dynamic>{})
          : await engine.detail(id, originalIndex);
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
      final page = await _rawPage(
        engine,
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
      final page = await _rawPage(
        engine,
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

  /// 取一页原始 JSON —— 实时与文件两条来源的唯一分叉点。
  Future<List<Map<String, dynamic>>> _rawPage(
    EngineClient engine,
    int offset,
    int limit,
  ) => isLive
      ? Future.value(live!.page(offset, limit))
      : engine.page(id, offset, limit);

  Future<void> loadBus(EngineClient engine, {int targetPoints = 2400}) async {
    if (busLoading || bus != null) return;
    busLoading = true;
    notifyListeners();
    try {
      if (isLive) {
        // 实时：直接用内存里的采样点，不走核心。
        bus = BusSeries(live!.busSeriesJson(targetPoints: targetPoints));
      } else {
        final j = await engine.busSeries(id, targetPoints: targetPoints);
        bus = j == null ? null : BusSeries(j);
      }
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
