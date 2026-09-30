// ffi.dart — C ABI 的原始绑定
//
// 对应 core/include/pdscope/pdscope.h。这一层只做「把 C 签名翻成 Dart 签名」，
// 不掺业务逻辑；调用它的地方见 engine.dart（跑在工作 isolate 上）。
//
// ignore_for_file: library_private_types_in_public_api
// 上面那条是有意为之：函数指针字段的 typedef 保持私有，是为了不把 C 的原始签名
// 扩散成对外 API；对外可用的是本文件末尾那几个封装（callBufAsBytes / jsonOf）与
// engine.dart 里的语义方法。
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// 库分配的字节块。`data == nullptr` 表示空结果（合法）。
final class PdscopeBuf extends Struct {
  external Pointer<Uint8> data;

  /// `size_t`。用 `@UintPtr()` 而不是默认的 IntPtr：32 位上也才对得上。
  @UintPtr()
  external int len;
}

/// 解码选项。
final class PdscopeDecodeOpts extends Struct {
  @Int32()
  external int channel; // <0 = 让核心自动挑通道

  /// `double` 在结构体里**必须显式标注**，否则 Dart 不知道它是 8 字节还是 4 字节。
  @Double()
  external double sampleRateOverride; // >0 时强制（Hz）
  @Int32()
  external int metadataOnly;
  @Int32()
  external int reserved;
}

/// 导出选项。
final class PdscopeExportOpts extends Struct {
  @Uint64()
  external int limit; // 0 = 全部
  @Int32()
  external int bom;
  @Int32()
  external int reserved;
}

/// 解码进度快照。
final class PdscopeProgress extends Struct {
  @Int32()
  external int phase; // 0=空闲 1=读容器 2=解码
  @Int32()
  external int channel;
  @Uint64()
  external int done;
  @Uint64()
  external int total;
  @Uint64()
  external int packets;
}

/* ── 函数签名 ───────────────────────────────────────────────────────── */

typedef _OpenBytesNative =
    Pointer<Void> Function(
      Pointer<Uint8>,
      IntPtr,
      Pointer<Utf8>,
      Pointer<Pointer<Utf8>>,
    );
typedef _OpenBytesDart =
    Pointer<Void> Function(
      Pointer<Uint8>,
      int,
      Pointer<Utf8>,
      Pointer<Pointer<Utf8>>,
    );

typedef _OpenFileNative =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Pointer<Utf8>>);
typedef _OpenFileDart =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Pointer<Utf8>>);

typedef _CloseNative = Void Function(Pointer<Void>);
typedef _CloseDart = void Function(Pointer<Void>);

typedef _BufOutNative = Int32 Function(Pointer<Void>, Pointer<PdscopeBuf>);
typedef _BufOutDart = int Function(Pointer<Void>, Pointer<PdscopeBuf>);

typedef _DecodeNative =
    Int32 Function(
      Pointer<Void>,
      Pointer<PdscopeDecodeOpts>,
      Pointer<PdscopeBuf>,
    );
typedef _DecodeDart =
    int Function(
      Pointer<Void>,
      Pointer<PdscopeDecodeOpts>,
      Pointer<PdscopeBuf>,
    );

typedef _CancelNative = Void Function(Pointer<Void>);
typedef _CancelDart = void Function(Pointer<Void>);

typedef _ProgressNative =
    Int32 Function(Pointer<Void>, Pointer<PdscopeProgress>);
typedef _ProgressDart = int Function(Pointer<Void>, Pointer<PdscopeProgress>);

typedef _SetFilterNative =
    Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Pointer<Utf8>>);
typedef _SetFilterDart =
    int Function(Pointer<Void>, Pointer<Utf8>, Pointer<Pointer<Utf8>>);

typedef _CountNative = Int32 Function(Pointer<Void>, Pointer<Uint64>);
typedef _CountDart = int Function(Pointer<Void>, Pointer<Uint64>);

typedef _PageNative =
    Int32 Function(Pointer<Void>, Uint64, Uint32, Pointer<PdscopeBuf>);
typedef _PageDart = int Function(Pointer<Void>, int, int, Pointer<PdscopeBuf>);

typedef _DetailNative =
    Int32 Function(Pointer<Void>, Uint64, Pointer<PdscopeBuf>);
typedef _DetailDart = int Function(Pointer<Void>, int, Pointer<PdscopeBuf>);

typedef _BufFreeNative = Void Function(Pointer<PdscopeBuf>);
typedef _BufFreeDart = void Function(Pointer<PdscopeBuf>);

typedef _StrFreeNative = Void Function(Pointer<Utf8>);
typedef _StrFreeDart = void Function(Pointer<Utf8>);

typedef _WaveNative =
    Int32 Function(
      Pointer<Void>,
      Int32,
      Uint64,
      Uint64,
      Uint32,
      Pointer<PdscopeBuf>,
    );
typedef _WaveDart =
    int Function(Pointer<Void>, int, int, int, int, Pointer<PdscopeBuf>);

typedef _BusNative = Int32 Function(Pointer<Void>, Uint32, Pointer<PdscopeBuf>);
typedef _BusDart = int Function(Pointer<Void>, int, Pointer<PdscopeBuf>);

typedef _MarksNative = Int32 Function(Pointer<Void>, Pointer<PdscopeBuf>);
typedef _MarksDart = int Function(Pointer<Void>, Pointer<PdscopeBuf>);

typedef _ExportNative =
    Int32 Function(
      Pointer<Void>,
      Pointer<PdscopeExportOpts>,
      Pointer<PdscopeBuf>,
    );
typedef _ExportDart =
    int Function(
      Pointer<Void>,
      Pointer<PdscopeExportOpts>,
      Pointer<PdscopeBuf>,
    );

typedef _DefNameNative = Int32 Function(Pointer<Void>, Pointer<Pointer<Utf8>>);
typedef _DefNameDart = int Function(Pointer<Void>, Pointer<Pointer<Utf8>>);

typedef _U32Native = Uint32 Function();
typedef _U32Dart = int Function();

typedef _CStrNative = Pointer<Utf8> Function();
typedef _CStrDart = Pointer<Utf8> Function();

typedef _StatusNameNative = Pointer<Utf8> Function(Int32);
typedef _StatusNameDart = Pointer<Utf8> Function(int);

/// 状态码（与 pdscope.h 的枚举一一对应）。
class Status {
  static const int ok = 0;
  static const int argument = -1;
  static const int io = -2;
  static const int format = -3;
  static const int unsupported = -4;
  static const int cancelled = -5;
  static const int state = -6;
  static const int memory = -7;
  static const int internal = -8;
}

/// 一次 FFI 调用失败。
class PdscopeException implements Exception {
  PdscopeException(this.message, [this.status = Status.internal]);

  final String message;
  final int status;

  bool get isCancelled => status == Status.cancelled;

  @override
  String toString() => message;
}

/// 动态库句柄 + 全部函数指针。
class PdscopeBindings {
  PdscopeBindings._(this._lib, this.libraryPath) {
    abiVersion = _lib.lookupFunction<_U32Native, _U32Dart>(
      'pdscope_abi_version',
    );
    version = _lib.lookupFunction<_CStrNative, _CStrDart>('pdscope_version');
    statusName = _lib.lookupFunction<_StatusNameNative, _StatusNameDart>(
      'pdscope_status_name',
    );
    bufFree = _lib.lookupFunction<_BufFreeNative, _BufFreeDart>(
      'pdscope_buf_free',
    );
    strFree = _lib.lookupFunction<_StrFreeNative, _StrFreeDart>(
      'pdscope_str_free',
    );
    openBytes = _lib.lookupFunction<_OpenBytesNative, _OpenBytesDart>(
      'pdscope_open_bytes',
    );
    openFile = _lib.lookupFunction<_OpenFileNative, _OpenFileDart>(
      'pdscope_open_file',
    );
    close = _lib.lookupFunction<_CloseNative, _CloseDart>('pdscope_close');
    metadata = _lib.lookupFunction<_BufOutNative, _BufOutDart>(
      'pdscope_metadata',
    );
    decode = _lib.lookupFunction<_DecodeNative, _DecodeDart>('pdscope_decode');
    cancel = _lib.lookupFunction<_CancelNative, _CancelDart>('pdscope_cancel');
    getProgress = _lib.lookupFunction<_ProgressNative, _ProgressDart>(
      'pdscope_get_progress',
    );
    setFilter = _lib.lookupFunction<_SetFilterNative, _SetFilterDart>(
      'pdscope_set_filter',
    );
    viewCount = _lib.lookupFunction<_CountNative, _CountDart>(
      'pdscope_view_count',
    );
    packetCount = _lib.lookupFunction<_CountNative, _CountDart>(
      'pdscope_packet_count',
    );
    queryPage = _lib.lookupFunction<_PageNative, _PageDart>(
      'pdscope_query_page',
    );
    packetDetail = _lib.lookupFunction<_DetailNative, _DetailDart>(
      'pdscope_packet_detail',
    );
    waveformRange = _lib.lookupFunction<_WaveNative, _WaveDart>(
      'pdscope_waveform_range',
    );
    busSeries = _lib.lookupFunction<_BusNative, _BusDart>('pdscope_bus_series');
    typeCounts = _lib.lookupFunction<_BufOutNative, _BufOutDart>(
      'pdscope_type_counts',
    );
    packetMarks = _lib.lookupFunction<_MarksNative, _MarksDart>(
      'pdscope_packet_marks',
    );
    exportCsv = _lib.lookupFunction<_ExportNative, _ExportDart>(
      'pdscope_export_csv',
    );
    exportJson = _lib.lookupFunction<_ExportNative, _ExportDart>(
      'pdscope_export_json',
    );
    defaultCsvName = _lib.lookupFunction<_DefNameNative, _DefNameDart>(
      'pdscope_default_csv_name',
    );
  }

  final DynamicLibrary _lib;
  final String libraryPath;

  late final _U32Dart abiVersion;
  late final _CStrDart version;
  late final _StatusNameDart statusName;
  late final _BufFreeDart bufFree;
  late final _StrFreeDart strFree;
  late final _OpenBytesDart openBytes;
  late final _OpenFileDart openFile;
  late final _CloseDart close;
  late final _BufOutDart metadata;
  late final _DecodeDart decode;
  late final _CancelDart cancel;
  late final _ProgressDart getProgress;
  late final _SetFilterDart setFilter;
  late final _CountDart viewCount;
  late final _CountDart packetCount;
  late final _PageDart queryPage;
  late final _DetailDart packetDetail;
  late final _WaveDart waveformRange;
  late final _BusDart busSeries;
  late final _BufOutDart typeCounts;
  late final _MarksDart packetMarks;
  late final _ExportDart exportCsv;
  late final _ExportDart exportJson;
  late final _DefNameDart defaultCsvName;

  /// 本进程内只加载一次（同一份 DLL 反复 open 会各拿一个句柄，浪费且易踩引用计数）。
  static PdscopeBindings? _instance;

  static PdscopeBindings instance() => _instance ??= _load();

  static PdscopeBindings _load() {
    final fileName = Platform.isWindows
        ? 'pdscope.dll'
        : Platform.isMacOS
        ? 'libpdscope.dylib'
        : 'libpdscope.so';

    final tried = <String>[];
    for (final dir in _searchDirs()) {
      final path = '$dir${Platform.pathSeparator}$fileName';
      final f = File(path);
      tried.add(path);
      if (!f.existsSync()) continue;
      return PdscopeBindings._(DynamicLibrary.open(path), path);
    }
    throw PdscopeException(
      '找不到核心动态库 $fileName。已尝试：\n${tried.map((t) => '  · $t').join('\n')}\n'
      '开发时先构建核心或设置 PDSCOPE_LIB_DIR，发行版应携带对应平台的核心库。',
      Status.io,
    );
  }

  /// 找库的顺序：显式环境变量 → 可执行文件同目录 → 开发期的构建输出目录。
  ///
  /// ⚠ 路径层级随 Flutter 版本会变（`build/windows/x64/runner/Release/` 这类），
  /// 所以不写死相对层级，而是**逐级往上找到含 CMakeLists.txt 的那一层**（仓库根），
  /// 再进 `build/out`。`flutter run` / `flutter test` 的工作目录是 `app/`，
  /// 它们看不到 runner 的产物目录，所以两条起点都要试。
  static List<String> _searchDirs() {
    final out = <String>[];
    final env = Platform.environment['PDSCOPE_LIB_DIR'];
    if (env != null && env.isNotEmpty) out.add(env);

    final exeDir = File(Platform.resolvedExecutable).parent.path;
    out.add(exeDir);
    if (Platform.isMacOS) {
      out.add('${File(exeDir).parent.path}/Frameworks');
    } else if (Platform.isLinux) {
      out.add('$exeDir/lib');
    }
    _addRepoOut(out, exeDir);
    _addRepoOut(out, Directory.current.path);
    return out;
  }

  /// 从 [start] 往上找仓库根，把它的 `build/out` 加进候选。
  static void _addRepoOut(List<String> out, String start) {
    final sep = Platform.pathSeparator;
    var dir = Directory(start);
    for (var i = 0; i < 10; i++) {
      if (File(
        '${dir.path}${sep}core${sep}include${sep}pdscope${sep}pdscope.h',
      ).existsSync()) {
        for (final relative in [
          'build${sep}out',
          if (Platform.isMacOS) 'build${sep}macos-native${sep}out',
          if (Platform.isLinux) 'build${sep}linux-native${sep}out',
        ]) {
          final candidate = '${dir.path}$sep$relative';
          if (!out.contains(candidate)) out.add(candidate);
        }
        return;
      }
      final parent = dir.parent;
      if (parent.path == dir.path) return;
      dir = parent;
    }
  }

  /// 库版本字符串（静态存储，不需要释放）。界面「关于」用。
  String versionString() => version().toDartString();

  /* ── 便捷封装：负责内存的所有权与释放 ───────────────────────────── */

  /// 调一个「输出 pdscope_buf」的函数并把结果取成字符串。
  String _callBufAsString(int Function(Pointer<PdscopeBuf>) call) {
    final buf = calloc<PdscopeBuf>();
    try {
      final st = call(buf);
      if (st != Status.ok) {
        throw PdscopeException('调用失败（${statusName(st)}）', st);
      }
      final ref = buf.ref;
      if (ref.data == nullptr || ref.len == 0) return '';
      return utf8.decode(ref.data.asTypedList(ref.len));
    } finally {
      bufFree(buf);
      calloc.free(buf);
    }
  }

  /// 同上，但结果是字节（波形用）。
  Uint8List callBufAsBytes(int Function(Pointer<PdscopeBuf>) call) {
    final buf = calloc<PdscopeBuf>();
    try {
      final st = call(buf);
      if (st != Status.ok) {
        throw PdscopeException('调用失败（${statusName(st)}）', st);
      }
      final ref = buf.ref;
      if (ref.data == nullptr || ref.len == 0) return Uint8List(0);
      return Uint8List.fromList(ref.data.asTypedList(ref.len));
    } finally {
      bufFree(buf);
      calloc.free(buf);
    }
  }

  String jsonOf(Pointer<Void> session, _BufOutDart fn) =>
      _callBufAsString((b) => fn(session, b));

  /// 读取并释放库返回的错误文本。
  static String takeError(Pointer<Pointer<Utf8>> slot) {
    final p = slot.value;
    if (p == nullptr) return '';
    final msg = p.toDartString();
    PdscopeBindings.instance().strFree(p);
    return msg;
  }
}
