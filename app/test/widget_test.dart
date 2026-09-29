// widget_test.dart — 界面冒烟测试
//
// 刻意只测「不依赖核心动态库」的部分：真正的解析行为由 C++ 侧的
// `pdscope-tests`（51 个用例）与 `tools/diff-against-js.mjs` 的逐字节差分负责，
// 界面这边再做一遍只是重复。界面测试的价值在于布局与取色这类自洽性。
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pdscope_app/core/palette.dart';

void main() {
  testWidgets('明暗两套色板的取色接口自洽', (tester) async {
    for (final p in [Palette.light, Palette.dark]) {
      // 六种类别各有一个颜色，都能取到（`forKind` 的 default 分支不该被走到）。
      for (final kind in ['Control', 'Data', 'Extended', 'VDM', 'Error', 'Custom']) {
        expect(p.forKind(kind), isNot(p.tx2), reason: '$kind 应该有自己的颜色');
      }
      // 三个方向各有一组前景/背景色，且不相同。
      for (final role in ['SRC', 'SNK', 'Plug']) {
        final (fg, bg) = p.forRole(role);
        expect(fg, isNot(bg));
      }
      expect(p.groups.length, 8, reason: '详情分组是 8 色循环');
    }
  });

  testWidgets('未知类别退化成次要文字色而不是抛错', (tester) async {
    expect(Palette.light.forKind('无此类别'), Palette.light.tx2);
  });

  testWidgets('色板能通过 InheritedWidget 取到', (tester) async {
    await tester.pumpWidget(
      const PaletteScope(
        palette: Palette.dark,
        child: MaterialApp(home: SizedBox()),
      ),
    );
    final ctx = tester.element(find.byType(SizedBox));
    expect(PaletteScope.of(ctx).brightness, Brightness.dark);
  });
}
