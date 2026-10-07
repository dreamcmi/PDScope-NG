/// Windows 串口字节传输实现。
///
/// 串口读写在独立 isolate 中通过同步 Win32 I/O 执行；串口读取配置了有限
/// 超时，因此 Flutter UI isolate 不会等待串口数据。枚举只返回 COM 端点，
/// 打开后仍须由上层确认 PCL HELLO。
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'pcl_transport.dart';

/// UART 传输错误。
class PclUartException implements Exception {
  /// 创建带有可读说明的 UART 错误。
  const PclUartException(this.message);

  /// 错误原因。
  final String message;

  @override
  String toString() => 'PclUartException: $message';
}

/// 使用 Windows COM 端口承载 PCL 字节流。
///
/// 实例每次只允许一个打开的端点。关闭后可以重新打开；重新打开后应重新
/// 验证 PCL HELLO。字节流在实例生命周期内保持稳定，打开前即可订阅。
class PclUartTransport implements PclTransport {
  /// 创建尚未连接的 UART 传输。
  PclUartTransport() : _incoming = StreamController<Uint8List>.broadcast();

  static const int _maxWriteLength = 64 * 1024;
  static const Duration _openTimeout = Duration(seconds: 10);
  static const Duration _closeTimeout = Duration(seconds: 4);

  final StreamController<Uint8List> _incoming;
  ReceivePort? _messages;
  ReceivePort? _workerErrors;
  ReceivePort? _workerExit;
  Isolate? _worker;
  SendPort? _commands;
  Completer<void>? _opening;
  Completer<void>? _closing;
  Completer<void>? _exited;
  final Map<int, Completer<void>> _writes = <int, Completer<void>>{};
  int _nextWriteId = 1;
  bool _isOpen = false;
  bool _writeInFlight = false;

  /// 当前连接收到的原始字节。
  @override
  Stream<Uint8List> get incoming => _incoming.stream;

  /// 枚举当前 Windows DOS 设备命名空间中的 COM 端口。
  ///
  /// 枚举不会打开或写入串口。COM 端口可能属于任意串口设备，返回结果不
  /// 表示它是 PCL 设备。
  @override
  Future<List<PclEndpoint>> enumerate() async {
    if (!Platform.isWindows) {
      throw UnsupportedError('PCL UART 目前只支持 Windows。');
    }
    return Isolate.run<List<PclEndpoint>>(_enumerateComPorts);
  }

  /// 在后台 isolate 打开 Windows COM 端口。
  ///
  /// 串口参数为 8 个数据位、无校验、一个停止位、无软硬件流控。DTR 与
  /// RTS 均关闭，避免主动拉高常用于复位的控制线。
  @override
  Future<void> open(PclEndpoint endpoint, {int baudRate = 921600}) async {
    if (!Platform.isWindows) {
      throw UnsupportedError('PCL UART 目前只支持 Windows。');
    }
    if (endpoint.transport != PclTransportKind.uart ||
        !_isComName(endpoint.id)) {
      throw ArgumentError.value(endpoint, 'endpoint', '不是有效的 UART COM 端点');
    }
    if (baudRate < 1 || baudRate > 4_000_000) {
      throw RangeError.range(baudRate, 1, 4_000_000, 'baudRate');
    }
    if (_worker != null || _opening != null) {
      throw StateError('UART 传输已经打开或正在打开。');
    }
    if (_incoming.isClosed) throw StateError('UART 传输已经释放。');

    final messages = ReceivePort();
    final workerErrors = ReceivePort();
    final workerExit = ReceivePort();
    final opening = Completer<void>();
    final exited = Completer<void>();
    _messages = messages;
    _workerErrors = workerErrors;
    _workerExit = workerExit;
    _opening = opening;
    _exited = exited;
    messages.listen(_onWorkerMessage);
    workerErrors.listen(_onWorkerError);
    workerExit.listen((_) => _onWorkerExit());

    try {
      _worker = await Isolate.spawn<List<Object?>>(
        _uartWorkerMain,
        <Object?>[messages.sendPort, endpoint.id.toUpperCase(), baudRate],
        onError: workerErrors.sendPort,
        onExit: workerExit.sendPort,
        errorsAreFatal: true,
        debugName: 'PCL UART ${endpoint.id.toUpperCase()}',
      );
      await opening.future.timeout(_openTimeout);
    } on Object {
      await _stopWorker(kill: true);
      rethrow;
    } finally {
      if (identical(_opening, opening)) _opening = null;
    }
  }

  /// 将一段字节写入串口，并等待后台 worker 确认全部写完。
  ///
  /// 为限制跨 isolate 的排队内存，单次最多接受 64 KiB，且同一实例不接受
  /// 并发写入。输入会先复制，调用方可在 Future 完成前安全复用原缓冲区。
  @override
  Future<void> write(Uint8List bytes) async {
    if (!_isOpen || _commands == null) {
      throw StateError('UART 尚未打开。');
    }
    if (bytes.isEmpty) return;
    if (bytes.length > _maxWriteLength) {
      throw ArgumentError.value(
        bytes.length,
        'bytes.length',
        '单次串口写入不能超过 $_maxWriteLength 字节',
      );
    }
    if (_writeInFlight) {
      throw StateError('上一笔 UART 写入尚未完成。');
    }

    _writeInFlight = true;
    final id = _nextWriteId++;
    final done = Completer<void>();
    _writes[id] = done;
    try {
      _commands!.send(<String, Object?>{
        'type': 'write',
        'id': id,
        'bytes': Uint8List.fromList(bytes),
      });
      await done.future;
    } finally {
      _writes.remove(id);
      _writeInFlight = false;
    }
  }

  /// 停止当前连接的 worker 并关闭串口，实例字节流保持可订阅。
  @override
  Future<void> close() async {
    final opening = _opening;
    if (opening != null && !opening.isCompleted) {
      try {
        await opening.future;
      } on Object {
        return;
      }
    }
    final commands = _commands;
    if (_worker == null || commands == null) {
      await _stopWorker(kill: false);
      return;
    }

    final closing = Completer<void>();
    _closing = closing;
    commands.send(const <String, Object?>{'type': 'close'});
    try {
      await Future.any<void>(<Future<void>>[
        closing.future,
        _exited!.future,
      ]).timeout(_closeTimeout);
    } on TimeoutException {
      // 终止 worker 会释放 isolate；worker 的同步读写均有超时上限。
    } finally {
      await _stopWorker(kill: true);
      _closing = null;
      _isOpen = false;
    }
  }

  void _onWorkerMessage(Object? message) {
    if (message is! Map<Object?, Object?>) return;
    switch (message['type']) {
      case 'opened':
        _commands = message['commands'] as SendPort;
        _isOpen = true;
        final opening = _opening;
        if (opening != null && !opening.isCompleted) opening.complete();
        break;
      case 'data':
        final bytes = message['bytes'];
        if (bytes is Uint8List && !_incoming.isClosed) _incoming.add(bytes);
        break;
      case 'writeComplete':
        final id = message['id'];
        if (id is int) {
          final done = _writes[id];
          if (done != null && !done.isCompleted) done.complete();
        }
        break;
      case 'error':
        _fail(PclUartException(message['message']?.toString() ?? 'UART 传输失败。'));
        break;
      case 'closed':
        _isOpen = false;
        final closing = _closing;
        if (closing != null && !closing.isCompleted) closing.complete();
        break;
    }
  }

  void _onWorkerError(Object? error) {
    final details = error is List<Object?> ? error : <Object?>[error];
    final message = details.isEmpty ? 'UART worker 异常退出。' : details.first;
    _fail(PclUartException('UART worker 异常：$message'));
  }

  void _onWorkerExit() {
    final exited = _exited;
    if (exited != null && !exited.isCompleted) exited.complete();
    if (_isOpen || (_opening != null && !_opening!.isCompleted)) {
      _fail(const PclUartException('UART worker 已意外退出。'));
    }
  }

  void _fail(PclUartException error) {
    _isOpen = false;
    _commands = null;
    final opening = _opening;
    if (opening != null && !opening.isCompleted) opening.completeError(error);
    for (final done in _writes.values) {
      if (!done.isCompleted) done.completeError(error);
    }
    if (!_incoming.isClosed) {
      _incoming.addError(error);
    }
    final closing = _closing;
    if (closing != null && !closing.isCompleted) closing.complete();
    unawaited(_stopWorker(kill: false));
  }

  Future<void> _stopWorker({required bool kill}) async {
    if (kill) _worker?.kill(priority: Isolate.immediate);
    _worker = null;
    _commands = null;
    _messages?.close();
    _workerErrors?.close();
    _workerExit?.close();
    _messages = null;
    _workerErrors = null;
    _workerExit = null;
    _isOpen = false;
    _writeInFlight = false;
    _writes.clear();
  }

  /// 释放实例的字节流；后续不得重新打开。
  Future<void> dispose() async {
    await close();
    if (!_incoming.isClosed) await _incoming.close();
  }
}

bool _isComName(String value) =>
    RegExp(r'^COM[1-9][0-9]*$', caseSensitive: false).hasMatch(value);

/// 在独立 isolate 中读取 Windows 当前公布的 COM 设备名。
List<PclEndpoint> _enumerateComPorts() {
  final api = _Win32SerialApi.instance;
  const initialCapacity = 4096;
  const maximumCapacity = 65536;
  var capacity = initialCapacity;
  final buffer = calloc<Uint16>(maximumCapacity);
  try {
    while (true) {
      final count = api.queryDosDevice(
        nullptr.cast<Uint16>(),
        buffer,
        capacity,
      );
      if (count != 0) {
        final names = <String>{};
        var start = 0;
        for (var i = 0; i < count; i++) {
          if (buffer[i] != 0) continue;
          if (i > start) {
            final name = String.fromCharCodes(
              (buffer + start).asTypedList(i - start),
            );
            if (_isComName(name)) names.add(name.toUpperCase());
          }
          start = i + 1;
        }
        final ports = names.toList()
          ..sort((a, b) {
            final aNumber = int.parse(a.substring(3));
            final bNumber = int.parse(b.substring(3));
            return aNumber.compareTo(bNumber);
          });
        return ports
            .map(
              (port) => PclEndpoint(
                id: port,
                name: port,
                transport: PclTransportKind.uart,
                description: 'Windows 串口',
              ),
            )
            .toList(growable: false);
      }

      final error = api.getLastError();
      if (error == _Win32SerialApi.errorInsufficientBuffer &&
          capacity < maximumCapacity) {
        capacity = (capacity * 2)
            .clamp(initialCapacity, maximumCapacity)
            .toInt();
        continue;
      }
      throw PclUartException('枚举 Windows COM 端口失败（错误码 $error）。');
    }
  } finally {
    calloc.free(buffer);
  }
}

/// 串口 worker 入口；所有阻塞式 Win32 调用都只在这个 isolate 中运行。
@pragma('vm:entry-point')
void _uartWorkerMain(List<Object?> arguments) {
  final parent = arguments[0] as SendPort;
  final portName = arguments[1] as String;
  final baudRate = arguments[2] as int;
  final commands = ReceivePort();
  final api = _Win32SerialApi.instance;
  Pointer<Void>? handle;
  Timer? readTimer;
  Pointer<Uint8>? readBuffer;
  Pointer<Uint32>? readCount;
  Pointer<Uint32>? commErrors;
  Pointer<_ComStat>? commStat;
  var stopped = false;

  void releaseNativeResources() {
    readTimer?.cancel();
    readTimer = null;
    if (handle != null) {
      api.closeHandle(handle!);
      handle = null;
    }
    if (readBuffer != null) calloc.free(readBuffer!);
    if (readCount != null) calloc.free(readCount!);
    if (commErrors != null) calloc.free(commErrors!);
    if (commStat != null) calloc.free(commStat!);
    readBuffer = null;
    readCount = null;
    commErrors = null;
    commStat = null;
  }

  void fatal(Object error) {
    if (stopped) return;
    stopped = true;
    releaseNativeResources();
    commands.close();
    parent.send(<String, Object?>{
      'type': 'error',
      'message': error.toString(),
    });
    Isolate.exit();
  }

  try {
    handle = api.open(portName, baudRate);
    readBuffer = calloc<Uint8>(_Win32SerialApi.readBufferSize);
    readCount = calloc<Uint32>();
    commErrors = calloc<Uint32>();
    commStat = calloc<_ComStat>();
    parent.send(<String, Object?>{
      'type': 'opened',
      'commands': commands.sendPort,
    });

    readTimer = Timer.periodic(const Duration(milliseconds: 1), (_) {
      if (stopped || handle == null) return;
      try {
        commErrors!.value = 0;
        commStat!.ref.flags = 0;
        commStat!.ref.inQueue = 0;
        commStat!.ref.outQueue = 0;
        if (api.clearCommError(handle!, commErrors!, commStat!) == 0) {
          throw api.lastError('读取串口状态');
        }
        if (commErrors!.value != 0) {
          throw PclUartException(
            '串口报告接收错误（状态 0x${commErrors!.value.toRadixString(16)}）。',
          );
        }
        readCount!.value = 0;
        if (api.readFile(
              handle!,
              readBuffer!.cast<Void>(),
              _Win32SerialApi.readBufferSize,
              readCount!,
              nullptr,
            ) ==
            0) {
          throw api.lastError('读取串口');
        }
        final count = readCount!.value;
        if (count > 0) {
          final bytes = Uint8List.fromList(readBuffer!.asTypedList(count));
          parent.send(<String, Object?>{'type': 'data', 'bytes': bytes});
        }
      } on Object catch (error) {
        fatal(error);
      }
    });

    commands.listen((Object? message) {
      if (stopped || message is! Map<Object?, Object?>) return;
      switch (message['type']) {
        case 'write':
          final id = message['id'];
          final bytes = message['bytes'];
          if (id is! int || bytes is! Uint8List || handle == null) {
            fatal(const PclUartException('收到无效的 UART 写入命令。'));
            return;
          }
          try {
            api.write(handle!, bytes);
            parent.send(<String, Object?>{'type': 'writeComplete', 'id': id});
          } on Object catch (error) {
            fatal(error);
          }
          break;
        case 'close':
          if (stopped) return;
          stopped = true;
          releaseNativeResources();
          parent.send(const <String, Object?>{'type': 'closed'});
          commands.close();
          Isolate.exit();
      }
    });
  } on Object catch (error) {
    releaseNativeResources();
    commands.close();
    parent.send(<String, Object?>{
      'type': 'error',
      'message': error.toString(),
    });
    Isolate.exit();
  }
}

final class _Dcb extends Struct {
  @Uint32()
  external int length;

  @Uint32()
  external int baudRate;

  @Uint32()
  external int flags;

  @Uint16()
  external int reserved;

  @Uint16()
  external int xonLimit;

  @Uint16()
  external int xoffLimit;

  @Uint8()
  external int byteSize;

  @Uint8()
  external int parity;

  @Uint8()
  external int stopBits;

  @Uint8()
  external int xonChar;

  @Uint8()
  external int xoffChar;

  @Uint8()
  external int errorChar;

  @Uint8()
  external int eofChar;

  @Uint8()
  external int eventChar;

  @Uint16()
  external int reserved1;
}

final class _CommTimeouts extends Struct {
  @Uint32()
  external int readInterval;

  @Uint32()
  external int readMultiplier;

  @Uint32()
  external int readConstant;

  @Uint32()
  external int writeMultiplier;

  @Uint32()
  external int writeConstant;
}

final class _ComStat extends Struct {
  @Uint32()
  external int flags;

  @Uint32()
  external int inQueue;

  @Uint32()
  external int outQueue;
}

typedef _QueryDosDeviceNative =
    Uint32 Function(Pointer<Uint16>, Pointer<Uint16>, Uint32);
typedef _QueryDosDeviceDart =
    int Function(Pointer<Uint16>, Pointer<Uint16>, int);
typedef _GetLastErrorNative = Uint32 Function();
typedef _GetLastErrorDart = int Function();
typedef _CreateFileNative =
    Pointer<Void> Function(
      Pointer<Uint16>,
      Uint32,
      Uint32,
      Pointer<Void>,
      Uint32,
      Uint32,
      Pointer<Void>,
    );
typedef _CreateFileDart =
    Pointer<Void> Function(
      Pointer<Uint16>,
      int,
      int,
      Pointer<Void>,
      int,
      int,
      Pointer<Void>,
    );
typedef _CommStateNative = Int32 Function(Pointer<Void>, Pointer<_Dcb>);
typedef _CommStateDart = int Function(Pointer<Void>, Pointer<_Dcb>);
typedef _SetTimeoutsNative =
    Int32 Function(Pointer<Void>, Pointer<_CommTimeouts>);
typedef _SetTimeoutsDart = int Function(Pointer<Void>, Pointer<_CommTimeouts>);
typedef _ReadWriteNative =
    Int32 Function(
      Pointer<Void>,
      Pointer<Void>,
      Uint32,
      Pointer<Uint32>,
      Pointer<Void>,
    );
typedef _ReadWriteDart =
    int Function(
      Pointer<Void>,
      Pointer<Void>,
      int,
      Pointer<Uint32>,
      Pointer<Void>,
    );
typedef _ClearCommErrorNative =
    Int32 Function(Pointer<Void>, Pointer<Uint32>, Pointer<_ComStat>);
typedef _ClearCommErrorDart =
    int Function(Pointer<Void>, Pointer<Uint32>, Pointer<_ComStat>);
typedef _CloseHandleNative = Int32 Function(Pointer<Void>);
typedef _CloseHandleDart = int Function(Pointer<Void>);

/// 本 isolate 中加载的一组 Windows 串口 API。
class _Win32SerialApi {
  _Win32SerialApi._() {
    final kernel32 = DynamicLibrary.open('kernel32.dll');
    queryDosDevice = kernel32
        .lookupFunction<_QueryDosDeviceNative, _QueryDosDeviceDart>(
          'QueryDosDeviceW',
        );
    getLastError = kernel32
        .lookupFunction<_GetLastErrorNative, _GetLastErrorDart>('GetLastError');
    createFile = kernel32.lookupFunction<_CreateFileNative, _CreateFileDart>(
      'CreateFileW',
    );
    getCommState = kernel32.lookupFunction<_CommStateNative, _CommStateDart>(
      'GetCommState',
    );
    setCommState = kernel32.lookupFunction<_CommStateNative, _CommStateDart>(
      'SetCommState',
    );
    setCommTimeouts = kernel32
        .lookupFunction<_SetTimeoutsNative, _SetTimeoutsDart>(
          'SetCommTimeouts',
        );
    readFile = kernel32.lookupFunction<_ReadWriteNative, _ReadWriteDart>(
      'ReadFile',
    );
    writeFile = kernel32.lookupFunction<_ReadWriteNative, _ReadWriteDart>(
      'WriteFile',
    );
    clearCommError = kernel32
        .lookupFunction<_ClearCommErrorNative, _ClearCommErrorDart>(
          'ClearCommError',
        );
    closeHandle = kernel32.lookupFunction<_CloseHandleNative, _CloseHandleDart>(
      'CloseHandle',
    );
  }

  static const int errorInsufficientBuffer = 122;
  static const int readBufferSize = 1024;
  static const int _genericRead = 0x80000000;
  static const int _genericWrite = 0x40000000;
  static const int _openExisting = 3;
  static const int _writeTimeoutMs = 250;
  static const int _writeDeadlineMs = 3000;

  static _Win32SerialApi? _cached;

  /// 获取当前 isolate 的 Windows API 绑定。
  static _Win32SerialApi get instance => _cached ??= _Win32SerialApi._();

  late final _QueryDosDeviceDart queryDosDevice;
  late final _GetLastErrorDart getLastError;
  late final _CreateFileDart createFile;
  late final _CommStateDart getCommState;
  late final _CommStateDart setCommState;
  late final _SetTimeoutsDart setCommTimeouts;
  late final _ReadWriteDart readFile;
  late final _ReadWriteDart writeFile;
  late final _ClearCommErrorDart clearCommError;
  late final _CloseHandleDart closeHandle;

  /// 打开并配置一个 COM 端点。
  Pointer<Void> open(String portName, int baudRate) {
    final path = '\\\\.\\$portName'.toNativeUtf16();
    final dcb = calloc<_Dcb>();
    final timeouts = calloc<_CommTimeouts>();
    Pointer<Void> handle = nullptr;
    try {
      handle = createFile(
        path.cast<Uint16>(),
        _genericRead | _genericWrite,
        0,
        nullptr,
        _openExisting,
        0,
        nullptr,
      );
      if (_isInvalidHandle(handle)) throw lastError('打开 $portName');

      dcb.ref.length = sizeOf<_Dcb>();
      if (getCommState(handle, dcb) == 0) throw lastError('读取 $portName 配置');
      dcb.ref.length = sizeOf<_Dcb>();
      dcb.ref.baudRate = baudRate;
      dcb.ref.flags = 0x0001; // 二进制模式；关闭奇偶校验及软硬件流控。
      dcb.ref.byteSize = 8;
      dcb.ref.parity = 0; // NOPARITY
      dcb.ref.stopBits = 0; // ONESTOPBIT
      dcb.ref.xonChar = 0x11;
      dcb.ref.xoffChar = 0x13;
      if (setCommState(handle, dcb) == 0) throw lastError('配置 $portName');

      timeouts.ref.readInterval = 0;
      timeouts.ref.readMultiplier = 0;
      timeouts.ref.readConstant = 20;
      timeouts.ref.writeMultiplier = 0;
      timeouts.ref.writeConstant = _writeTimeoutMs;
      if (setCommTimeouts(handle, timeouts) == 0) {
        throw lastError('设置 $portName 超时');
      }

      return handle;
    } on Object {
      if (!_isInvalidHandle(handle)) closeHandle(handle);
      rethrow;
    } finally {
      calloc.free(path);
      calloc.free(dcb);
      calloc.free(timeouts);
    }
  }

  /// 完整写出一段数据；每次 Win32 写入有 250 ms 超时，总时限 3 秒。
  void write(Pointer<Void> handle, Uint8List bytes) {
    final source = calloc<Uint8>(bytes.length);
    final written = calloc<Uint32>();
    final timer = Stopwatch()..start();
    try {
      source.asTypedList(bytes.length).setAll(0, bytes);
      var offset = 0;
      while (offset < bytes.length) {
        if (timer.elapsedMilliseconds >= _writeDeadlineMs) {
          throw const PclUartException('UART 写入超时。');
        }
        written.value = 0;
        final ok = writeFile(
          handle,
          (source + offset).cast<Void>(),
          bytes.length - offset,
          written,
          nullptr,
        );
        if (ok == 0) throw lastError('写入串口');
        if (written.value == 0) throw const PclUartException('UART 写入没有进展。');
        offset += written.value;
      }
    } finally {
      calloc.free(written);
      calloc.free(source);
    }
  }

  /// 生成附带 Win32 错误码的说明。
  PclUartException lastError(String operation) =>
      PclUartException('$operation失败（Windows 错误码 ${getLastError()}）。');

  static bool _isInvalidHandle(Pointer<Void> handle) =>
      handle == nullptr || handle.address == -1 || handle.address == 0xFFFFFFFF;
}
