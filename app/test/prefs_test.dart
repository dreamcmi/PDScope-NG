import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/prefs.dart';

void main() {
  test('主题和面板尺寸重新启动后恢复，损坏的设置文件不阻止启动', () {
    final dir = Directory.systemTemp.createTempSync('pdscope-prefs-');
    try {
      final file = File('${dir.path}${Platform.pathSeparator}prefs.json');
      final first = Prefs(storage: file)
        ..themeMode = ThemeMode.dark
        ..detailW = 530
        ..tlH = 224;
      first.dispose();

      final second = Prefs(storage: file);
      expect(second.themeMode, ThemeMode.dark);
      expect(second.detailW, 530);
      expect(second.tlH, 224);
      second.dispose();

      file.writeAsStringSync('{broken');
      final fallback = Prefs(storage: file);
      expect(fallback.themeMode, ThemeMode.light);
      expect(fallback.detailW, kDetailWidthDefault);
      expect(fallback.tlH, kTimelineHeightDefault);
      fallback.dispose();
    } finally {
      dir.deleteSync(recursive: true);
    }
  });
}
