# 桌面外壳（Windows）

界面是 Flutter，解析是 C++17 核心，两者之间只有一层 C ABI。
**跟操作系统打交道的那一层**——接拖放、收命令行与文件关联送来的路径、把第二份抓包交给
已经开着的窗口、挂中文菜单——是这个外壳的事。

目录：`app/windows/runner/`

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 取命令行里的抓包路径；已有实例就先转交再退出；建窗口并起标题 |
| `shell.{h,cpp}` | 路径编解码、单实例转交、拖放取路径、菜单的构造与状态 |
| `flutter_window.{h,cpp}` | 建通道、注册拖放（含子窗口）、处理 `WM_DROPFILES` / `WM_COPYDATA` / `WM_COMMAND` |
| `win32_window.cpp` | Flutter 模板原样（窗口类、DPI、主题跟随） |

## 一条边界：外壳只送**路径**

外壳不知道这次打开的是 `.atkcc`、`.sqlite` 还是 `.pdStream`。
**格式判定始终由核心按文件内容做**，这样外壳不需要跟着格式变。

路径一律 **UTF-8**，与 C ABI 的约定相同。

## 接触面

Dart 与外壳共用一条通道：`pdscope/shell`（`app/lib/core/shell.dart` ↔ `runner/flutter_window.cpp`）。

| 方向 | 方法 | 参数 | 说明 |
| --- | --- | --- | --- |
| 外壳 → Dart | `openFiles` | `List<String>` 路径 | 拖放 / 文件关联 / 命令行 / 第二个实例，四条路都汇到这里 |
| 外壳 → Dart | `command` | `String` | 菜单点击（下表） |
| Dart → 外壳 | `ready` | — | 「通道接好了，可以把攒的路径送来了」 |
| Dart → 外壳 | `shellState` | `{hasDoc, filters, detail, title}` | 窗口标题 + 菜单项的灰/亮与勾选 |

`openFiles` 落到的就是界面自身的 `Workspace.openFiles` —— 与「打开文件」对话框同一个入口，
所以**每次打开都是新标签**、坏文件只让它自己那个标签变红，这些行为自动一致。

### 为什么要有 `ready` 这个握手

用「打开方式」启动时，路径在窗口刚建好那一刻就有了，而 Dart 侧此时还没建立通道 ——
直接发出去的消息会石沉大海。所以外壳**先把路径攒在窗口里**，等 Dart 报 `ready` 再一起送出。
同样是这个原因，Dart 的 `main()` 必须在 `runApp` 之前接线。

### 菜单

| 菜单 | 项 | 命令 | 备注 |
| --- | --- | --- | --- |
| 文件(&F) | 打开抓包… `Ctrl+O` | `openFile` | |
| | 连接设备… `Ctrl+D` | `connectDevice` | 实时采集；见 [`live-capture.md`](live-capture.md) |
| | 关闭当前标签 `Ctrl+W` | `closeCurrent` | |
| | 关闭全部抓包 | `closeAll` | |
| | 导出 CSV（当前筛选结果） | `exportCsv` | 没有打开的抓包时置灰；实时标签导的是快照 |
| | 导出 JSON（全部报文） | `exportJson` | 同上；实时采集只支持 CSV |
| | 退出 | — | 直接 `WM_CLOSE`，不经 Dart |
| 视图(&V) | 搜索报文 `Ctrl+F` | `search` | |
| | 切换明暗主题 `T` | `toggleTheme` | |
| | 紧凑 / 舒适行高 | `toggleDense` | |
| | 显示筛选栏 | `toggleFilters` | 勾选状态由 Dart 报 |
| | 显示详情面板 | `toggleDetail` | 同上 |
| | 重置布局 | `resetLayout` | 展开两栏并回到默认尺寸 |
| 帮助(&H) | 关于 PDScope | `about` | 与顶栏的「关于」是同一段代码 |

共 14 项。`Ctrl+O` / `Ctrl+W` / `Ctrl+F` / `Ctrl+D` 写进菜单文字是**显示用**的，
真正的快捷键由界面侧接管（`app.dart` 的 `_handleKey`）；`T` 同理。
Linux 外壳另用 `gtk_application_set_accels_for_action` 注册了一份。

菜单**不自己实现动作**：点击后一律把命令交给 Dart，与界面上的按钮走同一套代码
（`app/lib/ui/app.dart` 的 `_runShellCommand`）。否则同一件事迟早会在菜单和界面里长成两个样子。

⚠ 加菜单项时注意 `shell.h` 里的 `MenuCommand` 枚举**是连号的**（`kMenuOpen = 1001` 起）：
要在中间插一项，后面的 id 会整体顺延 —— `flutter_window.cpp` 用枚举名做映射，
所以两边一起改就没事，但**别只改一处**，也别硬写数字。

`shellState` 由界面在状态变化时上报（去重后才发）：外壳看不见 widget 树，
菜单项的灰/亮只能由界面说。

`title` 一并报上来是有用的：窗口标题、任务栏、Alt+Tab 里能看出开着哪一份抓包。
它同时也是**外壳唯一能从外部观测到的输出** —— 见下文的自检脚本。

### 窗口类名必须是本项目自己的

`PDScope.MainWindow`，定义在 `runner/shell.cpp`，`win32_window.cpp` 注册窗口时用同一个符号。

这不是命名风格问题：「已有实例就转交」靠 `FindWindowW(类名)` 找窗口，而 Flutter 模板默认的
`FLUTTER_RUNNER_WIN32_WINDOW` **每个 Flutter Windows 应用都在用**。用默认名的话，
机器上随便哪个 Flutter 应用开着，第二次打开就会把路径送给**它**、然后自己默默退出：
用户看到的是「PDScope 没起来，别的程序也没反应」。

这不是假想 —— 在本机枚举顶层窗口时，`FLUTTER_RUNNER_WIN32_WINDOW` 这一类名下当场就有一个
别的应用（LocalSend）在用。所以 `pdscope-tests` 里有一条断言钉死「类名不能等于模板默认值」。

## 四种打开方式

1. **拖进窗口** —— `WM_DROPFILES`
2. **命令行** —— `PDScope.exe "D:\抓包\绿联70w.atkcc"`（可多个）
3. **文件关联 / 「打开方式」** —— 与 2 同一条路（Windows 就是这么转的）
4. **第二个实例** —— 已经有窗口在跑时，新进程把路径转交给它并立刻退出

第 4 条是刻意的：用户再双击一份抓包，想要的是**同一个窗口多一个标签**，
而不是多出一个进程（还会各自持有一份引擎与动态库）。转交用 `WM_COPYDATA`
（系统会把数据复制进接收进程，发送方同步等到对方处理完），接收方找不到窗口就
按普通启动走。

> 单实例只在这种「带着文件来」的情况下生效。不带参数再次启动仍会开新窗口 ——
> 那是用户明确要一个窗口，不该被吞掉。

### 拖放为什么要注册两个窗口

客户区被 Flutter 自己的**子窗口**整个盖住。文件拖进来时，系统把 `WM_DROPFILES`
发给光标底下那个窗口 —— 也就是子窗口，顶层窗口的 `WndProc` 一次都收不到。
所以：

- 顶层窗口注册一次（拖到标题栏/边框时用得上）；
- Flutter 子窗口也注册一次，并**挂一级消息过程**（`SetWindowLongPtr(GWLP_WNDPROC)`），
  把 `WM_DROPFILES` 转给同一个处理函数；窗口销毁时把原来的过程还回去。

宿主指针挂在子窗口的属性上（`SetPropW`），消息过程靠它找回窗口对象。

### 菜单栏与源码编码

菜单里有中文。MSVC 默认按系统 ANSI 代码页解释源文件，所以 `runner/CMakeLists.txt`
给这个 target 加了 `/utf-8`。**实测**（CP936 环境的机器上把该选项去掉）：MSVC 先报一条
C4819，随后按 CP936 读源码、中文注释的字节能把后面的代码一起吃掉，直接报语法错 ——
这个 target 根本编不过。也就是说缺了它不是「菜单静默变乱码」，而是当场编不出东西。

顺带一提，`pdscope-tests` 里那几条菜单文字断言用的是**码点转义**写的期望值。
它防的是「标签被误改」（改了没红就是测试没牙），而不是编码问题 —— 编码错了这里编不过，
轮不到断言出场。期望值若也写成中文字面量，两边由同一份源码解码，出问题时一起错、
比出来相等，反而变成自我印证。

## 还没做的平台

首版只做了 Windows 外壳。macOS 与 Linux 需要在**同一条通道**上各写一份
（macOS 走 `NSDraggingDestination`、Linux 走 GTK 的拖放接口），把路径按同样的格式送进
`openFiles`；`shell.dart` 与界面**不用改**。

## 怎么验证

分三层，**前两层能自动、第三层也做成了自动**：

**① 纯逻辑（`pdscope-tests`，Windows 上一起跑）**

```bash
./build/out/pdscope-tests.exe shell      # 4 个用例：参数过滤、COPYDATA 编解码、菜单结构与文字、窗口类名
```

外壳里最「错了也不报错」的几段都在这里：开关/目录/不存在的路径不该被当成抓包、
`WM_COPYDATA` 载荷编解码两边要严格对上、菜单项与文字不能被顺手改掉、
窗口类名不能退回模板默认值。

**② 外壳 → 界面这一段**

```bash
cd app
flutter test test/shell_test.dart        # 6 个用例：ready 握手、混合拖放、坏文件隔离、菜单命令、状态上报、窗口标题
```

**③ 真窗口（原生探针，无需人眼）**

```bash
python tools/smoke-shell.py            # 自动挑样本；也可显式给两份
```

这条脚本起真进程、用 `EnumWindows` 找到窗口，然后从外部读**窗口标题与菜单状态**。
之所以能这样验，是因为界面把标题和菜单状态都回报给了外壳 —— 于是「看不见的界面状态」
变成了 `GetWindowTextW` / `GetMenuState` 这两个可断言的事实：

1. 无参数启动 → 菜单栏是中文三项；**没有文档时导出项置灰**、视图两项打勾
   （置灰与打勾都只能来自界面的回报，所以这条同时验了回报链路）
2. 带样本启动 → 标题变成 `PDScope — <文件名>`，导出项转为可点
3. 再起一个实例带第二份 → **它自己退出**，已有窗口的标题换成第二份，全程只有一个窗口

**哪些仍然没法自动验，为什么**：拖放要系统外壳分配的 `HDROP`，跨进程造不出来。
但第 3 条与拖放**共用同一个 `SendPaths`**，只有「从 `HDROP` 取路径」那十几行没被自动覆盖，
而那段是机械的 `DragQueryFile` 循环。「打开方式」（文件关联）与命令行是同一条路 ——
Windows 就是这么转的。

### 改完 runner 的 C++，注意产物可能是旧的

踩过一次：`shell.cpp` 改完后 `flutter build windows --release` 报了成功，但 `Release/PDScope.exe`
还是上一次的（时间戳没变、里面没有新字符串）。程序照旧能跑，只是**跑的是旧外壳** ——
现象是「第二个实例照样新开窗口」「标题不更新」，很容易误判成代码写错。

所以动过 `app/windows/runner/` 之后，除了看构建有没有报错，还要确认产物真的换了：

```bash
python - <<'EOF'
d = open("app/build/windows/x64/runner/Release/PDScope.exe","rb").read()
print("类名在里面:", d.count("PDScope.MainWindow".encode("utf-16-le")))
EOF
```

对不上就 `rm -rf app/build/windows` 再构建一次。判据和之前那条经验一样：
**读出来从不变化的字段/现象，先怀疑看的不是新东西。**

