// core_integration_test.dart — 界面这一侧的端到端检查
//
// 它验证的是**整条链路真的接通了**：Dart → FFI → 工作 isolate → C 核心的动态库。
// 这些断言刻意不重复 C++ 侧 `pdscope-tests` 的解析细节（那边 51 个用例已经盖住），
// 只盯「跨过 ABI 之后语义有没有走样」：
//   · 三种来源都能按内容识别（不看扩展名）
//   · POWER-Z 的 PD 报文 CRC 是「未记录」而不是「通过」
//   · UFCS 的方向**一条都不靠猜**（`dirInferred == 0`）—— 猜错的方向在双向命令上
//     会直接反过来，肉眼极难发现，所以要在这一层钉死
//   · 新增的 type_counts / packet_marks 与报表总数自洽
//   · GUI 的 CSV 导出走「当前视图」，与命令行「导全部报文」刻意不同
//
// 本机样本不入库（私有抓包），所以**样本不存在时整个用例跳过**而不是失败。
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/engine.dart';
import 'package:pdscope_app/core/document.dart';
import 'package:pdscope_app/core/filters.dart';
import 'package:pdscope_app/core/models.dart';
import 'sample_fixture.dart';

/// 找一份本机样本；找不到返回 null（私有抓包不在仓库里，缺失是正常情况）。
String? _sample(String name) {
  return sampleFixture(name);
}

void main() {
  late EngineClient engine;

  setUpAll(() async {
    engine = await EngineClient.instance();
  });

  test('POWER-Z 的 PD 抓包：CRC 只能写「未记录」，不能写「通过」', () async {
    final path = _sample('山泽60w-ip18pro.sqlite');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    final meta = await engine.openFile(101, path);
    expect(meta['protocol'], 'USB PD');
    expect(meta['container'], 'sqlite');

    final stats = await engine.decode(101);
    expect(stats, isNotNull, reason: '解码应当返回统计');
    final packetCount = stats!['packetCount'] as int;
    expect(packetCount, greaterThan(0));

    // 分析仪不存 CRC ⇒ 全部落进「未记录」。这是**口径**，不是统计口径的巧合。
    expect(
      stats['crcUnknown'],
      packetCount,
      reason: 'PD 的 pd_table 不存 CRC，应当全部记为「未记录」',
    );
    expect(stats['badCrc'], 0);

    // 默认筛选会屏蔽 GoodCRC，所以视图条数应当**少于**报表总数。
    final viewCount = await engine.viewCount(101);
    expect(viewCount, lessThan(packetCount));
    // ⚠ 光有「小于」是不够的：0 也满足「小于」。核心曾经有一个缺陷让
    //   `pdscope_view_count` 恒返回 0（视图没重建就直接读了长度），而
    //   `pdscope_query_page` 照常返回一行行数据 —— 界面于是变成
    //   「列表里有行、条数写 0 条」。必须同时钉死「大于 0」。
    expect(viewCount, greaterThan(0), reason: '视图里有行，条数就不该是 0');

    final rows = await engine.page(101, 0, 20);
    expect(rows, isNotEmpty);
    expect(rows.first['msgType'], isA<String>());
    expect(rows.first['crc'], 'none');
    expect(
      rows.where((r) => r['msgType'] == 'Accept').first['summary'],
      '接收请求',
      reason: 'C ABI 返回的 JSON 是 UTF-8，FFI 不能逐字节按 Latin-1 解码',
    );

    final detail = await engine.detail(101, 0);
    expect(detail['details'], isNotEmpty, reason: '详情应当有分组条目');
    expect(detail['synthetic'], isTrue, reason: '分析仪来源的报文不是从波形解出来的');

    await engine.close(101);
  });

  test('UFCS 抓包：方向一条都不靠推断，帧数与统计自洽', () async {
    final path = _sample('ufcs_vivo_x300u.sqlite');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    final meta = await engine.openFile(102, path);
    expect(meta['protocol'], 'UFCS');

    final stats = await engine.decode(102);
    expect(stats, isNotNull);
    expect(stats!['packetCount'], greaterThan(1000), reason: '这份是几万行的大样本');

    // ⚠ 关键断言：方向必须全部来自「规范单向命令表」或「容器链路字节」，
    //   一条都不许落到「接收方地址推断」那一级。
    expect(
      stats['ufcsDirInferred'],
      0,
      reason: '方向一旦靠猜，双向命令的 SRC/SNK 会直接反过来，肉眼发现不了',
    );
    expect(stats['ufcsDirFromLine'], greaterThan(0));
    expect(stats['ufcsUnlocatedRows'], 0, reason: '样本里每一行都应当能定位出帧');

    await engine.close(102);
  });

  test('UFCS 记录流按内容识别为 UFCS，帧数与 SQLite 一致', () async {
    final stream = _sample('rawdata/CTK10UL_X300U_UFCS.ufcsStream');
    final sqlite = _sample('rawdata/CTK10UL_X300U_UFCS.sqlite');
    if (stream == null || sqlite == null) {
      markTestSkipped('本机没有成对的 UFCS 样本，跳过');
      return;
    }

    final streamMeta = await engine.openFile(106, stream);
    final sqliteMeta = await engine.openFile(107, sqlite);
    expect(streamMeta['container'], 'ufcsstream');
    expect(streamMeta['protocol'], 'UFCS');
    expect(sqliteMeta['protocol'], 'UFCS');

    final streamStats = await engine.decode(106);
    final sqliteStats = await engine.decode(107);
    expect(streamStats!['packetCount'], sqliteStats!['packetCount']);
    expect(streamStats['ufcsUnlocatedRows'], 0);
    expect(streamStats['ufcsDirInferred'], 0);

    await engine.close(106);
    await engine.close(107);
  });

  test('排序后条数不变也会作废旧页缓存', () async {
    final path = _sample('山泽60w-ip18pro.sqlite');
    if (path == null) {
      markTestSkipped('本机没有 PD 样本，跳过');
      return;
    }
    final meta = await engine.openFile(108, path);
    await engine.decode(108);
    final doc = CaptureDocument(id: 108, path: path, displayName: 'sort-test')
      ..meta = CaptureMeta(meta)
      ..filters = FilterState.defaults('pd')
      ..state = DocState.done;
    await doc.applyFilters(engine);
    await doc.loadPage(engine, 0);
    final firstIndex = doc.rows.at(0)!.index;
    final count = doc.rows.total;

    doc.filters.sortKey = SortKey.timeMs;
    doc.filters.sortAsc = false;
    await doc.applyFilters(engine);
    expect(doc.rows.total, count);
    expect(doc.rows.at(0), isNull, reason: '相同条数的新视图不得沿用旧位置');
    await doc.loadPage(engine, 0);
    expect(doc.rows.at(0)!.index, isNot(firstIndex));
    final secondIndex = doc.rows.at(1)!.index;
    await doc.selectRow(engine, doc.rows.at(0)!.index, viewIndex: 0);
    await doc.stepDetail(engine, 1);
    expect(doc.selected, secondIndex, reason: '详情导航按筛选后的顺序前进');
    final earliest = (await engine.page(108, count - 1, 1)).first['index'];
    await doc.selectNearestAtTime(engine, 0);
    expect(doc.selected, earliest, reason: '时间轴单击按当前视图找最近报文');
    await doc.fitTimeToCurrentView(engine);
    expect(doc.filters.tFrom, inInclusiveRange(0, 1));
    expect(doc.filters.tTo, inInclusiveRange(doc.filters.tFrom, 1));

    await engine.close(108);
    doc.dispose();
  });

  test('ATK-C 的 .atkcc：按 ZIP 魔数识别，采样率与通道都拿得到', () async {
    final path = _sample('安可60w-ip18pro.atkcc');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    final meta = await engine.openFile(103, path);
    expect(meta['container'], 'atkcc');
    expect(meta['protocol'], 'USB PD');
    expect(meta['channels'], isA<List>());
    expect((meta['channels'] as List), isNotEmpty);

    final stats = await engine.decode(103);
    expect(stats, isNotNull);
    expect(stats!['packetCount'], greaterThan(0));

    // 采样率的三级策略要给出**来源标注**，界面顶栏就是照它显示的。
    expect(stats['sampleRateSource'], anyOf('declared', 'measured', 'default'));
    expect(stats['sampleRate'], greaterThan(0));

    await engine.close(103);
  });

  test('type_counts 与 packet_marks 和报表总数自洽', () async {
    // 用 UFCS 那份大样本（若没有则退回 PD 样本）。
    final path =
        _sample('ufcs_vivo_x300u.sqlite') ?? _sample('山泽60w-ip18pro.sqlite');
    if (path == null) {
      markTestSkipped('本机没有样本，跳过');
      return;
    }

    await engine.openFile(104, path);
    final stats = await engine.decode(104);
    final packetCount = stats!['packetCount'] as int;

    // 类型计数：**统计全部报文**，所以各项之和必须等于报文总数。
    final counts = await engine.typeCounts(104);
    expect(counts, isNotEmpty);
    final sum = counts.values.fold<int>(0, (a, b) => a + b);
    expect(sum, packetCount, reason: '类型计数是「全部报文」的分布，和应当等于报文总数（不受筛选影响）');

    // 时间轴标记：条数与报文总数一致，类别编号落在约定范围内。
    final marks = PacketMarks.decode(await engine.packetMarks(104));
    expect(marks.n, packetCount);
    expect(marks.ts.length, packetCount);
    for (var i = 0; i < marks.n; i += 997) {
      expect(marks.kind[i], lessThan(kKindNames.length));
    }

    await engine.close(104);
  });

  test('GUI 的 CSV 导出走当前视图，比全部报文少', () async {
    final path = _sample('山泽60w-ip18pro.sqlite');
    if (path == null) {
      markTestSkipped('本机没有这份样本，跳过');
      return;
    }

    await engine.openFile(105, path);
    final stats = await engine.decode(105);
    final packetCount = stats!['packetCount'] as int;

    final filtered = await engine.exportCsv(105, bom: false);
    // 每行都以 CRLF 结束，且**结尾不带换行** ⇒ 行数 = 换行数 + 1（含表头）。
    final lineCount = '\r\n'.allMatches(filtered).length + 1;
    expect(lineCount, lessThan(packetCount + 1), reason: '导出的是筛选后的视图，不是全部报文');
    expect(filtered, startsWith('"'));

    await engine.close(105);
  });
}
