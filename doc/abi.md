# C ABI 说明（`pdscope.dll` / `libpdscope.so` / `libpdscope.dylib`）

> 权威声明在 `core/include/pdscope/pdscope.h`，**实现在 `core/src/abi.cpp`**。
> 本文解释约定、逐条列出 JSON/二进制的形状，并写清几处容易踩的地方。
> 字段若与本文不符，以 `core/src/session.cpp` 的实现为准（`metadata()` /
> `decodeStatsJson()` / `packetListItemJson()` / `busSeriesJson()` 四个函数），
> 改字段时**必须同步改本文**。

---

## 1. 分层与线程约定

```
Flutter/Dart  ──dart:ffi──▶  C ABI（abi.cpp）  ──▶  C++17 核心（session.cpp 等）
                                    ▲
                          pdscope-cli.exe 直接调 C++（不经 C ABI）
```

- 边界只走 **`extern "C"`**：不把 C++ 类、异常、STL 容器跨出去。
- **会话句柄**：`pdscope_session*` 是不透明指针。对 Dart 而言它的**整数值**就是会话地址，
  可以跨 isolate 传递。
- **线程**：除下面两个之外，**同一会话的全部调用必须同一个线程串行执行**。
  - `pdscope_cancel` 与 `pdscope_get_progress` 是**可从任意线程调用**的（内部只读/写原子量）。
  - 这是「进度轮询与取消」能在工作 isolate 阻塞解码时依然生效的原因：Dart 侧把会话地址
    交给主 isolate，由主 isolate 直接调这两个函数，不必等工作 isolate 从解码里脱身。

## 2. 通用约定

| 约定 | 说明 |
| --- | --- |
| 字符串编码 | **一律 UTF-8**。包括**文件路径**（`path_utf8`）。 |
| 内存归属 | 返回 `pdscope_buf` / `char*` 的内存**由库分配、由库释放**（`pdscope_buf_free` / `pdscope_str_free`）。重复释放是安全的（会把指针清零）。 |
| 长度与时间 | 64 位整数；原始偏移、时间戳、采样序号**不做浮点化**（保整数精度）。 |
| 错误 | 返回 `int32_t` 状态码；失败原因走 `char** err_out`（UTF-8，人类可读，含路径/偏移）。`pdscope_status_name()` 给英文短名。 |
| 内存不足 | 内部异常的翻译在 `abi.cpp` 的 `guard()` 里统一做，不会让异常穿过 C ABI。 |

### 状态码

`0` = OK；负数是失败。另有 `PDSCOPE_ERR_CANCELLED`，用于区分「用户取消」与「真失败」。

### ⚠ 文件路径必须是真 UTF-8（不是 ANSI）

核心内部在 Windows 上把 UTF-8 路径转成 UTF-16 再走 `_wfopen`（`core/src/util.cpp` 的
`openFileForRead`）。**不要**为此改回 `fopen`：`fopen` 在 Windows 上按进程 ANSI 代码页
解释窄字符串，路径里只要有一个汉字就必然打不开。

这条约定当年在命令行下"看着一直能用"，纯粹是因为 MSVC 的 `main(argc, argv)` 本来给的就是
ANSI 字节 —— 两边错得刚好对上。真正的调用方（Dart FFI、其它语言绑定）传的一定是 UTF-8，
于是只有那些才暴露问题。`pdscope-cli` 现在用 `wmain` 把 argv 转成 UTF-8 再走核心。
回归测试见 `tests/test_abi.cpp`。

## 3. 函数清单

### 版本

| 函数 | 说明 |
| --- | --- |
| `pdscope_version()` | 人类可读版本串（静态存储，**不需要释放**） |
| `pdscope_abi_version()` | ABI 版本号。**调用方先比对再调用**，不兼容时不要往下走。 |
| `pdscope_status_name(int32_t)` | 状态码 → 英文短名（静态存储） |

### 会话

| 函数 | 说明 |
| --- | --- |
| `pdscope_open_bytes(data, len, name_hint, err)` | 从内存打开。**格式按内容判定**，不看扩展名。`name_hint` 只用于显示名与默认导出名。 |
| `pdscope_open_file(path_utf8, err)` | 从路径打开（内部读全文件）。语义同上。 |
| `pdscope_close(s)` | 关闭并释放。传 `NULL` 安全。 |
| `pdscope_metadata(s, out_json)` | 容器级元数据。**不触发报文解码**，大文件也是毫秒级。 |

### 解码

| 函数 | 说明 |
| --- | --- |
| `pdscope_decode(s, opts, out_stats_json)` | 解码全部报文。可重复调用（第二次起直接返回上次结果）。`opts` 传 `NULL` 用默认。 |
| `pdscope_cancel(s)` | 请求中断（**任意线程可调**） |
| `pdscope_get_progress(s, out_progress)` | 进度快照（**任意线程可调**） |

`pdscope_decode_opts`：`channel`（`<0` = 会话自动挑，多通道 `.atkcc` 用）、
`sample_rate_override`（`>0` 时覆盖「文件声明 → 波形自检 → 兜底」三级策略）、
`metadata_only`（非 0 = 只解元数据）、`reserved`（传 0）。

`pdscope_progress`：`phase`（0=空闲 1=读容器 2=解码）、`channel`、`done`、`total`、`packets`。

### 视图（筛选 / 排序）

| 函数 | 说明 |
| --- | --- |
| `pdscope_set_filter(s, filter_json, err)` | 设置筛选与排序。`NULL` 或 `{}` = 清空。 |
| `pdscope_view_count(s, out_count)` | 当前视图条数 |
| `pdscope_packet_count(s, out_count)` | **未筛选**的报文总条数 |

> **视图是懒重建的。** `view_count` / `query_page` / `export_csv` 都会在需要时先把视图
> 建出来，三者看到的必须是**同一个**视图。
> ⚠ 这里曾经有过一个缺陷：`view_count` 直接读了内部的 `vector::size()`，解码后视图还没
> 构建就返回 0，而 `query_page` 会顺手重建、照常返回一行行数据 —— 界面于是变成
> 「列表里有行、条数写 0 条」。它只在**视图非空**时才显形，视图恰好为空时看着完全正常，
> 所以躲过了很久。回归测试见 `tests/test_abi.cpp` 的 `abi_view_count_agrees_with_query_page`。

### 查询

| 函数 | 说明 |
| --- | --- |
| `pdscope_query_page(s, offset, limit, out_json)` | 当前视图的一页（JSON 数组） |
| `pdscope_packet_detail(s, index, out_json)` | 单条报文详情。`index` 是**原始报文序号**，不是视图行号 |
| `pdscope_waveform_range(s, channel, start, end, max_points, out_binary)` | 原始电平包络（**二进制**） |
| `pdscope_bus_series(s, target_points, out_json)` | 模拟量轨迹（JSON） |
| `pdscope_type_counts(s, out_json)` | 报文类型 → 条数 |
| `pdscope_packet_marks(s, out_binary)` | 时间轴报文标记（**二进制**） |

### 导出

| 函数 | 说明 |
| --- | --- |
| `pdscope_export_csv(s, opts, out_text)` | 导出**当前视图**为 CSV |
| `pdscope_export_json(s, opts, out_text)` | 导出**全部报文**为 JSON |
| `pdscope_default_csv_name(s, out_name)` | 默认文件名主干（如 `抓包-ch1.csv`），调用方负责拼目录 |

`pdscope_export_opts`：`limit`（`>0` 只导前 N 条）、`bom`（CSV 的 UTF-8 BOM；落盘带、管道不带）、
`reserved`（传 0）。

> GUI 的「导出 CSV」走**当前视图**，CLI 的 `--csv` 走**全部报文** —— 这是刻意的不一致，
> 两边各有各的道理，`tests/test_csv.cpp` 与 `test_abi.cpp` 都钉着。

---

## 4. JSON 结构

### `pdscope.metadata`

容器级信息。**共用字段**（三种来源都有）：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | string | 显示名（来自 `name_hint` 或路径的 basename） |
| `source` | string | 来源短名（如 `powerz` / `atkcc` / `pdstream`） |
| `container` | string | 容器种类（如 `sqlite` / `atkcc` / `pdstream`） |
| `protocol` | string | `"USB PD"` 或 `"UFCS"` |
| `fileBytes` | number | 文件字节数 |
| `decoded` | bool | 是否已解码 |
| `kind` | string | 容器子类型（POWER-Z 用；`.atkcc` 为空串） |
| `title` | string | 一句话描述（如 `ATK-C · .atkcc`） |
| `sampleRate` / `sampleRateSource` / `sampleRateNote` | number/string | 采样率及其**来源标注**与补充说明 |
| `sampleRateKey` / `sampleRateRaw` / `samplingFrequencyRaw` | string \| null | `.atkcc` 的声明原文（另外两种来源为 null） |
| `totalSamples` | number | 总采样点数 |
| `durationSec` | number | 时长（秒） |
| `entryCount` | number | ZIP 条目数（`.atkcc`；其它为 0） |
| `multiChannel` | bool | 是否多通道 |
| `busLabels` | string[] | 模拟量两路的显示名 |
| `hasBus` / `busPoints` | bool/number | 是否有模拟量轨迹 / 采样点数 |
| `tableRows` / `chartRows` | number | POWER-Z 的表行数 / 图行数（`.atkcc` 为 0） |
| `unsupported` | string \| null | 「认不出但没丢」的东西的一句话说明 |
| `channels` | object[] | 通道清单，见下 |

`channels[]`：`channel`（号）、`label`、`folder`（`.atkcc` 的目录名，其它为 null）、
`totalSamples`、`totalBytes`、`chunks`（分块数，POWER-Z 为 0）、`effectiveSampleLimit`。

`sqlite`：`.sqlite` 来源给 `{pageSize, pageCount, textEncoding, writeVersion}`，其它为 `null`。

### `pdscope.filter`

传给 `pdscope_set_filter` 的对象。**未出现的键保持默认值**。

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `roles` | string[] | 方向：`SRC` / `SNK` / `Plug` |
| `sops` | string[] | 链路。PD：`SOP` / `SOP'` / `SOP''` / `Hard Reset` / `Cable Reset`；UFCS：`D+` / `D-` / `D±` |
| `cats` | string[] | 类别。PD：`Control`/`Data`/`Extended`/`VDM`/`Error`；UFCS：`Control`/`Data`/`Custom`/`Error` |
| `types` | string[] | 具体报文类型。**空集 = 不按类型过滤** |
| `hideGoodCrc` | bool | 屏蔽自动确认报文（默认 **true**） |
| `onlyBad` / `onlyPower` / `onlyEnter` | bool | 快捷筛选：只看 CRC 错误 / 只看功率协商 / 只看状态切换 |
| `q` | string | 关键字（内部会 trim；大小写不敏感，匹配类型/摘要/hex 等） |
| `tFrom` / `tTo` | number | 归一化时间窗口 `0..1`（对应 `startSample / totalSamples`） |
| `viewMode` | string | `"all"` / `"neg"` / `"err"`。**不是这三个值一律当 `all`** |
| `sort` | object | `{key: string, asc: bool}` |

> ⚠ `roles` / `sops` / `cats` / `types` 只要**出现**（哪怕是空数组），就以调用方为准 ——
> 「全部取消勾选」是合法状态。反过来说，界面想表达「一个都不选」就**必须显式发空数组**，
> 不发会被当成「没意见」而退回协议默认集。

**默认值跟着协议走**（`newFilters(protocol)`）：用 PD 的默认集合去筛 UFCS，会把报文全筛没。

### `packet.list`

`pdscope_query_page` 返回数组，每项是列表用的紧凑对象：

| 字段 | 说明 |
| --- | --- |
| `index` | **原始报文序号**（排序后仍指向同一条 —— 详情、选中行都用它） |
| `seq` | 同 `index`，历史名字保留 |
| `channel` | 通道号 |
| `sop` | 链路（见 `sops` 取值） |
| `msgType` | 报文类型名 |
| `role` | 发送方：`SRC` / `SNK` / `Plug` |
| `kind` / `msgKind` | 类别名（= `cats` 取值）/ 更细的子类名 |
| `msgId` | 报文 ID（无则 `null`） |
| `objects` | 数据对象个数（**UFCS 下换成 `bytes`**：数据字节数） |
| `timeMs` / `elapsed` | 毫秒数 / `hh:mm:ss.mmm` 文本 |
| `startSample` / `endSample` | 采样区间 |
| `durationUs` | 线上时长（微秒） |
| `vbus` / `ibus` | 当时的母线电压/电流 |
| `dataHex` | 数据区 hex 文本 |
| `crc` | **`"ok"` / `"bad"` / `"none"`**，见下 |
| `summary` | 一句话解析 |
| `warn` | 告警条数 |
| `ackOf` / `ackType` | 若本条是某条的确认对象：被确认方的序号与类型 |

> ⚠ **`crc` 是三态，不是布尔。** `"none"` = **未记录**（POWER-Z 的 `pd_table` 根本不存 CRC），
> 它**不等于**「通过」。任何地方都不许把 `none` 当成 `ok`、不许因「重算对得上」就报全通过。
> CSV 里 `none` 是**空单元格**而不是 `OK`。

### `pdscope.packet_detail`

= `packet.list` 的全部字段，**再加**：

`text`（整帧文本）、`header` / `extHeader` / `rev` / `revText` / `msgTypeRaw`（位域与原文，
无则 `null`）、`link`、`category`、`eop`、`synthetic`（true = 分析仪给的逻辑字节，**不是从波形
解出来的**）、`roleInferred`（true = 方向靠推断，不是规范/容器给的）、
`crcValue` / `crcCalc`（记录值 / 重算值，未记录时 `crcValue` 为 `null`）、
`dataWords` / `dataBytes`、`warnings[]`（`{short, long}`）、
`details[]`（`{key, value}` 数组）。

> **详情分组契约**：`details` 是扁平的 `{key,value}[]`，靠 `key == "Object"` 当**标题哨兵**
> 切分组。发详情的模块必须**先 `em.object()` 再 `em.detail()`**，界面侧认 `Object` 与 `对象`。
> 容器观测字段（UFCS 的 `layout` / `training` / `lenField` / `counter` / `dirByte`）仍然透出，
> 但**界面不展示** —— 观测值不是规范字段，摆在一起读者分不清哪个能查规范。
> ⚠ 那段渲染被整块注释掉了，**数据层不要跟着删**。

### `decodeStats`（`pdscope_decode` 的输出）

| 字段 | 说明 |
| --- | --- |
| `channel` | 实际解码的通道 |
| `source` / `kind` / `protocol` | 来源短名 / 子类型 / 协议 |
| `totalSamples` / `durationSec` | 采样点数 / 时长 |
| `sampleRate` / `sampleRateSource` / `sampleRateNote` | 采样率与来源（`declared` / `measured` / `default`） |
| `sampleRateDeclared` / `sampleRateMeasured` | 声明值 / 波形反推值（后者可能为 `null`） |
| `edges` / `trimmedBytes` | 边沿数 / 裁掉的尾部字节 |
| `packetCount` | 报文总数 |
| `badCrc` / `crcUnknown` / `warnings` | CRC 错误数 / **未记录**数 / 告警总数 |
| `badWire` / `truncatedRows` | 线级错误 / 截断行 |
| `connectCount` / `disconnectCount` | 插拔事件数（PD） |
| `ufcsFrames` / `ufcsEvents` / `ufcsUnlocatedRows` | UFCS：帧数 / 状态事件数 / **没定位出帧的行数** |
| `ufcsDirFromLine` / `ufcsDirInferred` | UFCS 方向：来自规范单向命令表或容器链路字节的条数 / **落到「接收方地址推断」那一级的条数** |
| `ufcsEventCodes` | `[{code, n}]`，状态事件的 opcode 分布 |
| `events[]` | `{kind, tsMs, code}` |
| `unsupportedMsgs` / `unsupported` | 认不出但没丢的条数 / 说明 |
| `tableRows` / `chartRows` | POWER-Z 表/图行数 |
| `channelPick` | 多通道自动挑的结果：`{picked, noiseRejected, allNoisy}` |

> ⚠ **`ufcsDirInferred` 是必须为 0 的指标**（在这批样本上）。方向一旦靠猜，双向命令
> （Request/ACK）的 SRC/SNK 会直接反过来，肉眼极难发现。`tests/test_abi.cpp` 与 Dart 侧的
> 集成测试都钉着 `ufcsDirInferred == 0`。
>
> ⚠ 三级判据的第 ③ 级（接收方地址推断）**是工具取舍、不是规范规定**，别拿规范条文给它背书。

### `busSeries`

等间隔抽稀后的模拟量轨迹：

`t0`（起始秒）、`step`（步长秒）、`n`、`sampleRate`、`hasAux`（是否有第二组曲线）、
`vmax` / `imax` / `camax` / `cbmax`（各轴量程）、`labels`（第二组曲线的显示名）、
`vbus[]`、`ibus[]`、`ca[]` / `cb[]`（`hasAux` 为假时是 `null`）。

`.atkcc` 只有两路（VBUS/IBUS）；POWER-Z 还有 CC1/CC2 或 DP/DM。

### `pdscope.type_counts`

`[{type: string, n: number}]`，**按条数降序**。

**统计的是全部报文，不受筛选影响** —— 界面拿它当「具体报文类型」筛选的候选与计数，
那份候选列表本身不该随勾选变化（否则勾掉一个，它自己就从列表里消失了，再也勾不回来）。

---

## 5. 二进制布局（小端）

走 JSON 的都要解析一遍、几万条会明显浪费，所以两处改成紧凑二进制。

### `pdscope_waveform_range`

```
u32 n                 桶数
u64 bucket            每个桶覆盖的采样点数
u64 start_sample      起始采样点
f32 hi[n]             每桶是否出现过「高」
f32 lo[n]             每桶是否出现过「低」
```

共 `4 + 8 + 8 + 8n` 字节。

### `pdscope_packet_marks`

```
u32 n                 报文条数
f64 ts[n]             每条的时间（秒）
u8  kind[n]           类别编号：0=Control 1=Data 2=Extended 3=VDM 4=Error 5=Custom
u8  flags[n]          bit0 = CRC 校验未通过；bit1 = 是某个配对的确认对象
```

共 `4 + 10n` 字节。取的是**全部报文**（时间轴画整份抓包，不跟表格筛选走）。

> ⚠ **`kind` 的编号是接口契约**：加类别要在核心（`kindCodeOf`）与 Dart
> （`kKindNames`）两侧同时改，否则时间轴会按错的颜色画、且不会报错。

---

## 6. 典型调用序列

```c
char* err = NULL;
pdscope_session* s = pdscope_open_file("C:\\抓包\\我的样本.atkcc", &err);
if (!s) { fprintf(stderr, "%s\n", err); pdscope_str_free(err); return 1; }

// 1) 元数据不触发解码，可以立刻画界面
pdscope_buf meta = {0};
pdscope_metadata(s, &meta);
/* … 用 meta.data/meta.len … */ pdscope_buf_free(&meta);

// 2) 解码（长任务）。进度与取消都不必等它
pdscope_decode_opts opts = { .channel = -1, .sample_rate_override = 0,
                             .metadata_only = 0, .reserved = 0 };
pdscope_buf stats = {0};
pdscope_decode(s, &opts, &stats);
pdscope_buf_free(&stats);

// 3) 筛选 → 视图
pdscope_set_filter(s, "{\"hideGoodCrc\":true,\"cats\":[\"VDM\"]}", &err);

// 4) 列表按页取，别一次拉全量
pdscope_buf page = {0};
pdscope_query_page(s, 0, 200, &page);
pdscope_buf_free(&page);

// 5) 详情按**原始序号**取
pdscope_buf detail = {0};
pdscope_packet_detail(s, 0, &detail);
pdscope_buf_free(&detail);

pdscope_close(s);
```

## 7. 与 JS 基线的差分口径

- **CSV 逐字节比对**（列名、转义、CRLF、BOM 规则、默认文件名），JSON 按字段语义比对。
- 特别要盖住的四件事：CRC「未记录」、UFCS 方向、GoodCRC 配对、`.pdStream` 无 ADC。

## 8. 改这个接口时的检查单

1. 改 `pdscope.h` 里的函数签名或结构体 → **`pdscope_abi_version()` 加一**。
2. 改任何 JSON 字段名 → 同步改本文、`app/lib/core/models.dart`，并跑
   `app/test/core_integration_test.dart`。
3. 改 `kind` 编号 → 同时改 `core/src/session.cpp` 的 `kindCodeOf` 与
   `app/lib/core/models.dart` 的 `kKindNames`。
4. 加导出符号 → 同时改 `core/pdscope.def`（Windows 的 `.def` 是导出白名单，
   漏了会在运行期找不到符号）。
5. 跑一轮：`pdscope-tests`（含 `tests/test_abi.cpp`）→ `flutter test`。
