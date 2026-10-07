// app.dart — 应用外壳：主题、总体布局、快捷键
//
// 布局（自上而下、左右分栏）：
//   顶栏 / 标签栏 / ┌ 左筛选 │ 中（提示条 · 报文表 · 拖条 · 时间轴） │ 右详情 ┐ / 状态栏
//
// 两条硬约定：
//   · 尺寸一律走 [Prefs]（行高、详情宽、曲线区高），拖动改的是偏好，不是某个文档的状态。
//   · 详情收起后右缘必须留一条**不依赖数据**的竖栏，否则「一行报文都没有」的抓包
//     一旦收起面板就再也打不开了。
import 'dart:async';
import 'dart:ui' show AppExitResponse;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/document.dart';
import '../core/live_source.dart';
import '../core/palette.dart';
import '../core/prefs.dart';
import '../core/shell.dart';
import '../core/workspace.dart';
import 'connect_panel.dart';
import 'detail_panel.dart';
import 'filter_panel.dart';
import 'live_bar.dart';
import 'open_files.dart';
import 'packet_table.dart';
import 'tab_strip.dart';
import 'timeline.dart';
import 'top_bar.dart';
import 'trace_panel.dart';

class PdScopeApp extends StatelessWidget {
  const PdScopeApp({super.key, required this.workspace});

  final Workspace workspace;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: workspace.prefs,
      builder: (context, _) {
        final palette = workspace.prefs.isDark ? Palette.dark : Palette.light;
        return PaletteScope(
          palette: palette,
          child: MaterialApp(
            title: 'PDScope',
            debugShowCheckedModeBanner: false,
            theme: themeFor(palette),
            home: AppShell(workspace: workspace),
          ),
        );
      },
    );
  }

  static ThemeData themeFor(Palette p) => ThemeData(
    useMaterial3: true,
    brightness: p.brightness,
    scaffoldBackgroundColor: p.bg,
    colorScheme: ColorScheme.fromSeed(
      seedColor: p.accent,
      brightness: p.brightness,
    ).copyWith(surface: p.panel, error: p.bad),
    dividerColor: p.line,
    tooltipTheme: TooltipThemeData(
      waitDuration: const Duration(milliseconds: 420),
      decoration: BoxDecoration(
        color: p.brightness == Brightness.dark
            ? const Color(0xF01B212A)
            : const Color(0xF01B2027),
        borderRadius: BorderRadius.circular(6),
      ),
      textStyle: const TextStyle(
        color: Colors.white,
        fontSize: 11.5,
        height: 1.5,
      ),
    ),
    progressIndicatorTheme: ProgressIndicatorThemeData(color: p.accent),
    scrollbarTheme: ScrollbarThemeData(
      thickness: const WidgetStatePropertyAll(9),
      thumbColor: WidgetStatePropertyAll(p.scroll),
    ),
  );
}

/// 应用外壳。
class AppShell extends StatefulWidget {
  const AppShell({super.key, required this.workspace});

  final Workspace workspace;

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  AppLifecycleListener? _lifecycle;
  final searchFocus = FocusNode();

  /// 外壳命令的订阅。原生菜单点一下就从这里进来 ——
  /// 和界面上的入口走**同一套动作**，不各写一份。
  void Function()? _unlistenShell;

  /// 上一次报给外壳的状态；没变就不重复发。
  String? _reportedShellState;

  /// 重绘源：工作区 + 当前文档（订阅由工作区的通知主动驱动，见 [_RepaintSource]）。
  late _RepaintSource _repaint = _RepaintSource(ws);

  @override
  void initState() {
    super.initState();
    _lifecycle = AppLifecycleListener(
      onExitRequested: () async {
        await ws.closeAll();
        return AppExitResponse.exit;
      },
    );
    _unlistenShell = ShellBridge.commands.listen(_runShellCommand);
    // 菜单的灰/亮与勾选跟着界面走：外壳看不见 widget 树，只能由这边报。
    ws.addListener(_reportShellState);
    ws.prefs.addListener(_reportShellState);
    _reportShellState();
  }

  @override
  void didUpdateWidget(AppShell old) {
    super.didUpdateWidget(old);
    // 正常不会换工作区；真换了就得把订阅也换过去，否则新工作区的通知全打空。
    if (!identical(old.workspace, ws)) {
      _repaint.dispose();
      _repaint = _RepaintSource(ws);
    }
  }

  @override
  void dispose() {
    _lifecycle?.dispose();
    _unlistenShell?.call();
    ws.removeListener(_reportShellState);
    ws.prefs.removeListener(_reportShellState);
    _repaint.dispose();
    searchFocus.dispose();
    super.dispose();
  }

  Workspace get ws => widget.workspace;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return AnimatedBuilder(
      animation: _repaint,
      builder: (context, _) {
        final doc = ws.active;
        return Scaffold(
          backgroundColor: p.bg,
          body: Focus(
            autofocus: true,
            onKeyEvent: (node, ev) => _handleKey(doc, ev),
            child: Column(
              children: [
                TopBar(workspace: ws, doc: doc, searchFocus: searchFocus),
                if (ws.docs.isNotEmpty) TabStrip(workspace: ws),
                // 实时状态条：整宽结构条，描述的是**会话**而不是某一份数据。
                // 未进入采集阶段时不出现 —— 那时连接面板自己会说话。
                if (doc != null && doc.showLiveBar)
                  LiveBar(
                    doc: doc,
                    onExportCsv: () => _exportLiveCsv(context, doc),
                    onOpenTrace: () => setState(() => doc.showTrace = true),
                  ),
                Expanded(
                  child: _Body(workspace: ws, doc: doc),
                ),
                _StatusBar(workspace: ws, doc: doc),
              ],
            ),
          ),
        );
      },
    );
  }

  /// 键盘快捷键。
  ///
  /// ⚠ 字母快捷键只在「没在输入框里」时才认 —— 否则在搜索框里打一个 `t`
  /// 会顺手把主题切了。
  KeyEventResult _handleKey(CaptureDocument? doc, KeyEvent ev) {
    final arrow =
        ev.logicalKey == LogicalKeyboardKey.arrowUp ||
        ev.logicalKey == LogicalKeyboardKey.arrowDown;
    if (ev is! KeyDownEvent && !(ev is KeyRepeatEvent && arrow)) {
      return KeyEventResult.ignored;
    }
    final typing = searchFocus.hasFocus;

    if (HardwareKeyboard.instance.isControlPressed ||
        HardwareKeyboard.instance.isMetaPressed) {
      if (ev.logicalKey == LogicalKeyboardKey.keyO) {
        unawaited(openFilesViaDialog(ws));
        return KeyEventResult.handled;
      }
      // Ctrl/⌘+D = 连接设备（D 取 Device）。与文件菜单里那一项同一条命令。
      if (ev.logicalKey == LogicalKeyboardKey.keyD) {
        ws.openLive();
        return KeyEventResult.handled;
      }
      if (ev.logicalKey == LogicalKeyboardKey.keyW) {
        if (ws.activeIndex >= 0) unawaited(ws.closeAt(ws.activeIndex));
        return KeyEventResult.handled;
      }
      if (ev.logicalKey == LogicalKeyboardKey.keyF) {
        searchFocus.requestFocus();
        return KeyEventResult.handled;
      }
      if (ev.logicalKey == LogicalKeyboardKey.keyS) {
        if (doc != null && doc.decoded) {
          unawaited(exportViaDialog(context, ws, doc, 'csv'));
        }
        return KeyEventResult.handled;
      }
      if (ev.logicalKey == LogicalKeyboardKey.keyT) {
        ws.prefs.toggleTheme();
        return KeyEventResult.handled;
      }
      if (ev.logicalKey == LogicalKeyboardKey.keyB) {
        ws.prefs.filtersCollapsed = !ws.prefs.filtersCollapsed;
        return KeyEventResult.handled;
      }
      return KeyEventResult.ignored;
    }

    // Alt + 1..9：切标签
    if (HardwareKeyboard.instance.isAltPressed) {
      final n = int.tryParse(ev.logicalKey.keyLabel);
      if (n != null && n >= 1 && n <= 9) {
        ws.activateByShortcut(n);
        return KeyEventResult.handled;
      }
      return KeyEventResult.ignored;
    }

    if (typing) return KeyEventResult.ignored;

    switch (ev.logicalKey) {
      case LogicalKeyboardKey.arrowUp:
      case LogicalKeyboardKey.arrowDown:
        if (doc == null || !doc.decoded || doc.rows.total == 0) {
          return KeyEventResult.ignored;
        }
        ws.prefs.detailCollapsed = false;
        final delta = ev.logicalKey == LogicalKeyboardKey.arrowUp ? -1 : 1;
        unawaited(ws.ready.then((e) => doc.stepDetail(e, delta)));
        return KeyEventResult.handled;
      case LogicalKeyboardKey.slash:
        searchFocus.requestFocus();
        return KeyEventResult.handled;
      case LogicalKeyboardKey.keyT:
        ws.prefs.toggleTheme();
        return KeyEventResult.handled;
      case LogicalKeyboardKey.keyG:
        if (doc != null && doc.decoded) {
          doc.filters.hideGoodCrc = !doc.filters.hideGoodCrc;
          unawaited(ws.ready.then((e) => doc.applyFilters(e)));
        }
        return KeyEventResult.handled;
      case LogicalKeyboardKey.escape:
        if (searchFocus.hasFocus) {
          searchFocus.unfocus();
          return KeyEventResult.handled;
        }
        ws.prefs.detailCollapsed = true;
        return KeyEventResult.handled;
      default:
        return KeyEventResult.ignored;
    }
  }

  /// 外壳来的命令（原生菜单）。
  ///
  /// 命令名与 `runner/shell.cpp` 的菜单 id 一一对应；**没认出来的静默忽略** ——
  /// 新版本外壳配旧版本界面时不该炸掉。
  void _runShellCommand(String command) {
    if (!mounted) return;
    switch (command) {
      case 'openFile':
        unawaited(openFilesViaDialog(ws));
        break;
      case 'connectDevice':
        // 三个入口（顶栏按钮 / 文件菜单 · Ctrl+D / 空态按钮）都落到这一条命令上。
        ws.openLive();
        break;
      case 'closeCurrent':
        if (ws.activeIndex >= 0) unawaited(ws.closeAt(ws.activeIndex));
        break;
      case 'closeAll':
        unawaited(ws.closeAll());
        break;
      case 'exportCsv':
        unawaited(exportViaDialog(context, ws, ws.active, 'csv'));
        break;
      case 'exportJson':
        unawaited(exportViaDialog(context, ws, ws.active, 'json'));
        break;
      case 'search':
        searchFocus.requestFocus();
        break;
      case 'toggleDense':
        ws.prefs.compact = !ws.prefs.compact;
        break;
      case 'toggleTheme':
        ws.prefs.toggleTheme();
        break;
      case 'toggleFilters':
        ws.prefs.filtersCollapsed = !ws.prefs.filtersCollapsed;
        break;
      case 'toggleDetail':
        ws.prefs.detailCollapsed = !ws.prefs.detailCollapsed;
        break;
      case 'resetLayout':
        ws.prefs
          ..filtersCollapsed = false
          ..detailCollapsed = false
          ..rowH = kRowHeightNormal
          ..resetDetailWidth()
          ..resetTimelineHeight();
        break;
      case 'about':
        showAboutPdScope(context);
        break;
      default:
        break;
    }
  }

  /// 采集中导出 CSV 快照 —— **复用离线那条出口**（open_files.dart 的
  /// exportViaDialog），不在这里再写一份：那个函数里已经处理了实时分支，
  /// 顶栏导出菜单、外壳原生菜单、状态条按钮因此共用同一套口径与提示语。
  Future<void> _exportLiveCsv(BuildContext context, CaptureDocument doc) =>
      exportViaDialog(context, ws, doc, 'csv');

  /// 导出通讯日志（制表符分隔的纯文本）。
  ///
  /// 顶层函数而不是 _AppShellState 的方法：状态条的按钮、底部区的头部
  /// 都要能调到它，而 _Body 是无状态组件，拿不到外层的 State。
  static Future<void> _exportTrace(
    BuildContext context,
    CaptureDocument doc,
  ) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final l = doc.live;
    if (l == null || l.trace.isEmpty) {
      messenger?.showSnackBar(const SnackBar(content: Text('还没有通讯记录')));
      return;
    }
    final b = StringBuffer('时间(ms)\t方向\t字节\t说明\n');
    for (final e in l.trace) {
      b.write(
        '${e.ms.toStringAsFixed(3)}\t${e.tx ? 'TX' : 'RX'}\t${e.bytes}\t${e.note}\n',
      );
    }
    final path = await saveTextAs(
      text: b.toString(),
      suggestedName:
          '通讯日志-${doc.liveCsvName().replaceAll(RegExp(r'\.csv$'), '')}.txt',
      extension: 'txt',
      typeLabel: '文本',
    );
    messenger?.showSnackBar(
      SnackBar(content: Text(path == null ? '已取消' : '已导出 $path')),
    );
  }

  /// 把「界面现在什么样」报给外壳：窗口标题 + 菜单项的灰/勾选。
  ///
  /// 只在**真的变了**的时候发 —— 解码过程中工作区会通知很多次，每次都发一遍
  /// 平台消息没有必要。
  void _reportShellState() {
    final prefs = ws.prefs;
    final active = ws.active;
    final title = active == null
        ? 'PDScope'
        : 'PDScope — ${active.displayName}';
    final key =
        '$title|${ws.docs.isNotEmpty}'
        '|${prefs.filtersCollapsed}|${prefs.detailCollapsed}';
    if (key == _reportedShellState) return;
    _reportedShellState = key;
    unawaited(
      ShellBridge.reportState(
        hasDocument: ws.docs.isNotEmpty,
        filtersShown: !prefs.filtersCollapsed,
        detailShown: !prefs.detailCollapsed,
        title: title,
      ),
    );
  }
}

/// 「工作区 + 当前文档」的合成重绘源。
///
/// ⚠ 这件事**不能**写成下游的一个懒表达式（比如在 `AnimatedBuilder` 的
///   `animation:` 上现取现拼 `Listenable.merge([ws, ws.active])`）。那个表达式只在
///   **State 的 `build()` 里**求值，而 `AnimatedBuilder` 自己收到通知后的重建
///   **不经过 State 的 `build()`** —— 于是合并结果会一直停在「还没有文档」那一版，
///   文档的订阅永远接不上。现象极具误导性：工作区通知（打开文件、解码结束）都正常，
///   只有**文档自己的**通知（分页取回来、详情回来、模拟量轨迹回来）全都打空，
///   看起来就是「表格要滚一下才出内容」「VBUS / IBUS 一直转圈」。
///
/// 所以订阅要**主动驱动**：工作区一通知就重新对一下当前文档，顺手把通知转发出去。
class _RepaintSource extends ChangeNotifier {
  _RepaintSource(this._ws) {
    _ws.addListener(_onWorkspace);
    _attach(_ws.active);
  }

  final Workspace _ws;
  CaptureDocument? _doc;

  void _onWorkspace() {
    _attach(_ws.active);
    notifyListeners();
  }

  void _attach(CaptureDocument? doc) {
    if (identical(doc, _doc)) return;
    _doc?.removeListener(_bubble);
    _doc = doc;
    _doc?.addListener(_bubble);
  }

  void _bubble() => notifyListeners();

  @override
  void dispose() {
    _ws.removeListener(_onWorkspace);
    _doc?.removeListener(_bubble);
    _doc = null;
    super.dispose();
  }
}

class _Body extends StatelessWidget {
  const _Body({required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument? doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final prefs = workspace.prefs;
    final d = doc;

    if (d == null) return _EmptyState(workspace: workspace);

    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (!prefs.filtersCollapsed)
          FilterPanel(workspace: workspace, doc: d)
        else
          CollapsedRail(
            label: '筛选',
            edge: TextDirection.ltr,
            onTap: () => prefs.filtersCollapsed = false,
          ),

        Expanded(
          child: Container(
            color: p.bg2,
            child: Column(
              children: [
                if (d.notice != null) _NoticeBar(text: d.notice!),
                // 停车 / 掉线：横幅而不是弹窗 —— 弹窗会遮住表格，
                // 而此刻用户最想做的恰恰是去看那些已经采到的报文。
                if (d.liveState == LiveState.parked) _LiveStopBanner(doc: d),
                if (d.liveState == LiveState.disconnected)
                  _LiveDropoutBanner(doc: d),
                Expanded(
                  // 还没开始采集：中列让给连接面板。骨架不变 ——
                  // 左右两栏仍是筛选与详情，只是中间暂时没有数据。
                  child: d.livePreCapture
                      ? ConnectPanel(doc: d)
                      : PacketTable(workspace: workspace, doc: d),
                ),
                _ResizeBar(
                  axis: Axis.vertical,
                  onDrag: (dy) => prefs.tlH -= dy,
                  onReset: prefs.resetTimelineHeight,
                  tooltip: '上下拖动改曲线区高度（↑↓ 微调，双击回到 158）',
                ),
                SizedBox(
                  height: prefs.tlH,
                  // 底部区双模式：只有实时文档能切到通讯日志。
                  child: d.isLive && d.showTrace
                      ? TracePanel(
                          trace: d.live?.trace ?? const [],
                          modeSwitch: BottomModeSwitch(
                            showTrace: true,
                            // _Body 是无状态组件 —— 走文档自己的通知（touch）
                            // 而不是 setState；_RepaintSource 已经在听它了。
                            onChanged: (v) {
                              d.showTrace = v;
                              d.touch();
                            },
                          ),
                          onExport: () =>
                              _AppShellState._exportTrace(context, d),
                          onClear: () {
                            d.live?.trace.clear();
                            d.touch();
                          },
                        )
                      : Timeline(
                          workspace: workspace,
                          doc: d,
                          modeSwitch: d.isLive
                              ? BottomModeSwitch(
                                  showTrace: false,
                                  onChanged: (v) {
                                    d.showTrace = v;
                                    d.touch();
                                  },
                                )
                              : null,
                        ),
                ),
              ],
            ),
          ),
        ),

        if (!prefs.detailCollapsed) ...[
          _ResizeBar(
            axis: Axis.horizontal,
            onDrag: (dx) => prefs.detailW -= dx,
            onReset: prefs.resetDetailWidth,
            tooltip: '左右拖动改详情宽度（← → 微调，双击回到 390）',
          ),
          SizedBox(
            width: prefs.detailW.toDouble(),
            child: DetailPanel(workspace: workspace, doc: d),
          ),
        ] else
          CollapsedRail(
            label: '详情',
            edge: TextDirection.rtl,
            onTap: () => prefs.detailCollapsed = false,
          ),
      ],
    );
  }
}

/// 收起后的竖栏把手。**读不到任何数据也能点** —— 这是它存在的全部理由。
class CollapsedRail extends StatelessWidget {
  const CollapsedRail({
    super.key,
    required this.label,
    required this.onTap,
    required this.edge,
  });

  final String label;
  final VoidCallback onTap;

  /// ltr = 站在左边（收起筛选栏），rtl = 站在右边（收起详情面板）。
  final TextDirection edge;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Tooltip(
      message: '展开$label',
      child: InkWell(
        onTap: onTap,
        child: Container(
          width: 22,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: p.panel,
            border: Border(
              left: BorderSide(
                color: edge == TextDirection.rtl ? p.line : Colors.transparent,
              ),
              right: BorderSide(
                color: edge == TextDirection.ltr ? p.line : Colors.transparent,
              ),
            ),
          ),
          child: RotatedBox(
            quarterTurns: 1,
            child: Text(
              label,
              style: TextStyle(fontSize: 11.5, letterSpacing: 3, color: p.tx2),
            ),
          ),
        ),
      ),
    );
  }
}

/// 拖动改尺寸的分隔条（详情宽度 / 曲线区高度各一条）。
///
/// [onDrag] 收到的是**屏幕坐标下的轴向增量**（水平给 dx、竖直给 dy），
/// 由调用方决定「变大」对应哪个方向 —— 竖直条拖动向上是让曲线区变高。
class _ResizeBar extends StatefulWidget {
  const _ResizeBar({
    required this.axis,
    required this.onDrag,
    required this.onReset,
    required this.tooltip,
  });

  final Axis axis;
  final void Function(double delta) onDrag;
  final VoidCallback onReset;
  final String tooltip;

  @override
  State<_ResizeBar> createState() => _ResizeBarState();
}

class _ResizeBarState extends State<_ResizeBar> {
  bool _hot = false;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final horizontal = widget.axis == Axis.horizontal;
    return MouseRegion(
      cursor: horizontal
          ? SystemMouseCursors.resizeColumn
          : SystemMouseCursors.resizeRow,
      onEnter: (_) => setState(() => _hot = true),
      onExit: (_) => setState(() => _hot = false),
      child: Tooltip(
        message: widget.tooltip,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onVerticalDragUpdate: horizontal
              ? null
              : (d) => widget.onDrag(d.delta.dy),
          onHorizontalDragUpdate: horizontal
              ? (d) => widget.onDrag(d.delta.dx)
              : null,
          onDoubleTap: widget.onReset,
          child: Container(
            width: horizontal ? 5 : null,
            height: horizontal ? null : 5,
            color: _hot ? p.accent.withValues(alpha: .35) : p.line,
          ),
        ),
      ),
    );
  }
}

/// 常驻提示条：讲清「这份文件为什么只解出一部分」。
class _NoticeBar extends StatelessWidget {
  const _NoticeBar({required this.text});
  final String text;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
      decoration: BoxDecoration(
        color: p.brightness == Brightness.dark
            ? const Color(0xFF2A2410)
            : const Color(0xFFFFF8E6),
        border: Border(bottom: BorderSide(color: p.line)),
      ),
      child: Row(
        children: [
          Icon(Icons.info_outline, size: 14, color: p.warn),
          const SizedBox(width: 7),
          Expanded(
            child: Text(
              text,
              style: TextStyle(fontSize: 12, color: p.tx2, height: 1.5),
            ),
          ),
        ],
      ),
    );
  }
}

/// 主动停车 / 设备掉线的横幅。
///
/// 用横幅而不是弹窗：弹窗会遮住表格，而此刻用户最想做的恰恰是
/// **去看那些已经采到的报文**（设计第 8 节）。动作按钮在状态条上，
/// 横幅只负责说清「出事了 / 为什么 / 已采的还在」。
class _LiveStopBanner extends StatelessWidget {
  const _LiveStopBanner({required this.doc});
  final CaptureDocument doc;

  @override
  Widget build(BuildContext context) => _liveBanner(
    context,
    doc,
    title: '已主动停止采集并断开设备',
    icon: Icons.block,
    tail: '不会自动重连：要不要重来由你决定，重新连接请先「断开设备」再查找。',
  );
}

class _LiveDropoutBanner extends StatelessWidget {
  const _LiveDropoutBanner({required this.doc});
  final CaptureDocument doc;

  @override
  Widget build(BuildContext context) => _liveBanner(
    context,
    doc,
    // 不复用状态条上那两个词（「设备已断开」）—— 那句最响的行应该补上**后果**，
    // 而不是把用户刚在状态条上读过的四个字再喊一遍。
    title: '设备已断开，采集已停止',
    icon: Icons.usb_off,
    tail: '重新连接请先「重新查找」。',
  );
}

Widget _liveBanner(
  BuildContext context,
  CaptureDocument doc, {
  required String title,
  required IconData icon,
  required String tail,
}) {
  final p = PaletteScope.of(context);
  final n = doc.live?.rowCount ?? 0;
  return Container(
    width: double.infinity,
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 11),
    decoration: BoxDecoration(
      color: p.bad.withValues(alpha: 0.12),
      border: Border(bottom: BorderSide(color: p.line)),
    ),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Icon(icon, size: 14, color: p.bad),
        const SizedBox(width: 7),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                title,
                style: TextStyle(
                  fontSize: 12,
                  fontWeight: FontWeight.w600,
                  color: p.bad,
                ),
              ),
              if (doc.liveMessage != null)
                Padding(
                  padding: const EdgeInsets.only(top: 3),
                  child: Text(
                    doc.liveMessage!,
                    style: TextStyle(fontSize: 12, color: p.tx2, height: 1.6),
                  ),
                ),
              Padding(
                padding: const EdgeInsets.only(top: 5),
                child: Text(
                  '已采到的 $n 条报文仍然完整可用 —— 可以直接筛选、看详情、导出 CSV。$tail',
                  style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.6),
                ),
              ),
            ],
          ),
        ),
      ],
    ),
  );
}

/// 一份文件都没开时的引导。
class _EmptyState extends StatelessWidget {
  const _EmptyState({required this.workspace});
  final Workspace workspace;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.usb_rounded, size: 42, color: p.tx3),
          const SizedBox(height: 14),
          Text('把抓包文件拖进来，或', style: TextStyle(fontSize: 13, color: p.tx2)),
          const SizedBox(height: 10),
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              OutlinedButton.icon(
                onPressed: () => unawaited(openFilesViaDialog(workspace)),
                icon: const Icon(Icons.folder_open, size: 16),
                label: const Text('打开文件'),
              ),
              const SizedBox(width: 10),
              // 第三个入口就在空态里 —— 首次使用的人一眼能看到「还能连设备」。
              OutlinedButton.icon(
                onPressed: workspace.openLive,
                icon: const Icon(Icons.usb_rounded, size: 16),
                label: const Text('连接设备'),
              ),
            ],
          ),
          const SizedBox(height: 18),
          Text(
            '支持 .atkcc、.sqlite、.pdStream、.ufcsStream\n'
            '也可以接上分析仪边抓边看（Ctrl+D）',
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.7),
          ),
        ],
      ),
    );
  }
}

/// 底部状态栏：串行队列当前在做什么 + 本份的规模摘要。
class _StatusBar extends StatelessWidget {
  const _StatusBar({required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument? doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final d = doc;
    final parts = <String>[];
    if (d?.meta != null) {
      // 实时文档**没有文件**：说「文件 0 字节」既没用又容易让人以为没读到东西。
      // 换成设备与传输方式，那才是这份数据真正的来源。
      if (d!.isLive) {
        final dev = d.liveDevice;
        parts.add(dev == null ? '实时采集' : '设备 ${dev.name} · ${dev.transport}');
      } else {
        parts.add('文件 ${d.meta!.fileBytes} 字节');
      }
      parts.add('报文 ${d.rows.total}');
      if (d.decoded && d.stats != null) {
        parts.add('总采样 ${d.stats!.totalSamples}');
        parts.add('时长 ${d.spanSec.toStringAsFixed(3)} s');
      }
    }
    return Container(
      height: 24,
      padding: const EdgeInsets.symmetric(horizontal: 12),
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(top: BorderSide(color: p.line)),
      ),
      child: Row(
        children: [
          if (workspace.busy) ...[
            SizedBox(
              width: 10,
              height: 10,
              child: CircularProgressIndicator(
                strokeWidth: 1.6,
                color: p.accent,
              ),
            ),
            const SizedBox(width: 7),
            Text(
              workspace.currentJob,
              style: TextStyle(fontSize: 11.5, color: p.tx2),
            ),
          ] else
            Text('就绪', style: TextStyle(fontSize: 11.5, color: p.tx3)),
          const Spacer(),
          Text(
            parts.join(' · '),
            style: TextStyle(fontSize: 11.5, color: p.tx3),
          ),
        ],
      ),
    );
  }
}
