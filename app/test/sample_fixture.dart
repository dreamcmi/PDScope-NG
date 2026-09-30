import 'dart:io';

/// Prefer the named legacy sample; use the supplied rawdata captures when the
/// private legacy set is absent from this checkout.
String? sampleFixture(String name) {
  const fallback = {
    '山泽60w-ip18pro.sqlite': 'DJIPOWER_VIVOX300U_PPS.sqlite',
    'ufcs_vivo_x300u.sqlite': 'CTK10UL_X300U_UFCS.sqlite',
    '安可60w-ip18pro.atkcc': 'x300u_pps.atkcc',
    '酷泰科10u线-ip18pro.atkcc': 'x300u_pps.atkcc',
    '苹果40w-ip18pro.atkcc': 'x300u_pps.atkcc',
  };
  final candidates = [name, if (fallback.containsKey(name)) 'rawdata/${fallback[name]}'];
  for (final base in ['..', '.', '../..']) {
    for (final candidate in candidates) {
      final f = File('$base${Platform.pathSeparator}$candidate');
      if (f.existsSync()) return f.absolute.path;
    }
  }
  return null;
}
