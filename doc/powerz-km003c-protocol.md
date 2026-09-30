# POWER-Z KM003C USB 通讯协议

> 本文档整理 POWER-Z **KM003C**（ChargerLAB 出品的 USB PD / 快充测试分析仪）与上位机之间的
> USB 实时通讯协议，用于后续「实时采集 / 选择性屏蔽」适配。
>
> 内容来源：ChargerLAB 官方 API 文档 + 开源逆向工程
> `trothwell/km003c-protocol-research`（含真实抓包验证）。**这是实时通讯协议，
> 与已有的离线 `.sqlite` / `.pdStream` 文件解析是两层东西**，后者见
> `../PDScope/doc/format-powerz.md`。

---

## 1. 设备识别

| 属性 | 值 |
| ---- | ---- |
| 厂商 | ChargerLAB |
| VID | `0x5FC9` |
| PID（KM003C） | `0x0063` |
| USB 版本 | 2.10 |
| 设备类 | `0xEF`（Miscellaneous，用 IAD 组织多接口） |
| 速度 | Full Speed（12 Mbps） |
| 最大包长 | EP0 32 字节；bulk/interrupt 64 字节 |
| 供电 | 总线供电 100 mA |

---

## 2. 三种接口（官方口径）

官方 API 文档明确：KM003C 提供 **三种接口**，命令**完全通用**（同一条命令
三种接口都能发），只差传输速率与驱动需求：

| 接口 | 平台叫法 | 传输速率 | 驱动 | 备注 |
| ---- | ---- | ---- | ---- | ---- |
| USER / WINUSB | Windows 叫 WINUSB，其它平台叫 USB USER | 200 KB/s | 需驱动 | 主通道，**AdcQueue 流与认证只能用这个** |
| CDC 虚拟串口 | 任意波特率均可 | 200 KB/s | Windows 7 需驱动，10/11 免驱 | 最容易上手，任何串口工具都能调 |
| HID | 全平台免驱 | 60 KB/s | 无 | 数据长度限制 64 字节 |

**接口选择建议**（逆向实测）：

| 用途 | 接口 | 说明 |
| ---- | ---- | ---- |
| 高速流（AdcQueue）/ 认证 / 内存读取 | IF0（vendor bulk） | Linux 下需先解绑 `powerz` 驱动 |
| 跨平台免驱 | IF3（HID） | 稍慢，认证与流在 HID 上**不可用** |
| 串口调试 | IF1+2（CDC） | 仅限串口类协议 |

---

## 3. USB 层细节

KM003C 有 4 个接口：

| 接口 | 类 | 端点 | 用途 |
| ---- | ---- | ---- | ---- |
| IF0 | `0xFF` Vendor | `0x01` OUT / `0x81` IN（Bulk，64B） | **主协议通道** |
| IF1 | `0x02` CDC Comm（ACM） | `0x83` IN（Interrupt，8B，10ms） | CDC 控制 |
| IF2 | `0x0A` CDC Data | `0x02` OUT / `0x82` IN（Bulk，64B） | 虚拟串口数据 |
| IF3 | `0x03` HID | `0x05` OUT / `0x85` IN（Interrupt，64B，1ms） | 免驱备选通道 |

端点汇总：

| 端点 | 接口 | 类型 | 方向 | 最大包 |
| ---- | ---- | ---- | ---- | ---- |
| `0x01` | IF0 | Bulk | OUT | 64 |
| `0x81` | IF0 | Bulk | IN | 64 |
| `0x83` | IF1 | Interrupt | IN | 8 |
| `0x02` | IF2 | Bulk | OUT | 64 |
| `0x82` | IF2 | Bulk | IN | 64 |
| `0x05` | IF3 | Interrupt | OUT | 64 |
| `0x85` | IF3 | Interrupt | IN | 64 |

**厂商控制请求**（枚举阶段）：`bmRequestType=0xC2`、`bRequest=0x32`、`wLength=170`，
返回 170 字节 blob（能力/校准查询）。

**Bulk 传输时序**：主机先发 OUT 命令 → 设备空 Complete（status=0）确认；再发 IN URB
（status=-115 EINPROGRESS）→ 设备返回数据。**注意**：`urb_id` 是内核地址、会复用，
关联 Submit→Complete 必须按时间顺序配对，不能按 `urb_id` 分。

---

## 4. 报文头（核心，4 字节）

所有报文开头是 4 字节头。官方定义成一个 union（`MsgHeader_TypeDef`），小端序。

### 4.1 控制头（命令 / 响应）

```
Byte 0: [type:7][extend:1]
Byte 1: [id:8]
Byte 2-3: [未用:1][att:15]   (小端)
```

| 字段 | 位 | 含义 |
| ---- | ---- | ---- |
| `type` | 0–6 | 命令类型（`0x02`、`0x0C`…），**>63 表示数据报文** |
| `extend` | 7 | 传输超大数据包标志（通常 0） |
| `id` | 8–15 | 事务 ID，8 位滚动计数（0–255），响应回显请求的 id |
| `att` | 17–31 | 属性代码（bitmask）或参数 |

> ⚠ **位序坑（实测反复强调）**：`att` 从 32 位小端的 **bit 17** 开始。所以属性值 `N` 在字节流里
> 表现为 `(N << 1)`。例如 `att=0x0001`（ADC）→ 字节 2 是 `0x02`。**别把 `0x02` 误读成
> `0x0002`（AdcQueue）**。要解析就按 32 位小端位域整体解，或直接用 `km003c_lib`。

32 位位域布局（小端）：

```
 31 ─────────────── 17 │16 │15 ───────────── 8 │7 │6 ───────── 0
 │      att (15)      │ u │     id (8)        │ e │   type (7)  │
```

### 4.2 数据头（PutData，`0x41`）

```
Byte 0: [type:7][extend:1]   type=0x41，extend 通常=1
Byte 1: [id:8]
Byte 2-3: [未用:6][obj_count_words:10]   约等于 总字节数/4
```

### 4.3 扩展头（逻辑包，PutData 内每个逻辑包自带）

```
bit 0–14:  att (15)     ADC=1, AdcQueue=2, Settings=8, PdPacket=16
bit 15:    next        1=后面还有逻辑包，0=最后一个
bit 16–21: chunk (6)   通常 0
bit 22–31: size (10)   该逻辑包 payload 字节数
```

> 官方约束：`size` 不能超过 `1024-8` 字节；`chunk × size` 不能超过 4080；HID 接口不能超过 60。

链式逻辑包示例：

```
[主头 4B][扩展头 4B][payload N 字节][扩展头 4B][payload M 字节]…
             └─ next=1 ───────────────────┘└─ next=0（末包）──┘
```

---

## 5. 命令表

### 5.1 命令汇总

| type | 名称 | 方向 | att | 说明 |
| ---- | ---- | ---- | ---- | ---- |
| `0x01` | SYNC | OUT→IN | — | 同步 |
| `0x02` | Connect | OUT→IN | `0x0000` | 开始会话 |
| `0x03` | Disconnect | OUT→IN | `0x0000` | 结束会话 |
| `0x04` | Reset | OUT→IN | — | 复位 |
| `0x05` | Accept | IN | `0x0000` | 命令成功 |
| `0x06` | Reject | IN | 变长 | 命令被拒 |
| `0x0C` | GetData | OUT→IN | 属性掩码 | 按属性位掩码取数据 |
| `0x0E` | StartGraph | OUT→IN | 采样率 | 开始 AdcQueue 流（rate 0–3） |
| `0x0F` | StopGraph | OUT→IN | `0x0000` | 停止流 |
| `0x10` | EnablePdMonitor | OUT→IN | `0x0002` | 开 PD 嗅探（作用未明，可选） |
| `0x11` | DisablePdMonitor | OUT→IN | `0x0000` | 关 PD 嗅探（可选） |
| `0x40` | Head | OUT→IN | — | 数据头（离线日志用） |
| `0x41` | PutData | IN | 变长 | 数据响应 |
| `0x44` | MemoryRead | OUT→IN | `0x0101` | 读设备内存 |
| `0x4C` | StreamingAuth | OUT→IN | `0x0002` | 启用流（认证） |

> 官方枚举里还有 `CMD_FINISHED / CMD_JUMP_APROM / CMD_JUMP_DFU / CMD_GET_STATUS / CMD_ERROR /
> CMD_GET_FILE`，以及数据类 `CMD_HEAD=64 / CMD_PUT_DATA`。上表只列适配会实际用到的。

### 5.2 关键命令细节

**Connect**：`02 TID 00 00` → `05 TID 00 00`（Accept）

**GetData**（属性位掩码可 bitwise OR 合并）：

| 掩码 | 字节 | 请求内容 |
| ---- | ---- | ---- |
| `0x0001` | `02 00` | ADC |
| `0x0002` | `04 00` | AdcQueue |
| `0x0008` | `10 00` | Settings |
| `0x0010` | `20 00` | PdPacket |
| `0x0011` | `22 00` | ADC + PdPacket |
| `0x0200` | `00 04` | LogMetadata |

**StartGraph**（采样率）：

| rate_index | 字节值 | 采样率 | 实测有效速率 |
| ---- | ---- | ---- | ---- |
| 0 | `0x00` | 2 SPS | ~1.8 SPS |
| 1 | `0x01` | 10 SPS | ~9.6 SPS |
| 2 | `0x02` | 50 SPS | ~47 SPS |
| 3 | `0x03` | 1000 SPS | ~956 SPS |

> 前置条件：先发 StreamingAuth（`0x4C`），否则 StartGraph 成功但拿不到样本。

---

## 6. 属性（Attribute）

| 值 | 名称 | 大小 | 说明 |
| ---- | ---- | ---- | ---- |
| `0x0001` | ADC | 44 字节 | 电压/电流/温度快照 |
| `0x0002` | AdcQueue | 20 字节/样本 | 高速流采样 |
| `0x0004` | AdcQueue_10K | — | **定义了但从未被使用**，勿请求 |
| `0x0008` | Settings | 180 字节 | 设备配置 |
| `0x0010` | PdPacket | 变长 | PD 状态或事件 |
| `0x0020` | PdStatus | — | PD 状态 |
| `0x0040` | QcPacket | — | QC 报文 |
| `0x0200` | LogMetadata | 48 字节 | 离线日志元数据 |

---

## 7. 数据结构

### 7.1 ADC 载荷（44 字节，att=0x0001）

| 偏移 | 大小 | 类型 | 字段 | 单位 |
| ---- | ---- | ---- | ---- | ---- |
| 0 | 4 | i32 | vbus_uV | 微伏 |
| 4 | 4 | i32 | ibus_uA | 微安（**有符号**） |
| 8 | 4 | i32 | vbus_avg_uV | 平滑滤波平均电压 |
| 12 | 4 | i32 | ibus_avg_uA | 平滑滤波平均电流 |
| 16 | 4 | i32 | vbus_ori_avg_uV | 未校准平均电压 |
| 20 | 4 | i32 | ibus_ori_avg_uA | 未校准平均电流 |
| 24 | 2 | i16 | temp_raw | 内部温度，`raw / 128.0` = °C |
| 26 | 2 | u16 | vcc1_tenth_mV | 0.1 mV |
| 28 | 2 | u16 | vcc2_tenth_mV | 0.1 mV |
| 30 | 2 | u16 | vdp_tenth_mV | D+，0.1 mV |
| 32 | 2 | u16 | vdm_tenth_mV | D−，0.1 mV |
| 34 | 2 | u16 | vdd_tenth_mV | 内部 VDD |
| 36 | 1 | u8 | sample_rate_idx | 0–4 |
| 37 | 1 | u8 | flags | 状态标志 |
| 38 | 2 | u16 | cc2_avg_mV | 毫伏 |
| 40 | 2 | u16 | vdp_avg_mV | 毫伏 |
| 42 | 2 | u16 | vdm_avg_mV | 毫伏 |

> 官方文档里 `AdcData_TypeDef` 的字段顺序与本表一致（`Rate` 是 2 bit 位域 + 3 字节保留）。
> **电流方向**：正 = USB 母口（输入）→ USB 公口（输出）；负 = 反向。

### 7.2 AdcQueue 样本（20 字节/样本，att=0x0002）

| 偏移 | 大小 | 类型 | 字段 | 单位 |
| ---- | ---- | ---- | ---- | ---- |
| 0 | 2 | u16 | sequence | 递增计数（可据此检丢包） |
| 2 | 2 | u16 | marker | 恒 `0x3C`（60） |
| 4 | 4 | i32 | vbus_uV | 微伏 |
| 8 | 4 | i32 | ibus_uA | 微安（有符号） |
| 12 | 2 | u16 | cc1_tenth_mV | 0.1 mV |
| 14 | 2 | u16 | cc2_tenth_mV | 0.1 mV |
| 16 | 2 | u16 | vdp_tenth_mV | D+，0.1 mV |
| 18 | 2 | u16 | vdm_tenth_mV | D−，0.1 mV |

> 不含温度。要温度就周期性单读 ADC 合并。样本数 = `(payload_len - 8) / 20`。

**ADC vs AdcQueue**：

| 维度 | ADC（0x0001） | AdcQueue（0x0002） |
| ---- | ---- | ---- |
| 用途 | 详细快照 | 高速记录 |
| 大小 | 44 B × 1 | 20 B × N（5–50） |
| 温度 | 有 | 无 |
| 统计（min/max/avg） | 有 | 无 |
| D+/D− | 有 | 有（新固件） |
| 序号 | 无 | 有 |

### 7.3 Settings（180 字节，att=0x0008）

| 偏移 | 大小 | 字段 | 说明 |
| ---- | ---- | ---- | ---- |
| 0x00 | 4 | flags | 配置标志 |
| 0x04 | 4 | reserved | 恒 0 |
| 0x08 | 2 | sample_interval | 微秒（10000 = 10ms） |
| 0x0A | 1 | display_brightness | 0–100（默认 65） |
| 0x0B | 1 | unknown | 恒 `0xFF` |
| 0x0C | 4 | reserved | 恒 0 |
| 0x10 | 32 | thresholds[8] | 报警阈值（-1 = 禁用） |
| 0x30 | 40 | calibration[10] | ADC 校准偏移 |
| 0x58 | 4 | counter | Settings 版本 |
| 0x5C | 4 | timestamp | Unix epoch |
| 0x60 | 1 | mode_flags | 工作模式（bit 2–3） |
| 0x61 | 15 | reserved | 零 |
| 0x70 | 64 | device_name | UTF-8，null 结尾（如 "POWER-Z"） |
| 0xB0 | 4 | checksum | 设备自算，主机不校验，可忽略 |

### 7.4 PD 报文（att=0x0010）

PD 数据有两种 12 字节形态，**不可混用**：

**PD Status（ADC+PD 组合包 68B 总）**：

| 偏移 | 字段 |
| ---- | ---- |
| 0 | type_id（u8） |
| 1–3 | timestamp24（u24，~40ms/tick） |
| 4–5 | vbus_mV（u16） |
| 6–7 | ibus_mA（u16） |
| 8–9 | cc1_mV（u16） |
| 10–11 | cc2_mV（u16） |

**PD Preamble（PD 专属/事件流）**：

| 偏移 | 字段 |
| ---- | ---- |
| 0–3 | timestamp_ms（u32，毫秒基准） |
| 4–5 | vbus_mV（u16） |
| 6–7 | ibus_mA（i16，**有符号**） |
| 8–9 | cc1_mV（u16） |
| 10–11 | cc2_mV（u16） |

> 之后紧跟重复的「6 字节事件头 + PD wire 字节」。空 PD 响应 = 12 字节 preamble + 1 个空事件头
> （wire_len=0），表示没有新报文。

**事件头（6 字节）**：

- 连接/状态事件：`0x45 | ts(LE24) | 保留 | code`，code `0x11`=连接、`0x12`=断开；
- 包裹 PD 报文：`size_flag | ts(LE32) | sop`，`size_flag ∈ 0x80..0x9F`，wire 长度 =
  `(size_flag & 0x3F) - 5` 字节；sop `0`=SOP、`1`=SOP′、`2`=SOP″。

wire 字节 = 标准 USB PD 报文（2 字节头 + 数据对象），**无 CRC、无 SOP/EOP**。

> 这与离线 `.sqlite` 的 `pd_table.Raw` blob 结构**完全一致**（少了 12 字节 preamble），
> 时间戳也对得上。也就是说：**实时抓到的 PD 事件，和你已有的离线解析是同一套语义**，
> 适配时可以复用现有的 PD 解码路径。

### 7.5 LogMetadata（48 字节，att=0x0200）

| 偏移 | 大小 | 字段 |
| ---- | ---- | ---- |
| 0x00 | 16 | filename（null 结尾，如 "A01.d"） |
| 0x10 | 2 | unknown |
| 0x12 | 2 | sample_count |
| 0x14 | 2 | interval_ms |
| 0x16 | 2 | flags |
| 0x18 | 4 | estimated_size |
| 0x1C | 20 | metadata（校验等） |

---

## 8. 认证与加密

所有加密用 **AES-128-ECB**。

### 8.1 密钥表（唯一真源）

| 索引 | 密钥 | 用途 |
| ---- | ---- | ---- |
| 0 | `Lh2yfB7n6X7d9a5Z` | 内存读取（0x44）、固件、离线日志 |
| 3（加密） | `Fa0b4tA25f4R038a` | 流认证（0x4C）加密 |
| 3′（解密） | `FX0b4tA25f4R038a` | 流认证解密（byte[1] 由 'a'→'X'） |

> 固件变体里 key 0 也见过 `Lh2yfB7n6X7d9a4Z`，两种都实测可用。

### 8.2 StreamingAuth（0x4C）

前置：读 HardwareID（见 8.3），再认证。

请求 36 字节：`4C TID 00 02` + 32 字节 AES 密文。

32 字节明文（加密前）：

| 偏移 | 大小 | 字段 | 必需 |
| ---- | ---- | ---- | ---- |
| 0 | 8 | 时间戳 | 任意值，不校验 |
| 8 | 12 | HardwareID | **必须匹配** 0x40010450 |
| 20 | 12 | 填充 | 任意值 |

响应 36 字节：`4C 00 attr(LE)` + 32 字节密文。attr：

| 值 | 含义 | AdcQueue |
| ---- | ---- | ---- |
| `0x0201` | 认证失败（HardwareID 不符） | 返回空 |
| `0x0203` | 认证成功（Level 1） | 可用 |

### 8.3 MemoryRead（0x44）

请求 36 字节：`44 TID 01 01` + 32 字节 AES 密文。

32 字节明文（加密前）：

| 偏移 | 大小 | 字段 |
| ---- | ---- | ---- |
| 0 | 4 | Address（LE） |
| 4 | 4 | Size（LE） |
| 8 | 4 | Magic `0xFFFFFFFF` |
| 12 | 4 | CRC32（bytes 0–11） |
| 16 | 16 | 填充 `0xFF` |

响应分两包：确认包（`C4 TID attr addr size …`）+ 数据包（AES 密文，16 字节对齐）。

已知内存地址：

| 地址 | 大小 | 内容 |
| ---- | ---- | ---- |
| `0x00000420` | 64 | 设备信息（型号 "KM003C"、HW 版本、生产日期） |
| `0x00004420` | 64 | 固件信息（FW 版本、构建号、日期） |
| `0x03000C00` | 64 | 校准数据（真序列号、UUID、时间戳） |
| `0x40010450` | 12 | **HardwareID（认证用，不是序列号）** |
| `0x98100000` | 变长 | 离线 ADC 日志 |

HardwareID（12 字节）结构：6 字节标识符（如 "071KBP"）+ 2 字节分隔符（`0x0D 0xFF`）+
2 字节设备 ID（LE）+ 2 字节填充（`0xFFFF`）。**每台设备唯一**，认证前必须先读它。

### 8.4 认证等级

| 等级 | 名称 | 权限 |
| ---- | ---- | ---- |
| 0 | 未认证 | 仅基础 ADC、PD |
| 1 | 设备认证 | AdcQueue、flash 写、扩展属性 |
| 2 | 校准认证 | 工厂/校准命令 |

固件在 GetData 处理器里：`auth_level == 0` 时只放行 `0x19`（ADC + Settings + PD）；
Level 1 才放行 AdcQueue。

---

## 9. 通信流程

### 9.1 基础 ADC 轮询（免认证）

```
Connect(02) → Accept(05)
GetData(0C, attr=0x0001) → PutData(41, ADC)   [每 ~200ms 重复]
Disconnect(03) → Accept(05)
```

### 9.2 AdcQueue 高速流（需认证）

```
Connect(02) → Accept(05)
MemoryRead(44, 0x40010450) → HardwareID
StreamingAuth(4C, 含 HardwareID) → attr=0x0203  [必需]
StartGraph(0E, rate=0..3) → Accept(05)
GetData(0C, attr=0x0002) → PutData(AdcQueue)   [每 20–200ms 重复]
StopGraph(0F) → Accept(05)
Disconnect(03) → Accept(05)
```

### 9.3 PD 抓包（免认证）

```
Connect(02) → Accept(05)
GetData(0C, attr=0x0010) → PutData(PdPacket)   [每 ~40ms 重复]
Disconnect(03) → Accept(05)
```

> `EnablePdMonitor(0x10)` / `DisablePdMonitor(0x11)` 可选：设备默认就缓冲 PD 事件，直接轮询
> GetData 就能拿到，不必显式开关。

---

## 10. 与现有离线解析的衔接

- 实时 PD 事件（USB PdPacket）的 wire 字节 = 标准 PD 报文头 + 数据对象，**无 CRC/SOP/EOP**，
  与 `.sqlite` `pd_table.Raw` blob 同构（少 preamble）。适配实时采集时，**可复用现有 PD 解码路径**
  （报文头 / VDM / PDO / 扩展消息 / 跨报文状态）。
- 实时 ADC / AdcQueue 拿到的是原始电压电流，可映射成现有「模拟量轨迹」的采样点。
- 时间基准：PD 事件用 32 位 preamble 毫秒时间戳（与 SQLite `Time` 同源）；ADC+PD 组合包里的
  24 位计数（~40ms/tick）是另一套粗粒度计数，**别和 SQLite 时间戳混用**。

---

## 11. 实测数据（逆向仓库抓包）

- 事务覆盖：7 次抓包 2836 组请求/响应，100% id 匹配、位掩码→属性全对得上。
- PutData 常见大小：52B（仅 ADC）、68B（ADC+PD）、52–900B（AdcQueue）、变长 PD-only（≥18B）。
- 延迟（中位）：ADC ~182 µs、PD ~158 µs、ADC+PD ~198 µs、AdcQueue ~1.06 ms。
- 空响应：`obj_count_words=0` 的 PutData 合法（AdcQueue 无缓冲样本时）。
- 最大持续吞吐：~133 packets/s；AdcQueue 1000 SPS 实测 ~956 SPS。

---

## 12. 参考

- ChargerLAB 官方 API 文档（本文档第 2–7 节字段定义的主依据）
- 官方《KM003C Protocol Trigger by Virtual Serial Port (Instructions)》（虚拟串口触发命令，见下）
- `trothwell/km003c-protocol-research`（逆向 + 抓包验证）
- Linux 内核 `powerz` hwmon 驱动

### 附：虚拟串口协议触发命令（官方，可选能力）

连接 CDC 虚拟串口后可发文本命令主动触发协议（`pdm open/close`、`pd pdo`、`pd req=`、
`pd data=`、`ufcs pdo`、`qc 5V`、`scp volt=` 等）。其中 `pd data=` 发原始 PD 报文：
首字节 SOP、第二/三字节头，**不含 CRC**。若后续要做「主动触发/注入」可参考，纯采集不需要。
