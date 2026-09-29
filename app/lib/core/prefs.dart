// prefs.dart — 全局偏好
//
// 与「一份抓包是什么」无关、只跟「我怎么看」有关的东西放这里：行高、详情面板宽度、
// 曲线区高度、筛选栏折叠、主题。这几个切标签不变；
// Flutter 侧同理，放在工作区级别而不是文档级别。
// 持久化留待后面接平台存储。
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
  }

  bool get isDark => _themeMode == ThemeMode.dark;

  void toggleTheme() {
    themeMode = isDark ? ThemeMode.light : ThemeMode.dark;
  }

  void resetDetailWidth() => detailW = kDetailWidthDefault;
  void resetTimelineHeight() => tlH = kTimelineHeightDefault;
}
