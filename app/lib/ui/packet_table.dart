// packet_table.dart — 报文表（虚拟滚动）
//
// 列与宽度：
//   # / SOP / 报文类型 / ID / 方向 / Obj / 时间 / VBUS-IBUS / 数据hex / 解析详情
//
// 布局是「一条横向滚动 + 一条纵向虚拟列表」：
//   · 表头放在纵向列表**外面** ⇒ 天然吸顶，不用 sticky 技巧；
//   · 整块（表头 + 行）在同一个横向滚动容器里 ⇒ 横滚时列头跟着走；
//   · 行用 `ListView.builder` + `itemExtent` ⇒ 只建视口内的行，几万条报文不卡。
// 后两列按剩余宽度分配（对应 CSS 的 `minmax(200px,1fr)`），窗口太窄就横向滚动。
//
// ⚠ 点行取详情用的是 `PacketRow.index`，也就是**采集顺序里的下标**（核心的
//   `packets[i].index = i`），所以排序之后点谁就是谁。
import 'dart:async';

import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/filters.dart';
import '../core/models.dart';
import '../core/palette.dart';
import '../core/workspace.dart';

/// 固定列宽（与 CSS 的 `--cols` 前八列对齐）。
const double _wIndex = 56;
const double _wSop = 64;
const double _wType = 200;
const double _wId = 44;
const double _wRole = 76;
const double _wObj = 48;
const double _wTime = 116;
const double _wBus = 130;

/// 后两列的最小宽度（对应 CSS 的 minmax 下限）。
const double _minHex = 200;
const double _minSummary = 230;

const double _fixedSum =
    _wIndex + _wSop + _wType + _wId + _wRole + _wObj + _wTime + _wBus;

class PacketTable extends StatefulWidget {
  const PacketTable({super.key, required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument doc;

  @override
  State<PacketTable> createState() => _PacketTableState();
}

class _PacketTableState extends State<PacketTable> {
  final _v = ScrollController();
  int? _lastDocId;
  int? _lastSelection;

  @override
  void dispose() {
    _v.dispose();
    super.dispose();
  }

  CaptureDocument get doc => widget.doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    if (_lastDocId != doc.id) {
      _lastDocId = doc.id;
      _lastSelection = null;
    }
    final selected = doc.selectedViewIndex;
    if (_lastSelection != selected) {
      _lastSelection = selected;
      if (selected != null) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (!mounted || !_v.hasClients || doc.selectedViewIndex != selected) {
            return;
          }
          final top = selected * widget.workspace.prefs.rowH;
          final bottom = top + widget.workspace.prefs.rowH;
          final pos = _v.position;
          if (top < pos.pixels) {
            _v.jumpTo(
              (top - widget.workspace.prefs.rowH).clamp(0, pos.maxScrollExtent),
            );
          } else if (bottom > pos.pixels + pos.viewportDimension) {
            _v.jumpTo(
              (bottom - pos.viewportDimension + widget.workspace.prefs.rowH)
                  .clamp(0, pos.maxScrollExtent),
            );
          }
        });
      }
    }

    if (!doc.decoded) {
      return _Centered(
        text: doc.busy ? '正在解码…' : '等待解码',
        hint: doc.state == DocState.failed ? doc.error : null,
      );
    }
    if (doc.rows.total == 0) return _emptyState(p);

    return LayoutBuilder(
      builder: (context, c) {
        final widths = _columnWidths(c.maxWidth);
        final total = widths.fold<double>(0, (a, b) => a + b);
        return Scrollbar(
          controller: _v,
          child: SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: SizedBox(
              width: total,
              child: Column(
                children: [
                  _header(p, widths),
                  Expanded(
                    child: ListView.builder(
                      controller: _v,
                      itemExtent: widget.workspace.prefs.rowH,
                      itemCount: doc.rows.total,
                      itemBuilder: (context, i) => _row(context, p, widths, i),
                    ),
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }

  /// 后两列吃掉剩余宽度；不够就退回最小值，让外层横向滚动接手。
  List<double> _columnWidths(double avail) {
    final rest = avail - _fixedSum;
    if (rest >= _minHex + _minSummary) {
      final hex = rest * 0.32;
      return [
        _wIndex,
        _wSop,
        _wType,
        _wId,
        _wRole,
        _wObj,
        _wTime,
        _wBus,
        hex,
        rest - hex,
      ];
    }
    return [
      _wIndex,
      _wSop,
      _wType,
      _wId,
      _wRole,
      _wObj,
      _wTime,
      _wBus,
      _minHex,
      _minSummary,
    ];
  }

  Widget _header(Palette p, List<double> w) {
    const titles = [
      '#',
      'SOP',
      '报文类型',
      'ID',
      '方向',
      'Obj',
      '时间',
      'VBUS / IBUS',
      '数据 hex',
      '解析详情',
    ];
    final keys = [
      SortKey.index,
      SortKey.sop,
      SortKey.msgType,
      SortKey.msgId,
      SortKey.role,
      doc.isUfcs ? SortKey.dataLen : SortKey.nObjects,
      SortKey.timeMs,
      null,
      null,
      null,
    ];
    return Container(
      height: 30,
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(bottom: BorderSide(color: p.line)),
      ),
      child: Row(
        children: [
          for (var i = 0; i < titles.length; i++)
            SizedBox(
              width: w[i],
              child: InkWell(
                onTap: keys[i] == null ? null : () => _sortBy(keys[i]!),
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8),
                  child: Align(
                    alignment: _alignOf(i),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Flexible(
                          child: Text(
                            titles[i],
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(
                              fontSize: 11,
                              fontWeight: FontWeight.w600,
                              color: doc.filters.sortKey == keys[i]
                                  ? p.accent
                                  : p.tx3,
                            ),
                          ),
                        ),
                        if (doc.filters.sortKey == keys[i])
                          Icon(
                            doc.filters.sortAsc
                                ? Icons.arrow_upward
                                : Icons.arrow_downward,
                            size: 10,
                            color: p.accent,
                          ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  void _sortBy(String key) {
    final f = doc.filters;
    if (f.sortKey == key) {
      f.sortAsc = !f.sortAsc;
    } else {
      f.sortKey = key;
      f.sortAsc = true;
    }
    if (_v.hasClients) _v.jumpTo(0);
    doc.touch();
    unawaited(widget.workspace.ready.then((e) => doc.applyFilters(e)));
  }

  Widget _row(BuildContext context, Palette p, List<double> w, int i) {
    // 顺手把这一行所在的页要过来（没装才发请求）。
    _ensurePage(i);
    final r = doc.rows.at(i);
    final selected = r != null && r.index == doc.selected;
    final bg = selected
        ? p.rowSel
        : (i.isEven ? Colors.transparent : p.panel2.withValues(alpha: .5));

    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: r == null
          ? null
          : () {
              widget.workspace.prefs.detailCollapsed = false;
              unawaited(
                widget.workspace.ready.then(
                  (e) => doc.selectRow(e, r.index, viewIndex: i),
                ),
              );
            },
      child: Container(
        decoration: BoxDecoration(
          color: bg,
          border: Border(bottom: BorderSide(color: p.line2)),
        ),
        child: r == null
            ? const SizedBox.shrink()
            : Row(
                children: [
                  for (var col = 0; col < w.length; col++)
                    SizedBox(width: w[col], child: _cell(p, col, r)),
                ],
              ),
      ),
    );
  }

  void _ensurePage(int row) {
    final start = (row - 40).clamp(0, doc.rows.total);
    final end = (row + 120).clamp(0, doc.rows.total);
    final missing = doc.rows.missingPages(start, end);
    if (missing.isEmpty) return;
    for (final page in missing) {
      unawaited(widget.workspace.ready.then((e) => doc.loadPage(e, page)));
    }
  }

  Alignment _alignOf(int col) => switch (col) {
    0 || 3 || 5 => Alignment.centerRight,
    4 || 6 || 7 => Alignment.center,
    _ => Alignment.centerLeft,
  };

  Widget _cell(Palette p, int col, PacketRow r) {
    final fg = p.forKind(r.kind);
    final mono = p.mono.copyWith(fontSize: 11.5);

    switch (col) {
      case 0:
        return _wrap(
          Text('${r.index}', style: mono.copyWith(color: p.tx3)),
          Alignment.centerRight,
        );
      case 1:
        return _wrap(
          Text(r.sop, style: mono.copyWith(color: p.tx2, fontSize: 11)),
        );
      case 2:
        // GoodCRC 取「它确认的那条报文」的类别色 —— 否则一屏心跳全同一个颜色，
        // 看不出谁在回谁。
        final color = r.ackOf != null
            ? p.forKind(_kindOf(r.ackOf!) ?? r.kind)
            : fg;
        return _wrap(
          Tooltip(
            message: r.ackOf == null
                ? r.summary
                : '确认 #${r.ackOf}${r.ackType == null ? '' : ' · ${r.ackType}'}',
            child: Row(
              children: [
                Flexible(
                  child: Text(
                    r.msgType,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 12,
                      color: color,
                      fontWeight: FontWeight.w500,
                    ),
                  ),
                ),
                if (r.warn > 0) ...[
                  const SizedBox(width: 5),
                  _WarnBadge(p: p, count: r.warn),
                ],
              ],
            ),
          ),
        );
      case 3:
        return _wrap(
          Text(r.msgId?.toString() ?? '—', style: mono.copyWith(color: p.tx2)),
          Alignment.centerRight,
        );
      case 4:
        final (rf, rb) = p.forRole(r.role);
        return _wrap(
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
            decoration: BoxDecoration(
              color: rb,
              borderRadius: BorderRadius.circular(5),
              border: Border.all(color: rf.withValues(alpha: .3)),
            ),
            child: Text(
              r.role.isEmpty ? '—' : r.role,
              style: TextStyle(
                fontSize: 10.5,
                color: rf,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
          Alignment.center,
        );
      case 5:
        return _wrap(
          Text(r.objText, style: mono.copyWith(color: p.tx2)),
          Alignment.centerRight,
        );
      case 6:
        return _wrap(
          Text(r.elapsed, style: mono.copyWith(color: p.tx2, fontSize: 11)),
          Alignment.center,
        );
      case 7:
        // .pdStream / 无 ADC 的容器：这两项恒为 0 —— 留 0 而不是编一个值。
        final hasBus = doc.meta?.hasBus ?? false;
        return _wrap(
          Text(
            hasBus
                ? '${r.vbus.toStringAsFixed(2)} / ${r.ibus.toStringAsFixed(3)}'
                : '—',
            style: mono.copyWith(color: p.tx2, fontSize: 11),
          ),
          Alignment.center,
        );
      case 8:
        return _wrap(
          Text(
            r.dataHex,
            overflow: TextOverflow.ellipsis,
            style: mono.copyWith(color: p.tx2),
          ),
        );
      default:
        return _wrap(
          Text(
            r.summary,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(fontSize: 11.5, color: r.isBadCrc ? p.bad : p.tx2),
          ),
        );
    }
  }

  Widget _wrap(Widget child, [Alignment align = Alignment.centerLeft]) =>
      Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8),
        child: Align(alignment: align, child: child),
      );

  /// 取某条原始报文在**已装进来的页里**已知的类别（GoodCRC 取色用）。
  /// 找不到就退化成它自己的类别色 —— 这只是配色，取不到不影响正确性。
  String? _kindOf(int originalIndex) {
    final total = doc.rows.total;
    for (var i = 0; i < total; i++) {
      final r = doc.rows.at(i);
      if (r != null && r.index == originalIndex) return r.kind;
    }
    return null;
  }

  /// 空态：分成「真的没有报文」和「有行但一行都认不出」两种说法。
  Widget _emptyState(Palette p) {
    final st = doc.stats;
    final unlocated = st?.ufcsUnlocatedRows ?? 0;
    if (unlocated > 0) {
      return _Centered(text: '已读入 $unlocated 行，没有一行能定位出报文');
    }
    final filtered =
        doc.filters.hideGoodCrc ||
        doc.filters.onlyBad ||
        doc.filters.onlyPower ||
        doc.filters.onlyEnter ||
        doc.filters.types.isNotEmpty ||
        doc.filters.q.isNotEmpty;
    return _Centered(
      text: filtered ? '当前筛选下一行报文都没有' : '这份抓包里没有解出报文',
      hint: doc.filters.hideGoodCrc ? '「屏蔽心跳」处于开启状态' : null,
    );
  }
}

class _WarnBadge extends StatelessWidget {
  const _WarnBadge({required this.p, required this.count});

  final Palette p;
  final int count;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
    decoration: BoxDecoration(
      color: p.warn.withValues(alpha: .16),
      borderRadius: BorderRadius.circular(4),
    ),
    child: Text(
      '$count',
      style: TextStyle(
        fontSize: 9.5,
        color: p.warn,
        fontWeight: FontWeight.w700,
      ),
    ),
  );
}

class _Centered extends StatelessWidget {
  const _Centered({required this.text, this.hint});

  final String text;
  final String? hint;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.inbox_outlined, size: 34, color: p.tx3),
              const SizedBox(height: 12),
              Text(
                text,
                textAlign: TextAlign.center,
                style: TextStyle(fontSize: 13, color: p.tx2),
              ),
              if (hint != null) ...[
                const SizedBox(height: 8),
                Text(
                  hint!,
                  textAlign: TextAlign.center,
                  style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.7),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
