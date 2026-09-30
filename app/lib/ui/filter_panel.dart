// filter_panel.dart — 左侧筛选栏
//
// 「选择性屏蔽」的落点：方向、链路（PD 是 SOP / UFCS 是物理链路）、报文类别、
// 具体报文类型（多选 + 计数）、时间窗口、关键字，任意组合。
//
// ⚠ 候选值**跟着协议走**（`linkValues` / `catValues`）—— 拿 PD 的 SOP 列表去筛 UFCS
// 会把报文全筛没。这份判断只在 [filters.dart] 里写一次，界面这里只读结果。
import 'dart:async';

import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/filters.dart';
import '../core/formatting.dart';
import '../core/palette.dart';
import '../core/workspace.dart';

class FilterPanel extends StatelessWidget {
  const FilterPanel({super.key, required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      width: 236,
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(right: BorderSide(color: p.line)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _header(context, p),
          Expanded(
            child: SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(12, 6, 12, 16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  if (!doc.decoded)
                    Text(
                      doc.busy ? '正在读取…' : '等待解码',
                      style: TextStyle(fontSize: 11.5, color: p.tx3),
                    )
                  else ...[
                    _channels(p),
                    const _Gap(),
                    _multi(context, p, '数据方向', roleValues, doc.filters.roles),
                    const _Gap(),
                    _multi(
                      context,
                      p,
                      linkTitle(doc.protocol),
                      linkValues(doc.protocol),
                      doc.filters.sops,
                    ),
                    const _Gap(),
                    _multi(
                      context,
                      p,
                      '报文类别',
                      catValues(doc.protocol),
                      doc.filters.cats,
                    ),
                    const _Gap(),
                    _quickFilters(context, p),
                    const _Gap(),
                    _types(context, p),
                    const _Gap(),
                    _timeWindow(context, p),
                    const _Gap(),
                    _keywords(p),
                    const _Gap(),
                    OutlinedButton(
                      onPressed: () {
                        final previous = doc.filters;
                        doc.filters = FilterState.defaults(doc.protocol)
                          ..sortKey = previous.sortKey
                          ..sortAsc = previous.sortAsc;
                        _apply(null);
                      },
                      child: const Text('重置全部筛选'),
                    ),
                  ],
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _header(BuildContext context, Palette p) => Container(
    height: 34,
    padding: const EdgeInsets.symmetric(horizontal: 12),
    decoration: BoxDecoration(
      border: Border(bottom: BorderSide(color: p.line)),
    ),
    child: Row(
      children: [
        Text(
          '筛选',
          style: TextStyle(
            fontSize: 12.5,
            fontWeight: FontWeight.w600,
            color: p.tx,
          ),
        ),
        const Spacer(),
        Tooltip(
          message: '折叠筛选栏',
          child: InkWell(
            onTap: () => workspace.prefs.filtersCollapsed = true,
            borderRadius: BorderRadius.circular(5),
            child: Padding(
              padding: const EdgeInsets.all(3),
              child: Icon(Icons.chevron_left, size: 16, color: p.tx3),
            ),
          ),
        ),
      ],
    ),
  );

  Widget _channels(Palette p) {
    final channels = doc.meta?.channels ?? const [];
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionTitle(
          title: '通道',
          trailing: Text(
            doc.meta?.isPowerz == true ? '单通路' : '${channels.length} 个通道',
            style: TextStyle(fontSize: 10.5, color: p.tx3),
          ),
        ),
        if (doc.meta?.isPowerz == true)
          Container(
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 6),
            decoration: BoxDecoration(
              color: p.accentSoft,
              border: Border.all(color: p.accent),
              borderRadius: BorderRadius.circular(5),
            ),
            child: Row(
              children: [
                Text(
                  doc.protocol,
                  style: TextStyle(
                    fontSize: 11,
                    fontWeight: FontWeight.w600,
                    color: p.tx,
                  ),
                ),
                const Spacer(),
                Text(
                  '${doc.meta!.tableRows} 行事件',
                  style: TextStyle(fontSize: 10, color: p.tx3),
                ),
              ],
            ),
          )
        else
          Wrap(
            spacing: 5,
            runSpacing: 5,
            children: [
              for (final ch in channels)
                Tooltip(
                  message: '${ch.chunks} 块 · ${ch.totalSamples} 采样点',
                  child: _Toggle(
                    label: 'CH${ch.channel}',
                    on: doc.channel == ch.channel,
                    color: p.accent,
                    onTap: () =>
                        unawaited(workspace.switchChannel(doc, ch.channel)),
                  ),
                ),
            ],
          ),
      ],
    );
  }

  /// 快捷过滤：一键屏蔽 GoodCRC 心跳 / 只看 CRC 错误 / 只看功率协商 / 只看状态切换。
  Widget _quickFilters(BuildContext context, Palette p) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: [
      const _SectionTitle(title: '快速过滤'),
      _SwitchRow(
        label: doc.isUfcs ? '屏蔽 ACK / NCK 心跳' : '屏蔽 GoodCRC 心跳',
        value: doc.filters.hideGoodCrc,
        onChanged: (v) => _apply((f) => f.hideGoodCrc = v),
      ),
      _SwitchRow(
        label: '只看 CRC 错误',
        value: doc.filters.onlyBad,
        onChanged: (v) => _apply((f) => f.onlyBad = v),
      ),
      _SwitchRow(
        label: '只看功率协商',
        value: doc.filters.onlyPower,
        onChanged: (v) => _apply((f) => f.onlyPower = v),
      ),
      _SwitchRow(
        label: '只看状态切换',
        value: doc.filters.onlyEnter,
        onChanged: (v) => _apply((f) => f.onlyEnter = v),
      ),
    ],
  );

  /// 多选一栏。[values] 是候选，[selected] 是当前选中的子集。
  Widget _multi(
    BuildContext context,
    Palette p,
    String title,
    List<String> values,
    Set<String> selected,
  ) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionTitle(
          title: title,
          trailing: Text(
            '${selected.length}/${values.length}',
            style: TextStyle(fontSize: 10.5, color: p.tx3),
          ),
        ),
        Wrap(
          spacing: 5,
          runSpacing: 5,
          children: [
            for (final v in values)
              _Toggle(
                label: v,
                on: selected.contains(v),
                color: _valueColor(p, v),
                onTap: () => _apply((f) {
                  // 允许选空 —— 「一个都不选」是合法且有用的状态（等价于全屏蔽）。
                  if (!selected.remove(v)) selected.add(v);
                }, rebuildAfter: true),
              ),
          ],
        ),
      ],
    );
  }

  /// 具体报文类型（多选 + 计数）。候选与计数由核心在解码后给出。
  Widget _types(BuildContext context, Palette p) {
    final counts = doc.typeCounts.entries.toList()
      ..sort((a, b) {
        final countOrder = b.value.compareTo(a.value);
        return countOrder != 0 ? countOrder : a.key.compareTo(b.key);
      });
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionTitle(
          title: '报文类型',
          trailing: doc.filters.types.isEmpty
              ? Text('全部', style: TextStyle(fontSize: 10.5, color: p.tx3))
              : TextButton(
                  onPressed: () =>
                      _apply((f) => f.types.clear(), rebuildAfter: true),
                  style: TextButton.styleFrom(
                    minimumSize: Size.zero,
                    padding: const EdgeInsets.symmetric(horizontal: 4),
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  child: Text(
                    '清空',
                    style: TextStyle(fontSize: 10.5, color: p.accent),
                  ),
                ),
        ),
        Material(
          color: p.panel,
          child: ExpansionTile(
            dense: true,
            tilePadding: const EdgeInsets.symmetric(horizontal: 8),
            childrenPadding: const EdgeInsets.symmetric(horizontal: 8),
            title: Text(
              doc.filters.types.isEmpty
                  ? '全部类型'
                  : '已选 ${doc.filters.types.length} 类型',
              style: TextStyle(fontSize: 11.5, color: p.tx2),
            ),
            children: [
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 208),
                child: SingleChildScrollView(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      for (final e in counts)
                        InkWell(
                          onTap: () => _apply((f) {
                            if (!f.types.remove(e.key)) f.types.add(e.key);
                          }, rebuildAfter: true),
                          child: Padding(
                            padding: const EdgeInsets.symmetric(vertical: 2),
                            child: Row(
                              children: [
                                Icon(
                                  doc.filters.types.contains(e.key)
                                      ? Icons.check_box
                                      : Icons.check_box_outline_blank,
                                  size: 14,
                                  color: doc.filters.types.contains(e.key)
                                      ? p.accent
                                      : p.tx3,
                                ),
                                const SizedBox(width: 6),
                                Expanded(
                                  child: Text(
                                    e.key,
                                    overflow: TextOverflow.ellipsis,
                                    style: TextStyle(
                                      fontSize: 11.5,
                                      color: p.tx2,
                                    ),
                                  ),
                                ),
                                Text(
                                  '${e.value}',
                                  style: TextStyle(
                                    fontSize: 10.5,
                                    color: p.tx3,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _keywords(Palette p) {
    final words = doc.isUfcs
        ? const [
            'Request',
            'Output_Capabilities',
            'Power_Ready',
            'Cable',
            'Refuse',
            'Verify',
          ]
        : const ['PPS', 'AVS', 'VID', 'Alert', 'CRC'];
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const _SectionTitle(title: '关键字'),
        Wrap(
          spacing: 5,
          runSpacing: 5,
          children: [
            for (final word in words)
              ActionChip(
                label: Text(word, style: const TextStyle(fontSize: 10.5)),
                visualDensity: VisualDensity.compact,
                onPressed: () => _apply((f) => f.q = word),
              ),
          ],
        ),
      ],
    );
  }

  /// 时间窗口。归一化 0..1，与时间轴的刷选是同一套坐标。
  Widget _timeWindow(BuildContext context, Palette p) {
    final f = doc.filters;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionTitle(
          title: '时间窗口',
          trailing: f.hasTimeWindow
              ? TextButton(
                  onPressed: () => _apply((fl) {
                    fl.tFrom = 0;
                    fl.tTo = 1;
                  }),
                  style: TextButton.styleFrom(
                    minimumSize: Size.zero,
                    padding: const EdgeInsets.symmetric(horizontal: 4),
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  child: Text(
                    '复位视图',
                    style: TextStyle(fontSize: 10.5, color: p.accent),
                  ),
                )
              : null,
        ),
        RangeSlider(
          values: RangeValues(f.tFrom, f.tTo),
          onChanged: (v) {
            f.tFrom = v.start;
            f.tTo = v.end;
            doc.touch();
          },
          // 松手才重建视图：拖动过程中每帧都重建会卡。
          onChangeEnd: (_) => _apply(null),
        ),
        Text(
          '${fmtReadout(f.tFrom * doc.spanSec, doc.spanSec)} — '
          '${fmtReadout(f.tTo * doc.spanSec, doc.spanSec)}',
          style: TextStyle(fontSize: 11, color: p.tx3),
        ),
        Row(
          children: [
            TextButton(
              onPressed: () => _apply((fl) {
                fl.tFrom = 0;
                fl.tTo = 1;
              }),
              child: const Text('全时段'),
            ),
            TextButton(
              onPressed: () => unawaited(
                workspace.ready.then((e) => doc.fitTimeToCurrentView(e)),
              ),
              child: const Text('当前视图'),
            ),
          ],
        ),
      ],
    );
  }

  Color _valueColor(Palette p, String v) {
    if (roleValues.contains(v)) return p.forRole(v).$1;
    return p.accent;
  }

  /// 改完筛选后把新条件推给核心并刷新视图。
  ///
  /// [mutate] 为 null 表示「调用方已经改好了 filters，只要重算」。
  void _apply(
    void Function(FilterState f)? mutate, {
    bool rebuildAfter = false,
  }) {
    if (mutate != null) mutate(doc.filters);
    doc.touch();
    unawaited(workspace.ready.then((e) => doc.applyFilters(e)));
    if (rebuildAfter) doc.touch();
  }
}

class _Gap extends StatelessWidget {
  const _Gap();

  @override
  Widget build(BuildContext context) => const SizedBox(height: 13);
}

class _SectionTitle extends StatelessWidget {
  const _SectionTitle({required this.title, this.trailing});

  final String title;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Row(
        children: [
          Text(
            title,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w600,
              color: p.tx2,
              letterSpacing: .4,
            ),
          ),
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }
}

class _Toggle extends StatelessWidget {
  const _Toggle({
    required this.label,
    required this.on,
    required this.color,
    required this.onTap,
  });

  final String label;
  final bool on;
  final Color color;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(6),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: on ? color.withValues(alpha: .14) : p.panel2,
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: on ? color.withValues(alpha: .55) : p.line),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 11.5,
            color: on ? color : p.tx3,
            fontWeight: on ? FontWeight.w600 : FontWeight.w400,
          ),
        ),
      ),
    );
  }
}

class _SwitchRow extends StatelessWidget {
  const _SwitchRow({
    required this.label,
    required this.value,
    required this.onChanged,
  });

  final String label;
  final bool value;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return InkWell(
      onTap: () => onChanged(!value),
      borderRadius: BorderRadius.circular(6),
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(
          children: [
            Expanded(
              child: Text(
                label,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(fontSize: 11.5, color: p.tx2),
              ),
            ),
            const SizedBox(width: 8),
            MiniSwitch(value: value),
          ],
        ),
      ),
    );
  }
}

/// 小号开关（自绘）。
///
/// ⚠ **别把 Material 的 [Switch] 塞进小尺寸的 `SizedBox`**：它有自己固定的固有几何
///   （M3 的轨道约 52×32，外面还有一圈最小点击区）。约束被压到 30×18 之后，
///   它**仍然按自己的尺寸去画** —— 轨道以 `(size.width - 轨道宽) / 2` 定位，算出来
///   是负的，于是往左右各越界一段，**正好压在左边的标签文字上**。
///   这种错不抛异常、不报溢出，只是几行糊在一起，所以只能靠「不用它」来避免。
///   这里自己画：尺寸就是这个尺寸，绝不越界。
///
/// 公开是为了让界面冒烟测试能量到它的尺寸（见 `test/ui_smoke_test.dart`）。
class MiniSwitch extends StatelessWidget {
  const MiniSwitch({super.key, required this.value});

  final bool value;

  static const Size size = Size(32, 18);
  static const double _knob = 12;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    const move = Duration(milliseconds: 120);
    return SizedBox(
      width: size.width,
      height: size.height,
      child: AnimatedContainer(
        duration: move,
        curve: Curves.easeOut,
        decoration: BoxDecoration(
          color: value ? p.accent : p.panel2,
          borderRadius: BorderRadius.circular(size.height / 2),
          border: Border.all(color: value ? p.accent : p.line),
        ),
        child: AnimatedAlign(
          duration: move,
          curve: Curves.easeOut,
          alignment: value ? Alignment.centerRight : Alignment.centerLeft,
          child: Container(
            width: _knob,
            height: _knob,
            margin: const EdgeInsets.symmetric(horizontal: 2),
            decoration: BoxDecoration(
              color: value ? Colors.white : p.tx3,
              shape: BoxShape.circle,
            ),
          ),
        ),
      ),
    );
  }
}
