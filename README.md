# PDScope-NG

USB Power Delivery / UFCS 抓包分析工具。**解析内核是 C++17，界面是 Flutter**，
两者以一层稳定的 C ABI（`core/include/pdscope/pdscope.h`）相连；
同一个内核另配一个不依赖图形环境的命令行程序 `pdscope-cli`。

支持的抓包来源：

| 来源 | 容器 | 解析路径 |
| --- | --- | --- |
| 正点原子 ATK-C | `.atkcc` | 通道电平采样 → BMC → 4B5B → PD 报文 |
| POWER-Z 分析仪 | `.sqlite` + `pd_table` | 报文已解到逻辑字节，直接走语义层 |
| POWER-Z 分析仪 | `.sqlite` + `ufcs_table` | UFCS 帧、方向、CRC-8 与数据字段 |
| 记录流 | `.pdStream` | PD 报文，无 ADC 波形 |
| 记录流 | `.ufcsStream` | UFCS 帧与事件，无 ADC 波形 |

## 目录

```
core/         C++17 解析内核 + C ABI 实现        → pdscope.dll / .so / .dylib
  include/pdscope/pdscope.h   ← 唯一的公开接口
  src/abi.cpp                 ← C ABI 的唯一实现
  src/session.cpp             ← 统一会话/报文/统计模型
cli/          独立命令行（直接调 C++，不经 C ABI）→ pdscope-cli
app/          Flutter 桌面界面（Dart FFI）       → PDScope（Windows 桌面程序）
tests/        核心单测（含 C ABI 契约测试）
doc/abi.md    C ABI 说明：约定、JSON 结构与二进制布局
doc/desktop.md 桌面外壳：拖放、文件关联、菜单这一层怎么接
doc/migration-audit.md  旧版迁移对照、样本验证结果与尚存差异
third_party/  SQLite / zlib / nlohmann-json（见 THIRD_PARTY_NOTICES.md）
_dl/          依赖的原始归档（用于核对校验值，不入发行包）
tools/        msvc-env.sh（Git Bash 里搭 MSVC 环境）、smoke-shell.py（外壳端到端探针）、
              capture-window.py（按标题抓窗口截图）、csv-diff.mjs
artifacts/    自检产出的截图（不参与打包）
```

## 外壳

桌面程序除了界面本身，还接了一层**跟操作系统打交道**的代码（Windows：
`app/windows/runner/`）：拖放、命令行与文件关联、第二个实例转交、中文菜单。
它交给界面的**只有路径**，格式一律由核心判定。

## 构建

### 核心与命令行

Windows 上从 Git Bash 构建时，先 `source` 环境脚本（它把 MSVC 的 `INCLUDE`/`LIB`/`PATH`
摆好；不 source 的话 `cl.exe` 找不到 `<cstdint>` 这种标准头）：

```bash
source tools/msvc-env.sh
cmake -S . -B build -G Ninja
cmake --build build
```

产物在 `build/out/`：`pdscope.dll`、`pdscope-cli.exe`、`pdscope-tests.exe`。

### 桌面界面

```bash
cd app
flutter pub get
flutter test          # 需要先构建好核心（界面经 FFI 找 build/out/）
flutter build windows --release
```

界面在运行期自己找动态库：先看环境变量 `PDSCOPE_LIB_DIR`，再看可执行文件同目录
（打包后就在这里），最后**逐级向上找到含 `CMakeLists.txt` 的仓库根**、进 `build/out`。
所以开发时不必手动拷贝。

## 打开抓包的四种方式

1. 界面里「打开文件」，或 `Ctrl+O`
2. **把文件拖进窗口**（支持一次拖多份，混合格式也行）
3. `PDScope.exe "D:\抓包\绿联70w.atkcc"`（可带多个路径）
4. 装了文件关联之后双击 `.atkcc`；已经有窗口开着时，**第二份会作为新标签进同一个窗口**

四条的落点相同：外壳只把**路径**交给界面，界面走同一个 `openFiles` 入口
（所以每次都是新标签、坏文件只红自己那一个标签）。

中文菜单、单实例转交、拖放为什么要在 Flutter 的子窗口上再挂一级消息过程 ——
见 [`doc/desktop.md`](doc/desktop.md)。

## 命令行用法

```bash
pdscope-cli <抓包文件>                      # 打印报文表
pdscope-cli <抓包文件> --csv [路径 | -]     # 导出 CSV（省略路径 = 同目录自动命名）
pdscope-cli <抓包文件> --json               # 导出 JSON（含统计摘要）
pdscope-cli <抓包文件> --channels           # 只列通道清单
```

`--channel N` 指定通道、`--rate HZ` 强制采样率、`--limit N` 只输出前 N 条、
`--bom` / `--no-bom` 控制 BOM（默认**写文件带、走管道不带**）。

CSV 一律 UTF-8；退出码 **0** 成功 · **1** 解析/导出失败 · **2** 用法错误。
标准输出传 CSV 时，提示信息走标准错误，不混进管道。

## 测试

```bash
./build/out/pdscope-tests.exe        # 核心单测（可带关键字过滤，如 ... csv）
./build/out/pdscope-tests.exe abi    # 只跑 C ABI 契约测试
./build/out/pdscope-tests.exe shell  # 只跑桌面外壳的纯逻辑（Windows）

cd app && flutter test               # FFI 集成 + 界面冒烟 + 外壳通道

python tools/smoke-shell.py          # 真窗口端到端：启动参数、单实例转交、菜单状态
```

几条值得知道的口径：

- **「跳过」不是「通过」**。缺样例文件的用例会记为跳过并单独报出来 —— 那些断言一条都没验证过。
- **私有抓包不入库**。依赖真实样本的用例在样本缺失时跳过；合成夹具则人人可跑。
- 测试会优先找旧版命名的样本，缺失时使用本地 `rawdata/` 中对应的抓包；`csv-diff.mjs` 默认扫描 `rawdata/`。
- 界面冒烟用 widget test 顶替开窗口：`flutter_tester` 是个真的 Dart VM（FFI、isolate
  都在），整个控件树照常布局与绘制，只是不出窗口，所以在 CI 上也能跑。
- `smoke-shell.py` 走的是**真进程 + 真窗口**：它从外部读窗口标题与菜单勾选/置灰。
  之所以读得到，是因为界面把这些状态回报给了外壳 —— 界面状态因此变成可断言的事实，
  不必再靠人看。「拖放」是唯一仍需人跑的一条（`HDROP` 跨进程造不出来），
  但它与命令行共用同一个出口。

## 许可

本项目以 **Apache-2.0** 发布（见 `LICENSE`）。

第三方组件各自保留其许可，**不重新标称为 Apache-2.0**：
SQLite（公有领域）、zlib（Zlib）、nlohmann/json（MIT），
以及 Flutter SDK 与 pub 包（主体 BSD-3-Clause）。
完整清单（含版本、来源、校验值、链接方式）见 `THIRD_PARTY_NOTICES.md`；
界面里「关于 → 查看许可」由 Flutter 自带的 `LicenseRegistry` 汇总展示。
