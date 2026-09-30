// prefs.dart — 全局偏好
//
// 与「一份抓包是什么」无关、只跟「我怎么看」有关的东西放这里：行高、详情面板宽度、
// 曲线区高度、筛选栏折叠、主题。这几个切标签不变；
// Flutter 侧同理，放在工作区级别而不是文档级别。
// 旧版会记住主题、详情宽度和曲线高度；桌面版把这三项写入用户配置目录。
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

/// 详情面板宽度的默认 / 上下限。
const double kDetailWidthDefault = 390;
const double kDetailWidthMin = 280;
const double kDetailWidthMax = 900;

/// 曲线区的默认 / 上下限。
const double kTimelineHeightDefault = 158;
const double kTimelineHeightMin = 92;
const double kTimelineHeightMax = 640;

/// 表格行高（与 CSS 的 `--rh: 30px` 一致）。
const double kRowHeightNormal = 30;
const double kRowHeightCompact = 24;

class Prefs extends ChangeNotifier {
  Prefs({this.storage}) {
    _load();
  }

  Prefs.persistent() : this(storage: _defaultStorage());

  final File? storage;
  Timer? _saveTimer;

  static File _defaultStorage() {
    final base =
        Platform.environment['APPDATA'] ??
        Platform.environment['HOME'] ??
        Directory.current.path;
    return File(
      '$base${Platform.pathSeparator}PDScope-NG${Platform.pathSeparator}prefs.json',
    );
  }

  void _load() {
    final file = storage;
    if (file == null || !file.existsSync()) return;
    try {
      final value = jsonDecode(file.readAsStringSync());
      if (value is! Map<String, dynamic>) return;
      final theme = value['theme'];
      if (theme == 'dark') _themeMode = ThemeMode.dark;
      final detail = value['detailW'];
      if (detail is num && detail.isFinite) {
        _detailW = detail.clamp(kDetailWidthMin, kDetailWidthMax).toDouble();
      }
      final timeline = value['tlH'];
      if (timeline is num && timeline.isFinite) {
        _tlH = timeline
            .clamp(kTimelineHeightMin, kTimelineHeightMax)
            .toDouble();
      }
    } catch (_) {
      // A damaged or inaccessible preference file must not block opening captures.
    }
  }

  void _saveSoon() {
    if (storage == null) return;
    _saveTimer?.cancel();
    _saveTimer = Timer(const Duration(milliseconds: 250), flush);
  }

  void flush() {
    _saveTimer?.cancel();
    _saveTimer = null;
    final file = storage;
    if (file == null) return;
    try {
      file.parent.createSync(recursive: true);
      file.writeAsStringSync(
        jsonEncode({
          'theme': isDark ? 'dark' : 'light',
          'detailW': _detailW,
          'tlH': _tlH,
        }),
      );
    } catch (_) {
      // Match the old localStorage behavior when user storage is unavailable.
    }
  }

  @override
  void dispose() {
    flush();
    super.dispose();
  }

  /// 表格行高。
  double _rowH = kRowHeightNormal;
  double get rowH => _rowH;
  set rowH(double v) {
    final c = v.clamp(kRowHeightCompact - 2, 56).toDouble();
    if (c == _rowH) return;
    _rowH = c;
    notifyListeners();
  }

  /// 紧凑行高开关。
  bool get compact => _rowH <= kRowHeightCompact;
  set compact(bool v) => rowH = v ? kRowHeightCompact : kRowHeightNormal;

  /// 详情面板宽度。
  double _detailW = kDetailWidthDefault;
  double get detailW => _detailW;
  set detailW(double v) {
    final c = v.clamp(kDetailWidthMin, kDetailWidthMax).toDouble();
    if (c == _detailW) return;
    _detailW = c;
    notifyListeners();
    _saveSoon();
  }

  /// 详情面板是否收起。收起后右缘留一条竖栏作为**不依赖数据的重开入口**。
  bool _detailCollapsed = false;
  bool get detailCollapsed => _detailCollapsed;
  set detailCollapsed(bool v) {
    if (v == _detailCollapsed) return;
    _detailCollapsed = v;
    notifyListeners();
  }

  /// 底部曲线区高度。
  double _tlH = kTimelineHeightDefault;
  double get tlH => _tlH;
  set tlH(double v) {
    final c = v.clamp(kTimelineHeightMin, kTimelineHeightMax).toDouble();
    if (c == _tlH) return;
    _tlH = c;
    notifyListeners();
    _saveSoon();
  }

  /// 左侧筛选栏是否折叠。
  bool _filtersCollapsed = false;
  bool get filtersCollapsed => _filtersCollapsed;
  set filtersCollapsed(bool v) {
    if (v == _filtersCollapsed) return;
    _filtersCollapsed = v;
    notifyListeners();
  }

  /// 主题。默认亮色。
  ThemeMode _themeMode = ThemeMode.light;
  ThemeMode get themeMode => _themeMode;
  set themeMode(ThemeMode v) {
    if (v == _themeMode) return;
    _themeMode = v;
    notifyListeners();
    _saveSoon();
  }

  bool get isDark => _themeMode == ThemeMode.dark;

  void toggleTheme() {
    themeMode = isDark ? ThemeMode.light : ThemeMode.dark;
  }

  void resetDetailWidth() => detailW = kDetailWidthDefault;
  void resetTimelineHeight() => tlH = kTimelineHeightDefault;
}
