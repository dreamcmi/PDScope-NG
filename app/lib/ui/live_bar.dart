// live_bar.dart — 实时状态条
//
// 设计第 4 节：它描述的是**会话**而不是某一份数据，所以和顶栏、标签栏一样是整宽结构条，
// 而不是挤在顶栏右侧跟一堆 chip 抢位置。整宽之后它才有地方表达
// 「出事了、什么事、要不要停」这三件事。
//
// 配色与尺寸全部取自现有控件（见设计第 10 节）：
//   · 实心主按钮 = 顶栏分段控件**选中态**的配方（accent 实底 + 白字 w600）
//   · 描边次按钮 = 信息 chip 的配方（1px line 边 + panel 底）
//   · 健康计数   = 顶栏统计 chip 的配方（前景 100% + 底 12~16% + 1px 同色边 @18%）
import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/live_session.dart';
import '../core/live_source.dart';
import '../core/palette.dart';

class LiveBar extends StatelessWidget {
  const LiveBar({
    super.key,
    required this.doc,
    required this.onExportCsv,
    required this.onOpenTrace,
  });

  final CaptureDocument doc;
  final VoidCallback onExportCsv;
  final VoidCallback onOpenTrace;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final l = doc.live;
    if (l == null) return const SizedBox.shrink();

    final st = liveStateStyle(p, doc.liveState);

    return Container(
      height: 40,
      padding: const EdgeInsets.symmetric(horizontal: 14),
      decoration: BoxDecoration(
        color: st.tint == null
            ? p.panel
            : Color.alphaBlend(st.tint!.withValues(alpha: 0.10), p.panel),
        border: Border(bottom: BorderSide(color: p.line)),
      ),
      child: Row(
        children: [
          _PulseDot(
            color: st.color,
            animate: doc.liveState == LiveState.capturing,
          ),
          const SizedBox(width: 6),
          Text(
            doc.liveState.label,
            style: TextStyle(
              fontSize: 11.5,
              fontWeight: FontWeight.w600,
              color: st.color,
            ),
          ),
          const SizedBox(width: 6),
          Expanded(
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Row(children: _metrics(p, l)),
            ),
          ),
          IgnorePointer(
            ignoring: doc.liveBusy,
            child: Opacity(
              opacity: doc.liveBusy ? 0.5 : 1,
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: _actions(context, p),
              ),
            ),
          ),
        ],
      ),
    );
  }

  /* ── 读数 ─────────────────────────────────────────────────── */

  List<Widget> _metrics(Palette p, LiveSession l) {
    final st = l.stats;
    // 平均速率：用「报文数 / 时长」而不是瞬时值 —— 实时流的瞬时值跳得厉害，
    // 看不出节拍是否失控，而那正是这个读数要回答的问题。
    final rate = st.durationSec > 0 ? st.packets / st.durationSec : 0.0;
    final out = <Widget>[];

    void metric(String label, String value) {
      out.add(
        Padding(
          padding: const EdgeInsets.only(right: 10),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(label, style: TextStyle(fontSize: 11.5, color: p.tx3)),
              const SizedBox(width: 5),
              Text(
                value,
                style: p.mono.copyWith(
                  fontSize: 11.5,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ],
          ),
        ),
      );
    }

    metric('报文', fmtCount(st.packets));
    if (st.timeUncertain) metric('时间', '连续性不确定');
    if (doc.liveState == LiveState.capturing ||
        doc.liveState == LiveState.paused) {
      metric('速率', rate.toStringAsFixed(1));
      out.add(
        Padding(
          padding: const EdgeInsets.only(right: 10),
          child: Text('pkt/s', style: TextStyle(fontSize: 11.5, color: p.tx3)),
        ),
      );
      metric('采样', fmtCount(st.samples));
      metric('时长', fmtDuration(st.durationSec));
    }

    // 丢包**永远显示**（0 也是绿的）：它一旦非 0 会被误读成「设备没发」，
    // 所以这个「我们没丢」的证据要一直在场。
    if (st.lossCountKnown) {
      out.add(_health(p, '丢包', st.dropped, HealthLevel.warn));
    } else {
      metric('丢包数', '未知');
      out.add(_health(p, '损失上报', st.lossReports, HealthLevel.warn));
      out.add(_health(p, '帧缺口', st.sequenceGaps, HealthLevel.warn));
      if (st.invalidFrames > 0) {
        out.add(_health(p, '坏帧', st.invalidFrames, HealthLevel.warn));
      }
    }
    // 超时只有非 0 才显示 —— 空闲超时是正常现象，常年挂个 0 只会占地方。
    if (st.ioTimeouts > 0) {
      out.add(_health(p, '超时', st.ioTimeouts, HealthLevel.warn));
    }
    // 拒绝非 0 即异常：设备明确说了「这条我不认」。
    if (st.rejects > 0) {
      out.add(_health(p, '拒绝', st.rejects, HealthLevel.bad));
    }
    if (st.trimmed > 0) {
      out.add(_health(p, '已裁', st.trimmed, HealthLevel.warn));
    }
    return out;
  }

  Widget _health(Palette p, String label, int n, HealthLevel level) {
    // 非 0 才升级成 warn / bad；0 一律是绿（「确实没发生」也是一种结论）。
    final eff = n == 0 ? HealthLevel.ok : level;
    final (fg, bg) = switch (eff) {
      HealthLevel.ok => (p.ok, p.ok),
      HealthLevel.warn => (p.warn, p.warn),
      HealthLevel.bad => (p.bad, p.bad),
    };
    return Padding(
      padding: const EdgeInsets.only(right: 6),
      child: Tooltip(
        message: switch (label) {
          '丢包' => n == 0 ? '队列没有溢出，一条都没丢' : '队列溢出丢掉了 $n 条 —— 少的报文不是「设备没发」',
          '超时' => 'USB 空闲超时 $n 次。设备没话说时这是正常现象，不必慌',
          '拒绝' => '设备明确拒绝了 $n 条命令',
          '损失上报' => '粘滞损失位出现新增标志 $n 次，不能换算成精确丢包数量',
          '帧缺口' => 'DATA/EVT 编号出现 $n 个可观察帧缺口，连续也不能证明无损',
          '坏帧' => '长度、CRC 或语义验证失败 $n 次，整帧拒绝',
          _ => '因内存上限裁掉了最旧的 $n 条',
        },
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
          decoration: BoxDecoration(
            color: bg.withValues(alpha: 0.14),
            borderRadius: BorderRadius.circular(6),
            border: Border.all(color: fg.withValues(alpha: 0.18)),
          ),
          child: Text(
            '$label $n',
            style: TextStyle(
              fontSize: 11.5,
              fontWeight: FontWeight.w500,
              color: fg,
            ),
          ),
        ),
      ),
    );
  }

  /* ── 操作 ─────────────────────────────────────────────────── */

  List<Widget> _actions(BuildContext context, Palette p) {
    final out = <Widget>[];
    void gap() => out.add(const SizedBox(width: 8));

    switch (doc.liveState) {
      case LiveState.capturing:
        // 原则⑤：任何时刻只有一个填充主按钮。
        // 采集中它是「停止」—— 停止后数据仍然完整可用（原则④），所以不危险。
        gap();
        out.add(_ghost(p, '日志', onOpenTrace));
        out.add(_drill(context, p));
        gap();
        out.add(_outlinedAccent(p, '导出 CSV', onExportCsv));
        gap();
        if (doc.liveSource?.supportsPause == true) {
          out.add(_outlined(p, '暂停', doc.livePause));
          gap();
        }
        out.add(_filled(p, '停止', () => doc.liveStop()));
      case LiveState.paused:
        gap();
        out.add(_ghost(p, '日志', onOpenTrace));
        out.add(_drill(context, p));
        gap();
        out.add(_outlinedAccent(p, '导出 CSV', onExportCsv));
        gap();
        out.add(_outlined(p, '停止', () => doc.liveStop()));
        gap();
        out.add(_filled(p, '继续', doc.liveResume));
      case LiveState.listening:
        gap();
        out.add(
          Text(
            '只收不发，不向设备写任何字节',
            style: TextStyle(fontSize: 11.5, color: p.tx3),
          ),
        );
      case LiveState.ready:
      case LiveState.stopped:
        gap();
        out.add(_ghost(p, '日志', onOpenTrace));
        gap();
        out.add(_outlinedAccent(p, '导出 CSV', onExportCsv));
        gap();
        out.add(_outlined(p, '重新开始', doc.liveStart));
        gap();
        out.add(
          _outlined(p, '采集配置', () {
            doc.liveShowConfig = true;
            doc.touch();
          }),
        );
      case LiveState.recoverableError:
        gap();
        out.add(_ghost(p, '日志', onOpenTrace));
        gap();
        out.add(_outlined(p, '停止', () => doc.liveStop()));
        gap();
        out.add(_outlined(p, '断开设备', () => doc.liveDisconnect()));
      case LiveState.parked:
      case LiveState.disconnected:
        // 原则③：**不自动重连**，也不给「重连」按钮 ——
        // 要不要重来由人决定，且要先明确断开。
        gap();
        out.add(
          _ghost(p, '查看通讯日志', () {
            doc.showTrace = true;
            doc.touch();
            onOpenTrace();
          }),
        );
        if (doc.liveState == LiveState.parked) {
          gap();
          out.add(_outlined(p, '断开设备', () => doc.liveDisconnect()));
        } else {
          gap();
          out.add(_outlined(p, '重新查找', () => doc.liveDisconnect()));
        }
        gap();
        out.add(_filled(p, '导出 CSV', onExportCsv));
      default:
        break;
    }
    return out;
  }

  /* ── 演练（只有模拟设备源有）────────────────────────────── */

  /// 「主动停车」「设备掉线」这两个状态在真机上不好随时复现，
  /// 但审查界面时必须能看到它们 —— 尤其是**采到数据之后**的样子。
  ///
  /// 连接面板上那两个按钮只在采集前可达，所以这里再给一个入口。
  /// 只在模拟源下出现，措辞也写明是演练，不会让人误以为是产品功能。
  Widget _drill(BuildContext context, Palette p) {
    if (doc.liveSource is! MockLiveSource) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.only(left: 6),
      child: PopupMenuButton<String>(
        tooltip: '演练：复现真机上不好等的故障状态（仅模拟设备源）',
        position: PopupMenuPosition.under,
        color: p.panel,
        padding: EdgeInsets.zero,
        onSelected: (v) {
          if (v == 'park') doc.liveDrillPark();
          if (v == 'drop') doc.liveDrillDropout();
        },
        itemBuilder: (_) => const [
          PopupMenuItem(value: 'park', height: 34, child: Text('演练：主动停车')),
          PopupMenuItem(value: 'drop', height: 34, child: Text('演练：设备掉线')),
        ],
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
          child: Text('演练', style: TextStyle(fontSize: 11.5, color: p.tx3)),
        ),
      ),
    );
  }

  /* ── 按钮（配方全部来自现有控件）──────────────────────────── */

  static Widget _filled(Palette p, String label, VoidCallback onTap) => _Btn(
    label: label,
    onTap: onTap,
    bg: p.accent,
    fg: Colors.white,
    bold: true,
  );

  static Widget _outlined(Palette p, String label, VoidCallback onTap) =>
      _Btn(label: label, onTap: onTap, bg: p.panel, fg: p.tx2, border: p.line);

  /// 导出用描边 + accent 文字：它是个有用的次要动作，
  /// 不能跟唯一的实心主按钮抢注意力。
  static Widget _outlinedAccent(Palette p, String label, VoidCallback onTap) =>
      _Btn(
        label: label,
        onTap: onTap,
        bg: p.panel,
        fg: p.accent,
        border: p.accent.withValues(alpha: 0.45),
        bold: true,
      );

  static Widget _ghost(Palette p, String label, VoidCallback onTap) =>
      _Btn(label: label, onTap: onTap, bg: Colors.transparent, fg: p.tx2);
}

enum HealthLevel { ok, warn, bad }

/// 状态 → 颜色。停车/掉线用 bad，暂停/可恢复错误用 warn，其余用 accent/ok。
({Color color, Color? tint}) liveStateStyle(Palette p, LiveState s) {
  if (s.isBad) return (color: p.bad, tint: p.bad);
  if (s.isWarn) return (color: p.warn, tint: p.warn);
  return switch (s) {
    LiveState.capturing => (color: p.accent, tint: null),
    LiveState.listening => (color: p.ctrl, tint: null),
    LiveState.stopped => (color: p.ok, tint: null),
    LiveState.ready => (color: p.ok, tint: null),
    _ => (color: p.tx3, tint: null),
  };
}

class _Btn extends StatelessWidget {
  const _Btn({
    required this.label,
    required this.onTap,
    required this.bg,
    required this.fg,
    this.border,
    this.bold = false,
  });

  final String label;
  final VoidCallback onTap;
  final Color bg;
  final Color fg;
  final Color? border;
  final bool bold;

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(6),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
        decoration: BoxDecoration(
          color: bg,
          borderRadius: BorderRadius.circular(6),
          border: border == null ? null : Border.all(color: border!),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 11.5,
            fontWeight: bold ? FontWeight.w600 : FontWeight.w500,
            color: fg,
          ),
        ),
      ),
    );
  }
}

/// 采集中的呼吸光环。只有「真的在进数据」时才动 ——
/// 暂停/停车时它必须停下来，否则一眼看不出是不是还在采。
class _PulseDot extends StatefulWidget {
  const _PulseDot({required this.color, required this.animate});
  final Color color;
  final bool animate;

  @override
  State<_PulseDot> createState() => _PulseDotState();
}

class _PulseDotState extends State<_PulseDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1600),
  );

  @override
  void initState() {
    super.initState();
    if (widget.animate) _c.repeat();
  }

  @override
  void didUpdateWidget(covariant _PulseDot old) {
    super.didUpdateWidget(old);
    if (widget.animate && !_c.isAnimating) {
      _c.repeat();
    } else if (!widget.animate && _c.isAnimating) {
      _c.stop();
      _c.value = 0;
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 16,
      height: 16,
      child: Stack(
        alignment: Alignment.center,
        children: [
          if (widget.animate)
            AnimatedBuilder(
              animation: _c,
              builder: (_, _) => Opacity(
                opacity: (1 - _c.value).clamp(0.0, 1.0) * 0.9,
                child: Transform.scale(
                  scale: 0.5 + _c.value * 0.65,
                  child: Container(
                    width: 16,
                    height: 16,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: widget.color.withValues(alpha: 0.25),
                    ),
                  ),
                ),
              ),
            ),
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: widget.color,
            ),
          ),
        ],
      ),
    );
  }
}

/* ── 与核心同口径的几个格式化 ─────────────────────────────── */

/// 千位分隔。与 `csv.cpp` 的读数风格一致。
String fmtCount(int n) {
  final s = n.toString();
  final b = StringBuffer();
  for (var i = 0; i < s.length; i++) {
    if (i > 0 && (s.length - i) % 3 == 0) b.write(',');
    b.write(s[i]);
  }
  return b.toString();
}

/// `m:ss` / `h:mm:ss`。与 `formatting.dart` 的 fmtSeconds 同一口径。
String fmtDuration(double sec) {
  final t = sec.floor();
  final h = t ~/ 3600;
  final m = (t % 3600) ~/ 60;
  final s = t % 60;
  String two(int v) => v.toString().padLeft(2, '0');
  return h > 0 ? '$h:${two(m)}:${two(s)}' : '${two(m)}:${two(s)}';
}
