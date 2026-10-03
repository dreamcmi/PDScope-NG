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
| **实时采集** | 设备（无文件） | 直接从设备收报文与母线采样；**设备层待定，现为模拟源** |

## 目录

```
core/         C++17 解析内核 + C ABI 实现        → pdscope.dll / .so / .dylib
  include/pdscope/pdscope.h   ← 唯一的公开接口
  src/abi.cpp                 ← C ABI 的唯一实现
  src/session.cpp             ← 统一会话/报文/统计模型
cli/          独立命令行（直接调 C++，不经 C ABI）→ pdscope-cli
app/          Flutter 桌面界面（Dart FFI）       → PDScope（Windows / Linux / macOS）
tests/        核心单测（含 C ABI 契约测试）
doc/abi.md    C ABI 说明：约定、JSON 结构与二进制布局
doc/desktop.md 桌面外壳：拖放、文件关联、菜单这一层怎么接
doc/live-capture.md  实时采集：入口、状态、读数、导出与「还没做的」
doc/migration-audit.md  旧版迁移对照、样本验证结果与尚存差异
third_party/  SQLite / zlib / nlohmann-json（见 THIRD_PARTY_NOTICES.md）
_dl/          依赖的原始归档（用于核对校验值，不入发行包）
tools/        build-all.sh / build-all.ps1（一键构建 + 归置）、msvc-env.sh（搭 MSVC 环境）、
              smoke-shell.py（外壳端到端探针）、capture-window.py（按标题抓窗口截图）、
              csv-diff.mjs
artifacts/    自检产出的报告（不参与打包）
dist/         一键构建归置出的发行目录（不参与打包）
```

## 外壳

桌面程序通过 Windows `app/windows/runner/`、Linux `app/linux/runner/`、
macOS `app/macos/Runner/` 接入拖放、命令行与文件打开事件、中文菜单。
Windows 和 Linux 转交第二个实例的文件；macOS 通过系统的应用打开事件复用窗口。
它交给界面的**只有路径**，格式一律由核心判定。

## 构建

### 一键构建（推荐）

各用各平台自带的 shell，两份做的是同一件事（同样的五步、同样的开关、同样的归置布局）：

```bash
tools/build-all.sh                                            # Linux / macOS
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-all.ps1  # Windows
```

Windows 也可以用 Git Bash 跑 `tools/build-all.sh`；不想开 Git Bash 就用上面那条 PowerShell。

默认做完全部五步：构建 C++ 核心 → 跑核心单测 → `pub get` / `analyze` / `flutter test`
→ 构建 Flutter 发行版 → 归置到 `dist/PDScope-<平台>-<架构>/`。
最后那个目录是**可以直接双击运行**的：Windows 双击 `PDScope.exe`，macOS 双击
`PDScope.app`，Linux 双击 `PDScope`；核心动态库、`data/` 与许可声明都在同一层，
不需要额外装什么。归置完还会断言核心库确实躺在可执行文件旁边 —— 因为
`runner/CMakeLists.txt` 在核心库缺失时**只 WARNING 不报错**，界面照样编译成功、
运行时才弹「核心动态库没有加载成功」。

开关（bash / PowerShell）：`--no-tests` / `-NoTests`（只编译）、`--core-only` /
`-CoreOnly`、`--app-only` / `-AppOnly`、`--debug` / `-DebugBuild`、`--clean` / `-Clean`、
`--jobs N` / `-Jobs N`、`--flutter <SDK 目录>` / `-Flutter <SDK 目录>`、
`--out <目录>` / `-OutDir <目录>`。
找不到 Flutter SDK 时会按 参数 → `PDSCOPE_FLUTTER`/`FLUTTER_ROOT` → `PATH`
→ 几个常见位置依次找，都不中就报错并说明怎么给。

两份脚本都自己找一套**真的带 `cl.exe`** 的 MSVC 工具集（`build-all.sh` 走
`msvc-env.sh`，判据已改成必须存在 `cl.exe`），所以不会撞上「按版本号取最大」挑到
残缺目录的坑。

⚠ `tools/build-all.ps1` 必须保持**带 BOM 的 UTF-8**。Windows PowerShell 5.1 读 `.ps1`
时若没有 BOM 会按系统代码页解码，中文注释变乱码，而乱码字节里只要有一个落成引号或
括号，脚本就报语法错误、行号还指向注释中间。改完记得确认 BOM 还在。

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

### Linux 与 macOS

Linux 需要 Clang、CMake ≥ 3.20、Ninja、pkg-config、GTK 3 和 liblzma 开发库；
macOS 需要 Xcode、CMake 和 Flutter；使用 Flutter 3.47.1，macOS 部署目标为 12.0。
Linux 和 macOS 的 Flutter 构建会同时构建并打包核心库：

```bash
cd app
flutter pub get
flutter build linux --release    # 在 Linux 上执行
flutter build macos --release    # 在 macOS 上执行
```

Linux 产物是 `app/build/linux/x64/release/bundle/`（包括 `PDScope`、`lib/` 和 `data/`），
macOS 产物是 `app/build/macos/Build/Products/Release/PDScope.app`。
两者必须按完整目录分发。macOS 使用临时签名；面向公众分发时还需开发者签名与公证。

macOS 请在未被文件同步服务管理的目录中构建和解压运行。部分同步目录（例如同步中的
Desktop）会持续给 `.app` / `.framework` 写入 Finder 元数据，造成
`resource fork, Finder information, or similar detritus not allowed` 签名错误；
遇到这种情况，应将源码移到普通本地目录重新构建。新增依赖缓存可放在仓库忽略的
`.local/` 中，例如先设置 `export PUB_CACHE="$PWD/.local/pub-cache"` 再进入 `app/`。

核心、CLI 与测试也可单独构建，使用不同目录避免复用另一平台的 CMake 缓存：

```bash
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 3
ctest --test-dir build/native --output-on-failure
cd app
PDSCOPE_LIB_DIR="$PWD/../build/native/out" flutter test
```

动态库搜索依次为 `PDSCOPE_LIB_DIR`、可执行文件目录、Linux 的 `lib/` 或 macOS 的
`Contents/Frameworks/`，最后查找仓库的开发构建输出。macOS 核心会随 Xcode 的目标架构构建并签名。

## 打开抓包的四种方式

1. 界面里「打开文件」，或 `Ctrl+O`
2. **把文件拖进窗口**（支持一次拖多份，混合格式也行）
3. `PDScope.exe "D:\抓包\绿联70w.atkcc"`（可带多个路径）
4. 装了文件关联之后双击 `.atkcc`；已经有窗口开着时，**第二份会作为新标签进同一个窗口**

四条的落点相同：外壳只把**路径**交给界面，界面走同一个 `openFiles` 入口
（所以每次都是新标签、坏文件只红自己那一个标签）。

中文菜单、单实例转交、拖放为什么要在 Flutter 的子窗口上再挂一级消息过程 ——
见 [`doc/desktop.md`](doc/desktop.md)。

## 实时采集（边抓边看）

接上分析仪直接看，不必先落成文件。采到的报文与抓包文件走**同一套**表格、筛选、
详情、时间轴与导出，所以离线能做的这里都能做。

三个入口，落点是同一条命令：顶栏插头图标 · 文件菜单「连接设备…」（**Ctrl+D**）·
空工作区空态里的「连接设备」。**实时标签全局只允许一个** —— 再点只是切回它
（一台设备只能有一个会话）。

采集期间状态条一直挂着报文数、速率、时长，以及三个健康计数：**丢包**（永远显示，
0 也是绿的）、**超时**（非 0 才出现，属正常现象）、**拒绝**（非 0 即异常）。
底部区是双模式，可以切到**通讯日志**看原始命令往来。

导出走的是离线那条出口，但语义是**到此刻为止的 CSV 快照**：文件名带时间戳，
导出后明说「已导出 N 条（截至 T s）」，免得那个文件交出去之后没人知道它不完整。

> ⚠ **设备层还没定**，现在挂的是模拟设备源（约 8 条/秒的脚本化 PD 协商流程）。
> 界面整条链路已经能用，换真设备要写的只有一个 `LiveSource` 实现。
> 「主动停车」「设备掉线」在真机上不好等，模拟源下有两个演练入口可以随时复现。

逐项说明、状态表、读数含义、导出细则、已知缺口与**怎么接真设备** ——
见 [`doc/live-capture.md`](doc/live-capture.md)。

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
- `rawdata/` 中的测试抓包已入库；缺少额外旧版私有样本的用例仍按实际情况报告跳过。
- 测试会优先找旧版命名的样本，缺失时使用本地 `rawdata/` 中对应的抓包；`csv-diff.mjs` 默认扫描 `rawdata/`。
- 界面冒烟用 widget test 顶替开窗口：`flutter_tester` 是个真的 Dart VM（FFI、isolate
  都在），整个控件树照常布局与绘制，只是不出窗口，所以在 CI 上也能跑。
- `smoke-shell.py` 走的是**真进程 + 真窗口**：它从外部读窗口标题与菜单勾选/置灰。
  之所以读得到，是因为界面把这些状态回报给了外壳 —— 界面状态因此变成可断言的事实，
  不必再靠人看。「拖放」是唯一仍需人跑的一条（`HDROP` 跨进程造不出来），
  但它与命令行共用同一个出口。

Linux/macOS 发行包检查使用 `tools/smoke-desktop.py --bundle <完整包路径> --sample <抓包路径>`：
验证随包核心的 ABI 和真实样本解码，再从仓库外启动 GUI，确认完成首帧绘制，检测启动错误及提前退出。
Linux 无显示服务器时可用 `dbus-run-session -- xvfb-run -a python3 tools/smoke-desktop.py ...`。
该检查不代替拖放、菜单和布局的完整交互验收。

GitHub Actions 同时构建 Windows x64、Linux x64、macOS arm64 和 macOS x64，
执行核心测试、Flutter 静态检查/测试和 Unix 发行包运行检查，分别上传便携包与 SHA256 校验和。

## 许可

本项目以 **Apache-2.0** 发布（见 `LICENSE`）。

第三方组件各自保留其许可，**不重新标称为 Apache-2.0**：
SQLite（公有领域）、zlib（Zlib）、nlohmann/json（MIT），
以及 Flutter SDK 与 pub 包（主体 BSD-3-Clause）。
完整清单（含版本、来源、校验值、链接方式）见 `THIRD_PARTY_NOTICES.md`；
界面里「关于 → 查看许可」由 Flutter 自带的 `LicenseRegistry` 汇总展示。
