import 'dart:async';
import 'dart:typed_data';

import 'live_source.dart';
import 'pcl_pd_decoder.dart';
import 'pcl_protocol.dart';
import 'pcl_transport.dart';
import 'pcl_uart_transport.dart';

/// 当前 PCL v1 的真实 PD 设备源；收发方式由通用传输接口提供。
///
/// 默认 UART 921600、8N1。先验证 HELLO，CFG 后等待匹配 START，END 后
/// 继续接收尾部直到 STOP。命令操作串行，读取和后台解码持续运行。
class PclLiveSource implements LiveSource {
  // 公共注入参数 decoder 与内部持有字段 _decoder 有意分开命名。
  PclLiveSource({
    PclTransport? transport,
    PclPdDecoder? decoder,
    this.baudRate = 921600,
    this.responseTimeout = const Duration(seconds: 3),
  }) : _transport = transport ?? PclUartTransport(),
       // ignore: prefer_initializing_formals
       _decoder = decoder;

  final PclTransport _transport;
  PclPdDecoder? _decoder;
  final int baudRate;
  final Duration responseTimeout;
  final _events = StreamController<LiveEvent>.broadcast();
  final _parser = PclStreamParser();
  final _hostClock = Stopwatch()..start();
  final Map<String, PclEndpoint> _endpoints = {};
  StreamSubscription<Uint8List>? _incoming;
  Timer? _parserTimer;
  Future<void> _rx = Future.value();
  int _generation = 0;
  int _backlog = 0;
  bool _open = false;
  bool _disposed = false;
  bool _busy = false;
  bool _listenOnly = false;
  bool _recovering = false;
  bool _running = false;
  bool _finishing = false;
  bool _waitingStart = false;
  bool _stopConfirmed = false;
  bool _currentHello = false;
  bool _failing = false;
  Future<void>? _disposing;
  int _reads = 0;
  PclEndpoint? _endpoint;
  PclHello? _hello;
  LiveDevice? _device;
  PclConfig? _config;
  PclTickClock? _ticks;
  Completer<void>? _helloWait;
  Completer<void>? _startWait;
  Completer<void>? _stopWait;
  Uint8List? _lastStop;
  int? _nextFrameId;
  int? _lastFrameId;
  int _index = 0;
  int _lossBits = 0;
  int _lossReports = 0;
  int _sequenceGaps = 0;
  int _semanticErrors = 0;
  int _parserErrorBase = 0;
  int _wireErrors = 0;
  int _commands = 0;
  int _rejects = 0;
  int _timeouts = 0;
  double _duration = 0;
  bool _timeUncertain = false;

  /// 当前连接中已验证的 HELLO，未验证前为 null。
  PclHello? get hello => _hello;

  /// HELLO 更新后的设备能力，枚举结果不代表设备已确认。
  LiveDevice? get connectedDevice => _device;

  @override
  String get backendName => 'pcl/${_endpoint?.transport.name ?? 'uart'}';
  @override
  String get displayName => _device?.name ?? 'PCL PD 分析仪';
  @override
  bool get supportsPause => false;
  @override
  Stream<LiveEvent> get events => _events.stream;

  @override
  Future<List<LiveDevice>> enumerate() => _control(() async {
    _state(LiveState.scanning);
    try {
      final endpoints = await _transport.enumerate();
      _endpoints.clear();
      for (final endpoint in endpoints) {
        _endpoints[endpoint.id] = endpoint;
      }
      final devices = endpoints
          .map((endpoint) => _describe(endpoint, null))
          .toList();
      _state(
        devices.isEmpty ? LiveState.idle : LiveState.found,
        devices.isEmpty ? '未发现串口，请检查设备连接和串口驱动' : null,
      );
      return devices;
    } catch (error) {
      _state(LiveState.recoverableError, '设备枚举失败：$error');
      throw LiveSourceError('设备枚举失败：$error');
    }
  });

  @override
  Future<void> connect(LiveDevice device) => _control(() async {
    if (_open) throw LiveSourceError('请先断开当前设备');
    final endpoint = _endpoints[device.id];
    if (endpoint == null) throw LiveSourceError('端点已失效，请重新查找设备');
    _state(LiveState.connecting);
    _helloWait = _newWait();
    try {
      await _openEndpoint(endpoint);
      await _helloWait!.future.timeout(responseTimeout);
      if (_hello?.cfgError != 0) {
        _trace(false, Uint8List(0), 'HELLO.cfg_err=${_hello!.cfgError}');
      }
      _state(LiveState.ready);
    } catch (error) {
      await _closeEndpoint();
      _state(LiveState.recoverableError, '连接失败，未确认 PCL HELLO：$error');
      throw LiveSourceError('连接失败，未确认 PCL HELLO：$error');
    } finally {
      _helloWait = null;
    }
  });

  /// 直接打开一个已枚举端点进行零写监听；不发 END、CFG 或其他字节。
  Future<int> listenDevice(LiveDevice device, int windowMs) =>
      _control(() async {
        if (_open) throw LiveSourceError('请先断开当前端点，再进行零写监听');
        final endpoint = _endpoints[device.id];
        if (endpoint == null) throw LiveSourceError('端点已失效，请重新查找设备');
        _listenOnly = true;
        try {
          await _openEndpoint(endpoint);
          return await _listenWindow(windowMs);
        } finally {
          await _closeEndpoint();
          _listenOnly = false;
          _state(LiveState.found);
        }
      });

  @override
  Future<int> listen(int windowMs) => _control(() async {
    if (!_open || _running || _waitingStart || _finishing) {
      throw LiveSourceError('零写监听只适用于已连接的空闲端点');
    }
    _listenOnly = true;
    try {
      return await _listenWindow(windowMs);
    } finally {
      _listenOnly = false;
      if (_open) _state(LiveState.ready);
    }
  });

  Future<int> _listenWindow(int windowMs) async {
    if (windowMs < 1 || windowMs > 60000) throw LiveSourceError('监听时长超出范围');
    final before = _reads;
    _state(LiveState.listening);
    await Future<void>.delayed(Duration(milliseconds: windowMs));
    return _reads - before;
  }

  @override
  Future<void> start(LiveStartOptions options) => _control(() async {
    final endpoint = _endpoint;
    if (!_open ||
        _hello == null ||
        endpoint == null ||
        _running ||
        _finishing) {
      throw LiveSourceError('设备未就绪，或旧采集尚未收尾');
    }
    if (!_currentHello) {
      _helloWait = _newWait();
      try {
        await _helloWait!.future.timeout(responseTimeout);
      } finally {
        _helloWait = null;
      }
    }
    if (!options.pd || options.highSpeed) {
      throw LiveSourceError('当前 PCL 页面只配置 PD 抓包');
    }
    final declaration = _hello;
    if (declaration == null) throw LiveSourceError('设备已断开');
    final opts =
        (options.goodCrcFilter ? 1 : 0) |
        (options.analog && options.voltage ? 2 : 0) |
        (options.analog && options.current ? 4 : 0);
    final config = PclConfig(
      opts: opts,
      channelSelect: options.channelSelect,
      samplePeriodMs: (opts & 6) == 0 ? 0 : options.pollMs,
      hostTimeoutMs: endpoint.transport == PclTransportKind.uart ? 0 : 2000,
    );
    try {
      config.validate(declaration, endpoint.transport);
    } on PclProtocolError catch (error) {
      throw LiveSourceError(error.message);
    }
    _decoder ??= PclNativePdDecoder();
    await _decoder!.reset();
    _config = config;
    _startWait = _newWait();
    _waitingStart = true;
    _stopConfirmed = false;
    _lastStop = null;
    try {
      await _send(config.encode(), 'CFG：PD 抓包');
      await _startWait!.future.timeout(responseTimeout);
    } catch (error) {
      _waitingStart = false;
      _timeouts += error is TimeoutException ? 1 : 0;
      if (_open) {
        _recovering = true;
        _helloWait = _newWait();
        try {
          await _sendEnd(false);
          await _helloWait!.future.timeout(responseTimeout);
        } catch (_) {
          await _closeEndpoint();
        } finally {
          _helloWait = null;
          _recovering = false;
        }
      }
      _clearRun();
      _state(_open ? LiveState.ready : LiveState.parked, '采集未确认开始：$error');
      throw LiveSourceError('采集未确认开始：$error');
    } finally {
      _startWait = null;
    }
  });

  @override
  void pause() => throw LiveSourceError('PCL 不提供暂停命令；停止采集使用 END');
  @override
  void resume() => throw LiveSourceError('PCL 不提供恢复命令；请开始新的采集');

  @override
  Future<void> stop() => _control(_stop);

  Future<void> _stop() async {
    if (!_open || (!_running && !_waitingStart && !_finishing)) return;
    _finishing = true;
    _stopWait = _newWait();
    try {
      await _sendEnd(true);
      try {
        await _stopWait!.future.timeout(responseTimeout);
      } on TimeoutException {
        _timeouts++;
        // 未配置态设备可以幂等重发缓存 STOP；有限重试，不越过新 CFG。
        await _sendEnd(true);
        await _stopWait!.future.timeout(responseTimeout);
      }
      if (!_stopConfirmed) throw LiveSourceError('未收到 STOP，结束未确认');
    } catch (error) {
      _clearRun();
      await _closeEndpoint();
      _state(LiveState.parked, '结束未确认，不能保证尾部已排空：$error');
      throw LiveSourceError('结束未确认，不能保证尾部已排空：$error');
    } finally {
      _stopWait = null;
      _stats();
    }
  }

  @override
  Future<void> disconnect() => _control(() async {
    Object? failure;
    try {
      await _stop();
    } catch (error) {
      failure = error;
    }
    await _closeEndpoint();
    _clearRun();
    _state(LiveState.idle, failure?.toString());
  });

  @override
  Future<void> dispose() => _disposing ??= _dispose();

  Future<void> _dispose() async {
    if (_disposed) return;
    // 页面正常路径先等待 disconnect，兜底释放同样发送 END。
    try {
      if (_open) await _stop();
    } catch (_) {
      /* 保留结束未确认事件。 */
    }
    try {
      await _closeEndpoint();
    } finally {
      try {
        await _decoder?.close();
      } finally {
        try {
          final transport = _transport;
          if (transport is PclUartTransport) await transport.dispose();
        } finally {
          _disposed = true;
          await _events.close();
        }
      }
    }
  }

  Future<T> _control<T>(Future<T> Function() action) async {
    if (_disposed) throw LiveSourceError('设备源已经释放');
    if (_busy) throw LiveSourceError('设备操作尚未完成');
    _busy = true;
    try {
      return await action();
    } finally {
      _busy = false;
    }
  }

  Future<void> _openEndpoint(PclEndpoint endpoint) async {
    _endpoint = endpoint;
    _device = _describe(endpoint, null);
    _hello = null;
    _currentHello = false;
    _recovering = false;
    _clearRun();
    _parser.reset();
    _rx = Future.value();
    _backlog = 0;
    final generation = ++_generation;
    // 先挂监听，避免打开端点后即时 HELLO 丢失。
    _incoming = _transport.incoming.listen(
      (bytes) {
        if (generation != _generation) return;
        _reads++;
        // 串口无反压时应用仍需有界。超过上限明确停止，不能悄悄丢数据。
        if (_backlog + bytes.length > 512 * 1024) {
          unawaited(_transportFailure('上位机解码积压超过 512 KiB，结束未确认'));
          return;
        }
        final owned = Uint8List.fromList(bytes);
        final receivedAt = _hostClock.elapsedMilliseconds;
        _backlog += owned.length;
        _rx = _rx
            .then((_) async {
              if (generation != _generation) return;
              final frames = _parser.add(owned, nowMs: receivedAt);
              for (final frame in frames) {
                await _handleFrame(frame);
              }
              _stats();
            })
            .catchError((Object error, StackTrace stack) async {
              await _transportFailure('接收或解码失败：$error');
            })
            .whenComplete(() {
              if (generation == _generation) _backlog -= owned.length;
            });
      },
      onError: (Object error) {
        unawaited(_transportFailure('串口 I/O 错误：$error'));
      },
      onDone: () {
        if (_open) unawaited(_transportFailure('传输端点已关闭，结束未确认'));
      },
    );
    _open = true;
    await _transport.open(endpoint, baudRate: baudRate);
    _parserTimer = Timer.periodic(const Duration(milliseconds: 250), (_) {
      final checkedAt = _hostClock.elapsedMilliseconds;
      _rx = _rx
          .then((_) async {
            if (generation != _generation) return;
            for (final frame in _parser.expire(checkedAt)) {
              await _handleFrame(frame);
            }
            _stats();
          })
          .catchError((Object error) async {
            await _transportFailure('解析失败：$error');
          });
    });
  }

  Future<void> _closeEndpoint() async {
    _open = false;
    _generation++;
    _parserTimer?.cancel();
    _parserTimer = null;
    await _incoming?.cancel();
    _incoming = null;
    try {
      await _transport.close();
    } finally {
      _hello = null;
      _device = null;
      _parser.reset();
    }
  }

  Future<void> _handleFrame(PclFrame frame) async {
    final generation = _generation;
    _trace(false, frame.bytes, 'PCL TYPE=${frame.type}');
    try {
      if (_hello != null && frame.body.length > _hello!.maxFrame) {
        throw const PclProtocolError('BODY 超出 HELLO.max_frame');
      }
      if (frame.type == PclWire.hello) {
        final declaration = PclHello.decode(frame, _endpoint!.transport);
        if (_running || _finishing) {
          _trace(false, Uint8List(0), '采集中收到 HELLO：旧采集结束未确认');
          _clearRun();
          _state(LiveState.stopped, '设备回到未配置态，旧采集结束未确认');
          _complete(_stopWait);
        }
        if (_waitingStart && declaration.cfgError != 0) {
          _rejects++;
          _fail(_startWait, '设备拒绝 CFG：${declaration.cfgError}');
        }
        _hello = declaration;
        _currentHello = true;
        _parser.maxBodyLength = declaration.maxFrame;
        _device = _describe(_endpoint!, declaration);
        _emit(LiveDeviceEvent(_device!));
        _complete(_helloWait);
        return;
      }
      if (frame.type == PclWire.cfg || frame.type == PclWire.end) {
        throw const PclProtocolError('设备发送了反向 CFG/END');
      }
      if (frame.type != PclWire.data && frame.type != PclWire.evt) {
        _trace(false, Uint8List(0), '跳过未定义 TYPE，CRC 已验证');
        return;
      }
      if (_hello == null || _recovering) {
        if (!_listenOnly && !_recovering) {
          _recovering = true;
          await _sendEnd(false);
          _trace(false, Uint8List(0), '恢复旧采集，丢弃旧 DATA/EVT，等待 HELLO');
        }
        return;
      }
      if (_listenOnly) return;
      if (frame.type == PclWire.data) {
        if (!_running || _config == null) return;
        final data = PclData.decode(frame, _hello!, _config!);
        if (!_sequence(data.id, data.lossFlags)) return;
        final timestamps = data.records
            .map((record) => _time(record.ticks, frame.receivedAtMs))
            .toList();
        final inputs = <PclDecodeInput>[];
        for (var i = 0; i < data.records.length; i++) {
          final record = data.records[i];
          inputs.add(
            PclDecodeInput(
              bytes: record.bytes,
              sop: record.kind,
              cc: record.channel,
              flags: record.flags,
              elapsedTicks: timestamps[i].extendedTicks ?? 0,
              timebaseHz: _hello!.timebaseHz,
              index: _index++,
            ),
          );
        }
        final decoded = await _decoder!.decodeBatch(inputs);
        if (generation != _generation) return;
        final rows = <Map<String, dynamic>>[];
        final details = <int, Map<String, dynamic>>{};
        for (var i = 0; i < decoded.length; i++) {
          final row = decoded[i]['row'] as Map<String, dynamic>;
          final detail = decoded[i]['detail'] as Map<String, dynamic>;
          row['elapsedTicks'] = data.records[i].ticks;
          detail['elapsedTicks'] = data.records[i].ticks;
          row['frameId'] = data.id;
          detail['frameId'] = data.id;
          if (timestamps[i].uncertain) {
            for (final json in [row, detail]) {
              json['timeUncertain'] = true;
              json['startSample'] = null;
              json['timeMs'] = null;
              json['endSample'] = null;
              json['extendedTicks'] = null;
              json['elapsed'] = '未知';
            }
          }
          rows.add(row);
          details[row['index'] as int] = detail;
        }
        _emit(LiveRowsEvent(rows, details));
        return;
      }
      final events = PclEvents.decode(frame, _hello!, _config);
      final first = events.items.first;
      if (first.code == 1) {
        if (!_waitingStart || _config == null) return;
        if (!_config!.matches(first)) {
          _fail(_startWait, 'START 与 CFG 不匹配');
          return;
        }
        _ticks = PclTickClock(_hello!.timebaseHz, nowMs: frame.receivedAtMs);
        _resetStats();
        _parserErrorBase = frame.parserErrorsBefore;
        _commands = 1;
        _currentHello = false;
        _nextFrameId = 1;
        _lastFrameId = 0;
        _observeLoss(events.lossFlags);
        _running = true;
        _waitingStart = false;
        _state(LiveState.capturing);
        _complete(_startWait);
        return;
      }
      if (first.code == 2 &&
          _lastStop != null &&
          _sameBytes(_lastStop!, frame.bytes)) {
        return;
      }
      if (!_running) return;
      if (!_sequence(events.id, events.lossFlags)) return;
      for (final event in events.items) {
        final time = _time(event.ticks, frame.receivedAtMs);
        switch (event.code) {
          case 2:
            _lastStop = Uint8List.fromList(frame.bytes);
            _stopConfirmed = true;
            _clearRun();
            _state(
              event.flags == 0 ? LiveState.stopped : LiveState.parked,
              event.flags == 0
                  ? '已收到 STOP，尾部接收完成'
                  : '设备停止：reason=${event.flags}, error=${event.a}',
            );
            _complete(_stopWait);
          case 3:
            _rejects++;
            _trace(
              false,
              Uint8List(0),
              'ERROR：type=${event.flags}, code=${event.a}；等待设备 STOP',
            );
          case 4:
            if (!time.uncertain) {
              _emit(
                LiveBusEvent(
                  [time.extendedTicks! / _hello!.timebaseHz],
                  [event.a == 0xffff ? double.nan : event.a / 100.0],
                  [event.b == 0xffff ? double.nan : event.b / 1000.0],
                ),
              );
            }
          case 5:
            _trace(
              false,
              Uint8List(0),
              'LINK_STATE：valid=${event.flags}, state=${event.a}, ticks=${event.ticks}',
            );
        }
      }
    } on PclProtocolError catch (error) {
      _semanticErrors++;
      _trace(false, Uint8List(0), '整帧拒绝：${error.message}');
    }
  }

  bool _sequence(int id, int lossFlags) {
    final next = _nextFrameId;
    if (next == null) return false;
    final delta = (id - next) & 0xffffffff;
    if (id == _lastFrameId) {
      _semanticErrors++;
      _trace(false, Uint8List(0), '重复或逆序帧 id=$id，整帧拒绝');
      return false;
    }
    _sequenceGaps += delta;
    _nextFrameId = (id + 1) & 0xffffffff;
    _lastFrameId = id;
    _observeLoss(lossFlags);
    return true;
  }

  void _observeLoss(int lossFlags) {
    final fresh = lossFlags & ~_lossBits;
    if (fresh != 0) {
      _lossReports++;
      _trace(
        false,
        Uint8List(0),
        '损失标志新增 0x${fresh.toRadixString(16)}，无法推算精确丢包数',
      );
    }
    _lossBits |= lossFlags;
  }

  PclTimestamp _time(int raw, int arrivedMs) {
    final before = _ticks!.uncertain;
    final time = _ticks!.extend(raw, nowMs: arrivedMs);
    if (!before && time.uncertain) {
      _timeUncertain = true;
      _trace(false, Uint8List(0), '时间连续性不确定，保留原始刻度和接收顺序');
    }
    if (!time.uncertain) {
      final seconds = time.extendedTicks! / _hello!.timebaseHz;
      if (seconds > _duration) _duration = seconds;
    }
    return time;
  }

  Future<void> _send(Uint8List bytes, String note) async {
    if (!_open) throw LiveSourceError('设备已经关闭');
    if (_listenOnly) throw LiveSourceError('零写监听禁止发送命令');
    await _transport.write(Uint8List.fromList(bytes));
    _commands++;
    _trace(true, bytes, note);
    _stats();
  }

  Future<void> _sendEnd(bool flush) => _send(
    PclWire.encode(PclWire.end, [flush ? 1 : 0]),
    'END：${flush ? '排空' : '恢复并丢弃未组帧项'}',
  );

  Future<void> _transportFailure(String message) async {
    if (_failing) return;
    if (!_open && _incoming == null) return;
    _failing = true;
    try {
      _fail(_helloWait, message);
      _fail(_startWait, message);
      _fail(_stopWait, message);
      // UART 没有断开停采语义，尽力发 END 后关闭，不自动重连。
      if (_open && !_listenOnly && (_running || _waitingStart || _finishing)) {
        try {
          await _sendEnd(false);
        } catch (_) {
          /* 失联时不能保证发送成功。 */
        }
      }
      _clearRun();
      await _closeEndpoint();
      _state(LiveState.disconnected, message);
    } finally {
      _failing = false;
    }
  }

  Completer<void> _newWait() {
    final wait = Completer<void>();
    // 快速设备可能在 write/open 的 Future 返回前响应错误。
    wait.future.ignore();
    return wait;
  }

  void _clearRun() {
    _running = false;
    _waitingStart = false;
    _finishing = false;
    _nextFrameId = null;
    _lastFrameId = null;
    _ticks = null;
    _config = null;
  }

  void _resetStats() {
    _index = 0;
    _lossBits = 0;
    _lossReports = 0;
    _sequenceGaps = 0;
    _semanticErrors = 0;
    _wireErrors = 0;
    _commands = 0;
    _rejects = 0;
    _timeouts = 0;
    _duration = 0;
    _timeUncertain = false;
  }

  void _stats() {
    if (_open && _parser.invalidFrames - _parserErrorBase > _wireErrors) {
      _wireErrors = _parser.invalidFrames - _parserErrorBase;
    }
    _emit(
      LiveStatsEvent(
        durationSec: _duration,
        commands: _commands,
        ioTimeouts: _timeouts,
        rejects: _rejects,
        dropped: 0,
        lossReports: _lossReports,
        sequenceGaps: _sequenceGaps,
        invalidFrames: _semanticErrors + _wireErrors,
        lossCountKnown: false,
        timeUncertain: _timeUncertain,
      ),
    );
  }

  LiveDevice _describe(PclEndpoint endpoint, PclHello? hello) {
    LiveCapability cap(String label, int bit) => LiveCapability(
      label,
      hello == null
          ? LiveCapState.unknown
          : hello.has(bit)
          ? LiveCapState.yes
          : LiveCapState.no,
      hello == null
          ? '等待 HELLO 确认'
          : hello.has(bit)
          ? null
          : '设备未声明此能力',
    );
    return LiveDevice(
      id: endpoint.id,
      name: hello == null ? endpoint.name : 'PCL PD · ${endpoint.name}',
      serial: endpoint.id,
      transport: endpoint.transport.name.toUpperCase(),
      transportNote: hello == null
          ? '端点尚未验证'
          : 'PCL v1 · 固件 ${hello.firmwareMajor}.${hello.firmwareMinor} · ${hello.timebaseHz} Hz',
      capabilities: [
        cap('PD 报文', PclFeature.pd),
        cap('母线电压', PclFeature.voltage),
        cap('母线电流', PclFeature.current),
        cap('GoodCRC 过滤', PclFeature.goodCrcFilter),
        cap('CC 选择', PclFeature.ccSelect),
        cap('双 CC 接收', PclFeature.dualCc),
      ],
    );
  }

  void _trace(bool tx, Uint8List bytes, String note) => _emit(
    LiveTraceEvent(
      LiveTraceEntry(
        _hostClock.elapsedMilliseconds.toDouble(),
        tx,
        bytes
            .map(
              (value) => value.toRadixString(16).padLeft(2, '0').toUpperCase(),
            )
            .join(' '),
        note,
      ),
    ),
  );
  void _state(LiveState state, [String? message]) =>
      _emit(LiveStateEvent(state, message: message));
  void _emit(LiveEvent event) {
    if (!_disposed && !_events.isClosed) _events.add(event);
  }

  void _complete(Completer<void>? wait) {
    if (wait != null && !wait.isCompleted) wait.complete();
  }

  void _fail(Completer<void>? wait, String message) {
    if (wait != null && !wait.isCompleted) {
      wait.completeError(LiveSourceError(message));
    }
  }

  bool _sameBytes(Uint8List left, Uint8List right) {
    if (left.length != right.length) return false;
    for (var i = 0; i < left.length; i++) {
      if (left[i] != right[i]) return false;
    }
    return true;
  }
}
