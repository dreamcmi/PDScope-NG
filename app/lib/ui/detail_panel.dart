// detail_panel.dart — 右侧位域详情
//
// 结构：
//   · 分组靠**哨兵**：核心发来的 `details` 是 `{key,value}[]`，`key == 'Object'`
//     的那一条当标题，其余是行。这里只做分组与配色，
//     不猜任何字段含义。
//   · 相邻分组换色相（8 色循环）+ 左侧色条；`Source_Capabilities` 这种七八个 PDO
//     的长报文，不读标题也能看出边界。
//   · 分组标题**滚动吸顶**。
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

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
      decoration: BoxDecoration(
        border: Border(bottom: BorderSide(color: p.line)),
      ),
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
            Tooltip(message: '字节来自分析仪导出', child: _tag(p, '分析仪记录', p.ext)),
          const SizedBox(width: 4),
          _nav(p, Icons.chevron_left, '上一条报文', -1),
          _nav(p, Icons.chevron_right, '下一条报文', 1),
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

  Widget _nav(Palette p, IconData icon, String tip, int delta) {
    final at = doc.selectedViewIndex;
    final enabled =
        at != null && at + delta >= 0 && at + delta < doc.rows.total;
    return Tooltip(
      message: tip,
      child: InkWell(
        onTap: enabled
            ? () => unawaited(
                workspace.ready.then((e) => doc.stepDetail(e, delta)),
              )
            : null,
        borderRadius: BorderRadius.circular(5),
        child: Padding(
          padding: const EdgeInsets.all(5),
          child: Icon(icon, size: 15, color: enabled ? p.tx2 : p.tx3),
        ),
      ),
    );
  }

  Widget _body(BuildContext context, Palette p) {
    final d = doc.detail;
    if (d == null) {
      if (doc.detailLoading) {
        return const Center(
          child: SizedBox(
            width: 18,
            height: 18,
            child: CircularProgressIndicator(strokeWidth: 2),
          ),
        );
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
        if (d.header != null)
          SliverToBoxAdapter(child: _headerBits(context, p, d)),
        if (d.extHeader != null)
          SliverToBoxAdapter(child: _extendedHeaderBits(context, p, d)),
        if (d.row.dataHex.isNotEmpty)
          SliverToBoxAdapter(
            child: _copyBlock(
              context,
              p,
              doc.isUfcs ? '整帧字节 (hex)' : '数据对象 (hex)',
              d.row.dataHex,
            ),
          ),
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
            itemBuilder: (context, j) =>
                _row(context, p, groups[i].items[j], i),
          ),
        ],
        if (d.text.isNotEmpty)
          SliverToBoxAdapter(child: _textBlock(context, p, '解析', d.text)),
        if (d.pdFlags != null)
          SliverToBoxAdapter(
            child: _textBlock(
              context,
              p,
              'PCL 原始解码字节（头 / 正文 / 线上 CRC）',
              d.rawPayload.isEmpty
                  ? 'Reset，无 payload'
                  : d.rawPayload
                        .map(
                          (value) => value
                              .toRadixString(16)
                              .padLeft(2, '0')
                              .toUpperCase(),
                        )
                        .join(' '),
            ),
          ),
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
      ('VBUS', r.vbus.isFinite ? '${r.vbus.toStringAsFixed(3)} V' : '未知'),
      ('IBUS', r.ibus.isFinite ? '${r.ibus.toStringAsFixed(3)} A' : '未知'),
      (
        doc.isUfcs ? '数据长度' : '数据对象',
        doc.isUfcs ? '${r.bytes ?? 0} B' : '${r.objects ?? 0}',
      ),
      ('CRC', _crcText(d)),
    ];
    if (r.ackOf != null) {
      rows.add((
        '确认的报文',
        '#${r.ackOf}${r.ackType == null ? '' : ' · ${r.ackType}'}',
      ));
    }
    if (d.pdFlags != null) {
      rows.add((
        '设备 CRC 结论',
        (d.pdFlags! & 1) != 0
            ? '失败'
            : (d.pdFlags! & 2) != 0
            ? '未知'
            : '通过',
      ));
    }
    rows.add((
      '时间',
      r.timeUncertain
          ? '连续性不确定'
          : '${r.elapsed}（${r.timeMs.toStringAsFixed(3)} ms）',
    ));
    if (d.elapsedTicks != null) rows.add(('设备原始刻度', '${d.elapsedTicks}'));
    if (d.pdFlags == null) {
      rows.add(('采样区间', '${r.startSample} … ${r.endSample}'));
    }
    rows.add((
      d.synthetic ? '线上时长' : '报文时长',
      r.durationUs.isFinite
          ? '${(r.durationUs / 1000).toStringAsFixed(3)} ms'
                '${d.synthetic ? '（按 600 kbps 标称时钟折算）' : ''}'
          : '未记录',
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
                    child: Text(
                      k,
                      style: TextStyle(fontSize: 11.5, color: p.tx3),
                    ),
                  ),
                  Expanded(
                    child: Text(
                      v,
                      style: TextStyle(
                        fontSize: 11.5,
                        color: p.tx,
                        height: 1.5,
                      ),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }

  /// 按旧详情面板的 PD / UFCS 逐位语义展示核心已提供的 16 bit 消息头。
  Widget _headerBits(BuildContext context, Palette p, PacketDetail d) {
    final h = d.header!;
    final r = d.row;
    if (doc.isUfcs) {
      final msgKind = r.msgKind == 'custom'
          ? '自定义消息'
          : r.msgKind == 'data'
          ? '数据消息'
          : '控制消息';
      final version = d.revText?.isNotEmpty == true
          ? 'UFCS ${d.revText}'
          : 'UFCS';
      return _bitsBlock(context, p, '消息头 (16 bit)', [
        _bitLine(
          'B15-13',
          '设备地址',
          h,
          13,
          15,
          '${_bitNumber(h, 13, 15)}（接收方地址编号）',
        ),
        _bitLine('B12-9', '消息编号', h, 9, 12, '${r.msgId ?? 0}'),
        _bitLine('B8-3', '协议版本编号', h, 3, 8, version),
        _bitLine(
          'B2-0',
          '消息类型',
          h,
          0,
          2,
          '$msgKind · 命令 0x${(d.msgTypeRaw ?? 0).toRadixString(16).toUpperCase().padLeft(2, '0')}',
        ),
      ]);
    }

    final revision = _bitNumber(h, 6, 7);
    final revisionText = switch (revision) {
      0 => 'PD 1.0（已废弃；接收按 PD 2.0 解释）',
      1 => 'PD 2.0',
      2 => 'PD 3.x',
      _ => '保留',
    };
    final cableSop = r.sop == "SOP'" || r.sop == "SOP''";
    return _bitsBlock(context, p, '报文头 (16 bit)', [
      _bitLine(
        'B15',
        'Extended',
        h,
        15,
        15,
        r.msgKind == 'ext' ? '扩展消息' : '标准消息',
      ),
      _bitLine('B14-12', 'Object 数', h, 12, 14, '${r.objects ?? 0}'),
      _bitLine('B11-9', 'Message ID', h, 9, 11, '${r.msgId ?? 0}'),
      if (cableSop)
        _bitLine(
          'B8',
          'Cable Plug',
          h,
          8,
          8,
          _bitNumber(h, 8, 8) == 1 ? '1 · Cable Plug/VPD' : '0 · Port DFP/UFP',
        )
      else
        _bitLine(
          'B8',
          'Power Role',
          h,
          8,
          8,
          _bitNumber(h, 8, 8) == 1 ? '1 · Source' : '0 · Sink',
        ),
      _bitLine('B7-6', 'Spec Revision', h, 6, 7, '$revision · $revisionText'),
      if (cableSop)
        _bitLine('B5', 'Reserved', h, 5, 5, '保留')
      else
        _bitLine(
          'B5',
          'Data Role',
          h,
          5,
          5,
          _bitNumber(h, 5, 5) == 1 ? '1 · DFP' : '0 · UFP',
        ),
      _bitLine(
        'B4-0',
        'Message Type',
        h,
        0,
        4,
        '${d.msgTypeRaw ?? 0} · ${r.msgType}',
      ),
    ]);
  }

  Widget _extendedHeaderBits(BuildContext context, Palette p, PacketDetail d) {
    final h = d.extHeader!;
    final chunked = _bitNumber(h, 15, 15);
    return _bitsBlock(context, p, '扩展报文头 (16 bit)', [
      _bitLine('B15', 'Chunked', h, 15, 15, chunked == 1 ? '分块' : '不分块'),
      _bitLine('B14-11', 'Chunk Number', h, 11, 14, '${_bitNumber(h, 11, 14)}'),
      _bitLine('B10', 'Request Chunk', h, 10, 10, '${_bitNumber(h, 10, 10)}'),
      _bitLine('B9', 'Reserved', h, 9, 9, '保留'),
      _bitLine('B8-0', 'Data Size', h, 0, 8, '${_bitNumber(h, 0, 8)} B'),
    ]);
  }

  (String, String) _bitLine(
    String range,
    String label,
    int header,
    int low,
    int high,
    String decoded,
  ) => ('$range · $label', '${_bitBinary(header, low, high)} → $decoded');

  int _bitNumber(int value, int low, int high) =>
      (value >> low) & ((1 << (high - low + 1)) - 1);

  String _bitBinary(int value, int low, int high) {
    final width = high - low + 1;
    return '0b${_bitNumber(value, low, high).toRadixString(2).padLeft(width, '0')}';
  }

  Widget _bitsBlock(
    BuildContext context,
    Palette p,
    String title,
    List<(String, String)> fields,
  ) => Container(
    margin: const EdgeInsets.fromLTRB(10, 4, 10, 6),
    padding: const EdgeInsets.all(10),
    decoration: BoxDecoration(
      color: p.panel2,
      borderRadius: BorderRadius.circular(8),
      border: Border.all(color: p.line),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(bottom: 5),
          child: Text(
            title,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w600,
              color: p.tx3,
            ),
          ),
        ),
        for (final (label, value) in fields)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 2),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                SizedBox(
                  width: 116,
                  child: Text(
                    label,
                    style: TextStyle(fontSize: 11.5, color: p.tx3, height: 1.5),
                  ),
                ),
                Expanded(
                  child: SelectableText(
                    value,
                    style: p.mono.copyWith(
                      fontSize: 11.5,
                      color: p.tx,
                      height: 1.5,
                    ),
                  ),
                ),
              ],
            ),
          ),
      ],
    ),
  );

  Widget _copyBlock(
    BuildContext context,
    Palette p,
    String title,
    String value,
  ) => Container(
    margin: const EdgeInsets.fromLTRB(10, 4, 10, 6),
    padding: const EdgeInsets.all(10),
    decoration: BoxDecoration(
      color: p.panel2,
      borderRadius: BorderRadius.circular(8),
      border: Border.all(color: p.line),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Expanded(
              child: Text(
                title,
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  color: p.tx3,
                ),
              ),
            ),
            Tooltip(
              message: '复制原始数据',
              child: InkWell(
                onTap: () =>
                    unawaited(Clipboard.setData(ClipboardData(text: value))),
                borderRadius: BorderRadius.circular(5),
                child: Padding(
                  padding: const EdgeInsets.all(4),
                  child: Icon(Icons.copy, size: 14, color: p.tx3),
                ),
              ),
            ),
          ],
        ),
        const SizedBox(height: 5),
        SelectableText(
          value,
          style: p.mono.copyWith(fontSize: 11.5, height: 1.7, color: p.tx2),
        ),
      ],
    ),
  );

  /// CRC 是**三态**：通过 / 失败 / 未记录。未记录不能写成「通过」。
  String _crcText(PacketDetail d) {
    final rec = d.row.crc;
    if (d.pdFlags != null && rec == 'none') {
      if (d.crcValue == null) return '无法校验（未保留完整 CRC 字节）';
      return '设备结论未知；原始 CRC 0x${d.crcValue!.toRadixString(16).toUpperCase()}';
    }
    if (rec == 'none') {
      return '未记录（重算 0x${d.crcCalc.toRadixString(16).toUpperCase()}）';
    }
    final v = d.crcValue == null
        ? '—'
        : '0x${d.crcValue!.toRadixString(16).toUpperCase()}';
    return rec == 'ok'
        ? '通过（$v）'
        : '失败（记录 $v，重算 0x${d.crcCalc.toRadixString(16).toUpperCase()}）';
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

  Widget _textBlock(
    BuildContext context,
    Palette p,
    String title,
    String text,
  ) => Padding(
    padding: const EdgeInsets.fromLTRB(13, 14, 13, 4),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          title,
          style: TextStyle(
            fontSize: 11,
            fontWeight: FontWeight.w600,
            color: p.tx3,
          ),
        ),
        const SizedBox(height: 5),
        SelectableText(
          text,
          style: p.mono.copyWith(fontSize: 11.5, height: 1.7, color: p.tx2),
        ),
      ],
    ),
  );

  Widget _warnings(BuildContext context, Palette p, List<PacketWarning> ws) =>
      Padding(
        padding: const EdgeInsets.fromLTRB(13, 14, 13, 4),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '异常',
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w600,
                color: p.warn,
              ),
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
                        style: TextStyle(
                          fontSize: 11.5,
                          color: p.tx2,
                          height: 1.6,
                        ),
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
  _GroupHeader({
    required this.title,
    required this.color,
    required this.palette,
  });

  final String title;
  final Color color;
  final Palette palette;

  @override
  double get minExtent => 26;

  @override
  double get maxExtent => 26;

  @override
  Widget build(
    BuildContext context,
    double shrinkOffset,
    bool overlapsContent,
  ) => Container(
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
      style: TextStyle(
        fontSize: 11.5,
        fontWeight: FontWeight.w600,
        color: color,
      ),
    ),
  );

  @override
  bool shouldRebuild(_GroupHeader old) =>
      old.title != title || old.color != color;
}
