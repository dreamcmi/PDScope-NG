// top_bar.dart — 顶栏
//
// 信息层级：左边是「这份数据是什么」（文件名、来源 chip、采样率与
// 它的来源标注、各种计数 chip），右边是「我能做什么」（打开、导出、视图档位、搜索、
// 主题、关于）。
//
// 采样率那一栏是刻意做成 chip 的：**来源必须一直看得见**（文件声明 / 波形实测 /
// 默认值 / 分析仪时间戳）—— 分析仪导出的时间轴和 ATK-C 的采样时间轴量级完全不同，
// 不标出来读者会误读。
import 'dart:async';

import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/ffi.dart';
import '../core/filters.dart';
import '../core/formatting.dart';
import '../core/models.dart';
import '../core/palette.dart';
import '../core/workspace.dart';
import 'open_files.dart';

class TopBar extends StatelessWidget {
  const TopBar({
    super.key,
    required this.workspace,
    required this.doc,
    required this.searchFocus,
  });

  final Workspace workspace;
  final CaptureDocument? doc;
  final FocusNode searchFocus;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      height: 52,
      padding: const EdgeInsets.symmetric(horizontal: 14),
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(bottom: BorderSide(color: p.line)),
      ),
      child: Row(
        children: [
          _brand(p),
          const SizedBox(width: 10),
          const _Sep(),
          const SizedBox(width: 10),
          Expanded(child: _middle(context, p)),
          const SizedBox(width: 10),
          _actions(context, p),
        ],
      ),
    );
  }

  Widget _brand(Palette p) => Row(
    children: [
      Icon(Icons.usb_rounded, size: 20, color: p.accent),
      const SizedBox(width: 7),
      Text(
        'PDScope',
        style: TextStyle(
          fontSize: 14,
          fontWeight: FontWeight.w600,
          color: p.tx,
        ),
      ),
    ],
  );

  /// 中间一段：文件名 + 来源 + 采样率 + 计数。放不下就横向滚动，
  /// 不让它把右边的操作区挤掉。
  Widget _middle(BuildContext context, Palette p) {
    final d = doc;
    if (d == null) {
      return Text('没有打开的抓包', style: TextStyle(fontSize: 12, color: p.tx3));
    }
    final chips = <Widget>[];

    chips.add(
      // ⚠ 这里**不能**用 `Flexible`：这个 Row 挂在横向 `SingleChildScrollView` 下面，
      //   横向宽度是无界的，而带 flex 的子节点要求主轴约束有界 ——
      //   `Flexible` 会直接触发「RenderFlex children have non-zero flex but incoming
      //   width constraints are unbounded」的断言，**一打开抓包顶栏就报错**。
      //   要的效果只是「文件名太长时省略号截断」，给个 maxWidth 就够了：
      //   名字短就按自然宽度，长就截到上限，剩下的宽度还给后面的 chips。
      ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 280),
        child: Text(
          d.displayName,
          overflow: TextOverflow.ellipsis,
          style: TextStyle(
            fontSize: 12.5,
            fontWeight: FontWeight.w500,
            color: p.tx,
          ),
        ),
      ),
    );

    final meta = d.meta;
    if (meta != null) {
      chips.add(_Chip(p, _sourceLabel(meta), p.accent, p.accentSoft));
    }

    if (d.state == DocState.failed) {
      chips.add(
        _Chip(p, '打不开', p.bad, p.bad.withValues(alpha: .12), tip: d.error),
      );
    } else if (d.busy) {
      final pr = d.progress;
      final label = pr == null
          ? d.state.label
          : '${d.state.label} ${(pr.fraction * 100).toStringAsFixed(0)}%';
      chips.add(_Chip(p, label, p.accent, p.accentSoft));
    }

    if (d.stats != null) {
      final st = d.stats!;
      if (st.sampleRate > 0) {
        chips.add(
          _Chip(
            p,
            '${fmtRate(st.sampleRate)} · ${st.sourceTag}',
            p.tx2,
            p.panel2,
            tip: _rateTip(meta, st),
          ),
        );
      }
      chips.add(_Chip(p, '报文 ${st.packetCount}', p.tx2, p.panel2));
      chips.add(
        _Chip(
          p,
          '视图 ${d.rows.total}',
          p.tx2,
          p.panel2,
          tip: d.rows.total == st.packetCount
              ? '当前没有筛选'
              : '筛选前共 ${st.packetCount} 条',
        ),
      );
      if (st.connectCount > 0 || st.disconnectCount > 0) {
        chips.add(
          _Chip(
            p,
            '插拔 ${st.connectCount}/${st.disconnectCount}',
            p.tx2,
            p.panel2,
            tip: '插入 / 拔出',
          ),
        );
      }
      if (st.ufcsEvents > 0) {
        chips.add(
          _Chip(
            p,
            '状态事件 ${st.ufcsEvents}',
            p.tx2,
            p.panel2,
            tip: st.ufcsEventCodes.isEmpty
                ? '不含报文的状态事件'
                : '各 opcode：${st.ufcsEventCodes.map((e) => '0x${e.code.toRadixString(16)}×${e.n}').join('、')}',
          ),
        );
      }
      if (st.crcUnknown > 0) {
        chips.add(
          _Chip(
            p,
            'CRC 未记录 ${st.crcUnknown}',
            p.warn,
            p.warn.withValues(alpha: .13),
            tip: '导出文件未记录 CRC',
          ),
        );
      }
      if (st.badCrc > 0) {
        chips.add(
          _Chip(p, 'CRC 错误 ${st.badCrc}', p.bad, p.bad.withValues(alpha: .12)),
        );
      }
      final pick = st.channelPick;
      if (pick != null) {
        chips.add(
          _Chip(
            p,
            '通道 ch${pick['picked']}',
            p.tx2,
            p.panel2,
            tip: '自动选择；已排除 ${pick['noiseRejected']} 条噪声线',
          ),
        );
      }
    }

    return SingleChildScrollView(
      scrollDirection: Axis.horizontal,
      child: Row(
        children: [
          for (final c in chips) ...[c, const SizedBox(width: 6)],
        ],
      ),
    );
  }

  Widget _actions(BuildContext context, Palette p) {
    final d = doc;
    return Row(
      children: [
        if (d != null && d.decoded)
          _Segmented(
            options: ViewMode.values.map((v) => v.label).toList(),
            selected: d.filters.viewMode.index,
            onChanged: (i) {
              d.filters.viewMode = ViewMode.values[i];
              unawaited(workspace.ready.then((e) => d.applyFilters(e)));
            },
          ),
        if (d != null && d.decoded)
          _IconBtn(
            icon: workspace.prefs.compact
                ? Icons.unfold_more
                : Icons.unfold_less,
            tip: workspace.prefs.compact ? '恢复正常行高' : '紧凑行高',
            onTap: () => workspace.prefs.compact = !workspace.prefs.compact,
          ),
        const SizedBox(width: 8),
        if (d != null && d.decoded)
          _SearchBox(
            key: ValueKey(d.id),
            focusNode: searchFocus,
            initial: d.filters.q,
            onChanged: (v) {
              d.filters.q = v;
              unawaited(workspace.ready.then((e) => d.applyFilters(e)));
            },
          ),
        const SizedBox(width: 8),
        _IconBtn(
          icon: Icons.folder_open,
          tip: '打开文件（Ctrl+O）',
          onTap: () => unawaited(openFilesViaDialog(workspace)),
        ),
        // 实时采集的主入口（另两个：文件菜单 / Ctrl+D，以及空工作区空态里那个按钮）。
        _IconBtn(
          icon: Icons.usb_rounded,
          tip: '连接设备，边抓边看（Ctrl+D）',
          onTap: workspace.openLive,
        ),
        _ExportMenu(workspace: workspace, doc: d),
        _IconBtn(
          icon: workspace.prefs.isDark ? Icons.light_mode : Icons.dark_mode,
          tip: '切换明暗主题（T）',
          onTap: workspace.prefs.toggleTheme,
        ),
        _IconBtn(
          icon: Icons.info_outline,
          tip: '关于',
          onTap: () => showAboutPdScope(context),
        ),
      ],
    );
  }

  /// 顶栏 chip 上的「这份数据从哪来」。
  static String _sourceLabel(CaptureMeta m) {
    switch (m.container) {
      case 'atkcc':
        return 'ATK-C · .atkcc';
      case 'live':
        // 实时文档：来源是设备，不是文件 —— chip 要说清这一点。
        return '实时采集 · 设备';
      case 'pdstream':
        return 'POWER-Z · .pdStream';
      case 'ufcsstream':
        return 'POWER-Z · .ufcsStream';
      default:
        return m.isUfcs ? 'POWER-Z · .sqlite（UFCS）' : 'POWER-Z · .sqlite';
    }
  }

  static String _rateTip(CaptureMeta? m, DecodeStats st) {
    final lines = <String>[];
    if (m?.samplingFrequencyRaw != null) {
      lines.add('文件声明：${m!.samplingFrequencyRaw}');
    }
    if (st.sampleRateMeasured != null) {
      lines.add('波形实测：${fmtRate(st.sampleRateMeasured!)}');
    }
    if (st.sampleRateNote != null) lines.add(st.sampleRateNote!);
    if (lines.isEmpty) lines.add('来源：${st.sourceTag}');
    return lines.join('\n');
  }
}

/// 「关于」对话框。
///
/// 顶栏的按钮和外壳的「帮助 → 关于 PDScope」**都走这一个函数**。
/// （`showAboutDialog` 自己要一个能弹路由的 context，所以不能放到外壳那一层去。）
void showAboutPdScope(BuildContext context) {
  final version = _coreVersion();
  showAboutDialog(
    context: context,
    applicationName: 'PDScope',
    applicationVersion: version,
    applicationIcon: const Icon(Icons.usb_rounded, size: 32),
    children: const [
      SizedBox(height: 8),
      Text(
        'USB Power Delivery / UFCS 抓包分析。\n\n'
        '第三方组件（zlib / SQLite / nlohmann-json / Flutter 等）'
        '各自保留其许可，详见发行包内的 THIRD_PARTY_NOTICES。',
        style: TextStyle(fontSize: 12, height: 1.7),
      ),
    ],
  );
}

String _coreVersion() {
  try {
    return PdscopeBindings.instance().versionString();
  } catch (_) {
    return '未知';
  }
}

/// 顶栏上的一个信息 chip。
class _Chip extends StatelessWidget {
  const _Chip(this.p, this.text, this.fg, this.bg, {this.tip});

  final Palette p;
  final String text;
  final Color fg;
  final Color bg;
  final String? tip;

  @override
  Widget build(BuildContext context) {
    final body = Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: bg,
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: fg.withValues(alpha: .18)),
      ),
      child: Text(
        text,
        style: TextStyle(
          fontSize: 11.5,
          color: fg,
          fontWeight: FontWeight.w500,
        ),
      ),
    );
    return tip == null ? body : Tooltip(message: tip!, child: body);
  }
}

class _Sep extends StatelessWidget {
  const _Sep();

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(width: 1, height: 20, color: p.line);
  }
}

class _IconBtn extends StatelessWidget {
  const _IconBtn({required this.icon, required this.tip, required this.onTap});

  final IconData icon;
  final String tip;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Tooltip(
      message: tip,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(6),
        child: Padding(
          padding: const EdgeInsets.all(7),
          child: Icon(icon, size: 17, color: p.tx2),
        ),
      ),
    );
  }
}

/// 视图档位（全部 / 协商 / 错误）。
class _Segmented extends StatelessWidget {
  const _Segmented({
    required this.options,
    required this.selected,
    required this.onChanged,
  });

  final List<String> options;
  final int selected;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      decoration: BoxDecoration(
        color: p.panel2,
        borderRadius: BorderRadius.circular(7),
        border: Border.all(color: p.line),
      ),
      padding: const EdgeInsets.all(2),
      child: Row(
        children: [
          for (var i = 0; i < options.length; i++)
            InkWell(
              onTap: () => onChanged(i),
              borderRadius: BorderRadius.circular(5),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 3),
                decoration: BoxDecoration(
                  color: i == selected ? p.accent : Colors.transparent,
                  borderRadius: BorderRadius.circular(5),
                ),
                child: Text(
                  options[i],
                  style: TextStyle(
                    fontSize: 11.5,
                    color: i == selected ? Colors.white : p.tx2,
                    fontWeight: i == selected
                        ? FontWeight.w600
                        : FontWeight.w400,
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _SearchBox extends StatefulWidget {
  const _SearchBox({
    super.key,
    required this.focusNode,
    required this.initial,
    required this.onChanged,
  });

  final FocusNode focusNode;
  final String initial;
  final ValueChanged<String> onChanged;

  @override
  State<_SearchBox> createState() => _SearchBoxState();
}

class _SearchBoxState extends State<_SearchBox> {
  late final TextEditingController _c = TextEditingController(
    text: widget.initial,
  );
  Timer? _debounce;

  @override
  void didUpdateWidget(_SearchBox oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.initial != widget.initial && _c.text != widget.initial) {
      _debounce?.cancel();
      _c.text = widget.initial;
    }
  }

  @override
  void dispose() {
    _debounce?.cancel();
    _c.dispose();
    super.dispose();
  }

  /// 输入防抖：每敲一个字符就重建一次视图，几万条报文会卡。
  void _onChanged(String v) {
    _debounce?.cancel();
    _debounce = Timer(
      const Duration(milliseconds: 220),
      () => widget.onChanged(v),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return SizedBox(
      width: 210,
      height: 29,
      child: TextField(
        controller: _c,
        focusNode: widget.focusNode,
        onChanged: _onChanged,
        style: TextStyle(fontSize: 12, color: p.tx),
        decoration: InputDecoration(
          isDense: true,
          hintText: '搜索报文类型 / 内容（/）',
          hintStyle: TextStyle(fontSize: 11.5, color: p.tx3),
          prefixIcon: Icon(Icons.search, size: 15, color: p.tx3),
          prefixIconConstraints: const BoxConstraints(minWidth: 28),
          contentPadding: const EdgeInsets.symmetric(
            vertical: 6,
            horizontal: 6,
          ),
          filled: true,
          fillColor: p.panel2,
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(7),
            borderSide: BorderSide(color: p.line),
          ),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(7),
            borderSide: BorderSide(color: p.line),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(7),
            borderSide: BorderSide(color: p.accent),
          ),
        ),
      ),
    );
  }
}

class _ExportMenu extends StatelessWidget {
  const _ExportMenu({required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument? doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final d = doc;
    final enabled = d != null && d.decoded;
    return PopupMenuButton<String>(
      enabled: enabled,
      tooltip: '导出',
      position: PopupMenuPosition.under,
      itemBuilder: (context) => const [
        PopupMenuItem(
          value: 'csv',
          child: Text('导出 CSV（当前筛选结果）', style: TextStyle(fontSize: 12.5)),
        ),
        PopupMenuItem(
          value: 'json',
          child: Text('导出 JSON（全部报文）', style: TextStyle(fontSize: 12.5)),
        ),
      ],
      onSelected: (v) => unawaited(exportViaDialog(context, workspace, doc, v)),
      child: Padding(
        padding: const EdgeInsets.all(7),
        child: Icon(
          Icons.download_outlined,
          size: 17,
          color: enabled ? p.tx2 : p.tx3,
        ),
      ),
    );
  }
}
