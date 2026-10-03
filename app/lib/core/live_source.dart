// live_source.dart — 实时采集：设备源契约 + 状态机 + 模拟设备源
//
// 这一层只回答一个问题：**界面需要设备提供什么**。协议层还没定（设计第 11 节），
// 所以这里先立契约、再配一个模拟实现 —— 界面因此今天就能跑、能被审查，
// 真协议接入时只需再写一个 LiveSource 实现，界面与 LiveSession 都不用动。
//
// 与核心 JSON 的关系：模拟源产出的行与详情**逐字段对齐** core/src/session.cpp 的
// packetListItemJson / packetDetailJsonOf（也就是 models.dart 认的那套）。
// 这样做的好处是 PacketRow / PacketDetail / PacketMarks 全部原样复用 ——
// 界面不需要知道数据是文件来的还是设备来的。
import 'dart:async';
import 'dart:math' as math;

/// 一项能力的三种状态。
///
/// ⚠ 刻意是**三态**而不是布尔：设备源自己不知道时必须能说「未知」。
///   拿 false 冒充未知，界面就会把「不确定」显示成「做不到」——
///   这与核心那边「未记录 ≠ 通过」是同一条原则。
enum LiveCapState {
  yes('可'),
  no('不可'),
  unknown('未知');

  const LiveCapState(this.label);
  final String label;
}

/// 一项能力 + 不可用时的原因（原因要显示给用户，不能只置灰）。
class LiveCapability {
  const LiveCapability(this.label, this.state, [this.reason]);
  final String label;
  final LiveCapState state;

  /// 不可用/未知的原因。界面把它当 tooltip 与说明文字用。
  final String? reason;
}

/// 枚举出来的一台设备（或同一台设备的一个可用接口 —— 能力不同就算两台）。
class LiveDevice {
  const LiveDevice({
    required this.id,
    required this.name,
    required this.serial,
    required this.transport,
    required this.transportNote,
    required this.capabilities,
  });

  final String id;
  final String name;
  final String serial;

  /// 接口名（如 `接口 3 (HID)`）。
  final String transport;

  /// 接口的附带说明（免驱 / 需驱动）。
  final String transportNote;

  final List<LiveCapability> capabilities;

  bool can(String label) => capabilities.any(
    (c) => c.label == label && c.state == LiveCapState.yes,
  );

  LiveCapability? capOf(String label) {
    for (final c in capabilities) {
      if (c.label == label) return c;
    }
    return null;
  }
}

/// 实时采集的状态机（设计第 3 节，12 个状态）。
enum LiveState {
  idle('未连接'),
  scanning('正在查找设备'),
  found('已发现设备'),
  connecting('连接中'),
  ready('已连接'),
  listening('零写监听中'),
  capturing('采集中'),
  paused('已暂停'),
  stopped('已停止'),
  recoverableError('可恢复错误'),
  parked('已主动停车'),
  disconnected('设备已断开');

  const LiveState(this.label);
  final String label;

  /// 数据是否还在往里进。
  bool get isLive => this == LiveState.capturing;

  /// 出事了 —— 状态条要变色的那几个。
  bool get isBad => this == LiveState.parked || this == LiveState.disconnected;
  bool get isWarn => this == LiveState.paused || this == LiveState.recoverableError;
}

/// 采集期间一直在动的计数。全部来自设备源，界面只负责显示（原则①：不隐瞒）。
class LiveStats {
  int packets = 0;
  int samples = 0;
  double durationSec = 0;

  /// 当前速率（条/秒）。
  double rate = 0;

  /// 队列溢出丢掉的条数。**非 0 必须变色** —— 丢包会被误读成「设备没发」。
  int dropped = 0;

  /// I/O 空闲超时次数。**属正常现象**，用 warn 不用 bad。
  int ioTimeouts = 0;

  /// 设备明确拒绝的命令数。**非 0 即异常**。
  int rejects = 0;

  /// 已发出的命令条数。节拍本身就是设备健康的指标。
  int commands = 0;

  /// 因超出内存上限被裁掉的历史条数（模拟源才有，真源应尽量为 0）。
  int trimmed = 0;

  void reset() {
    packets = 0;
    samples = 0;
    durationSec = 0;
    rate = 0;
    dropped = 0;
    ioTimeouts = 0;
    rejects = 0;
    commands = 0;
    trimmed = 0;
  }
}

/// 一条原始通讯记录。
///
/// 排障时这是**唯一可信的原始证据**：命令究竟发出去了哪些字节、设备回了什么。
/// 环形保留最近 [kLiveTraceMax] 条 —— 出问题一定发生在最近，而内存要有上界。
class LiveTraceEntry {
  LiveTraceEntry(this.ms, this.tx, this.bytes, this.note);
  final double ms;
  final bool tx; // true = 上位机发出
  final String bytes;
  final String note;
}

const int kLiveTraceMax = 256;

/// 采集配置（连接面板上那几个勾与节拍）。
class LiveStartOptions {
  const LiveStartOptions({
    this.pd = true,
    this.analog = true,
    this.highSpeed = false,
    this.pollMs = 50,
  });
  final bool pd;
  final bool analog;
  final bool highSpeed;
  final int pollMs;
}

/* ────────────────────────── 事件 ────────────────────────── */

sealed class LiveEvent {
  const LiveEvent();
}

/// 新增了报文（行 JSON 与详情 JSON 同形于核心）。
class LiveRowsEvent extends LiveEvent {
  const LiveRowsEvent(this.rows, this.details);
  final List<Map<String, dynamic>> rows;

  /// 原始序号 → 详情 JSON。
  final Map<int, Map<String, dynamic>> details;
}

/// 新增了母线采样点。
class LiveBusEvent extends LiveEvent {
  const LiveBusEvent(this.t, this.vbus, this.ibus);
  final List<double> t;
  final List<double> vbus;
  final List<double> ibus;
}

/// 设备侧的计数上报。
///
/// 这几个数**只有设备源知道**（它在跟设备说话），所以由它报上来，
/// 界面只负责显示。报文数与采样数不在这里 —— 那是「收到了多少」，
/// 由 LiveSession 从实际存下的数据算，两者口径不会互相打架。
class LiveStatsEvent extends LiveEvent {
  const LiveStatsEvent({
    required this.durationSec,
    required this.commands,
    required this.ioTimeouts,
    required this.rejects,
    required this.dropped,
  });

  final double durationSec;
  final int commands;
  final int ioTimeouts;
  final int rejects;
  final int dropped;
}

class LiveTraceEvent extends LiveEvent {
  const LiveTraceEvent(this.entry);
  final LiveTraceEntry entry;
}

class LiveStateEvent extends LiveEvent {
  const LiveStateEvent(this.state, {this.message});
  final LiveState state;

  /// 停车原因 / 掉线原因 / 可恢复错误的说明。
  final String? message;
}

/* ────────────────────────── 契约 ────────────────────────── */

/// 设备源。界面只认这个接口。
abstract class LiveSource {
  /// 后端名，显示在顶栏 chip 与设备卡上（如 `hid` / `winusb` / `mock`）。
  String get backendName;

  /// 命令行/日志里用的短名。
  String get displayName;

  Stream<LiveEvent> get events;

  /// 枚举设备。失败抛 [LiveSourceError]。
  Future<List<LiveDevice>> enumerate();

  /// 打开设备（不开始采集）。
  Future<void> connect(LiveDevice device);

  /// 零写监听：**全程不向设备写任何字节**，只打开收一段时间。
  ///
  /// 存在的意义：设备一打开就异常时，这是唯一不会让情况变坏的第一步。
  /// @return 收到的报告条数
  Future<int> listen(int windowMs);

  Future<void> start(LiveStartOptions options);
  void pause();
  void resume();

  /// 停止采集。**收尾仍要发一次 Disconnect** —— 让设备结束会话、
  /// 屏幕从「抓包中」恢复回来。这条最该发出去。
  Future<void> stop();

  /// 断开设备（stop + 关闭句柄）。
  Future<void> disconnect();

  Future<void> dispose();
}

class LiveSourceError implements Exception {
  LiveSourceError(this.message, {this.fatal = false});
  final String message;

  /// true = 协议层异常，必须主动停车（原则③：不自动重连）。
  final bool fatal;

  @override
  String toString() => message;
}

/* ────────────────────────── 模拟设备源 ────────────────────────── */

/// 模拟设备源 —— **只为把界面跑起来**，不是协议实现。
///
/// 它按节拍吐一段脚本化的 PD 协商流程（Source_Capabilities → Request → Accept →
/// PS_RDY → 稳态 PPS/Alert），并支持两个演练入口：
///   · [drillPark]     —— 复现「主动停车」（协议层异常，工具主动停并断开）
///   · [drillDropout]  —— 复现「设备掉线」
/// 这两个状态在真实设备上很难随时复现，但界面必须被审查到。
class MockLiveSource implements LiveSource {
  MockLiveSource({this.seed = 7});

  final int seed;

  @override
  String get backendName => 'mock';

  @override
  String get displayName => '模拟设备';

  final _ctl = StreamController<LiveEvent>.broadcast();

  @override
  Stream<LiveEvent> get events => _ctl.stream;

  Timer? _timer;
  Timer? _clock;
  final _rng = math.Random(7);
  LiveStartOptions _opts = const LiveStartOptions();
  LiveState _state = LiveState.idle;
  LiveDevice? _device;

  /// 脚本化会话的游标（走到哪一步）。
  int _phase = 0;
  int _seq = 0;
  int _msgId = 0;
  int _ticks = 0;
  /// 时间基准：**由节拍数推出来**，不是每秒加一。
  /// 报文每 125 ms 一条，若时间只按秒走，同一秒里的 8 条会拿到同一个时间戳 ——
  /// 时间轴会挤成一根柱子、排序也失去意义。
  double _t = 0;
  static const double _tickSec = 0.125;
  double _vbus = 5.0;
  double _ibus = 0.0;
  double _temp = 38.0;

  // 设备上报的目标档位（脚本用）
  double _targetV = 5.0;
  double _targetI = 0.0;

  // 设备侧计数：节拍、空闲超时、被拒绝的命令、队列溢出。
  // 它们必须真的动起来 —— 否则状态条上那三个 chip 永远是 0，没法审查配色。
  int _commands = 0;
  int _ioTimeouts = 0;
  int _rejects = 0;
  int _dropped = 0;

  @override
  Future<List<LiveDevice>> enumerate() async {
    _emitState(LiveState.scanning);
    await Future<void>.delayed(const Duration(milliseconds: 800));
    final list = <LiveDevice>[
      const LiveDevice(
        id: 'mock-if3',
        name: 'POWER-Z KM003C',
        serial: 'SN 3A17F204',
        transport: '接口 3 (HID)',
        transportNote: '免驱',
        capabilities: [
          LiveCapability('PD 报文', LiveCapState.yes),
          LiveCapability('母线模拟量', LiveCapState.yes),
          LiveCapability(
            '高速采样流',
            LiveCapState.no,
            '该接口不支持设备认证，选了也采不到。换成接口 0 才能勾。',
          ),
        ],
      ),
      const LiveDevice(
        id: 'mock-if0',
        name: 'POWER-Z KM003C',
        serial: 'SN 3A17F204',
        transport: '接口 0 (WinUSB)',
        transportNote: '需驱动',
        capabilities: [
          LiveCapability('PD 报文', LiveCapState.yes),
          LiveCapability('母线模拟量', LiveCapState.yes),
          LiveCapability('高速采样流', LiveCapState.yes, '需要先做一次设备认证'),
        ],
      ),
    ];
    _emitState(LiveState.found);
    return list;
  }

  @override
  Future<void> connect(LiveDevice device) async {
    _emitState(LiveState.connecting);
    await Future<void>.delayed(const Duration(milliseconds: 420));
    _device = device;
    _emitState(LiveState.ready);
  }

  @override
  Future<int> listen(int windowMs) async {
    _emitState(LiveState.listening);
    _pushTrace(true, '02 00 00 00', 'GetData · att 0x0001（零写监听不发这条，仅示意）');
    var reports = 0;
    final end = DateTime.now().add(Duration(milliseconds: windowMs));
    while (DateTime.now().isBefore(end)) {
      await Future<void>.delayed(const Duration(milliseconds: 180));
      reports++;
      _pushTrace(false, '90 01 2C 00 41 02 30 00 ...', 'PutData · 设备主动上报');
    }
    _emitState(LiveState.ready);
    return reports;
  }

  @override
  Future<void> start(LiveStartOptions options) async {
    _opts = options;
    _phase = 0;
    _seq = 0;
    _msgId = 0;
    _ticks = 0;
    _t = 0;
    _vbus = 5.0;
    _ibus = 0.0;
    _targetV = 5.0;
    _targetI = 0.0;
    _commands = 0;
    _ioTimeouts = 0;
    _rejects = 0;
    _dropped = 0;
    _emitState(LiveState.capturing);
    _pushTrace(true, '00 00 00 00', 'Connect');
    _pushTrace(false, '90 00 00 00', 'Connect 确认');

    // 1 秒的时钟：只负责让「时长 / 速率」这两个读数动起来
    _clock = Timer.periodic(const Duration(seconds: 1), (_) {
      if (_state != LiveState.capturing) return;
      _emitStats();
    });

    // 报文节拍：约 8 条/秒，模拟真实的「一请求一确认」成对流量
    _timer = Timer.periodic(const Duration(milliseconds: 125), (_) {
      if (_state != LiveState.capturing) return;
      _tick();
    });
  }

  @override
  void pause() {
    if (_state != LiveState.capturing) return;
    _emitState(LiveState.paused);
  }

  @override
  void resume() {
    if (_state != LiveState.paused) return;
    _emitState(LiveState.capturing);
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _clock?.cancel();
    _timer = null;
    _clock = null;
    if (_device != null) {
      _pushTrace(true, '01 00 00 00', 'Disconnect · 收尾仍要发');
    }
    _emitState(LiveState.stopped);
  }

  @override
  Future<void> disconnect() async {
    _timer?.cancel();
    _clock?.cancel();
    _timer = null;
    _clock = null;
    _device = null;
    _emitState(LiveState.idle);
  }

  @override
  Future<void> dispose() async {
    _timer?.cancel();
    _clock?.cancel();
    await _ctl.close();
  }

  /* ── 演练入口（仅模拟源有）────────────────────────────────── */

  /// 演练「主动停车」：协议层异常 → 工具主动停止发送并断开。
  void drillPark() {
    _timer?.cancel();
    _clock?.cancel();
    _timer = null;
    _clock = null;
    // 先落一条看不懂的报文，再停车 —— 与设计第 8 节的样机一致：
    // 无法解释的字节要原样保留，它恰恰是排障时最需要看的。
    _emitPacket(
      kind: 'Error',
      sop: 'SOP',
      msgType: '无法解析',
      role: '',
      msgId: null,
      objects: null,
      dataHex: '41 02 3F 00 1A 8C 44 D2 09 31 7E 05 B3 6A 18 C4 92 7B 40 E1 5D 28 A7 3F',
      summary: '属性长度 63 字节超出上限',
      warn: 1,
      detail: [
        {'key': 'Object', 'value': '容器观测'},
        {'key': '属性', 'value': '0x0002'},
        {'key': '声明长度', 'value': '63 字节（超出该属性上限）'},
        {'key': '结论', 'value': '无法解释 —— 原样保留，未丢弃'},
      ],
    );
    _pushTrace(false, '41 02 3F 00 1A 8C 44 D2 ...', '长度超出上限 → 主动停车');
    _pushTrace(true, '01 00 00 00', 'Disconnect · 停车后收尾仍要发');
    _emit(const LiveStateEvent(LiveState.parked,
        message: '设备返回了一条无法解释的响应：cmd 0x41 · att 0x0002 · 长度 63 字节，'
            '超出了该属性的最大长度。继续发送可能让设备侧状态更糟，因此由本工具主动停车。'));
  }

  /// 演练「设备掉线」。
  void drillDropout() {
    _timer?.cancel();
    _clock?.cancel();
    _timer = null;
    _clock = null;
    _pushTrace(false, '——', 'USB 读错误：设备已断开');
    _emit(const LiveStateEvent(LiveState.disconnected, message: '设备已断开（USB 读错误）。'));
  }

  /* ── 脚本化的报文生成 ────────────────────────────────────── */

  void _tick() {
    _seq++;
    _ticks++;
    _t = _ticks * _tickSec;
    _commands++;
    // 每隔一阵子插一次 I/O 空闲超时与一次 Reject：
    // 这两个计数在界面上必须是可见的，否则没法审查它们的配色与位置。
    if (_seq % 37 == 0) {
      _rejects++;
      _pushTrace(true, '02 00 00 00', 'GetData');
      _pushTrace(false, '—', 'Reject（设备明确说不认）');
    }
    if (_seq % 23 == 0) {
      _ioTimeouts++;
      _pushTrace(true, '02 00 00 00', 'GetData · att 0x0001');
    }

    final step = _phase % 10;
    switch (step) {
      case 0:
        _targetV = 5.0;
        _emitPacket(
          kind: 'Data',
          sop: 'SOP',
          msgType: 'Source_Capabilities',
          role: 'SRC',
          msgId: _nextId(),
          objects: 7,
          dataHex: '2A 84 91 12 D2 2C 01 2C ...',
          summary: 'Fixed 5V/3A · Fixed 9V/3A · Fixed 12V/3A · Fixed 15V/3A · Fixed 20V/5A',
          detail: [
            {'key': 'Object', 'value': 'Object 1 · Fixed Supply'},
            {'key': '电压', 'value': '5.00 V'},
            {'key': '最大电流', 'value': '3.00 A'},
            {'key': 'Object', 'value': 'Object 3 · Fixed Supply'},
            {'key': '电压', 'value': '12.00 V'},
            {'key': '最大电流', 'value': '3.00 A'},
          ],
        );
      case 1:
        _emitGoodCrc('SNK', 'Source_Capabilities');
      case 2:
        _targetV = 12.0;
        _targetI = 1.9;
        _emitPacket(
          kind: 'Data',
          sop: 'SOP',
          msgType: 'Request',
          role: 'SNK',
          msgId: _nextId(),
          objects: 1,
          dataHex: '2C 91 12 00 ...',
          summary: 'RDO → PDO#3 Fixed 12V · 工作电流 3.00 A · 电流上限 3.00 A',
          detail: [
            {'key': 'Object', 'value': 'Object 1 · Request'},
            {'key': '目标', 'value': 'PDO #3（Fixed 12.00 V）'},
            {'key': '工作电流', 'value': '3.00 A'},
            {'key': '电流上限', 'value': '3.00 A'},
          ],
        );
      case 3:
        _emitGoodCrc('SRC', 'Request');
      case 4:
        _emitPacket(
          kind: 'Control',
          sop: 'SOP',
          msgType: 'Accept',
          role: 'SRC',
          msgId: _nextId(),
          summary: '接受请求',
          detail: [
            {'key': 'Object', 'value': '控制报文'},
            {'key': '含义', 'value': '接受对端请求（第 #3 个 PDO）'},
          ],
        );
      case 5:
        _emitGoodCrc('SNK', 'Accept');
      case 6:
        _emitPacket(
          kind: 'Control',
          sop: 'SOP',
          msgType: 'PS_RDY',
          role: 'SRC',
          msgId: _nextId(),
          summary: '电源就绪',
          detail: [
            {'key': 'Object', 'value': '控制报文'},
            {'key': '含义', 'value': '电源已切到新档位'},
          ],
        );
      case 7:
        _emitGoodCrc('SNK', 'PS_RDY');
      case 8:
        _emitPacket(
          kind: 'Extended',
          sop: 'SOP',
          msgType: 'PPS_Status',
          role: 'SRC',
          msgId: _nextId(),
          objects: 2,
          dataHex: 'E0 03 00 00 ...',
          summary: '输出 ${_vbus.toStringAsFixed(2)} V · ${_ibus.toStringAsFixed(2)} A'
              ' · 温度 ${_temp.toStringAsFixed(1)} ℃',
          detail: [
            {'key': 'Object', 'value': '扩展头'},
            {'key': '数据长度', 'value': '2 字节'},
            {'key': 'Object', 'value': 'PPS Status'},
            {'key': '输出电压', 'value': '12.01 V'},
            {'key': '输出电流', 'value': '1.92 A'},
            {'key': '温度', 'value': '41.5 ℃'},
          ],
        );
      default:
        // 偶尔来一条告警，让「异常」这条路径在界面上也能被看到
        if (_phase % 30 == 29) {
          _emitPacket(
            kind: 'Data',
            sop: 'SOP',
            msgType: 'Alert',
            role: 'SNK',
            msgId: _nextId(),
            objects: 1,
            dataHex: '07 00 00 00',
            summary: '电池状态告警',
            warn: 1,
            detail: [
              {'key': 'Object', 'value': 'Object 1 · Alert'},
              {'key': 'Alert Type', 'value': '0b00000111 电池状态'},
              {'key': 'Fixed Batteries', 'value': '0b0001'},
            ],
          );
        } else {
          _emitGoodCrc('SNK', 'PPS_Status');
        }
    }
    _phase++;

    // 母线模拟量：往目标档位缓动，带一点噪声
    if (_opts.analog) {
      _vbus += (_targetV - _vbus) * 0.25;
      _ibus += (_targetI - _ibus) * 0.25;
      _temp += (_rng.nextDouble() - 0.5) * 0.2;
      _emit(LiveBusEvent([_t], [_vbus + (_rng.nextDouble() - 0.5) * 0.02],
          [_ibus + (_rng.nextDouble() - 0.5) * 0.01]));
    }
    _emitStats();
  }

  void _emitStats() {
    _emit(LiveStatsEvent(
      durationSec: _t,
      commands: _commands,
      ioTimeouts: _ioTimeouts,
      rejects: _rejects,
      dropped: _dropped,
    ));
  }

  int _nextId() {
    final v = _msgId;
    _msgId = (_msgId + 1) & 0x07;
    return v;
  }

  void _emitGoodCrc(String role, String ackOf) {
    _emitPacket(
      kind: 'Control',
      sop: 'SOP',
      msgType: 'GoodCRC',
      role: role,
      msgId: _msgId,
      summary: '确认 ${ackOf == 'Source_Capabilities' ? '' : '#'}$ackOf',
      detail: [
        {'key': 'Object', 'value': '控制报文'},
        {'key': '含义', 'value': '确认上一条 $ackOf'},
      ],
      ackType: ackOf,
    );
  }

  void _emitPacket({
    required String kind,
    required String sop,
    required String msgType,
    required String role,
    required int? msgId,
    int? objects,
    String dataHex = '',
    String summary = '',
    int warn = 0,
    List<Map<String, String>> detail = const [],
    String? ackType,
  }) {
    final index = _seq;
    final ms = _t * 1000;
    final sample = (ms).round();
    final row = <String, dynamic>{
      'index': index,
      'seq': index,
      'channel': 0,
      'sop': sop,
      'msgType': msgType,
      'role': role,
      'kind': kind,
      'msgKind': switch (kind) {
        'Control' => 'control',
        'Data' => 'data',
        'Extended' => 'ext',
        'VDM' => 'vdm',
        _ => 'custom',
      },
      'msgId': msgId,
      'objects': objects,
      'timeMs': ms,
      'elapsed': _elapsed(ms),
      'startSample': sample,
      'endSample': sample + 180,
      'durationUs': 180.0,
      'vbus': _vbus,
      'ibus': _ibus,
      'dataHex': dataHex,
      // ⚠ 设备不保存 CRC —— 与 POWER-Z 路径同口径：留 none，界面留空。
      //   绝不能因为「我们算了一遍对上了」就写 ok。
      'crc': 'none',
      'summary': summary,
      'warn': warn,
      'ackOf': null,
      'ackType': ackType,
    };
    final det = <String, dynamic>{
      ...row,
      'text': summary,
      'header': 0x1042,
      'extHeader': kind == 'Extended' ? 0x0002 : null,
      'rev': 3,
      'revText': '3.1',
      'msgTypeRaw': 0,
      'link': sop.startsWith('SOP') ? 'port' : 'cable',
      'category': kind,
      'eop': true,
      // 分析仪/设备给的是逻辑字节，不是从波形解出来的 —— 与离线路径一致
      'synthetic': true,
      'roleInferred': role.isEmpty,
      'crcValue': null,
      'crcCalc': 0,
      'details': detail,
      'warnings': warn > 0
          ? [
              {'long': '这一条带有告警位', 'short': '告警'},
            ]
          : const [],
      'dataWords': const <int>[],
      'dataBytes': const <int>[],
    };
    _emit(LiveRowsEvent([row], {index: det}));
  }

  void _pushTrace(bool tx, String bytes, String note) {
    _emit(LiveTraceEvent(LiveTraceEntry(_t * 1000, tx, bytes, note)));
  }

  void _emitState(LiveState s) {
    _state = s;
    _emit(LiveStateEvent(s));
  }

  void _emit(LiveEvent e) {
    if (!_ctl.isClosed) _ctl.add(e);
  }

  /// 与核心 `csvClock` 同形的相对时间（秒，三位小数）。
  static String _elapsed(double ms) => '${(ms / 1000).toStringAsFixed(3)} s';
}
