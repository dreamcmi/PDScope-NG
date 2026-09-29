# 第三方组件声明 / Third-Party Notices

本文件随发行包分发（见 `CMakeLists.txt` 的 `install(FILES …)`）。
清单口径：**名称 · 版本 · 来源 · 校验值 · SPDX · 链接方式 · 所需声明**。

重新核对办法：对 `_dl/` 下的归档跑一次

```
sha256sum _dl/*
```

输出应与下表「校验值」逐字符相同。归档不入发行包，发行包里是静态编入后的二进制。

---

## 一、运行时依赖（静态编入 `pdscope.dll` 与 `pdscope-cli.exe`）

这三个都会进入产物二进制，所以声明必须随包分发。

### 1. SQLite

| 项 | 内容 |
| --- | --- |
| 名称 | SQLite（amalgamation） |
| 版本 | **3.53.4**（`sqlite3.h` 的 `SQLITE_VERSION`） |
| 来源 | <https://sqlite.org/download.html> · 归档 `sqlite-amalgamation-3530400.zip` |
| 校验值 | `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d` |
| SPDX | 无（**公有领域 / Public Domain**） |
| 链接方式 | 源码静态编入（`third_party/sqlite/sqlite3.c`） |
| 所需声明 | 记录官方来源、版本与校验值，并说明其为公有领域。SQLite 作者声明放弃版权，以一段 blessing 代替法律声明 —— 该声明原样保留在 `third_party/sqlite/sqlite3.h` 中，未做改动。 |

用途：只读打开 POWER-Z 导出库（`pd_table` / `ufcs_table` / ADC 表）。

### 2. zlib

| 项 | 内容 |
| --- | --- |
| 名称 | zlib |
| 版本 | **1.3.2**（`zlib.h` 的 `ZLIB_VERSION`） |
| 来源 | <https://zlib.net/> · 归档 `zlib-1.3.2.tar.gz` |
| 校验值 | `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` |
| SPDX | `Zlib` |
| 链接方式 | 源码静态编入（`third_party/zlib/`） |
| 所需声明 | **完整许可文本见 `third_party/zlib/LICENSE`**（上游原样保留）。源文件顶部的许可声明未被删除或修改。按 zlib 许可第 1 条：不得谎称本软件为原创；第 3 条：本声明不得从源码分发中移除。 |

用途：`.atkcc` 内 ZIP 条目的 raw DEFLATE 解压，以及 ZIP 条目的 CRC-32 核对。

### 3. nlohmann/json

| 项 | 内容 |
| --- | --- |
| 名称 | JSON for Modern C++（nlohmann/json） |
| 版本 | **3.12.0**（`NLOHMANN_JSON_VERSION_*`） |
| 来源 | <https://github.com/nlohmann/json/releases/tag/v3.12.0> · 单头文件发行版 `json.hpp` |
| 校验值 | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| SPDX | `MIT` |
| 链接方式 | **仅头文件**，编入核心（`third_party/json/nlohmann/json.hpp`，`INTERFACE` 目标 `pdscope_json`，无 .c/.cpp 参与编译） |
| 所需声明 | **完整许可文本见 `third_party/json/LICENSE.MIT`**。版权行：`Copyright (c) 2013-2025 Niels Lohmann`，与头文件里的 `SPDX-FileCopyrightText` 一致。 |

用途：元数据、报文详情、统计摘要与 JSON 导出的序列化。

---

## 二、构建与打包期依赖（**不进入**产物二进制）

这些不静态编入 `pdscope.dll` / `pdscope-cli.exe`，但会随 Flutter 桌面产物一起分发，
所以仍在这里登记。

| 名称 | 版本 | 来源 | SPDX | 说明 |
| --- | --- | --- | --- | --- |
| Flutter SDK | 见下「版本偏差」 | <https://github.com/flutter/flutter> | BSD-3-Clause（主体） | 桌面界面与运行时 |
| Dart SDK | 随 Flutter | <https://dart.dev/> | BSD-3-Clause | 与 Flutter 同源分发 |
| file_selector | 1.1.0 | <https://pub.dev/packages/file_selector> | BSD-3-Clause | 打开/保存对话框 |
| file_selector_windows | 0.9.3+6 | pub.dev | BSD-3-Clause | Windows 平台实现 |
| file_selector_linux | 0.9.4+1 | pub.dev | BSD-3-Clause | Linux 平台实现（GTK） |
| file_selector_macos | 0.9.5+1 | pub.dev | BSD-3-Clause | macOS 平台实现 |
| ffi | 2.2.0 | <https://pub.dev/packages/ffi> | BSD-3-Clause | Dart FFI 辅助（`toNativeUtf8`、`calloc`） |

上表的许可类型**不是凭印象填的**，是逐个打开本机 pub 缓存里的 `LICENSE`
（`…/Pub/Cache/hosted/pub.dev/<包>-<版本>/LICENSE`）核对出来的：判定依据是正文里
「Redistribution and use in source and binary forms」+「Neither the name」两条是否同时出现
（即 BSD-3-Clause）。顺带核过 `cross_file`、`plugin_platform_interface`、`http` 三个传递依赖，
同为 BSD-3-Clause；`ffi` 一开始我按印象记成了 MIT，实际是 BSD-3-Clause —— 这类事只能看正文。

pub 侧依赖的**完整解析结果与每个包的 sha256 锁定在 `app/pubspec.lock`**，
命令 `flutter pub deps` 可重放同一份清单。其余传递依赖（`path`、`collection`、
`material_color_utilities`、`vector_math` …）都随 Flutter SDK 一同分发，同属该档宽松许可。

**许可证展示**：界面顶栏「关于」→ Flutter 自带的许可页（`showAboutDialog` 的
「查看许可」入口）。该页由 `LicenseRegistry` 汇总，会列出 Flutter 引擎与所有 pub 包的
许可正文 —— 这是 pub 侧依赖的**权威展示位置**，比手工维护的表格更不容易过期。

### 版本偏差（必须如实记录）

计划 §4 锁定 Flutter **3.47.5**；本次实际构建环境是本机的 Flutter **3.44.4 / Dart 3.12.2**。
这是环境限制，不是有意降级。在拿到 3.47.5 之前，本文与产物对应的是 3.44.4 的实际行为；
换 SDK 后需重跑一遍自检与打包，并重新核对该版本的引擎许可清单。
`app/pubspec.yaml` 里约束到 `file_selector: ^1.1.0`、`ffi: ^2.1.0`，
实际解析版本见上表与 `pubspec.lock`。

---

## 三、刻意不采用的组件

记录在此是为了避免以后有人"顺手加回来"。

| 组件 | 不采用的原因 |
| --- | --- |
| `desktop_drop` | 本身是 Apache-2.0，但其 Linux 传递依赖 `dbus` 为 **MPL-2.0**，会引入额外的源码分发义务。改用平台原生拖放接口（Windows 原生拖放事件 / macOS `NSDraggingDestination` / Linux GTK），只把文件路径交给 Dart。 |
| libzip / minizip | `.atkcc` 只需要只读 ZIP/ZIP64 中央目录 + raw DEFLATE 这个子集，为此重新实现受边界检查的读取器，用 zlib 解压，避免多一份许可与依赖。 |
| Boost / Qt / OpenSSL / libusb / HIDAPI / BlueZ | 首版不涉及实时采集，不需要设备传输层；引入会显著放大许可面与打包体积。 |

依赖准入边界：运行时依赖默认只接受 Apache-2.0、MIT、BSD-2/3-Clause、zlib
与已核实的公有领域；超出这一组就换库或自行实现，并重新核验。

---

## 四、本项目自身的许可

本项目以 **Apache-2.0** 发布，完整正文见仓库根目录的 `LICENSE`。

第三方组件**各自保留其许可**，不重新标称为 Apache-2.0。
注意 `LICENSE` 只覆盖本项目自己写的代码（`core/`、`cli/`、`app/`、`tests/`、`tools/`），
不覆盖 `third_party/` 与 `_dl/` 下的内容。
