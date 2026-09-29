// tab_strip.dart — 抓包标签栏
//
// 一份抓包一个标签。标签上要有三样东西（都来自基线的约定）：
//   · 状态圆点：灰=未解码 / 蓝=解码中 / 绿=完成 / 黄=有告警 / 红=打不开
//   · 报文条数徽章
//   · 关闭入口（× / 中键 / 右键菜单）
// 标签栏在「一份都没开」时整个不出现 —— 让位给表格里的空态引导。
import 'dart:async';

import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/palette.dart';
import '../core/workspace.dart';

class TabStrip extends StatelessWidget {
  const TabStrip({super.key, required this.workspace});

  final Workspace workspace;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Container(
      height: 34,
      decoration: BoxDecoration(
        color: p.panel,
        border: Border(bottom: BorderSide(color: p.line)),
      ),
      child: ListView.builder(
        scrollDirection: Axis.horizontal,
        itemCount: workspace.docs.length,
        itemBuilder: (context, i) => _Tab(
          workspace: workspace,
          doc: workspace.docs[i],
          index: i,
          active: i == workspace.activeIndex,
        ),
      ),
    );
  }
}

class _Tab extends StatelessWidget {
  const _Tab({
    required this.workspace,
    required this.doc,
    required this.index,
    required this.active,
  });

  final Workspace workspace;
  final CaptureDocument doc;
  final int index;
  final bool active;

  @override
  Widget build(BuildContext context) {
    final p = PaletteScope.of(context);
    return Tooltip(
      message: doc.error ?? doc.path,
      child: GestureDetector(
        onSecondaryTapUp: (d) => _menu(context, d.globalPosition),
        onTertiaryTapUp: (_) => unawaited(workspace.closeAt(index)),
        child: InkWell(
          onTap: () => workspace.activate(index),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 10),
            decoration: BoxDecoration(
              color: active ? p.bg2 : p.panel,
              border: Border(
                right: BorderSide(color: p.line),
                top: BorderSide(
                  color: active ? p.accent : Colors.transparent,
                  width: 2,
                ),
              ),
            ),
            child: Row(
              children: [
                _dot(p),
                const SizedBox(width: 6),
                ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 200),
                  child: Text(
                    doc.displayName,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 12,
                      color: active ? p.tx : p.tx2,
                      fontWeight: active ? FontWeight.w500 : FontWeight.w400,
                    ),
                  ),
                ),
                const SizedBox(width: 6),
                if (doc.hasPackets) _badge(p),
                const SizedBox(width: 4),
                InkWell(
                  onTap: () => unawaited(workspace.closeAt(index)),
                  borderRadius: BorderRadius.circular(4),
                  child: Padding(
                    padding: const EdgeInsets.all(2),
                    child: Icon(Icons.close, size: 13, color: p.tx3),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  /// 状态圆点。解码中画一个转圈，其余是实心点。
  Widget _dot(Palette p) {
    if (doc.state == DocState.decoding) {
      return SizedBox(
        width: 9,
        height: 9,
        child: CircularProgressIndicator(strokeWidth: 1.5, color: p.accent),
      );
    }
    final color = switch (doc.state) {
      DocState.failed => p.bad,
      DocState.done => (doc.stats?.warnings ?? 0) > 0 ? p.warn : p.ok,
      DocState.opening => p.tx3,
      _ => p.tx3,
    };
    return Container(
      width: 8,
      height: 8,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
    );
  }

  Widget _badge(Palette p) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
    decoration: BoxDecoration(
      color: p.accentSoft,
      borderRadius: BorderRadius.circular(8),
    ),
    child: Text(
      '${doc.rows.total}',
      style: TextStyle(fontSize: 10.5, color: p.accent, fontWeight: FontWeight.w600),
    ),
  );

  Future<void> _menu(BuildContext context, Offset at) async {
    final p = PaletteScope.of(context);
    final overlay = Overlay.of(context).context.findRenderObject()! as RenderBox;
    final choice = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        Rect.fromLTWH(at.dx, at.dy, 0, 0),
        Offset.zero & overlay.size,
      ),
      color: p.panel,
      items: const [
        PopupMenuItem(value: 'close', child: Text('关闭', style: TextStyle(fontSize: 12.5))),
        PopupMenuItem(value: 'others', child: Text('关闭其它', style: TextStyle(fontSize: 12.5))),
        PopupMenuItem(value: 'all', child: Text('全部关闭', style: TextStyle(fontSize: 12.5))),
      ],
    );
    if (choice == 'close') await workspace.closeAt(index);
    if (choice == 'others') await workspace.closeOthers(index);
    if (choice == 'all') await workspace.closeAll();
  }
}
