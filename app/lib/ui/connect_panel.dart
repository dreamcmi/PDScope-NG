// connect_panel.dart — 未采集时中列上的连接面板
//
// 设计第 4 节：把连接面板放进**中列**，用户看到的仍是同一个骨架
// （左筛选 / 中数据 / 右详情），只是中间暂时是「还没有数据，先去连设备」。
// 不新增布局，就不会有第二套视觉语言。
//
// 空态构图直接沿用空工作区那一套：图标 42 → 13px 主文案 → 胶囊按钮 → 11.5px 说明。
import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/live_source.dart';
import '../core/pcl_live_source.dart';
import '../core/palette.dart';
import 'filter_panel.dart' show MiniSwitch;

class ConnectPanel extends StatelessWidget {
  const ConnectPanel({super.key, required this.doc});

  final CaptureDocument doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      color: p.bg2,
      child: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.symmetric(vertical: 20, horizontal: 18),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 640),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (doc.liveMessage != null)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 12),
                    child: Text(
                      doc.liveMessage!,
                      style: TextStyle(color: p.warn, fontSize: 12),
                    ),
                  ),
                switch (doc.liveState) {
                  LiveState.idle => _idle(context),
                  LiveState.scanning => _scanning(context),
                  LiveState.found => _devices(context),
                  LiveState.connecting => _devices(context),
                  LiveState.listening => _listening(context),
                  LiveState.recoverableError => _devices(context),
                  _ => _ready(context),
                },
              ],
            ),
          ),
        ),
      ),
    );
  }

  /* ── 未连接：整页空态 ─────────────────────────────────────── */

  Widget _idle(BuildContext context) {
    final p = PaletteScope.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(Icons.usb_rounded, size: 42, color: p.tx3),
        const SizedBox(height: 14),
        Text('这里还没有设备', style: TextStyle(fontSize: 13, color: p.tx2)),
        const SizedBox(height: 10),
        OutlinedButton.icon(
          onPressed: () => doc.liveEnumerate(),
          icon: const Icon(Icons.search, size: 16),
          label: const Text('查找设备'),
        ),
        const SizedBox(height: 18),
        Text(
          '接上分析仪后点上面的按钮。\n'
          '采到的报文会和抓包文件一样显示在表格与时间轴里 —— '
          '筛选、详情、导出、另存为全部照常，不需要换个地方看。',
          textAlign: TextAlign.center,
          style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.7),
        ),
      ],
    );
  }

  /* ── 枚举中：骨架 + 取消 ──────────────────────────────────── */

  Widget _scanning(BuildContext context) {
    final p = PaletteScope.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            SizedBox(
              width: 10,
              height: 10,
              child: CircularProgressIndicator(
                strokeWidth: 1.6,
                color: p.accent,
              ),
            ),
            const SizedBox(width: 7),
            Text('正在查找设备…', style: TextStyle(fontSize: 11.5, color: p.tx2)),
          ],
        ),
        const SizedBox(height: 12),
        for (var i = 0; i < 3; i++) ...[
          Container(
            height: 58,
            decoration: BoxDecoration(
              color: p.panel2,
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: p.line),
            ),
          ),
          const SizedBox(height: 8),
        ],
      ],
    );
  }

  /* ── 已发现 / 连接中：设备列表 ───────────────────────────── */

  Widget _devices(BuildContext context) {
    final p = PaletteScope.of(context);
    final busy = doc.liveState == LiveState.connecting;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _sectionTitle(p, '设备', '${doc.liveDevices.length}'),
        for (final d in doc.liveDevices)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: _deviceCard(context, p, d, busy: busy),
          ),
        const SizedBox(height: 8),
        Row(
          children: [
            TextButton(
              onPressed: busy || doc.liveBusy
                  ? null
                  : () => doc.liveEnumerate(),
              child: const Text('重新查找'),
            ),
          ],
        ),
      ],
    );
  }

  Widget _deviceCard(
    BuildContext context,
    Palette p,
    LiveDevice d, {
    required bool busy,
  }) {
    final selected = doc.liveDevice?.id == d.id;
    return Opacity(
      opacity: busy && !selected ? 0.5 : 1,
      child: Container(
        padding: const EdgeInsets.all(11),
        decoration: BoxDecoration(
          color: selected ? p.accentSoft : p.panel2,
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: selected ? p.accent : p.line),
        ),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Container(
              width: 34,
              height: 34,
              decoration: BoxDecoration(
                color: p.panel,
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: p.line),
              ),
              child: Icon(Icons.usb_rounded, size: 17, color: p.tx2),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    d.name,
                    style: TextStyle(
                      fontSize: 12.5,
                      fontWeight: FontWeight.w500,
                      color: p.tx,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    '${d.serial} · ${d.transport} · ${d.transportNote}',
                    style: p.mono.copyWith(fontSize: 11, color: p.tx3),
                  ),
                  const SizedBox(height: 6),
                  Wrap(
                    spacing: 5,
                    runSpacing: 5,
                    children: [for (final c in d.capabilities) _capBadge(p, c)],
                  ),
                ],
              ),
            ),
            const SizedBox(width: 10),
            if (busy && selected)
              SizedBox(
                width: 14,
                height: 14,
                child: CircularProgressIndicator(
                  strokeWidth: 1.6,
                  color: p.accent,
                ),
              )
            else
              Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  OutlinedButton(
                    onPressed: busy || doc.liveBusy
                        ? null
                        : () => doc.liveConnect(d),
                    child: const Text('连接'),
                  ),
                  if (doc.liveSource is PclLiveSource)
                    TextButton(
                      onPressed: doc.liveBusy
                          ? null
                          : () => doc.liveListenDevice(d, 3000),
                      child: const Text('零写监听'),
                    ),
                ],
              ),
          ],
        ),
      ),
    );
  }

  /// 能力徽章 —— 借详情 tag 的配方（tint 底 @14% + 同色边 @35%）。
  ///
  /// ⚠ 不可用时**必须带上原因**（tooltip + 下方说明）。
  ///   只置灰不解释，用户会以为是软件坏了。
  Widget _capBadge(Palette p, LiveCapability c) {
    final (fg, bg) = switch (c.state) {
      LiveCapState.yes => (p.ok, p.ok),
      LiveCapState.no => (p.tx3, p.line2),
      LiveCapState.unknown => (p.warn, p.warn),
    };
    final badge = Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: bg.withValues(alpha: c.state == LiveCapState.yes ? 0.14 : 1.0),
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: fg.withValues(alpha: 0.35)),
      ),
      child: Text(
        c.state == LiveCapState.yes
            ? c.label
            : '${c.label} · ${_stateWord(c.state)}',
        style: TextStyle(
          fontSize: 10.5,
          fontWeight: FontWeight.w600,
          color: fg,
        ),
      ),
    );
    return c.reason == null ? badge : Tooltip(message: c.reason!, child: badge);
  }

  static String _stateWord(LiveCapState s) => switch (s) {
    LiveCapState.yes => '可',
    LiveCapState.no => '不可用',
    LiveCapState.unknown => '未知',
  };

  /* ── 零写监听 ─────────────────────────────────────────────── */

  Widget _listening(BuildContext context) {
    final p = PaletteScope.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: p.panel,
            borderRadius: BorderRadius.circular(8),
            border: Border.all(color: p.line),
          ),
          child: Column(
            children: [
              Row(
                children: [
                  SizedBox(
                    width: 10,
                    height: 10,
                    child: CircularProgressIndicator(
                      strokeWidth: 1.6,
                      color: p.ctrl,
                    ),
                  ),
                  const SizedBox(width: 7),
                  Text(
                    '零写监听中',
                    style: TextStyle(
                      fontSize: 11.5,
                      fontWeight: FontWeight.w600,
                      color: p.ctrl,
                    ),
                  ),
                  const Spacer(),
                  Text(
                    '收到 ${doc.liveListenReports} 条',
                    style: p.mono.copyWith(fontSize: 11.5, color: p.tx2),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  '本次**不向设备写入任何字节** —— 只打开收一段时间。\n'
                  '设备一打开就异常时，这是唯一不会让情况变坏的第一步。',
                  style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.7),
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }

  /* ── 待机：设备信息 + 采集配置 ───────────────────────────── */

  Widget _ready(BuildContext context) {
    final p = PaletteScope.of(context);
    final d = doc.liveDevice;
    final o = doc.liveOptions;
    final mock = doc.liveSource is MockLiveSource;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _sectionTitle(p, '设备', null),
        Container(
          padding: const EdgeInsets.all(11),
          decoration: BoxDecoration(
            color: p.panel2,
            borderRadius: BorderRadius.circular(8),
            border: Border.all(color: p.accent),
          ),
          child: Row(
            children: [
              Container(
                width: 34,
                height: 34,
                decoration: BoxDecoration(
                  color: p.panel,
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: p.line),
                ),
                child: Icon(Icons.usb_rounded, size: 17, color: p.accent),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      d?.name ?? '未知设备',
                      style: TextStyle(
                        fontSize: 12.5,
                        fontWeight: FontWeight.w500,
                        color: p.tx,
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      '${d?.serial ?? ''} · ${d?.transport ?? ''} · ${d?.transportNote ?? ''}',
                      style: p.mono.copyWith(fontSize: 11, color: p.tx3),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),

        const SizedBox(height: 16),
        _sectionTitle(p, '采集配置', null),
        _switchRow(
          p,
          'PD 报文',
          o.pd,
          (v) => _setOpt(pd: v),
          enabled: mock && !doc.liveBusy,
          hint: mock ? null : '当前页面采集 PD 解码包，包含原始线上 CRC',
        ),
        if (mock)
          _switchRow(
            p,
            '母线电压 / 电流',
            o.analog,
            (v) => _setOpt(analog: v),
            hint: '时间轴的主副曲线靠它',
          ),
        if (mock)
          _switchRow(
            p,
            '高速采样流',
            o.highSpeed,
            (v) => _setOpt(highSpeed: v),
            enabled: _canHighSpeed(d),
            hint: _canHighSpeed(d) ? '需要先做一次设备认证' : _whyNoHighSpeed(d),
          ),
        if (!mock) ...[
          _switchRow(
            p,
            '母线电压',
            o.voltage,
            (v) => _setOpt(voltage: v, analog: true),
            enabled: !doc.liveBusy && d?.can('母线电压') == true,
            hint: d?.capOf('母线电压')?.reason,
          ),
          _switchRow(
            p,
            '母线电流',
            o.current,
            (v) => _setOpt(current: v, analog: true),
            enabled: !doc.liveBusy && d?.can('母线电流') == true,
            hint: d?.capOf('母线电流')?.reason,
          ),
          _switchRow(
            p,
            'GoodCRC 设备过滤',
            o.goodCrcFilter,
            (v) => _setOpt(goodCrcFilter: v),
            enabled: !doc.liveBusy && d?.can('GoodCRC 过滤') == true,
            hint: '关闭时保留设备上报的 GoodCRC，表格仍可单独筛选',
          ),
          Row(
            children: [
              Text('数据通道', style: TextStyle(fontSize: 11, color: p.tx2)),
              const SizedBox(width: 12),
              DropdownButton<int>(
                value: o.channelSelect,
                isDense: true,
                items: [
                  const DropdownMenuItem(value: 0, child: Text('自动')),
                  if (d?.can('CC 选择') == true) ...[
                    const DropdownMenuItem(value: 1, child: Text('CC1')),
                    const DropdownMenuItem(value: 2, child: Text('CC2')),
                  ],
                  if (d?.can('双 CC 接收') == true)
                    const DropdownMenuItem(value: 3, child: Text('CC1 + CC2')),
                ],
                onChanged: doc.liveBusy
                    ? null
                    : (value) => _setOpt(channelSelect: value),
              ),
            ],
          ),
        ],

        const SizedBox(height: 12),
        Row(
          children: [
            Text(
              mock ? '轮询节拍' : 'ADC 最短采样周期',
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w600,
                color: p.tx2,
                letterSpacing: 0.4,
              ),
            ),
            const SizedBox(width: 10),
            if (mock)
              _rateSegmented(p, o.pollMs, (v) => _setOpt(pollMs: v))
            else
              DropdownButton<int>(
                value: o.pollMs,
                isDense: true,
                items: _periods()
                    .map(
                      (period) => DropdownMenuItem(
                        value: period,
                        child: Text('$period ms'),
                      ),
                    )
                    .toList(),
                onChanged: doc.liveBusy || !(o.voltage || o.current)
                    ? null
                    : (value) => _setOpt(pollMs: value),
              ),
          ],
        ),

        const SizedBox(height: 18),
        Row(
          children: [
            const Spacer(),
            if (doc.live?.isEmpty == false)
              TextButton(
                onPressed: doc.liveBusy
                    ? null
                    : () {
                        doc.liveShowConfig = false;
                        doc.touch();
                      },
                child: const Text('返回报文'),
              ),
            TextButton(
              onPressed: doc.liveBusy ? null : () => doc.liveDisconnect(),
              child: const Text('断开设备'),
            ),
            OutlinedButton(
              onPressed: doc.liveBusy ? null : () => doc.liveListen(3000),
              child: const Text('零写监听'),
            ),
            const SizedBox(width: 10),
            FilledButton(
              onPressed: _canStart(o) && !doc.liveBusy
                  ? () => doc.liveStart()
                  : null,
              child: Text(doc.liveBusy ? '等待设备确认…' : '开始采集'),
            ),
          ],
        ),
        if (!_canStart(o)) ...[
          const SizedBox(height: 8),
          Text(
            '至少勾一路数据才能开始。',
            textAlign: TextAlign.right,
            style: TextStyle(fontSize: 11.5, color: p.tx3),
          ),
        ],

        // 演练入口：这两个状态在真机上很难随时复现，但界面必须被审查到。
        // 只在模拟设备源下出现，措辞也写明是模拟。
        if (mock) ...[
          const SizedBox(height: 20),
          Container(
            padding: const EdgeInsets.all(11),
            decoration: BoxDecoration(
              color: p.panel2,
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: p.line),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '模拟设备源 · 演练',
                  style: TextStyle(
                    fontSize: 11,
                    fontWeight: FontWeight.w600,
                    color: p.tx2,
                    letterSpacing: 0.4,
                  ),
                ),
                const SizedBox(height: 6),
                Text(
                  '真机上「主动停车」「设备掉线」不好随时复现，这里可以直接做出来，'
                  '用来核对那两个状态的颜色、文案与按钮。',
                  style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.6),
                ),
                const SizedBox(height: 10),
                Row(
                  children: [
                    OutlinedButton(
                      onPressed: () => doc.liveDrillPark(),
                      child: const Text('演练：主动停车'),
                    ),
                    const SizedBox(width: 8),
                    OutlinedButton(
                      onPressed: () => doc.liveDrillDropout(),
                      child: const Text('演练：设备掉线'),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ],
    );
  }

  List<int> _periods() {
    final source = doc.liveSource;
    final minimum = source is PclLiveSource
        ? source.hello?.minSamplePeriodMs ?? 1
        : 1;
    return {
      ...[20, 50, 100, 200, 1000].where((period) => period >= minimum),
      minimum.clamp(1, 1000),
      doc.liveOptions.pollMs,
    }.toList()..sort();
  }

  void _setOpt({
    bool? pd,
    bool? analog,
    bool? highSpeed,
    int? pollMs,
    bool? voltage,
    bool? current,
    bool? goodCrcFilter,
    int? channelSelect,
  }) {
    doc.liveOptions = LiveStartOptions(
      pd: pd ?? doc.liveOptions.pd,
      analog: analog ?? doc.liveOptions.analog,
      highSpeed: highSpeed ?? doc.liveOptions.highSpeed,
      pollMs: pollMs ?? doc.liveOptions.pollMs,
      voltage: voltage ?? doc.liveOptions.voltage,
      current: current ?? doc.liveOptions.current,
      goodCrcFilter: goodCrcFilter ?? doc.liveOptions.goodCrcFilter,
      channelSelect: channelSelect ?? doc.liveOptions.channelSelect,
    );
    doc.touch();
  }

  bool _canStart(LiveStartOptions o) => doc.liveSource is MockLiveSource
      ? o.pd || o.analog || o.highSpeed
      : o.pd && doc.liveDevice?.can('PD 报文') == true;

  bool _canHighSpeed(LiveDevice? d) {
    final c = d?.capOf('高速采样流');
    return c?.state == LiveCapState.yes;
  }

  /// 能力不可用时，把**原因**写在行下面 —— 不是简单置灰。
  String? _whyNoHighSpeed(LiveDevice? d) {
    final c = d?.capOf('高速采样流');
    if (c == null) return null;
    return c.reason ?? '此接口不支持';
  }

  Widget _switchRow(
    Palette p,
    String label,
    bool value,
    ValueChanged<bool> onChanged, {
    bool enabled = true,
    String? hint,
  }) {
    final fg = enabled ? p.tx2 : p.tx3;
    return InkWell(
      borderRadius: BorderRadius.circular(6),
      onTap: enabled ? () => onChanged(!value) : null,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(
                  child: Text(
                    label,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(fontSize: 11.5, color: fg),
                  ),
                ),
                const SizedBox(width: 8),
                // 复用筛选栏那个自绘开关 —— 绝不用 Material Switch（会越界压字）
                Opacity(
                  opacity: enabled ? 1 : 0.45,
                  child: MiniSwitch(value: enabled && value),
                ),
              ],
            ),
            if (hint != null)
              Padding(
                padding: const EdgeInsets.only(top: 2, right: 40),
                child: Text(
                  hint,
                  style: TextStyle(
                    fontSize: 11,
                    color: enabled ? p.tx3 : p.warn,
                    height: 1.5,
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _rateSegmented(Palette p, int value, ValueChanged<int> onChanged) {
    const rates = [20, 50, 100, 200];
    return Container(
      padding: const EdgeInsets.all(2),
      decoration: BoxDecoration(
        color: p.panel2,
        borderRadius: BorderRadius.circular(7),
        border: Border.all(color: p.line),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (final r in rates)
            InkWell(
              borderRadius: BorderRadius.circular(5),
              onTap: () => onChanged(r),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 3),
                decoration: BoxDecoration(
                  color: r == value ? p.accent : Colors.transparent,
                  borderRadius: BorderRadius.circular(5),
                ),
                child: Text(
                  '$r ms',
                  style: TextStyle(
                    fontSize: 11.5,
                    fontWeight: r == value ? FontWeight.w600 : FontWeight.w400,
                    color: r == value ? Colors.white : p.tx2,
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _sectionTitle(Palette p, String title, String? trailing) => Padding(
    padding: const EdgeInsets.only(bottom: 6),
    child: Row(
      children: [
        Text(
          title,
          style: TextStyle(
            fontSize: 11,
            fontWeight: FontWeight.w600,
            color: p.tx2,
            letterSpacing: 0.4,
          ),
        ),
        const Spacer(),
        if (trailing != null)
          Text(trailing, style: TextStyle(fontSize: 10.5, color: p.tx3)),
      ],
    ),
  );
}
