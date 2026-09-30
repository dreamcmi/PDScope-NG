// filters.dart — 筛选 / 排序 / 视图档位的界面侧状态
//
// 语义与 core/src/filters.cpp 严格对齐：
//   · 筛选改变的是「视图」，不影响报文总数
//   · **默认值跟着协议走**（用 PD 的默认集合去筛 UFCS 会把报文全筛没）
//   · 时间窗口是归一化的 0..1（对应 startSample / totalSamples）
//
// 发回核心的 JSON 字段名就是 `pdscope_set_filter` 认识的那几个，别自作主张加名字。

/// 视图档位（对应核心的 ViewMode）。
enum ViewMode {
  all('all', '全部'),
  neg('neg', '协商'),
  err('err', '错误');

  const ViewMode(this.wire, this.label);
  final String wire;
  final String label;
}

/// 排序键。取值必须落在核心 `sortValueOf` 认识的集合里。
class SortKey {
  static const index = 'index';
  static const timeMs = 'timeMs';
  static const startSample = 'startSample';
  static const durationUs = 'durationUs';
  static const bitrate = 'bitrate';
  static const msgType = 'msgType';
  static const sop = 'sop';
  static const role = 'role';
  static const kind = 'kind';
  static const msgId = 'msgId';
  static const nObjects = 'nObjects';
  static const dataLen = 'dataLen';
  static const crcOk = 'crcOk';
  static const summary = 'summary';
}

/// 链路一栏的候选（UFCS 是 D+/D-/D±，PD 是 SOP 序列）—— 与 `linkValues()` 同源。
List<String> linkValues(String protocol) => protocol == 'UFCS'
    ? const ['D+', 'D-', 'D±']
    : const ['SOP', "SOP'", "SOP''", 'Hard Reset', 'Cable Reset'];

/// 类别一栏的候选 —— 与 `catValues()` 同源。
List<String> catValues(String protocol) => protocol == 'UFCS'
    ? const ['Control', 'Data', 'Custom', 'Error']
    : const ['Control', 'Data', 'Extended', 'VDM', 'Error'];

/// 方向一栏的候选 —— 与 `newFilters()` 的 roles 同源。
const List<String> roleValues = ['SRC', 'SNK', 'Plug'];

/// 链路一栏在界面上的标题（PD 是 SOP，UFCS 是物理链路）。
String linkTitle(String protocol) => protocol == 'UFCS' ? '链路' : 'SOP';

class FilterState {
  FilterState({
    required this.roles,
    required this.sops,
    required this.cats,
    required this.types,
    this.hideGoodCrc = true,
    this.onlyBad = false,
    this.onlyPower = false,
    this.onlyEnter = false,
    this.q = '',
    this.tFrom = 0,
    this.tTo = 1,
    this.sortKey = SortKey.index,
    this.sortAsc = true,
    this.viewMode = ViewMode.all,
  });

  /// 一份抓包的默认筛选。**必须跟着协议走**。
  factory FilterState.defaults(String protocol) => FilterState(
    roles: roleValues.toSet(),
    sops: linkValues(protocol).toSet(),
    cats: catValues(protocol).toSet(),
    types: <String>{},
  );

  Set<String> roles;
  Set<String> sops;
  Set<String> cats;

  /// 空集 = 不按类型过滤（核心的约定）。
  Set<String> types;

  bool hideGoodCrc;
  bool onlyBad;
  bool onlyPower;
  bool onlyEnter;
  String q;

  /// 归一化时间窗口（0..1）。
  double tFrom;
  double tTo;

  String sortKey;
  bool sortAsc;
  ViewMode viewMode;

  bool get hasTimeWindow => tFrom > 0 || tTo < 1;

  FilterState copy() => FilterState(
    roles: {...roles},
    sops: {...sops},
    cats: {...cats},
    types: {...types},
    hideGoodCrc: hideGoodCrc,
    onlyBad: onlyBad,
    onlyPower: onlyPower,
    onlyEnter: onlyEnter,
    q: q,
    tFrom: tFrom,
    tTo: tTo,
    sortKey: sortKey,
    sortAsc: sortAsc,
    viewMode: viewMode,
  );

  /// 复位视图：时间窗口与排序一起回到默认（对应界面的「复位视图」）。
  void resetView() {
    tFrom = 0;
    tTo = 1;
    sortKey = SortKey.index;
    sortAsc = true;
    viewMode = ViewMode.all;
  }

  /// 交给核心的 JSON。
  ///
  /// ⚠ 三处刻意为之：
  ///   · `roles` / `sops` / `cats` **一律显式发**——核心的规则是「给了就以调用方为准，
  ///     空数组表示一个都不选」，不发会退回协议默认集，那时「全部取消勾选」就静默失效。
  ///   · `types` 只在非空时发——核心把空集当成「不按类型过滤」，空数组发过去同样会被
  ///     当成空集而放行，反而更容易读错，索性不发。
  ///   · `q` 不做 trim——核心的 `setFilter` 自己会 trim。
  Map<String, dynamic> toJson() {
    final m = <String, dynamic>{
      'roles': roles.toList()..sort(),
      'sops': sops.toList()..sort(),
      'cats': cats.toList()..sort(),
      'hideGoodCrc': hideGoodCrc,
      'onlyBad': onlyBad,
      'onlyPower': onlyPower,
      'onlyEnter': onlyEnter,
      'q': q,
      'tFrom': tFrom,
      'tTo': tTo,
      'viewMode': viewMode.wire,
      'sort': {'key': sortKey, 'asc': sortAsc},
    };
    if (types.isNotEmpty) m['types'] = types.toList()..sort();
    return m;
  }
}
