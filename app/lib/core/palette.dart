// palette.dart — 主题色板
//
// 取值直接对应 JS 基线 `src/ui/styles.css` 的 `:root` / `html[data-theme=…]`，
// 目的是让 Flutter 版的观感与既有界面一致（计划 §3：视觉尺寸、配色以现有界面为基准）。
// ⚠ 改这里要对着那份 CSS 改，别凭印象调色。
import 'package:flutter/material.dart';

/// 一套完整的界面色（明/暗各一份）。
@immutable
class Palette {
  const Palette({
    required this.bg,
    required this.bg2,
    required this.panel,
    required this.panel2,
    required this.line,
    required this.line2,
    required this.tx,
    required this.tx2,
    required this.tx3,
    required this.accent,
    required this.accent2,
    required this.accentSoft,
    required this.src,
    required this.srcBg,
    required this.snk,
    required this.snkBg,
    required this.plug,
    required this.plugBg,
    required this.ctrl,
    required this.data,
    required this.ext,
    required this.vdm,
    required this.err,
    required this.custom,
    required this.rowHover,
    required this.rowSel,
    required this.scroll,
    required this.ok,
    required this.warn,
    required this.bad,
    required this.groups,
    required this.brightness,
  });

  // 背景 / 面板
  final Color bg;
  final Color bg2;
  final Color panel;
  final Color panel2;
  final Color line;
  final Color line2;

  // 文字
  final Color tx;
  final Color tx2;
  final Color tx3;

  // 主色
  final Color accent;
  final Color accent2;
  final Color accentSoft;

  // 方向徽章（Source / Sink / Plug）
  final Color src;
  final Color srcBg;
  final Color snk;
  final Color snkBg;
  final Color plug;
  final Color plugBg;

  // 报文类别（控制 / 数据 / 扩展 / VDM / 异常 / 厂家自定义）
  final Color ctrl;
  final Color data;
  final Color ext;
  final Color vdm;
  final Color err;
  final Color custom;

  final Color rowHover;
  final Color rowSel;
  final Color scroll;

  final Color ok;
  final Color warn;
  final Color bad;

  /// 详情分组配色（相邻 VDO / PDO 换色相，8 组循环）。
  final List<Color> groups;

  final Brightness brightness;

  TextStyle get mono => TextStyle(
    fontFamily: 'Consolas',
    fontFamilyFallback: const ['Cascadia Mono', 'JetBrains Mono', 'Menlo', 'monospace'],
    fontSize: 12,
    color: tx,
  );

  static const Palette light = Palette(
    bg: Color(0xFFEEF1F5),
    bg2: Color(0xFFF7F9FB),
    panel: Color(0xFFFFFFFF),
    panel2: Color(0xFFFAFBFD),
    line: Color(0xFFE0E5EB),
    line2: Color(0xFFECEFF3),
    tx: Color(0xFF1B2027),
    tx2: Color(0xFF5B6572),
    tx3: Color(0xFF8B95A3),
    accent: Color(0xFF2563EB),
    accent2: Color(0xFF1D4ED8),
    accentSoft: Color(0xFFE8EFFF),
    src: Color(0xFF1D64E0),
    srcBg: Color(0xFFE7F0FF),
    snk: Color(0xFFD2691E),
    snkBg: Color(0xFFFDEFE2),
    plug: Color(0xFF7A3CD8),
    plugBg: Color(0xFFF1E9FF),
    ctrl: Color(0xFF0E8BA1),
    data: Color(0xFF159C53),
    ext: Color(0xFFB98600),
    vdm: Color(0xFF7A3CD8),
    err: Color(0xFFD93838),
    custom: Color(0xFF4F46E5),
    rowHover: Color(0xFFF2F6FD),
    rowSel: Color(0xFFDBEAFE),
    scroll: Color(0xFFC9D1DA),
    ok: Color(0xFF159C53),
    warn: Color(0xFFB98600),
    bad: Color(0xFFD93838),
    groups: [
      Color(0xFF2563EB),
      Color(0xFF0E8BA1),
      Color(0xFF159C53),
      Color(0xFF7A3CD8),
      Color(0xFFC2410C),
      Color(0xFFBE185D),
      Color(0xFF4F46E5),
      Color(0xFFA16207),
    ],
    brightness: Brightness.light,
  );

  static const Palette dark = Palette(
    bg: Color(0xFF0D1014),
    bg2: Color(0xFF12161C),
    panel: Color(0xFF161B22),
    panel2: Color(0xFF1B212A),
    line: Color(0xFF262E38),
    line2: Color(0xFF1F272F),
    tx: Color(0xFFE6EAF0),
    tx2: Color(0xFFA2ACBA),
    tx3: Color(0xFF737E8D),
    accent: Color(0xFF4D8DFF),
    accent2: Color(0xFF6BA0FF),
    accentSoft: Color(0xFF17233A),
    src: Color(0xFF6BA0FF),
    srcBg: Color(0xFF16233C),
    snk: Color(0xFFF0A35E),
    snkBg: Color(0xFF33260F),
    plug: Color(0xFFB28BFA),
    plugBg: Color(0xFF251A3C),
    ctrl: Color(0xFF45C7DD),
    data: Color(0xFF4ED08A),
    ext: Color(0xFFE6BF4A),
    vdm: Color(0xFFB28BFA),
    err: Color(0xFFFF6B6B),
    custom: Color(0xFFA5B4FC),
    rowHover: Color(0xFF1C2430),
    rowSel: Color(0xFF1E3352),
    scroll: Color(0xFF39424E),
    ok: Color(0xFF4ED08A),
    warn: Color(0xFFE6BF4A),
    bad: Color(0xFFFF6B6B),
    groups: [
      Color(0xFF6BA0FF),
      Color(0xFF45C7DD),
      Color(0xFF4ED08A),
      Color(0xFFB28BFA),
      Color(0xFFF0A35E),
      Color(0xFFF472B6),
      Color(0xFFA5B4FC),
      Color(0xFFE6BF4A),
    ],
    brightness: Brightness.dark,
  );

  /// 报文类别 → 颜色。类别名取自核心的 `toneOf` / `packetKind`。
  Color forKind(String kind) {
    switch (kind) {
      case 'Control':
        return ctrl;
      case 'Data':
        return data;
      case 'Extended':
        return ext;
      case 'VDM':
        return vdm;
      case 'Error':
        return err;
      case 'Custom':
        return custom;
      default:
        return tx2;
    }
  }

  /// 方向 → 徽章配色。UFCS 的角色名与 PD 不同，这里一并覆盖。
  (Color fg, Color bg) forRole(String role) {
    switch (role) {
      case 'SRC':
      case 'Source':
      case 'SRC_SNK':
        return (src, srcBg);
      case 'SNK':
      case 'Sink':
        return (snk, snkBg);
      case 'Plug':
        return (plug, plugBg);
      default:
        return (tx2, panel2);
    }
  }
}

/// 把 [Palette] 暴露成 [ThemeExtension]，界面里用 `Palette.of(context)` 取。
class PaletteScope extends InheritedWidget {
  const PaletteScope({super.key, required this.palette, required super.child});

  final Palette palette;

  static Palette of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<PaletteScope>();
    assert(scope != null, 'PaletteScope 不在祖先链上');
    return scope!.palette;
  }

  @override
  bool updateShouldNotify(PaletteScope oldWidget) => oldWidget.palette != palette;
}
