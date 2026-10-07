// open_files.dart — 打开 / 另存为
//
// 格式**按内容判定**（核心的 `dispatch` 认 ZIP 魔数 / SQLite 魔数 / 结构自证），
// 所以这里**不加扩展名过滤** —— 加过滤反而会把 `.pdStream`、没后缀的导出、
// 或者被改过名字的 `.atkcc` 挡在门外。用户在对话框里选到别的文件也没关系，
// 核心会给出「不是抓包」这种能读的说明。
import 'dart:io';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';

import '../core/document.dart';
import '../core/workspace.dart';

/// 弹「打开文件」对话框（可多选），选中的路径全部作为新标签打开。
Future<List<String>> openFilesViaDialog(Workspace ws) async {
  final files = await openFiles();
  if (files.isEmpty) return const [];
  final paths = files.map((f) => f.path).toList();
  await ws.openFiles(paths);
  return paths;
}

/// GUI 导出。`kind` 取 `csv` 或 `json`。
///
/// 顶栏的导出菜单、外壳的原生菜单**都走这一个函数** —— 不各写一份，
/// 否则「CSV 导当前视图、JSON 导全部」这种口径迟早会在其中一处走样。
Future<void> exportViaDialog(
  BuildContext context,
  Workspace ws,
  CaptureDocument? doc,
  String kind,
) async {
  final messenger = ScaffoldMessenger.maybeOf(context);
  if (doc == null) {
    messenger?.showSnackBar(const SnackBar(content: Text('还没有打开的抓包')));
    return;
  }

  // 实时文档：导出本地持有数据的当前视图 CSV 或全部保留详情 JSON 快照。
  // 与离线共用同一个出口，所以「顶栏导出菜单」和「外壳原生菜单」两边都是一致的。
  if (doc.isLive) {
    final l = doc.live!;
    final csv = kind == 'csv';
    final n = csv ? l.viewCount : l.rowCount;
    final t = l.stats.durationSec;
    try {
      // 落盘带 BOM（与核心 csv.cpp 的「写文件带、走管道不带」同口径）。
      final path = await saveTextAs(
        text: csv ? '\uFEFF${doc.liveCsvText()}' : l.jsonText(),
        suggestedName: csv
            ? doc.liveCsvName()
            : doc.liveCsvName().replaceAll(RegExp(r'\.csv$'), '.json'),
        extension: csv ? 'csv' : 'json',
        typeLabel: csv ? 'CSV' : 'JSON',
      );
      messenger?.showSnackBar(
        SnackBar(
          duration: const Duration(seconds: 6),
          content: Text(
            path == null
                ? '已取消'
                : '已导出 $n 条（截至 ${t.toStringAsFixed(1)} s）'
                      ' · ${csv ? '当前视图' : '全部保留报文，含原始字节和诊断'}快照',
          ),
        ),
      );
    } catch (err) {
      messenger?.showSnackBar(SnackBar(content: Text('导出失败：$err')));
    }
    return;
  }

  final e = await ws.ready;
  try {
    final String text;
    final String suggested;
    if (kind == 'csv') {
      // GUI 导出走**当前视图**（受筛选影响）—— 与命令行「导全部报文」刻意不同。
      text = await e.exportCsv(doc.id, bom: true);
      final name = await e.defaultCsvName(doc.id);
      suggested = name.isEmpty ? '抓包.csv' : name;
    } else {
      text = await e.exportJson(doc.id);
      final base = doc.displayName.replaceAll(RegExp(r'\.[^.]*$'), '');
      suggested = '$base.json';
    }

    final path = await saveTextAs(
      text: text,
      suggestedName: suggested,
      extension: kind,
      typeLabel: kind.toUpperCase(),
    );
    messenger?.showSnackBar(
      SnackBar(content: Text(path == null ? '已取消' : '已导出 $path')),
    );
  } catch (err) {
    messenger?.showSnackBar(SnackBar(content: Text('导出失败：$err')));
  }
}

/// 另外存一份文本（CSV / JSON）。
///
/// @param suggestedName 默认文件名（CSV 用核心给的 `defaultCsvName()`）
/// @return 写入的路径；用户取消返回 null
Future<String?> saveTextAs({
  required String text,
  required String suggestedName,
  required String extension,
  required String typeLabel,
}) async {
  final location = await getSaveLocation(
    suggestedName: suggestedName,
    acceptedTypeGroups: [
      XTypeGroup(label: typeLabel, extensions: [extension]),
    ],
  );
  if (location == null) return null;
  final path = location.path;
  // 用户可能把后缀删了 —— 补回去，别产出没有后缀的文件。
  final finalPath = path.toLowerCase().endsWith('.$extension')
      ? path
      : '$path.$extension';
  await File(finalPath).writeAsString(text, flush: true);
  return finalPath;
}
