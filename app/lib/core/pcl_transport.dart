import 'dart:typed_data';

/// PCL 设备可使用的字节传输类型。
enum PclTransportKind { uart, hid, bulk, cdc }

/// 可供用户选择的传输端点。
///
/// 枚举到端点只表示系统发现了对应接口，不代表设备已经通过 PCL HELLO
/// 确认。上层应在打开端点后等待并验证 HELLO。
class PclEndpoint {
  /// 创建一个传输端点。
  const PclEndpoint({
    required this.id,
    required this.name,
    required this.transport,
    this.description,
  });

  /// 供 [PclTransport.open] 使用的稳定接口标识。
  final String id;

  /// 面向用户显示的接口名称。
  final String name;

  /// 接口采用的传输类型。
  final PclTransportKind transport;

  /// 操作系统提供的附加说明。
  final String? description;
}

/// PCL 上位机使用的异步字节传输接口。
///
/// 实现应将阻塞式设备 I/O 放在后台上下文中。传输层只搬运字节，设备身份
/// 和 PCL HELLO 校验由调用方负责。
abstract interface class PclTransport {
  /// 枚举当前可用的传输端点。
  Future<List<PclEndpoint>> enumerate();

  /// 打开 [endpoint]。UART 实现默认使用 921600、8N1。
  Future<void> open(PclEndpoint endpoint, {int baudRate = 921600});

  /// 从设备收到的原始字节流。
  ///
  /// 同一实例跨 close/open 保持此流稳定；调用者会在 open 前订阅。
  Stream<Uint8List> get incoming;

  /// 将 [bytes] 原样写入设备。
  ///
  /// 实现须在第一次异步等待前复制输入，完成时表示所有字节已写出。
  Future<void> write(Uint8List bytes);

  /// 关闭端点并释放后台 I/O 资源。
  Future<void> close();
}
