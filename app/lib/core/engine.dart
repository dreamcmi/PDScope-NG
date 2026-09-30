// engine.dart — 核心调用都放在工作 isolate 上
//
// 为什么必须有这一层：`pdscope_decode` 是一次**阻塞**的 C 调用（大文件的
// .atkcc 解码要几百毫秒到数秒）。直接在界面 isolate 上调它，窗口会假死 ——
// 所以用「换线程」而不是在主线程上让出。
//
// 进出的约定：
//   · 主 isolate 只发命令、只收结果；所有 session 句柄都住在工作 isolate 里。
//   · 打开成功后工作 isolate 会把**会话地址**（整数）报给主 isolate，
//     于是进度轮询与取消可以由主 isolate 直接发起 —— 不必等工作 isolate 从
//     阻塞的 decode 里脱身（它那时根本没有机会处理消息）。
//   · 只有 `pdscope_cancel` / `pdscope_get_progress` 这两个是「可从别的线程调用」的
//     （见 pdscope.h 的线程约定），其余调用一律留在工作 isolate 内串行执行。
import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'ffi.dart';

/// 解码进度。
class DecodeProgress {
  const DecodeProgress({
    required this.phase,
    required this.channel,
    required this.done,
    required this.total,
    required this.packets,
  });

  final int phase; // 0=空闲 1=读容器 2=解码
  final int channel;
  final int done;
  final int total;
  final int packets;

  double get fraction => total > 0 ? (done / total).clamp(0.0, 1.0) : 0.0;
}

class _Request {
  _Request(this.seq, this.cmd, this.payload);
  final int seq;
  final String cmd;
  final Map<String, dynamic> payload;
}

/* ────────────────────────── 主 isolate 侧 ────────────────────────── */

/// 与工作 isolate 的会话。
class EngineClient {
  EngineClient._(this._toWorker, this._fromWorker, this._errors);

  final SendPort _toWorker;

  // 这两个 ReceivePort 必须**留在字段里**：订阅是由监听回调持有的，端口本身
  // 一旦被回收，工作 isolate 之后发的消息就再也到不了这里（症状是「界面永远转圈，
  // 但工作线程其实已经干完了」）。所以它们看着没被读，却不能删。
  // ignore: unused_field
  final ReceivePort _fromWorker;
  // ignore: unused_field
  final ReceivePort _errors;

  var _seq = 0;
  final _pending = <int, Completer<dynamic>>{};

  /// docId → 会话地址（可跨 isolate 传的整数），供取消与进度轮询用。
  final _addresses = <int, int>{};

  /// docId → 进度回调。
  final _progressSinks = <int, void Function(DecodeProgress)>{};
  final _pollers = <int, Timer>{};

  static EngineClient? _instance;

  /// 全局单例：一份进程一个工作 isolate（会话在它内部按 docId 编号）。
  static Future<EngineClient> instance() async {
    final existing = _instance;
    if (existing != null) return existing;

    final fromWorker = ReceivePort();
    final errors = ReceivePort();
    final ready = Completer<SendPort>();
    fromWorker.listen((msg) {
      if (msg is SendPort) {
        if (!ready.isCompleted) ready.complete(msg);
        return;
      }
      _instance?._onMessage(msg);
    });
    errors.listen((e) {
      // 工作 isolate 崩了：把所有等待中的请求一次性失败掉，别让界面永远转圈。
      final self = _instance;
      if (self == null) return;
      self._failAll('核心工作线程异常终止：$e');
    });

    await Isolate.spawn(_engineMain, fromWorker.sendPort, onError: errors.sendPort);
    final toWorker = await ready.future;
    return _instance = EngineClient._(toWorker, fromWorker, errors);
  }

  void _onMessage(dynamic msg) {
    if (msg is! Map) return;
    final type = msg['type'];
    if (type == 'addr') {
      _addresses[msg['doc'] as int] = msg['addr'] as int;
      return;
    }
    if (type == 'reply') {
      final seq = msg['seq'] as int;
      final c = _pending.remove(seq);
      if (c == null) return;
      if (msg['ok'] == true) {
        c.complete(msg['value']);
      } else {
        c.completeError(PdscopeException(
          (msg['error'] as String?) ?? '未知错误',
          (msg['status'] as int?) ?? Status.internal,
        ));
      }
      return;
    }
  }

  void _failAll(String why) {
    final all = List.of(_pending.values);
    _pending.clear();
    for (final c in all) {
      if (!c.isCompleted) c.completeError(PdscopeException(why));
    }
  }

  Future<dynamic> _send(String cmd, Map<String, dynamic> payload) {
    final seq = ++_seq;
    final c = Completer<dynamic>();
    _pending[seq] = c;
    _toWorker.send(_Request(seq, cmd, payload));
    return c.future;
  }

  /// 会话地址（未打开时返回 null）。
  int? addressOf(int docId) => _addresses[docId];

  /// 打开一份抓包。返回元数据 JSON（`Map<String, dynamic>`）。
  ///
  /// ⚠ 这里**不要**在 `finally` 里再补一次 `meta`：打开失败时那个"补一次"也必然
  /// 失败，而 `finally` 里的异常会**顶掉**原始错误，最后报出来的是
  /// 「会话 N 不存在」这种二级症状 —— 真正的病因（路径打不开 / 格式不对）
  /// 被完全盖住，排查时会一路往会话生命周期上找，方向全错。
  /// 开文件这一步本来就是 `pdscope_open_file` 直接把元数据一起返回的，也不需要补。
  Future<Map<String, dynamic>> openFile(int docId, String path) async {
    // 先把旧地址清掉：同一 docId 复用（重开标签）时不能读到上一份的地址。
    _addresses.remove(docId);
    return Map<String, dynamic>.from(
      await _send('openFile', {'doc': docId, 'path': path}) as Map,
    );
  }

  Future<Map<String, dynamic>> openBytes(int docId, Uint8List bytes, String name) async {
    _addresses.remove(docId);
    return Map<String, dynamic>.from(
      await _send('openBytes', {'doc': docId, 'bytes': bytes, 'name': name}) as Map,
    );
  }

  /// 解码。可以边解边看进度（进度由主 isolate 直接读会话的原子量）。
  Future<Map<String, dynamic>?> decode(
    int docId, {
    int channel = -1,
    double sampleRateOverride = 0,
    bool metadataOnly = false,
    void Function(DecodeProgress)? onProgress,
    Duration pollInterval = const Duration(milliseconds: 100),
  }) async {
    if (onProgress != null) {
      _progressSinks[docId] = onProgress;
      _startPolling(docId, pollInterval);
    }
    try {
      final v = await _send('decode', {
        'doc': docId,
        'channel': channel,
        'rate': sampleRateOverride,
        'metadataOnly': metadataOnly,
      });
      return v == null ? null : Map<String, dynamic>.from(v as Map);
    } finally {
      _stopPolling(docId);
      _progressSinks.remove(docId);
    }
  }

  /// 请求中断：**从主 isolate 直接调**，这是唯一能在工作 isolate 阻塞时生效的路径。
  void cancel(int docId) {
    final addr = _addresses[docId];
    if (addr == null) return;
    PdscopeBindings.instance().cancel(Pointer<Void>.fromAddress(addr));
  }

  void _startPolling(int docId, Duration interval) {
    _stopPolling(docId);
    final b = PdscopeBindings.instance();
    final prog = calloc<PdscopeProgress>();
    _pollers[docId] = Timer.periodic(interval, (_) {
      final addr = _addresses[docId];
      final sink = _progressSinks[docId];
      if (addr == null || sink == null) return;
      final st = b.getProgress(Pointer<Void>.fromAddress(addr), prog);
      if (st != Status.ok) return;
      final p = prog.ref;
      sink(DecodeProgress(
        phase: p.phase,
        channel: p.channel,
        done: p.done,
        total: p.total,
        packets: p.packets,
      ));
    });
    _pollCleanup[docId] = () => calloc.free(prog);
  }

  final _pollCleanup = <int, void Function()>{};

  void _stopPolling(int docId) {
    _pollers.remove(docId)?.cancel();
    _pollCleanup.remove(docId)?.call();
  }

  Future<void> setFilter(int docId, Map<String, dynamic> filter) =>
      _send('filter', {'doc': docId, 'json': filter});

  Future<int> viewCount(int docId) async =>
      (await _send('viewCount', {'doc': docId}) as int);

  Future<int> packetCount(int docId) async =>
      (await _send('packetCount', {'doc': docId}) as int);

  /// 取一页列表行。
  Future<List<Map<String, dynamic>>> page(int docId, int offset, int limit) async {
    final v = await _send('page', {'doc': docId, 'offset': offset, 'limit': limit});
    return (v as List)
        .map((e) => Map<String, dynamic>.from(e as Map))
        .toList(growable: false);
  }

  Future<Map<String, dynamic>> detail(int docId, int index) async {
    final v = await _send('detail', {'doc': docId, 'index': index});
    return v == null ? <String, dynamic>{} : Map<String, dynamic>.from(v as Map);
  }

  Future<Map<String, dynamic>?> busSeries(int docId, {int targetPoints = 2400}) async {
    final v = await _send('bus', {'doc': docId, 'points': targetPoints});
    return v == null ? null : Map<String, dynamic>.from(v as Map);
  }

  /// 报文类型 → 条数。**统计的是全部报文，不受筛选影响** —— 它是筛选列表的候选来源，
  /// 那份列表不该随勾选变化（否则选掉一个选项，它自己就从列表里消失了）。
  Future<Map<String, int>> typeCounts(int docId) async {
    final v = await _send('typeCounts', {'doc': docId});
    final out = <String, int>{};
    for (final e in v as List) {
      final m = Map<String, dynamic>.from(e as Map);
      final type = m['type'];
      if (type is String) out[type] = (m['n'] as num?)?.toInt() ?? 0;
    }
    return out;
  }

  /// 时间轴的报文标记（紧凑二进制，见 `pdscope_packet_marks`）。
  Future<Uint8List> packetMarks(int docId) async {
    final v = await _send('marks', {'doc': docId});
    return v is Uint8List ? v : Uint8List.fromList((v as List).cast<int>());
  }

  Future<Uint8List> waveform(
    int docId, {
    required int channel,
    required int startSample,
    required int endSample,
    required int maxPoints,
  }) async {
    final v = await _send('wave', {
      'doc': docId,
      'channel': channel,
      'start': startSample,
      'end': endSample,
      'maxPoints': maxPoints,
    });
    return v is Uint8List ? v : Uint8List.fromList((v as List).cast<int>());
  }

  Future<String> exportCsv(int docId, {int limit = 0, bool bom = true}) async =>
      (await _send('csv', {'doc': docId, 'limit': limit, 'bom': bom}) as String);

  Future<String> exportJson(int docId, {int limit = 0}) async =>
      (await _send('json', {'doc': docId, 'limit': limit}) as String);

  Future<String> defaultCsvName(int docId) async =>
      (await _send('defname', {'doc': docId}) as String);

  Future<void> close(int docId) async {
    _stopPolling(docId);
    _addresses.remove(docId);
    try {
      await _send('close', {'doc': docId});
    } on PdscopeException {
      // 已经关了就算了：关闭路径不该因为「本来就没了」而报错。
    }
  }
}

/* ────────────────────────── 工作 isolate 侧 ────────────────────────── */

Future<void> _engineMain(SendPort toMain) async {
  final inbox = ReceivePort();
  toMain.send(inbox.sendPort);

  final b = PdscopeBindings.instance();
  final sessions = <int, Pointer<Void>>{};

  void reply(int seq, {Object? value, String? error, int status = Status.ok}) {
    toMain.send({
      'type': 'reply',
      'seq': seq,
      'ok': error == null,
      'value': value,
      'error': error,
      'status': status,
    });
  }

  Pointer<Void> need(int doc) {
    final s = sessions[doc];
    if (s == null) {
      throw PdscopeException('会话 $doc 不存在（可能已关闭）', Status.state);
    }
    return s;
  }

  await for (final msg in inbox) {
    if (msg is! _Request) continue;
    final doc = msg.payload['doc'] as int?;
    try {
      final value = _handle(b, msg.cmd, msg.payload, sessions, need, toMain);
      reply(msg.seq, value: value);
    } on PdscopeException catch (e) {
      reply(msg.seq, error: e.message, status: e.status);
    } catch (e) {
      reply(msg.seq, error: '$e');
    }
    // doc 占位，避免分析器把未使用的参数报错
    assert(doc == null || doc >= 0 || doc < 0);
  }
}

Object? _handle(
  PdscopeBindings b,
  String cmd,
  Map<String, dynamic> p,
  Map<int, Pointer<Void>> sessions,
  Pointer<Void> Function(int) need,
  SendPort toMain,
) {
  final doc = p['doc'] as int;

  switch (cmd) {
    case 'openFile':
      _doOpen(b, doc, sessions, name: p['path'] as String, path: p['path'] as String);
      toMain.send({'type': 'addr', 'doc': doc, 'addr': sessions[doc]!.address});
      return _meta(b, need(doc));

    case 'openBytes':
      _doOpen(
        b,
        doc,
        sessions,
        name: p['name'] as String,
        bytes: p['bytes'] as Uint8List,
      );
      toMain.send({'type': 'addr', 'doc': doc, 'addr': sessions[doc]!.address});
      return _meta(b, need(doc));

    case 'close':
      final s = sessions.remove(doc);
      if (s != null) b.close(s);
      return null;

    case 'meta':
      return _meta(b, need(doc));

    case 'decode':
      final opts = calloc<PdscopeDecodeOpts>();
      opts.ref
        ..channel = p['channel'] as int
        ..sampleRateOverride = (p['rate'] as num).toDouble()
        ..metadataOnly = (p['metadataOnly'] as bool) ? 1 : 0
        ..reserved = 0;
      final out = calloc<PdscopeBuf>();
      try {
        final st = b.decode(need(doc), opts, out);
        if (st == Status.cancelled) {
          throw PdscopeException('已取消', Status.cancelled);
        }
        if (st != Status.ok) {
          throw PdscopeException('解码失败（${b.statusName(st)}）', st);
        }
        if (out.ref.data == nullptr || out.ref.len == 0) return null;
        final s = utf8.decode(out.ref.data.asTypedList(out.ref.len));
        return Map<String, dynamic>.from(_jsonMap(s));
      } finally {
        b.bufFree(out);
        calloc.free(out);
        calloc.free(opts);
      }

    case 'filter':
      final err = calloc<Pointer<Utf8>>();
      final jsonStr = _encodeFilter(p['json'] as Map<String, dynamic>);
      final cstr = jsonStr.toNativeUtf8();
      try {
        final st = b.setFilter(need(doc), cstr, err);
        if (st != Status.ok) {
          final m = PdscopeBindings.takeError(err);
          throw PdscopeException(m.isEmpty ? '设置筛选失败' : m, st);
        }
        return null;
      } finally {
        calloc.free(cstr);
        calloc.free(err);
      }

    case 'viewCount':
      final out = calloc<Uint64>();
      try {
        final st = b.viewCount(need(doc), out);
        if (st != Status.ok) throw PdscopeException('取视图条数失败', st);
        return out.value;
      } finally {
        calloc.free(out);
      }

    case 'packetCount':
      final out = calloc<Uint64>();
      try {
        final st = b.packetCount(need(doc), out);
        if (st != Status.ok) throw PdscopeException('取报文总数失败', st);
        return out.value;
      } finally {
        calloc.free(out);
      }

    case 'page':
      final s = b.jsonOf(
        need(doc),
        (sess, buf) => b.queryPage(sess, p['offset'] as int, p['limit'] as int, buf),
      );
      return _jsonList(s).cast<Map>();

    case 'detail':
      final s = b.jsonOf(
        need(doc),
        (sess, buf) => b.packetDetail(sess, p['index'] as int, buf),
      );
      return _jsonMap(s);

    case 'bus':
      final s = b.jsonOf(
        need(doc),
        (sess, buf) => b.busSeries(sess, p['points'] as int, buf),
      );
      return _jsonMap(s);

    case 'typeCounts':
      final s = b.jsonOf(need(doc), (sess, buf) => b.typeCounts(sess, buf));
      return _jsonList(s)
          .map((e) => Map<String, dynamic>.from(e as Map))
          .toList(growable: false);

    case 'marks':
      return b.callBufAsBytes((buf) => b.packetMarks(need(doc), buf));

    case 'wave':
      return b.callBufAsBytes(
        (buf) => b.waveformRange(
          need(doc),
          p['channel'] as int,
          p['start'] as int,
          p['end'] as int,
          p['maxPoints'] as int,
          buf,
        ),
      );

    case 'csv':
      final opts = calloc<PdscopeExportOpts>();
      opts.ref
        ..limit = p['limit'] as int
        ..bom = (p['bom'] as bool) ? 1 : 0
        ..reserved = 0;
      try {
        return b.jsonOf(
          need(doc),
          (sess, buf) => b.exportCsv(sess, opts, buf),
        );
      } finally {
        calloc.free(opts);
      }

    case 'json':
      final opts = calloc<PdscopeExportOpts>();
      opts.ref
        ..limit = p['limit'] as int
        ..bom = 0
        ..reserved = 0;
      try {
        return b.jsonOf(
          need(doc),
          (sess, buf) => b.exportJson(sess, opts, buf),
        );
      } finally {
        calloc.free(opts);
      }

    case 'defname':
      final slot = calloc<Pointer<Utf8>>();
      try {
        final st = b.defaultCsvName(need(doc), slot);
        if (st != Status.ok) throw PdscopeException('取默认文件名失败', st);
        final v = slot.value;
        if (v == nullptr) return '';
        final s = v.toDartString();
        b.strFree(v);
        return s;
      } finally {
        calloc.free(slot);
      }

    default:
      throw PdscopeException('未知命令：$cmd', Status.argument);
  }
}

void _doOpen(
  PdscopeBindings b,
  int doc,
  Map<int, Pointer<Void>> sessions, {
  required String name,
  String? path,
  Uint8List? bytes,
}) {
  // 重新打开同一个 docId 时先把旧的关掉，否则会话泄漏。
  final old = sessions.remove(doc);
  if (old != null) b.close(old);

  final err = calloc<Pointer<Utf8>>();
  Pointer<Void> s;
  try {
    if (bytes != null) {
      final n = bytes.length;
      if (n == 0) throw PdscopeException('文件是空的', Status.format);
      final buf = calloc<Uint8>(n);
      try {
        buf.asTypedList(n).setAll(0, bytes);
        s = b.openBytes(buf, n, name.toNativeUtf8(), err);
      } finally {
        calloc.free(buf);
      }
    } else {
      s = _openFileUtf8(b, path!, err);
    }
    if (s == nullptr) {
      final m = PdscopeBindings.takeError(err);
      throw PdscopeException(m.isEmpty ? '打不开这份文件' : m, Status.format);
    }
    sessions[doc] = s;
  } finally {
    calloc.free(err);
  }
}

Pointer<Void> _openFileUtf8(PdscopeBindings b, String path, Pointer<Pointer<Utf8>> err) {
  final c = path.toNativeUtf8();
  try {
    return b.openFile(c, err);
  } finally {
    calloc.free(c);
  }
}

Map<String, dynamic> _meta(PdscopeBindings b, Pointer<Void> s) =>
    _jsonMap(b.jsonOf(s, (sess, buf) => b.metadata(sess, buf)));

/* ── JSON ─────────────────────────────────────────────────────────────
   核心的 JSON 只用对象/数组/字符串/数字/布尔/null，dart:convert 足够。
   这里收口「结果一定不是 null」这层语义，免得调用点到处判空。 */

Map<String, dynamic> _jsonMap(String s) {
  if (s.isEmpty) return <String, dynamic>{};
  final v = jsonDecode(s);
  return v is Map<String, dynamic> ? v : <String, dynamic>{};
}

List<dynamic> _jsonList(String s) {
  if (s.isEmpty) return const [];
  final v = jsonDecode(s);
  return v is List ? v : const [];
}

/// 筛选条件 → JSON 文本。字段由调用方（filters.dart）按核心的认识拼好。
String _encodeFilter(Map<String, dynamic> f) => jsonEncode(f);
