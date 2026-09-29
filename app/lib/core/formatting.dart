// formatting.dart — 数值 / 时间 / 速率的显示口径
//
// 显示口径：采样率按量级换单位、时间轴刻度按总时长自适应、
// 左侧列表的「时间」列用 `hh:mm:ss.mmm`。核心已经把 `elapsed` 这类字符串算好了，
// 所以这里只处理核心不提供的（刻度、悬停读数、字节数）。
import 'dart:math' as math;

/// 采样率 → `600.00 kHz` / `1.00 MHz` / `2.50 MHz`。
///
/// ≥1 MHz 用 MHz 两位小数，≥1 kHz 用 kHz 两位小数，
/// 否则写 Hz。注意**不是**「有效数字」而是固定小数位 —— 列表里竖排才对得齐。
String fmtRate(double hz) {
  if (!hz.isFinite || hz <= 0) return '—';
  if (hz >= 1e6) return '${(hz / 1e6).toStringAsFixed(2)} MHz';
  if (hz >= 1e3) return '${(hz / 1e3).toStringAsFixed(2)} kHz';
  return '${hz.toStringAsFixed(2)} Hz';
}

/// 采样点区间 → 秒数文案（用于「时长」这类摘要）。
String fmtSeconds(double s) {
  if (!s.isFinite || s <= 0) return '0.000 s';
  if (s < 1) return '${(s * 1000).toStringAsFixed(0)} ms';
  if (s < 60) return '${s.toStringAsFixed(3)} s';
  final m = s ~/ 60;
  final rest = s - m * 60;
  return '$m:${rest.toStringAsFixed(1).padLeft(4, '0')}';
}

/// 字节数 → `1.25 MiB` / `418 KiB` / `930 B`。
String fmtBytes(int bytes) {
  if (bytes < 1024) return '$bytes B';
  if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KiB';
  if (bytes < 1024 * 1024 * 1024) return '${(bytes / 1024 / 1024).toStringAsFixed(2)} MiB';
  return '${(bytes / 1024 / 1024 / 1024).toStringAsFixed(2)} GiB';
}

/// 千分位。
String fmtCount(num n) {
  final s = n.toInt().toString();
  final buf = StringBuffer();
  for (var i = 0; i < s.length; i++) {
    if (i > 0 && (s.length - i) % 3 == 0) buf.write(',');
    buf.write(s[i]);
  }
  return buf.toString();
}

/// 时间轴刻度文案。单位**随时长自适应**：
///   · 不到 1 秒 → `0ms … 170ms`（10ms 以内再补一位小数，免得相邻两格同名）
///   · 不到 60 秒 → `0.00s … 2.50s`（两位数以上收成一位小数）
///   · 更长 → `0:00 … 1:23`
String fmtTick(double seconds, double span) {
  if (span < 1) {
    final ms = seconds * 1000;
    if (span < 0.01) return '${ms.toStringAsFixed(1)}ms';
    return '${ms.round()}ms';
  }
  if (span < 60) {
    return seconds >= 10
        ? '${seconds.toStringAsFixed(1)}s'
        : '${seconds.toStringAsFixed(2)}s';
  }
  final total = seconds.round();
  final m = total ~/ 60;
  final s = total % 60;
  return '$m:${s.toStringAsFixed(0).padLeft(2, '0')}';
}

/// 悬停读数（`0.042s`）—— 与刻度同一套归一化坐标。
String fmtReadout(double seconds, double span) {
  if (span < 1) return '${(seconds * 1000).toStringAsFixed(1)}ms';
  if (span < 60) return '${seconds.toStringAsFixed(3)}s';
  final total = seconds;
  final m = total ~/ 60;
  final s = total - m * 60;
  return '$m:${s.toStringAsFixed(1).padLeft(4, '0')}';
}

/// 电压 / 电流读数。
String fmtVolt(double v) => '${v.toStringAsFixed(2)} V';
String fmtAmp(double a) => a.abs() < 1 ? '${(a * 1000).toStringAsFixed(1)} mA' : '${a.toStringAsFixed(3)} A';

/// 纵轴刻度数字：按量程自动加小数位（放大后要能看出台阶）。
String fmtAxis(double v, double maxAbs) {
  final m = maxAbs.abs();
  if (m >= 100) return v.toStringAsFixed(0);
  if (m >= 10) return v.toStringAsFixed(1);
  if (m >= 1) return v.toStringAsFixed(2);
  if (m >= 0.1) return v.toStringAsFixed(3);
  return v.toStringAsPrecision(3);
}

/// 在 lo..hi 上取 n 个「整齐」的刻度（1/2/5 × 10^k）。
List<double> niceTicks(double lo, double hi, int n) {
  if (!lo.isFinite || !hi.isFinite || hi <= lo || n < 2) return const [];
  final raw = (hi - lo) / n;
  final mag = math.pow(10, (math.log(raw) / math.ln10).floor()).toDouble();
  final norm = raw / mag;
  final step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
  final first = (lo / step).ceil() * step;
  final out = <double>[];
  for (var v = first; v <= hi + step * 1e-9; v += step) {
    out.add(v);
  }
  return out;
}

/// 两位十六进制大写。
String hex2(int b) => b.toRadixString(16).toUpperCase().padLeft(2, '0');
