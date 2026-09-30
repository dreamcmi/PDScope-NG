// timeline.dart — 底部模拟量时间轴
//
// 三件事：
//   · **刷选**：在曲线区拖动选一段时间，表格立即联动（写的是同一个归一化时间窗口，
//     所以和左侧滑杆、悬停读数是同一套坐标）。
//   · **两档**：电压/电流 ↔ 差分线（POWER-Z 是 CC1/CC2，UFCS 是 DP/DM）。
//     两档量程差一个数量级，叠在一起会糊，所以分开；ATK-C 只有两路，这一档自动隐藏。
//   · **横轴刻度随时长自适应**（不到 1 秒写 ms、不到 60 秒写 s、更长写 m:ss）。
//
// 纵轴缩放/平移：Ctrl/⌘ + 滚轮缩放（以光标处为锚点）、上下拖动平移、双击复位。
// 普通滚轮不做事 —— 在曲线上滚动却改了纵轴会很意外。
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/document.dart';
import '../core/formatting.dart';
import '../core/models.dart';
import '../core/palette.dart';
import '../core/workspace.dart';

const double _timelinePlotLeft = 46;
const double _timelinePlotTop = 4;
const double _timelinePlotRight = 6;
const double _timelinePlotBottom = 18;
const double _timelineDragAxisThreshold = 3;

Rect _timelinePlot(Size size) => Rect.fromLTRB(
  _timelinePlotLeft,
  _timelinePlotTop,
  size.width - _timelinePlotRight,
  size.height - _timelinePlotBottom,
);

enum _TimelineDragAxis { horizontal, vertical }

class Timeline extends StatefulWidget {
  const Timeline({super.key, required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument doc;

  @override
  State<Timeline> createState() => _TimelineState();
}

class _TimelineState extends State<Timeline> {
  double get _yZoom => doc.timelineView.zoom;
  set _yZoom(double value) => doc.timelineView.zoom = value;

  double get _yPan => doc.timelineView.pan;
  set _yPan(double value) => doc.timelineView.pan = value;

  Offset? _brushStart;
  Offset? _brushEnd;
  double? _hoverX;
  int? _secondaryPanPointer;
  double? _secondaryPanY;
  Offset? _dragStartPosition;
  double? _dragStartPan;
  double? _dragStartZoom;
  _TimelineDragAxis? _dragAxis;
  bool _followSelected = true;
  bool _loaded = false;
  int? _loadedDocId;
  int? _loadedChannel;

  CaptureDocument get doc => widget.doc;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _maybeLoad();
  }

  @override
  void didUpdateWidget(Timeline old) {
    super.didUpdateWidget(old);
    // 纵轴状态由文档自己保存；瞬时手势由 _maybeLoad 在身份变化时清理。
    _maybeLoad();
  }

  void _maybeLoad() {
    if (_loadedDocId != doc.id || _loadedChannel != doc.channel) {
      _loaded = false;
      _loadedDocId = doc.id;
      _loadedChannel = doc.channel;
      _clearInteractionState();
    }
    if (_loaded || !doc.decoded) return;
    _loaded = true;
    if (doc.meta?.hasBus == true) {
      widget.workspace.ready.then((e) => doc.loadBus(e)).ignore();
    }
  }

  bool get _isAdjusted => (_yZoom - 1).abs() > 1e-3 || _yPan.abs() > 1e-3;

  void _resetView() {
    setState(() {
      doc.filters.tFrom = 0;
      doc.filters.tTo = 1;
      _yZoom = 1;
      _yPan = 0;
    });
    doc.touch();
    widget.workspace.ready.then((e) => doc.applyFilters(e)).ignore();
  }

  double _clampZoom(double zoom) => zoom.clamp(0.25, 200.0).toDouble();

  double _clampPan(double pan) {
    final hi = math.max(-0.5, math.min(3.0, 4 - 1 / _yZoom));
    return pan.clamp(-0.5, hi).toDouble();
  }

  void _panBy(double dy, double plotHeight) {
    if (plotHeight <= 0 || dy == 0) return;
    _yPan = _clampPan(_yPan + dy / (_yZoom * plotHeight));
  }

  void _clearInteractionState() {
    _hoverX = null;
    _brushStart = null;
    _brushEnd = null;
    _secondaryPanPointer = null;
    _secondaryPanY = null;
    _dragStartPosition = null;
    _dragStartPan = null;
    _dragStartZoom = null;
    _dragAxis = null;
  }

  void _startSecondaryPan(PointerDownEvent e) {
    if (e.kind != PointerDeviceKind.mouse ||
        (e.buttons & kSecondaryMouseButton) == 0) {
      return;
    }
    setState(() {
      _secondaryPanPointer = e.pointer;
      _secondaryPanY = e.localPosition.dy;
      _brushStart = null;
      _brushEnd = null;
      _dragStartPosition = null;
      _dragStartPan = null;
      _dragStartZoom = null;
      _dragAxis = null;
    });
  }

  void _updateSecondaryPan(PointerMoveEvent e, double plotHeight) {
    if (e.pointer != _secondaryPanPointer) return;
    if ((e.buttons & kSecondaryMouseButton) == 0) {
      _finishSecondaryPan(e);
      return;
    }
    final lastY = _secondaryPanY;
    if (lastY == null) return;
    final dy = e.localPosition.dy - lastY;
    if (dy == 0) return;
    setState(() {
      _panBy(dy, plotHeight);
      _secondaryPanY = e.localPosition.dy;
    });
  }

  void _finishSecondaryPan(PointerEvent e) {
    if (e.pointer != _secondaryPanPointer) return;
    setState(() {
      _secondaryPanPointer = null;
      _secondaryPanY = null;
    });
  }

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    final series = doc.bus;
    final hasAux = series?.hasAux ?? false;

    return Container(
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(top: BorderSide(color: p.line)),
      ),
      child: Column(
        children: [
          _header(p, hasAux),
          Expanded(
            child: !doc.decoded
                ? Center(
                    child: Text(
                      '解码后显示模拟量轨迹',
                      style: TextStyle(fontSize: 11.5, color: p.tx3),
                    ),
                  )
                : (doc.meta?.hasBus != true ? _noBus(p) : _busArea(p)),
          ),
        ],
      ),
    );
  }

  /// 曲线区：**取过了才敢说「没有」**。
  ///
  /// ⚠ 先前这里只判 `series == null` 就转圈。一旦请求失败或返回空，
  ///   转圈就**永远停不下来** —— 数据永远到不了，条件永远成立，而界面上看不出
  ///   是「还在取」还是「根本取不到」。所以要三态：有数据 / 还在取 / 取过了没有。
  Widget _busArea(Palette p) {
    final s = doc.bus;
    if (s != null && !s.isEmpty) return _chart(p, s);
    if (!doc.busAttempted) {
      // 还没发起请求（`_maybeLoad` 是异步的）或正在取 —— 两种都该转圈。
      return const Center(
        child: SizedBox(
          width: 16,
          height: 16,
          child: CircularProgressIndicator(strokeWidth: 2),
        ),
      );
    }
    return _noBus(p);
  }

  Widget _header(Palette p, bool hasAux) {
    final series = doc.bus;
    final auxLabels = (series?.labels.isNotEmpty ?? false)
        ? series!.labels.join(' / ')
        : (doc.isUfcs ? 'DP / DM' : 'CC 线');
    return Container(
      height: 28,
      padding: const EdgeInsets.symmetric(horizontal: 10),
      child: Row(
        children: [
          Text(
            doc.tlMode == TlMode.aux ? auxLabels : 'VBUS / IBUS',
            style: TextStyle(
              fontSize: 11.5,
              fontWeight: FontWeight.w600,
              color: p.tx2,
            ),
          ),
          const SizedBox(width: 10),
          if (hasAux)
            _seg(
              p,
              ['电压/电流', '差分线'],
              doc.tlMode.index,
              (i) => setState(() {
                doc.tlMode = TlMode.values[i];
              }),
            ),
          const Spacer(),
          if (_hoverX != null && series != null) _readout(p, series),
          InkWell(
            onTap: () => setState(() => _followSelected = !_followSelected),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(
                  _followSelected ? Icons.toggle_on : Icons.toggle_off,
                  size: 20,
                  color: _followSelected ? p.accent : p.tx3,
                ),
                Text('跟随选中', style: TextStyle(fontSize: 10.5, color: p.tx2)),
              ],
            ),
          ),
          const SizedBox(width: 8),
          Tooltip(
            message: _isAdjusted
                ? '纵轴已缩放 ${_yZoom.toStringAsFixed(2)}×'
                : '复位时间范围与纵轴',
            child: OutlinedButton(
              onPressed: _resetView,
              style: OutlinedButton.styleFrom(
                minimumSize: Size.zero,
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                side: BorderSide(color: _isAdjusted ? p.accent : p.line),
              ),
              child: Text(
                '复位视图',
                style: TextStyle(
                  fontSize: 11,
                  color: _isAdjusted ? p.accent : p.tx2,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _readout(Palette p, BusSeries s) {
    final span = doc.spanSec;
    final fraction =
        ((_hoverX! - _timelinePlotLeft) / math.max(_lastPlotWidth, 1))
            .clamp(0.0, 1.0)
            .toDouble();
    final t = fraction * span;
    final i = ((t - s.t0) / (s.step <= 0 ? 1 : s.step))
        .round()
        .clamp(0, math.max(s.n - 1, 0))
        .toInt();
    final parts = <String>[fmtReadout(t, span)];
    if (s.n > 0) {
      parts.add('${fmtVolt(s.vbus[i])} · ${fmtAmp(s.ibus[i])}');
      if (doc.tlMode == TlMode.aux && s.ca != null && s.cb != null) {
        parts.add(
          '${s.ca![i].toStringAsFixed(2)} / ${s.cb![i].toStringAsFixed(2)}',
        );
      }
    }
    return Text(
      parts.join('   '),
      style: p.mono.copyWith(fontSize: 11, color: p.tx2),
    );
  }

  Widget _noBus(Palette p) => Center(
    child: Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text('没有模拟量轨迹', style: TextStyle(fontSize: 12, color: p.tx2)),
        const SizedBox(height: 5),
        Text('这份文件不含电压 / 电流采样', style: TextStyle(fontSize: 11, color: p.tx3)),
      ],
    ),
  );

  double _lastPlotWidth = 1;

  Widget _chart(Palette p, BusSeries s) {
    return LayoutBuilder(
      builder: (context, c) {
        final size = Size(c.maxWidth, c.maxHeight);
        final plot = _timelinePlot(size);
        _lastPlotWidth = plot.width;
        return Listener(
          onPointerDown: _startSecondaryPan,
          onPointerMove: (e) => _updateSecondaryPan(e, plot.height),
          onPointerUp: _finishSecondaryPan,
          onPointerCancel: _finishSecondaryPan,
          onPointerSignal: (e) {
            if (e is! PointerScrollEvent) return;
            if (!HardwareKeyboard.instance.isControlPressed &&
                !HardwareKeyboard.instance.isMetaPressed) {
              return; // 普通滚轮不做事，避免误改纵轴
            }
            if (plot.height <= 0) return;
            final y = ((e.localPosition.dy - plot.top) / plot.height)
                .clamp(0.0, 1.0)
                .toDouble();
            final anchor = _yPan + (1 - y) / _yZoom;
            setState(() {
              _yZoom = _clampZoom(
                _yZoom * math.exp(-e.scrollDelta.dy * 0.0016),
              );
              _yPan = _clampPan(anchor - (1 - y) / _yZoom);
            });
          },
          child: MouseRegion(
            onHover: (e) => setState(() {
              final x = e.localPosition.dx;
              _hoverX = x >= plot.left && x <= plot.right ? x : null;
            }),
            onExit: (_) => setState(() => _hoverX = null),
            child: GestureDetector(
              dragStartBehavior: DragStartBehavior.down,
              // Register secondary clicks so right-drag can pan without opening a context menu.
              onSecondaryTap: () {},
              onTapUp: (d) {
                if (doc.rows.total == 0) return;
                if (plot.width <= 0) return;
                final fraction = ((d.localPosition.dx - plot.left) / plot.width)
                    .clamp(0.0, 1.0)
                    .toDouble();
                widget.workspace.prefs.detailCollapsed = false;
                widget.workspace.ready
                    .then(
                      (e) => doc.selectNearestAtTime(e, fraction * doc.spanSec),
                    )
                    .ignore();
              },
              onDoubleTap: () => setState(() {
                _yZoom = 1;
                _yPan = 0;
              }),
              onPanStart: (d) => setState(() {
                final inGutter =
                    d.localPosition.dx < plot.left ||
                    d.localPosition.dx > plot.right;
                final forceVertical =
                    HardwareKeyboard.instance.isShiftPressed || inGutter;
                _dragStartPosition = d.localPosition;
                _dragStartPan = _yPan;
                _dragStartZoom = _yZoom;
                _dragAxis = forceVertical ? _TimelineDragAxis.vertical : null;
                _brushStart = null;
                _brushEnd = null;
              }),
              onPanUpdate: (d) => setState(() {
                final start = _dragStartPosition;
                if (start == null) return;
                final dx = d.localPosition.dx - start.dx;
                final dy = d.localPosition.dy - start.dy;
                if (_dragAxis == null) {
                  if (math.max(dx.abs(), dy.abs()) <
                      _timelineDragAxisThreshold) {
                    return;
                  }
                  // 首次明显移动决定方向，随后锁定，避免斜拖在刷选与平移间跳变。
                  _dragAxis = dy.abs() > dx.abs()
                      ? _TimelineDragAxis.vertical
                      : _TimelineDragAxis.horizontal;
                  if (_dragAxis == _TimelineDragAxis.horizontal) {
                    _brushStart = start;
                    _brushEnd = d.localPosition;
                  }
                }
                if (_dragAxis == _TimelineDragAxis.vertical) {
                  final startPan = _dragStartPan ?? _yPan;
                  final startZoom = _dragStartZoom ?? _yZoom;
                  if (plot.height > 0) {
                    _yPan = _clampPan(
                      startPan + dy / (startZoom * plot.height),
                    );
                  }
                } else {
                  _brushEnd = d.localPosition;
                }
              }),
              onPanCancel: () => setState(() {
                _brushStart = null;
                _brushEnd = null;
                _dragStartPosition = null;
                _dragStartPan = null;
                _dragStartZoom = null;
                _dragAxis = null;
              }),
              onPanEnd: (_) {
                final wasPanning = _dragAxis == _TimelineDragAxis.vertical;
                final wasBrushing = _dragAxis == _TimelineDragAxis.horizontal;
                final a = _brushStart;
                final b = _brushEnd;
                setState(() {
                  _brushStart = null;
                  _brushEnd = null;
                  _dragStartPosition = null;
                  _dragStartPan = null;
                  _dragStartZoom = null;
                  _dragAxis = null;
                });
                if (wasPanning || !wasBrushing) return;
                if (a == null || b == null) return;
                if (plot.width <= 0) return;
                final lo = ((math.min(a.dx, b.dx) - plot.left) / plot.width)
                    .clamp(0.0, 1.0)
                    .toDouble();
                final hi = ((math.max(a.dx, b.dx) - plot.left) / plot.width)
                    .clamp(0.0, 1.0)
                    .toDouble();
                // 太窄的手势当误触，不当作刷选。
                if (hi - lo < 0.004) return;
                doc.filters.tFrom = lo;
                doc.filters.tTo = hi;
                doc.touch();
                widget.workspace.ready
                    .then((e) => doc.applyFilters(e))
                    .ignore();
              },
              child: CustomPaint(
                painter: _TimelinePainter(
                  p: p,
                  series: s,
                  marks: doc.marks,
                  mode: doc.tlMode,
                  span: doc.spanSec,
                  yZoom: _yZoom,
                  yPan: _yPan,
                  brush: _brushStart != null && _brushEnd != null
                      ? Rect.fromPoints(_brushStart!, _brushEnd!)
                      : null,
                  hoverX: _hoverX,
                  windowFrom: doc.filters.tFrom,
                  windowTo: doc.filters.tTo,
                  selectedTime:
                      _followSelected &&
                          doc.selected != null &&
                          doc.selected! >= 0 &&
                          doc.selected! < doc.marks.n
                      ? doc.marks.ts[doc.selected!]
                      : null,
                ),
                size: Size.infinite,
              ),
            ),
          ),
        );
      },
    );
  }

  Widget _seg(
    Palette p,
    List<String> options,
    int selected,
    ValueChanged<int> onChanged,
  ) {
    return Container(
      decoration: BoxDecoration(
        color: p.panel2,
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: p.line),
      ),
      padding: const EdgeInsets.all(1.5),
      child: Row(
        children: [
          for (var i = 0; i < options.length; i++)
            InkWell(
              onTap: () => onChanged(i),
              borderRadius: BorderRadius.circular(4),
              child: Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 7,
                  vertical: 1.5,
                ),
                decoration: BoxDecoration(
                  color: i == selected ? p.accent : Colors.transparent,
                  borderRadius: BorderRadius.circular(4),
                ),
                child: Text(
                  options[i],
                  style: TextStyle(
                    fontSize: 10.5,
                    color: i == selected ? Colors.white : p.tx2,
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _TimelinePainter extends CustomPainter {
  _TimelinePainter({
    required this.p,
    required this.series,
    required this.marks,
    required this.mode,
    required this.span,
    required this.yZoom,
    required this.yPan,
    required this.brush,
    required this.hoverX,
    required this.windowFrom,
    required this.windowTo,
    required this.selectedTime,
  });

  final Palette p;
  final BusSeries series;
  final PacketMarks marks;
  final TlMode mode;
  final double span;
  final double yZoom;
  final double yPan;
  final Rect? brush;
  final double? hoverX;
  final double windowFrom;
  final double windowTo;
  final double? selectedTime;

  @override
  void paint(Canvas canvas, Size size) {
    final plot = _timelinePlot(size);
    if (plot.width <= 4 || plot.height <= 4) return;

    _paintWindow(canvas, plot);
    _paintSeries(canvas, plot);
    _paintMarks(canvas, plot);
    // 遮罩画在曲线之后：压暗的是「当前没在看」的那两段，别把曲线本身盖住。
    _paintWindowMask(canvas, plot);
    _paintSelected(canvas, plot);
    _paintXAxis(canvas, plot, size);
    _paintHover(canvas, plot);
    _paintBrush(canvas, plot);
  }

  void _paintSelected(Canvas canvas, Rect plot) {
    final t = selectedTime;
    if (t == null || span <= 0) return;
    final x = plot.left + (t / span) * plot.width;
    if (x < plot.left || x > plot.right) return;
    final paint = Paint()
      ..color = p.accent
      ..strokeWidth = 1.4;
    canvas.drawLine(Offset(x, plot.top), Offset(x, plot.bottom), paint);
    canvas.drawCircle(Offset(x, plot.top + 4), 2.6, paint);
  }

  /// 报文标记：按像素列**归并**后再画。
  ///
  /// ⚠ 别对每条报文各画一根线：UFCS 那种 26000 条的样本，一帧就是两万多次 draw，
  /// 拖动窗口时直接掉帧。归并后列数不超过绘图区宽度，画的是几十到一千条。
  void _paintMarks(Canvas canvas, Rect plot) {
    final m = marks;
    if (m.isEmpty || span <= 0) return;
    final w = plot.width.round();
    if (w <= 0) return;

    final colKind = Uint8List(w); // 0 = 该列没有报文；否则是类别编号 + 1
    final colBad = Uint8List(w);
    final colPaired = Uint8List(w);
    for (var i = 0; i < m.n; i++) {
      final x = ((m.ts[i] / span) * plot.width).floor();
      if (x < 0 || x >= w) continue;
      colKind[x] = m.kind[i] + 1;
      if (m.isBadCrc(i)) colBad[x] = 1;
      if (m.isPaired(i)) colPaired[x] = 1;
    }

    final tickPaint = Paint()..strokeWidth = 1;
    final tickTop = plot.bottom - 7;
    for (var x = 0; x < w; x++) {
      if (colKind[x] == 0 && colBad[x] == 0) continue;
      final px = plot.left + x + 0.5;
      if (colBad[x] != 0) {
        // CRC 未通过的报文：贯穿整高的一条红线，翻到哪都看得见。
        canvas.drawLine(
          Offset(px, plot.top),
          Offset(px, plot.bottom),
          Paint()
            ..color = p.bad.withValues(alpha: .55)
            ..strokeWidth = 1,
        );
      }
      if (colKind[x] != 0) {
        tickPaint.color = p
            .forKind(kindNameFromCode(colKind[x] - 1))
            .withValues(alpha: colPaired[x] != 0 ? 1 : .75);
        canvas.drawLine(
          Offset(px, tickTop),
          Offset(px, plot.bottom),
          tickPaint,
        );
      }
    }
  }

  /// 曲线区之外的底色略深一点，让「数据区」和坐标区一眼分开。
  void _paintWindow(Canvas canvas, Rect plot) {
    canvas.drawRect(plot, Paint()..color = p.bg2);
    final grid = Paint()
      ..color = p.line2
      ..strokeWidth = 1;
    for (var i = 1; i < 5; i++) {
      final x = plot.left + plot.width * i / 5;
      canvas.drawLine(Offset(x, plot.top), Offset(x, plot.bottom), grid);
    }
  }

  void _paintSeries(Canvas canvas, Rect plot) {
    if (series.n < 2) return;

    final useAux = mode == TlMode.aux && series.ca != null && series.cb != null;
    final mainMax = _niceMax(useAux ? series.camax : series.vmax);
    final secondMax = _niceMax(useAux ? series.cbmax : series.imax);

    // 刻度与旧版一致：pan 是自适应满量程单位，zoom 越大可见值范围越小。
    _paintYAxis(canvas, plot, mainMax);

    double yFor(double v, double max) {
      if (max <= 0) return plot.bottom;
      final norm = (v / max - yPan) * yZoom;
      return plot.bottom - norm.clamp(-2.0, 3.0) * plot.height;
    }

    // 主序列
    _polyline(
      canvas,
      plot,
      useAux ? series.ca! : series.vbus,
      (v) => yFor(v, mainMax),
      p.accent,
    );
    // 第二路：**用自己的量程铺满同一个像素区域**。两路单位不同、
    // 量级也可能差一个数量级，所以颜色区分 + 悬停读数分别带单位，不共用一条刻度。
    _polyline(
      canvas,
      plot,
      useAux ? series.cb! : series.ibus,
      (v) => yFor(v, secondMax),
      p.data,
    );
  }

  void _polyline(
    Canvas canvas,
    Rect plot,
    List<double> data,
    double Function(double) yFor,
    Color color,
  ) {
    final paint = Paint()
      ..color = color
      ..strokeWidth = 1.2
      ..style = PaintingStyle.stroke
      ..isAntiAlias = true;
    final path = Path();
    final n = data.length;
    final stepX = plot.width / math.max(n - 1, 1);
    for (var i = 0; i < n; i++) {
      final x = plot.left + stepX * i;
      final y = yFor(data[i]);
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    canvas.drawPath(path, paint);
  }

  void _paintYAxis(Canvas canvas, Rect plot, double baseMax) {
    final tp = TextPainter(textDirection: TextDirection.ltr);
    final visibleMax = baseMax * math.max(yPan.abs(), (yPan + 1 / yZoom).abs());
    for (var i = 0; i <= 4; i++) {
      final v = baseMax * (yPan + (i / 4) / yZoom);
      final y = plot.bottom - plot.height * i / 4;
      tp.text = TextSpan(
        text: fmtAxis(v, visibleMax),
        style: TextStyle(fontSize: 9.5, color: p.tx3),
      );
      tp.layout();
      tp.paint(canvas, Offset(plot.left - tp.width - 6, y - tp.height / 2));
      final grid = Paint()
        ..color = p.line2
        ..strokeWidth = 1;
      canvas.drawLine(Offset(plot.left, y), Offset(plot.right, y), grid);
    }
  }

  /// 横轴：五等分，单位随时长自适应（与左侧时间窗口滑杆同一套坐标）。
  void _paintXAxis(Canvas canvas, Rect plot, Size size) {
    final tp = TextPainter(textDirection: TextDirection.ltr);
    for (var i = 0; i <= 5; i++) {
      final f = i / 5;
      final x = plot.left + plot.width * f;
      final t = span * f;
      tp.text = TextSpan(
        text: fmtTick(t, span),
        style: TextStyle(fontSize: 9.5, color: p.tx3),
      );
      tp.layout();
      final tx = (x - tp.width / 2).clamp(0.0, size.width - tp.width);
      tp.paint(canvas, Offset(tx, plot.bottom + 3));
    }
  }

  /// 当前时间窗口之外的区域压暗：一眼看出「现在只看了中间一段」。
  void _paintWindowMask(Canvas canvas, Rect plot) {
    if (windowFrom <= 0 && windowTo >= 1) return;
    final mask = Paint()..color = p.bg.withValues(alpha: .55);
    final lo = plot.left + plot.width * windowFrom;
    final hi = plot.left + plot.width * windowTo;
    if (lo > plot.left) {
      canvas.drawRect(
        Rect.fromLTRB(plot.left, plot.top, lo, plot.bottom),
        mask,
      );
    }
    if (hi < plot.right) {
      canvas.drawRect(
        Rect.fromLTRB(hi, plot.top, plot.right, plot.bottom),
        mask,
      );
    }
    final edge = Paint()
      ..color = p.accent.withValues(alpha: .7)
      ..strokeWidth = 1;
    canvas.drawLine(Offset(lo, plot.top), Offset(lo, plot.bottom), edge);
    canvas.drawLine(Offset(hi, plot.top), Offset(hi, plot.bottom), edge);
  }

  void _paintHover(Canvas canvas, Rect plot) {
    final x = hoverX;
    if (x == null || x < plot.left || x > plot.right) return;
    canvas.drawLine(
      Offset(x, plot.top),
      Offset(x, plot.bottom),
      Paint()
        ..color = p.tx3.withValues(alpha: .5)
        ..strokeWidth = 1,
    );
  }

  void _paintBrush(Canvas canvas, Rect plot) {
    final b = brush;
    if (b == null) return;
    final r = Rect.fromLTRB(
      b.left.clamp(plot.left, plot.right),
      plot.top,
      b.right.clamp(plot.left, plot.right),
      plot.bottom,
    );
    canvas.drawRect(r, Paint()..color = p.accent.withValues(alpha: .18));
    canvas.drawRect(
      r,
      Paint()
        ..color = p.accent
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1,
    );
  }

  /// 把量程抬到一个「整」的数上，刻度数字才好看。
  double _niceMax(double v) {
    if (!v.isFinite || v <= 0) return 1;
    final mag = math.pow(10, (math.log(v) / math.ln10).floor()).toDouble();
    final n = v / mag;
    final stepped = n <= 1
        ? 1.0
        : n <= 2
        ? 2.0
        : n <= 5
        ? 5.0
        : 10.0;
    return stepped * mag;
  }

  @override
  bool shouldRepaint(_TimelinePainter old) =>
      old.series != series ||
      old.marks != marks ||
      old.mode != mode ||
      old.span != span ||
      old.yZoom != yZoom ||
      old.yPan != yPan ||
      old.brush != brush ||
      old.hoverX != hoverX ||
      old.windowFrom != windowFrom ||
      old.windowTo != windowTo ||
      old.selectedTime != selectedTime;
}
