// detail_panel.dart — 右侧位域详情
//
// 结构：
//   · 分组靠**哨兵**：核心发来的 `details` 是 `{key,value}[]`，`key == 'Object'`
//     的那一条当标题，其余是行。这里只做分组与配色，
//     不猜任何字段含义。
//   · 相邻分组换色相（8 色循环）+ 左侧色条；`Source_Capabilities` 这种七八个 PDO
//     的长报文，不读标题也能看出边界。
//   · 分组标题**滚动吸顶**。
import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/models.dart';
import '../core/palette.dart';
import '../core/workspace.dart';

class DetailPanel extends StatelessWidget {
  const DetailPanel({super.key, required this.workspace, required this.doc});

  final Workspace workspace;
  final CaptureDocument doc;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(left: BorderSide(color: p.line)),
      ),
      child: Column(
        children: [
          _header(context, p),
          Expanded(child: _body(context, p)),
        ],
      ),
    );
  }

  Widget _header(BuildContext context, Palette p) {
    final d = doc.detail;
    return Container(
      height: 34,
      padding: const EdgeInsets.only(left: 12, right: 4),
      decoration: BoxDecoration(border: Border(bottom: BorderSide(color: p.line))),
      child: Row(
        children: [
          Expanded(
            child: Text(
              d == null ? '详情' : '#${d.row.index} · ${d.row.msgType}',
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                fontSize: 12.5,
                fontWeight: FontWeight.w600,
                color: p.tx,
              ),
            ),
          ),
          if (d?.synthetic == true)
            Tooltip(
              message: '字节来自分析仪导出',
              child: _tag(p, '分析仪记录', p.ext),
            ),
          const SizedBox(width: 4),
          Tooltip(
            message: '收起详情（Esc）',
            child: InkWell(
              onTap: () => workspace.prefs.detailCollapsed = true,
              borderRadius: BorderRadius.circular(5),
              child: Padding(
                padding: const EdgeInsets.all(5),
                child: Icon(Icons.close, size: 15, color: p.tx3),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _body(BuildContext context, Palette p) {
    final d = doc.detail;
    if (d == null) {
      if (doc.detailLoading) {
        return const Center(child: SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)));
      }
      return Center(
        child: Text(
          doc.selected == null ? '在上面的表格里点一条报文' : '取不到这条报文的详情',
          style: TextStyle(fontSize: 12, color: p.tx3),
        ),
      );
    }

    final groups = d.grouped();
    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(child: _overview(context, p, d)),
        for (var i = 0; i < groups.length; i++) ...[
          SliverPersistentHeader(
            pinned: true,
            delegate: _GroupHeader(
              title: groups[i].title.isEmpty ? '字段' : groups[i].title,
              color: p.groups[i % p.groups.length],
              palette: p,
            ),
          ),
          SliverList.builder(
            itemCount: groups[i].items.length,
            itemBuilder: (context, j) => _row(context, p, groups[i].items[j], i),
          ),
        ],
        if (d.text.isNotEmpty)
          SliverToBoxAdapter(child: _textBlock(context, p, '解析', d.text)),
        if (d.warnings.isNotEmpty)
          SliverToBoxAdapter(child: _warnings(context, p, d.warnings)),
        const SliverToBoxAdapter(child: SizedBox(height: 20)),
      ],
    );
  }

  /// 顶部概览：链路、CRC 三态、方向是否推断、确认的报文。
  Widget _overview(BuildContext context, Palette p, PacketDetail d) {
    final r = d.row;
    final rows = <(String, String)>[
      ('方向', r.role + (d.roleInferred ? '（推断）' : '')),
      ('链路', d.link.isEmpty ? r.sop : d.link),
      ('类别', d.category.isEmpty ? r.kind : d.category),
      ('CRC', _crcText(d)),
    ];
    if (r.ackOf != null) {
      rows.add(('确认的报文', '#${r.ackOf}${r.ackType == null ? '' : ' · ${r.ackType}'}'));
    }
    rows.add(('时间', '${r.elapsed}（${r.timeMs.toStringAsFixed(3)} ms）'));
    rows.add(('采样区间', '${r.startSample} … ${r.endSample}'));
    rows.add((
      d.synthetic ? '线上时长' : '报文时长',
      '${(r.durationUs / 1000).toStringAsFixed(3)} ms'
          '${d.synthetic ? '（按 600 kbps 标称时钟折算）' : ''}',
    ));

    return Container(
      margin: const EdgeInsets.fromLTRB(10, 10, 10, 6),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: p.panel2,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: p.line),
      ),
      child: Column(
        children: [
          for (final (k, v) in rows)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 2),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 74,
                    child: Text(k, style: TextStyle(fontSize: 11.5, color: p.tx3)),
                  ),
                  Expanded(
                    child: Text(
                      v,
                      style: TextStyle(fontSize: 11.5, color: p.tx, height: 1.5),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }

  /// CRC 是**三态**：通过 / 失败 / 未记录。未记录不能写成「通过」。
  String _crcText(PacketDetail d) {
    final rec = d.row.crc;
    if (rec == 'none') {
      return '未记录（重算 0x${d.crcCalc.toRadixString(16).toUpperCase()}）';
    }
    final v = d.crcValue == null ? '—' : '0x${d.crcValue!.toRadixString(16).toUpperCase()}';
    return rec == 'ok' ? '通过（$v）' : '失败（记录 $v，重算 0x${d.crcCalc.toRadixString(16).toUpperCase()}）';
  }

  Widget _row(BuildContext context, Palette p, DetailItem it, int groupIndex) {
    final color = p.groups[groupIndex % p.groups.length];
    return Container(
      padding: const EdgeInsets.fromLTRB(10, 3, 10, 3),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(width: 3, height: 15, color: color.withValues(alpha: .5)),
          const SizedBox(width: 7),
          SizedBox(
            width: 116,
            child: Text(
              it.key,
              style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.5),
            ),
          ),
          Expanded(
            child: SelectableText(
              it.value,
              style: TextStyle(fontSize: 11.5, color: p.tx, height: 1.5),
            ),
          ),
        ],
      ),
    );
  }

  Widget _textBlock(BuildContext context, Palette p, String title, String text) => Padding(
    padding: const EdgeInsets.fromLTRB(13, 14, 13, 4),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          title,
          style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: p.tx3),
        ),
        const SizedBox(height: 5),
        SelectableText(
          text,
          style: p.mono.copyWith(fontSize: 11.5, height: 1.7, color: p.tx2),
        ),
      ],
    ),
  );

  Widget _warnings(BuildContext context, Palette p, List<PacketWarning> ws) => Padding(
    padding: const EdgeInsets.fromLTRB(13, 14, 13, 4),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          '异常',
          style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: p.warn),
        ),
        const SizedBox(height: 5),
        for (final w in ws)
          Padding(
            padding: const EdgeInsets.only(bottom: 4),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (w.short.isNotEmpty) ...[
                  _tag(p, w.short, p.bad),
                  const SizedBox(width: 6),
                ],
                Expanded(
                  child: Text(
                    w.long,
                    style: TextStyle(fontSize: 11.5, color: p.tx2, height: 1.6),
                  ),
                ),
              ],
            ),
          ),
      ],
    ),
  );

  Widget _tag(Palette p, String text, Color color) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
    decoration: BoxDecoration(
      color: color.withValues(alpha: .14),
      borderRadius: BorderRadius.circular(4),
      border: Border.all(color: color.withValues(alpha: .35)),
    ),
    child: Text(
      text,
      style: TextStyle(fontSize: 10, color: color, fontWeight: FontWeight.w600),
    ),
  );
}

/// 吸顶的分组标题 + 左侧色条。
class _GroupHeader extends SliverPersistentHeaderDelegate {
  _GroupHeader({required this.title, required this.color, required this.palette});

  final String title;
  final Color color;
  final Palette palette;

  @override
  double get minExtent => 26;

  @override
  double get maxExtent => 26;

  @override
  Widget build(BuildContext context, double shrinkOffset, bool overlapsContent) => Container(
    decoration: BoxDecoration(
      color: palette.panel,
      border: Border(
        left: BorderSide(color: color, width: 3),
        bottom: BorderSide(color: palette.line2),
      ),
    ),
    padding: const EdgeInsets.only(left: 9),
    alignment: Alignment.centerLeft,
    child: Text(
      title,
      style: TextStyle(fontSize: 11.5, fontWeight: FontWeight.w600, color: color),
    ),
  );

  @override
  bool shouldRebuild(_GroupHeader old) => old.title != title || old.color != color;
}
