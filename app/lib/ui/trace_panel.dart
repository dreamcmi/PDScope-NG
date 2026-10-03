// trace_panel.dart — 原始通讯日志（底部区的第二模式）
//
// 设计第 9 节：它复用底部区域（原时间轴的位置），做成**双模式**而不是新开一块布局。
// 排障时这是唯一可信的原始证据：命令究竟发出去了哪些字节、设备回了什么。
//
// 环形保留最近 kLiveTraceMax 条：出问题一定发生在最近，而内存要有上界。
import 'package:flutter/material.dart';

import '../core/live_source.dart';
import '../core/palette.dart';

/// 底部区的模式切换。
///
/// 时间轴与通讯日志**共用同一个分段控件**（同尺寸、同配色），
/// 所以它单独抽出来，两边传同一个 widget —— 免得两处各写一份后慢慢走样。
class BottomModeSwitch extends StatelessWidget {
  const BottomModeSwitch({
    super.key,
    required this.showTrace,
    required this.onChanged,
  });

  final bool showTrace;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    // 尺寸照抄时间轴既有的 _seg：外壳 padding 1.5 / 圆角 6，项 padding 7×1.5 / 圆角 4 / 字号 10.5
    return Container(
      padding: const EdgeInsets.all(1.5),
      decoration: BoxDecoration(
        color: p.panel2,
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: p.line),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          _item(p, '模拟量轨迹', !showTrace, () => onChanged(false)),
          _item(p, '通讯日志', showTrace, () => onChanged(true)),
        ],
      ),
    );
  }

  /// 一段。
  ///
  /// ⚠ 这里**同时**挂 onTapDown 与 onTap，不是冗余：
  ///   这个控件放进时间轴头部时，onTap（抬手才触发）拿不到手势 ——
  ///   现象是画得出来、点了没反应，而同一个控件在通讯日志那一侧完全正常。
  ///   改成「按下即生效」就与放在哪儿无关了。两处都触发也不会重复切换：
  ///   回调设的是**绝对值**（showTrace = v），不是取反。
  Widget _item(Palette p, String label, bool on, VoidCallback onTap) => InkWell(
    borderRadius: BorderRadius.circular(4),
    onTap: onTap,
    child: GestureDetector(
      // opaque：整块区域都可点，而不是只有文字笔画上才算
      behavior: HitTestBehavior.opaque,
      onTapDown: (_) => onTap(),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 1.5),
        decoration: BoxDecoration(
          color: on ? p.accent : Colors.transparent,
          borderRadius: BorderRadius.circular(4),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 10.5,
            fontWeight: on ? FontWeight.w600 : FontWeight.w400,
            color: on ? Colors.white : p.tx2,
          ),
        ),
      ),
    ),
  );
}

class TracePanel extends StatelessWidget {
  const TracePanel({
    super.key,
    required this.trace,
    required this.modeSwitch,
    required this.onExport,
    required this.onClear,
  });

  final List<LiveTraceEntry> trace;
  final Widget modeSwitch;
  final VoidCallback onExport;
  final VoidCallback onClear;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      color: p.panel,
      child: Column(
        children: [
          // 与时间轴的头**同高同内边距**（28 / 10），两个模式切换时不会跳一下
          Container(
            height: 28,
            padding: const EdgeInsets.symmetric(horizontal: 10),
            child: Row(
              children: [
                modeSwitch,
                const Spacer(),
                Text(
                  '环形保留最近 ${trace.length} / $kLiveTraceMax 条',
                  style: TextStyle(fontSize: 10.5, color: p.tx3),
                ),
                const SizedBox(width: 12),
                _link(p, '导出日志', onExport),
                const SizedBox(width: 10),
                _link(p, '清空', onClear),
              ],
            ),
          ),
          Expanded(child: _table(p)),
        ],
      ),
    );
  }

  static Widget _link(Palette p, String label, VoidCallback onTap) => InkWell(
    onTap: onTap,
    child: Text(label, style: TextStyle(fontSize: 10.5, color: p.accent)),
  );

  Widget _table(Palette p) {
    if (trace.isEmpty) {
      return Center(
        child: Text(
          '还没有通讯记录',
          style: TextStyle(fontSize: 11.5, color: p.tx3),
        ),
      );
    }
    // 最新的在最下面 —— 与日志的直觉一致，也和「跟随」的方向一致。
    final rows = trace.reversed.toList();
    return Column(
      children: [
        Container(
          height: 24,
          padding: const EdgeInsets.symmetric(horizontal: 8),
          decoration: BoxDecoration(
            color: p.panel2,
            border: Border(bottom: BorderSide(color: p.line)),
          ),
          child: Row(
            children: [
              _h(p, '时间', 78),
              _h(p, '方向', 44),
              _h(p, '长度', 44),
              Expanded(child: _h(p, '字节', null)),
              _h(p, '说明', 230),
            ],
          ),
        ),
        Expanded(
          child: ListView.builder(
            itemCount: rows.length,
            itemExtent: 22,
            itemBuilder: (_, i) => _row(p, rows[i]),
          ),
        ),
      ],
    );
  }

  static Widget _h(Palette p, String t, double? w) {
    final child = Text(
      t,
      style: TextStyle(
        fontSize: 11,
        fontWeight: FontWeight.w600,
        color: p.tx3,
        letterSpacing: 0.4,
      ),
    );
    return w == null ? child : SizedBox(width: w, child: child);
  }

  Widget _row(Palette p, LiveTraceEntry e) {
    // TX 用 accent、RX 用 data —— 与时间轴主/副曲线同一套色，学一次就够。
    final dirColor = e.tx ? p.accent : p.data;
    final mono = p.mono.copyWith(fontSize: 11.5);
    final len = e.bytes.trim().isEmpty || e.bytes == '—'
        ? '—'
        : e.bytes.split(' ').where((s) => s.isNotEmpty).length.toString();

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8),
      decoration: BoxDecoration(
        border: Border(bottom: BorderSide(color: p.line2)),
      ),
      child: Row(
        children: [
          SizedBox(
            width: 78,
            child: Text(
              e.ms <= 0 ? '—' : (e.ms / 1000).toStringAsFixed(3),
              style: mono,
            ),
          ),
          SizedBox(
            width: 44,
            child: Text(
              e.tx ? 'TX' : 'RX',
              style: mono.copyWith(
                color: dirColor,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
          SizedBox(width: 44, child: Text(len, style: mono)),
          Expanded(
            child: Text(
              e.bytes,
              overflow: TextOverflow.ellipsis,
              style: mono,
            ),
          ),
          SizedBox(
            width: 230,
            child: Text(
              e.note,
              overflow: TextOverflow.ellipsis,
              style: mono.copyWith(color: p.tx3),
            ),
          ),
        ],
      ),
    );
  }
}
