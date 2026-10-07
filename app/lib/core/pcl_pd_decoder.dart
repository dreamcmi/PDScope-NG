import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'ffi.dart';

/// 一条交给核心解码器的 PD 记录；字节包含原始线上 CRC 或截短前缀。
class PclDecodeInput {
  const PclDecodeInput({
    required this.bytes,
    required this.sop,
    required this.cc,
    required this.flags,
    required this.elapsedTicks,
    required this.timebaseHz,
    required this.index,
  });
  final Uint8List bytes;
  final int sop;
  final int cc;
  final int flags;
  final int elapsedTicks;
  final int timebaseHz;
  final int index;

  Map<String, Object> _message() => {
    'bytes': bytes,
    'sop': sop,
    'cc': cc,
    'flags': flags,
    'ticks': elapsedTicks,
    'hz': timebaseHz,
    'index': index,
  };
}

/// 可替换的异步批量 PD 解码接口，便于无硬件测试。
abstract interface class PclPdDecoder {
  /// 新 START 清空跨报文协商状态。
  Future<void> reset();

  /// 同一批返回一一对应的 row/detail JSON，不修改输入字节。
  Future<List<Map<String, dynamic>>> decodeBatch(List<PclDecodeInput> records);

  /// 排空已提交操作并释放工作上下文。
  Future<void> close();
}

/// 后台 isolate 中调用现有 C++ PD 解码器，界面线程不执行 FFI 解码。
class PclNativePdDecoder implements PclPdDecoder {
  final _port = ReceivePort();
  final _ready = Completer<SendPort>();
  final Map<int, Completer<Object?>> _pending = {};
  late final StreamSubscription<Object?> _subscription;
  Future<Isolate>? _worker;
  Isolate? _isolate;
  SendPort? _commandPort;
  Future<void>? _closing;
  bool _exitExpected = false;
  int _nextId = 0;
  bool _closed = false;

  /// 延迟加载核心 DLL，不因枚举或只读监听启动解码工作线程。
  PclNativePdDecoder() {
    _ready.future.ignore();
    _subscription = _port.listen((message) {
      if (message == null) {
        if (!_exitExpected) _failWorker('PD 解码工作线程意外退出');
        return;
      }
      if (message is SendPort) {
        _commandPort = message;
        if (!_ready.isCompleted) _ready.complete(message);
        return;
      }
      if (message is List && message.length == 2) {
        _failWorker('PD 解码工作线程异常：${message[0]}');
        return;
      }
      if (message is! List || message.length != 3) return;
      final id = message[0] as int;
      if (id == -1) {
        _failWorker(message[2].toString());
        return;
      }
      final pending = _pending.remove(id);
      if (pending == null) return;
      if (message[1] == true) {
        pending.complete(message[2]);
      } else {
        pending.completeError(StateError(message[2].toString()));
      }
    });
  }

  /// 线程退出或初始化失败时唤醒所有等待者，避免关闭窗口无限等待。
  void _failWorker(String message) {
    final error = StateError(message);
    if (!_ready.isCompleted) _ready.completeError(error);
    for (final pending in _pending.values) {
      if (!pending.isCompleted) pending.completeError(error);
    }
    _pending.clear();
  }

  Future<Object?> _call(String command, [Object? body]) async {
    if (_closed) throw StateError('PD 解码器已经关闭');
    _worker ??= Isolate.spawn(
      _decoderMain,
      _port.sendPort,
      onError: _port.sendPort,
      onExit: _port.sendPort,
    );
    _isolate = await _worker;
    final worker = await _ready.future.timeout(const Duration(seconds: 5));
    final id = _nextId++;
    final result = Completer<Object?>();
    _pending[id] = result;
    worker.send([id, command, body]);
    try {
      return await result.future.timeout(const Duration(seconds: 15));
    } finally {
      _pending.remove(id);
    }
  }

  @override
  Future<void> reset() async {
    await _call('reset');
  }

  @override
  Future<List<Map<String, dynamic>>> decodeBatch(
    List<PclDecodeInput> records,
  ) async {
    final json = await _call(
      'decode',
      records.map((record) => record._message()).toList(),
    );
    return (jsonDecode(json as String) as List).cast<Map<String, dynamic>>();
  }

  @override
  Future<void> close() => _closing ??= _close();

  /// 正常关闭先让线程释放 C++ 句柄；初始化失败时直接回收通信端口。
  Future<void> _close() async {
    if (_closed) return;
    try {
      if (_worker != null) {
        try {
          _isolate = await _worker;
          await _ready.future.timeout(const Duration(seconds: 5));
        } catch (_) {
          // 初始化失败已经由 reset/decode 报告，没有创建可用句柄。
        }
        if (_commandPort != null) {
          _exitExpected = true;
          await _call('close');
        }
      }
    } finally {
      _closed = true;
      _isolate?.kill(priority: Isolate.immediate);
      await _subscription.cancel();
      _port.close();
      for (final pending in _pending.values) {
        pending.completeError(StateError('PD 解码器已经关闭'));
      }
      _pending.clear();
    }
  }
}

typedef _CreateNative = Pointer<Void> Function();
typedef _CreateDart = Pointer<Void> Function();
typedef _ResetNative = Int32 Function(Pointer<Void>);
typedef _ResetDart = int Function(Pointer<Void>);
typedef _CloseNative = Void Function(Pointer<Void>);
typedef _CloseDart = void Function(Pointer<Void>);
typedef _DecodeNative =
    Int32 Function(
      Pointer<Void>,
      Pointer<Uint8>,
      UintPtr,
      Uint8,
      Uint8,
      Uint8,
      Uint64,
      Uint32,
      Uint64,
      Pointer<PdscopeBuf>,
    );
typedef _DecodeDart =
    int Function(
      Pointer<Void>,
      Pointer<Uint8>,
      int,
      int,
      int,
      int,
      int,
      int,
      int,
      Pointer<PdscopeBuf>,
    );

/// 工作线程独占核心句柄；输入和库分配输出均在返回前释放。
void _decoderMain(SendPort owner) {
  final commands = ReceivePort();
  Pointer<Void> handle = nullptr;
  _CloseDart? close;
  try {
    final bindings = PdscopeBindings.instance();
    final library = DynamicLibrary.open(bindings.libraryPath);
    final create = library.lookupFunction<_CreateNative, _CreateDart>(
      'pdscope_live_pd_create',
    );
    final reset = library.lookupFunction<_ResetNative, _ResetDart>(
      'pdscope_live_pd_reset',
    );
    final decode = library.lookupFunction<_DecodeNative, _DecodeDart>(
      'pdscope_live_pd_decode',
    );
    close = library.lookupFunction<_CloseNative, _CloseDart>(
      'pdscope_live_pd_close',
    );
    handle = create();
    if (handle == nullptr) throw StateError('核心 PD 解码器分配失败');
    owner.send(commands.sendPort);
    commands.listen((message) {
      final request = message as List;
      final id = request[0] as int;
      try {
        switch (request[1]) {
          case 'reset':
            if (reset(handle) != 0) throw StateError('核心 PD 解码器复位失败');
            owner.send([id, true, null]);
          case 'decode':
            final packets = <Object>[];
            for (final value in request[2] as List) {
              final record = value as Map;
              final bytes = record['bytes'] as Uint8List;
              final input = bytes.isEmpty
                  ? nullptr.cast<Uint8>()
                  : calloc<Uint8>(bytes.length);
              final output = calloc<PdscopeBuf>();
              try {
                if (bytes.isNotEmpty) {
                  input.asTypedList(bytes.length).setAll(0, bytes);
                }
                final status = decode(
                  handle,
                  input,
                  bytes.length,
                  record['sop'] as int,
                  record['cc'] as int,
                  record['flags'] as int,
                  record['ticks'] as int,
                  record['hz'] as int,
                  record['index'] as int,
                  output,
                );
                if (status != 0) throw StateError('核心 PD 解码失败：$status');
                packets.add(
                  jsonDecode(
                    utf8.decode(output.ref.data.asTypedList(output.ref.len)),
                  ),
                );
              } finally {
                bindings.bufFree(output);
                calloc.free(output);
                if (input != nullptr) calloc.free(input);
              }
            }
            owner.send([id, true, jsonEncode(packets)]);
          case 'close':
            close!(handle);
            handle = nullptr;
            owner.send([id, true, null]);
            commands.close();
          default:
            throw StateError('未知 PD 解码命令');
        }
      } catch (error) {
        owner.send([id, false, error.toString()]);
      }
    });
  } catch (error) {
    if (handle != nullptr) close?.call(handle);
    commands.close();
    owner.send([-1, false, '无法加载实时 PD 解码接口，请重新构建核心：$error']);
  }
}
