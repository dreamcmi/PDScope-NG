// workspace.dart — 多文档工作区 + 串行作业队列
//
// 两条不能违反的约定：
//   1. **容器加载立即做，报文解码 lazy**：标签先出来（几百毫秒），解码等这份标签
//      第一次被激活时再排队做。一批文件拖进来时，不会被其中一份大文件拖住。
//   2. **作业走同一条串行队列**：解码是有状态的长时间任务，并发跑没有收益
//      （FFI 侧本来就是「同一会话不得并发」），排队反而让进度和取消可预测。
//
// ⚠ `run()` 的返回值必须原样交给作业自己的 Completer。漏传的症状是「标签出来了
//   但永远不解码」—— 报错现场和病因完全不在一处，很难查。
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'document.dart';
import 'engine.dart';
import 'ffi.dart';
import 'filters.dart';
import 'live_source.dart';
import 'models.dart';
import 'prefs.dart';

/// 队列里的一个作业。
class _Job {
  _Job(this.name, this.run);
  final String name;
  final Future<Object?> Function() run;
  final completer = Completer<Object?>();
}

class Workspace extends ChangeNotifier {
  Workspace(this.engine, {Prefs? prefs}) : prefs = prefs ?? Prefs();

  final Future<EngineClient> engine;
  EngineClient? _engine;

  final List<CaptureDocument> docs = [];
  int _activeIndex = -1;
  int _nextId = 1;

  final Prefs prefs;

  CaptureDocument? get active =>
      (_activeIndex >= 0 && _activeIndex < docs.length)
      ? docs[_activeIndex]
      : null;

  int get activeIndex => _activeIndex;

  /// 引擎就绪。
  ///
  /// ⚠ 这里**只是等构造函数收下的那个 future**，不要再自己开一个 Completer 挂在这儿。
  ///   曾经的写法是「另建 `_engineReady`，由外部的 `bindEngine()` 来点亮」——
  ///   构造函数既然已经把引擎的 future 收进来了，就存在两处状态不同步的可能：
  ///   谁忘了调 `bindEngine()`，`ready` 就**永久挂住**，症状是「标签出来了、
  ///   元数据也有，但解码永远不开始」，而报错现场跟病因完全不在一处。
  ///   Future 本身可以被多次 await，所以直接等它最省事也最不会错。
  Future<EngineClient> get ready async => _engine ??= await engine;

  /* ── 串行队列 ─────────────────────────────────────────────────── */

  final List<_Job> _queue = [];
  final Set<int> _switchingChannels = {};
  bool _pumping = false;
  String _currentJob = '';

  String get currentJob => _currentJob;
  bool get busy => _pumping;

  /// 排队执行。返回 `run()` 的结果（异常原样抛出）。
  Future<T> enqueue<T>(String name, Future<T> Function() run) {
    final job = _Job(name, () => run());
    _queue.add(job);
    unawaited(_pump());
    return job.completer.future.then((v) => v as T);
  }

  Future<void> _pump() async {
    if (_pumping) return;
    _pumping = true;
    notifyListeners();
    try {
      while (_queue.isNotEmpty) {
        final job = _queue.removeAt(0);
        _currentJob = job.name;
        notifyListeners();
        try {
          // ⚠ 返回值必须交给 completer —— 漏掉就是「永远不解码」那个故障。
          final result = await job.run();
          job.completer.complete(result);
        } catch (e, st) {
          job.completer.completeError(e, st);
        }
      }
    } finally {
      _currentJob = '';
      _pumping = false;
      notifyListeners();
    }
  }

  /* ── 打开 / 关闭 ─────────────────────────────────────────────── */

  /// 一次打开多份。**每次打开都是新标签**（这是唯一入口）。
  Future<List<CaptureDocument>> openFiles(List<String> paths) async {
    final out = <CaptureDocument>[];
    for (final p in paths) {
      // Create every tab and queue its container read, then decode only the tab
      // that ends up active. Decoding every intermediate tab makes a mixed drop
      // wait behind captures the user has not opened yet.
      out.add(await openFile(p, activate: false));
    }
    if (out.isNotEmpty) {
      _activeIndex = docs.indexOf(out.last);
      notifyListeners();
      unawaited(decode(out.last));
    }
    return out;
  }

  Future<CaptureDocument> openFile(String path, {bool activate = true}) async {
    final display = path.split(RegExp(r'[\\/]')).last;
    final doc = CaptureDocument(
      id: _nextId++,
      path: path,
      displayName: display,
    );
    docs.add(doc);
    if (activate) _activeIndex = docs.length - 1;
    notifyListeners();

    // 容器加载与解码分开：先只把标签与元数据弄出来。
    unawaited(
      enqueue('读取 $display', () async {
        try {
          final e = await ready;
          final metaJson = await e.openFile(doc.id, path);
          doc.meta = CaptureMeta(metaJson);
          doc.filters = FilterState.defaults(doc.isUfcs ? 'UFCS' : 'pd');
          doc.channel = doc.meta!.channels.length == 1
              ? doc.meta!.channels.first.channel
              : null;
          await doc.applyFilters(e);
          doc.state = DocState.ready;
          doc.notifyListeners();
        } catch (err) {
          // 坏文件只让这一个标签变红，不牵连其它标签。
          doc.state = DocState.failed;
          doc.error = '$err';
          doc.notifyListeners();
        }
      }),
    );

    if (activate) unawaited(decode(doc));
    return doc;
  }

  /// 打开（或切回）实时采集标签。
  ///
  /// 三个入口都落到这里：顶栏按钮、文件菜单 / Ctrl+D、空工作区空态。
  ///
  /// **实时标签全局只允许一个** —— 一台设备只能有一个会话，两个实时标签会互相抢设备，
  /// 而且「哪个标签代表设备」会变得含糊。已有就切过去，不新建。
  CaptureDocument openLive() {
    for (var i = 0; i < docs.length; i++) {
      if (docs[i].isLive) {
        _activeIndex = i;
        notifyListeners();
        return docs[i];
      }
    }
    final doc = CaptureDocument(
      id: _nextId++,
      path: '', // 实时采集没有路径
      displayName: '实时采集',
    );
    // 设备源可插拔：现在挂的是模拟源。真协议接入时换掉这一行即可，
    // 界面与 LiveSession 都不需要动。
    doc.attachLive(MockLiveSource());
    docs.add(doc);
    _activeIndex = docs.length - 1;
    notifyListeners();
    return doc;
  }

  /// 懒解码：已解码、正在解、或打不开的文件直接返回。
  /// （实时文档没有解码这一步：它的 state 直接就是 done。）
  Future<void> decode(CaptureDocument doc) async {
    if (!docs.contains(doc) ||
        doc.decoded ||
        doc.state == DocState.decoding ||
        doc.state == DocState.failed) {
      return;
    }
    // 等容器加载完（上一步可能还排在队列里）。
    await _awaitOpen(doc);
    if (!docs.contains(doc) || doc.state != DocState.ready) return;

    await enqueue('解码 ${doc.displayName}', () async {
      if (!docs.contains(doc)) return;
      doc.state = DocState.decoding;
      doc.notifyListeners();
      final e = await ready;
      try {
        final statsJson = await e.decode(
          doc.id,
          channel: doc.channel ?? -1,
          onProgress: (p) {
            doc.progress = p;
            doc.notifyListeners();
          },
        );
        doc.stats = statsJson == null ? null : DecodeStats(statsJson);
        doc.channel = doc.stats?.channel ?? doc.channel;
        final n = await e.viewCount(doc.id);
        doc.rows.setTotal(n);
        doc.state = DocState.done;
        // 解码完顺手把「类型计数」和「时间轴标记」取回来：
        // 这两样只在解码后才有意义，且都跟筛选无关（各取一次即可）。
        doc.typeCounts = await e.typeCounts(doc.id);
        doc.marks = PacketMarks.decode(await e.packetMarks(doc.id));
      } catch (err) {
        final pe = err is PdscopeException ? err : null;
        if (pe?.isCancelled == true) {
          doc.state = DocState.ready;
          doc.notice = '解码已取消';
        } else {
          doc.state = DocState.failed;
          doc.error = '$err';
        }
      } finally {
        doc.progress = null;
        doc.notifyListeners();
      }
    });
    notifyListeners();
  }

  /// 等待容器加载结束（轮询状态；队列是串行的，所以解排在后面天然有序）。
  Future<void> _awaitOpen(CaptureDocument doc) async {
    for (var i = 0; i < 600; i++) {
      if (!docs.contains(doc)) return;
      if (doc.state != DocState.opening) return;
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
  }

  Future<void> cancel(CaptureDocument doc) async {
    final e = await ready;
    e.cancel(doc.id);
  }

  /// ATK-C 切换通道需要新会话：核心会缓存一次解码的结果。
  Future<void> switchChannel(CaptureDocument doc, int channel) async {
    if (!docs.contains(doc) ||
        doc.meta?.container != 'atkcc' ||
        !doc.meta!.channels.any((c) => c.channel == channel) ||
        doc.channel == channel ||
        !_switchingChannels.add(doc.id)) {
      return;
    }
    try {
      final e = await ready;
      if (doc.state == DocState.decoding) e.cancel(doc.id);
      await enqueue<void>('切换 ${doc.displayName} 到 CH$channel', () async {
        await e.close(doc.id);
        doc.state = DocState.opening;
        doc.error = null;
        doc.notice = null;
        doc.stats = null;
        doc.progress = null;
        doc.selected = null;
        doc.selectedViewIndex = null;
        doc.detail = null;
        doc.rows.setTotal(0);
        doc.typeCounts = const {};
        doc.marks = PacketMarks.empty;
        doc.bus = null;
        doc.busAttempted = false;
        doc.wave = null;
        doc.filters.tFrom = 0;
        doc.filters.tTo = 1;
        doc.resetTimelineViews();
        doc.notifyListeners();
        try {
          doc.meta = CaptureMeta(await e.openFile(doc.id, doc.path));
          doc.channel = channel;
          await doc.applyFilters(e);
          doc.state = DocState.ready;
        } catch (err) {
          doc.error = '$err';
          doc.state = DocState.failed;
        }
        doc.notifyListeners();
      });
      if (docs.contains(doc) && doc.state == DocState.ready) await decode(doc);
    } finally {
      _switchingChannels.remove(doc.id);
    }
  }

  void activate(int index) {
    if (index < 0 || index >= docs.length || index == _activeIndex) return;
    _activeIndex = index;
    notifyListeners();
    // 切回来才解码 —— 这是「lazy」的落点。
    unawaited(decode(docs[index]));
  }

  void activateByShortcut(int n) {
    // Alt+1..9
    if (n >= 1 && n <= docs.length) activate(n - 1);
  }

  Future<void> closeAt(int index, {bool activateNext = true}) async {
    if (index < 0 || index >= docs.length) return;
    final doc = docs.removeAt(index);
    final wasActive = index == _activeIndex;
    if (index < _activeIndex) _activeIndex--;
    if (_activeIndex >= docs.length) _activeIndex = docs.length - 1;
    if (_activeIndex < 0) _activeIndex = -1;
    notifyListeners();
    // 实时标签**没有引擎会话**：它的 id 只是工作区自己编的号，
    // 交给 engine.close 会去关一个从没打开过的会话。
    if (doc.isLive) {
      await doc.liveSource?.stop();
      await doc.liveSource?.disconnect();
      doc.dispose();
      if (activateNext && wasActive && active != null) unawaited(decode(active!));
      return;
    }
    final e = await ready;
    if (doc.state == DocState.decoding) e.cancel(doc.id);
    // Opening, decoding and closing the same native session must stay in order.
    // Keep the document alive until earlier jobs have finished notifying it.
    await enqueue<void>('关闭 ${doc.displayName}', () => e.close(doc.id));
    doc.dispose();
    if (activateNext && wasActive && active != null) unawaited(decode(active!));
  }

  Future<void> closeOthers(int index) async {
    final keep = docs[index];
    for (var i = docs.length - 1; i >= 0; i--) {
      if (docs[i] != keep) await closeAt(i, activateNext: false);
    }
    if (active != null) unawaited(decode(active!));
  }

  Future<void> closeAll() async {
    for (var i = docs.length - 1; i >= 0; i--) {
      await closeAt(i, activateNext: false);
    }
  }

  /// 供命令行 / 文件关联使用：把路径转成绝对路径再打开。
  static String normalize(String path) => File(path).absolute.path;
}
