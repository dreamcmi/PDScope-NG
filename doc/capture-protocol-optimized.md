# PD 抓包上行协议 PCL v1

## 目录

- 1 范围与约定
- 2 通用帧格式
- 3 DATA（TYPE=01）
- 4 HELLO（TYPE=02）
- 5 CFG（TYPE=03）
- 6 END（TYPE=04）
- 7 EVT（TYPE=05）
- 8 传输与解析要求
- 9 硬件与固件注意事项
- 10 验证要求
- 附录A 长度汇总
- 附录B 参考资料

## 1. 范围与约定

本协议面向具有PD接收外设的小资源MCU。单个设备由一个上位机管理，同一时刻只运行一次采集。设备被动采集PD、DPDM及配置请求的电压/电流，向上位机传送解码字节和事件。

线上主版本为 `pcl_ver=1`，布局标识为 `schema_id=0x314F`。本文为设计草稿，两端须采用同一份定稿，不得混用其他布局。

所有多字节整数为小端，字段连续排列，不插入对齐字节。位域未定义位必须为0。十六进制帧类型及事件码以两位HEX表示；普通枚举和位编号使用十进制。

“解码字节”指接收PHY/decoder完成物理编码转换后取得的实际字节，不包括模拟波形和非字节定界符。“完整包”包括协议头、正文及线上收到的校验字节。设备不得重算CRC后替换所收到的CRC，不得修正原始报文字节。

## 2. 通用帧格式

### 2.1 帧外壳

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 2 | SYNC | 固定A5 5A |
| 2 | 1 | TYPE | 帧类型 |
| 3 | 2 | LEN | BODY字节数 |
| 5 | LEN | BODY | 帧体 |
| 5+LEN | 2 | CRC16 | 帧校验，低字节先发 |

总长度为 `7+LEN`。BODY可出现SYNC，无转义。主机BODY硬上限为512 B，完成HELLO协商后还须遵守max_frame。

### 2.2 帧类型

| TYPE | 名称 | 方向 | BODY长度 |
| ---: | --- | --- | --- |
| 01 | DATA | 设备→上位机 | 5 B公共头+至少一条完整记录 |
| 02 | HELLO | 设备→上位机 | 33 B |
| 03 | CFG | 上位机→设备 | 8 B |
| 04 | END | 上位机→设备 | 1 B |
| 05 | EVT | 设备→上位机 | 5 B公共头+12 B×N，N≥1 |

其他TYPE未定义。CFG和END是下行命令；DATA、HELLO和EVT仅上行。

### 2.3 帧校验

| 参数 | 值 |
| --- | --- |
| 算法 | CRC-16/CCITT-FALSE |
| 多项式 | 0x1021 |
| 初值 | 0xFFFF |
| 输入/输出反射 | false / false |
| 输出异或 | 0x0000 |
| 校验范围 | TYPE、LEN两字节及BODY，不含SYNC |
| `123456789`测试值 | 0x29B1，线上B1 29 |

PCL帧校验与payload内的PD/DPDM校验分别处理。

## 3. DATA（TYPE=01）

### 3.1 BODY布局

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 4 | id | DATA/EVT共用的u32上行帧编号 |
| 4 | 1 | loss_flags | 本次采集的粘滞损失标记 |
| 5 | 可变 | records | 一条或多条完整记录 |

记录紧密排列、不得跨帧。上位机完整验证一帧后再提交其中记录。mode=0x03时同一DATA允许包含PD和DPDM记录。

### 3.2 id

id为u32。首次START.id=0，此后按实际发送顺序递增，达到u32上限后按模2^32回绕。HELLO不占id，帧内多条记录或事件共用一个id。

示例：`START(0) → DATA(1) → MEASUREMENT EVT(2) → DATA(3) → STOP(4)`。

封定发送顺序时分配id，已分配但放弃的帧不复用编号。重复STOP重发缓存的原始完整帧，不分配新id。相邻id不连续表示可观察的缺口；连续不能证明PHY无漏包，也不能发现恰好缺失2^32整数倍帧。

END后的归零要求见§6.3。id不承担采集身份或连接身份识别，主机必须串行停止与开始并保持单来源有序解析。

### 3.3 loss_flags

| 位 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | RECORD_LOST | 记录队列满导致丢弃 |
| 1 | EVENT_LOST | EVT队列满或覆盖旧待发状态 |
| 2 | PHY_OVERRUN | 外设明确报告接收溢出 |
| 3 | DECODE_ERROR | 已检测异常但无法形成真实记录 |
| 4 | FLUSH_DISCARD | END.FLUSH=0主动丢弃普通项 |
| 5～7 | 保留 | 必须0 |

有效CFG时清零，采集中只置位不清除，在后续DATA/EVT和最终STOP中携带。未置位不构成无损证明，断链可能使最后发生的损失无法上报。

### 3.4 记录类型

| type | 类型 | 启用条件 | 固定头 | 记录总长 |
| ---: | --- | --- | ---: | --- |
| 1 | PD | mode.bit0=1 | 10 B | 10+nbytes |
| 2 | DPDM | mode.bit1=1 | 10 B | 10+nbytes |
| 其他 | 未定义 | — | — | 所在DATA整帧拒绝 |

外层TYPE表示DATA帧，记录type表示其中的数据种类。未被mode启用的记录非法。

### 3.5 PD记录

#### 3.5.1 布局

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 1 | type | 固定1 |
| 1 | 1 | sop_kind | SOP/Reset类型 |
| 2 | 1 | cc | 0未知、1 CC1、2 CC2 |
| 3 | 1 | flags | 接收状态 |
| 4 | 4 | elapsed_ticks | 从本次采集开始累计的u32刻度数 |
| 8 | 2 | nbytes | 实际上传的解码字节数，包含已保留CRC字节 |
| 10 | nbytes | payload | 消息头、消息体及原始wire CRC |

elapsed_ticks取驱动首次处理本包完成/错误结束通知的时刻；Reset取检测通知时刻。计时规则见§4.5。

#### 3.5.2 sop_kind

| 值 | 含义 |
| ---: | --- |
| 0 | SOP |
| 1 | SOP′ |
| 2 | SOP″ |
| 3 | Hard Reset |
| 4 | Cable Reset |
| 5 | 未知SOP，仅在确实取得可信头和字节边界时使用 |
| 6～255 | 保留 |

#### 3.5.3 flags

| 位 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | CRC_BAD | 明确确认所收到的PD CRC错误 |
| 1 | CRC_UNKNOWN | 没有可靠CRC结论，Reset也置位 |
| 2 | TRUNCATED | 包边界已知，但只上传解码包的连续前缀 |
| 3 | RX_ERROR | PHY/decoder报告接收错误 |
| 4～7 | 保留 | 必须0 |

CRC_BAD与CRC_UNKNOWN互斥，两者均0表示CRC明确通过。RX_ERROR独立于CRC结论。flags描述接收时取得的结论，CRC字节无论正确与否均按实际收到值保留。

#### 3.5.4 payload字节范围

| 完整包内偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 2 | PD Message Header，按接收字节顺序 |
| 2 | nbytes-6 | 消息体；扩展消息包括Extended Header、当前chunk数据及实际padding |
| nbytes-4 | 4 | 实际收到的PD wire CRC，保持原始字节顺序 |

完整普通包nbytes≥6。无正文控制包包含2 B消息头和4 B CRC。最大包为268 B（2+262+4）。payload不包含前导码、SOP、EOP等非字节符号；SOP/Reset通过sop_kind表达。Hard/Cable Reset的nbytes=0，无payload。

外设提供的头、正文和CRC须从头到尾按原顺序转发，不剥离CRC，不重编码、不重组chunk。驱动不得用计算CRC补齐硬件未提供的原始CRC字节。

仅在取得可信接收边界时形成记录。超过max_pd_bytes时保留从消息头开始的连续前缀，置TRUNCATED，nbytes只计已保留字节；不得截掉中间正文再接CRC。TRUNCATED或RX_ERROR记录不按“最后4 B必为CRC”解释。普通截短记录至少保留完整2 B消息头；无法形成记录时设置诊断标记。

正常包若PHY固定剥离CRC且无法读取其原始字节，该接收路径不满足本文完整PD转发能力，不能以常态截短记录替代完整支持。

GOODCRC_FILTER只过滤CRC通过、无RX_ERROR、普通SOP且Extended=0、NDO=0、MessageType=1的完整GoodCRC。坏/未知CRC和Cable GoodCRC保留。

### 3.6 DPDM记录

#### 3.6.1 布局

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 1 | type | 固定2 |
| 1 | 1 | link | 实际接收线路 |
| 2 | 1 | proto | 协议编号 |
| 3 | 1 | flags | bit0 TRUNCATED，其余0 |
| 4 | 4 | elapsed_ticks | decoder确认包完成时的u32累计刻度数 |
| 8 | 2 | nbytes | 实际字节数，1～max_dpdm_payload |
| 10 | nbytes | payload | 完整解码字节或带标志的连续前缀 |

#### 3.6.2 link和proto

| link | 线路 |
| ---: | --- |
| 0 | D+ |
| 1 | D− |
| 2 | 双线组合 |
| 3～255 | 保留 |

| proto | 协议 |
| ---: | --- |
| 1 | UFCS |
| 0、2～255 | 未定义，不发送 |

SCP、FCP、VOOC、vivo待后续实现。

#### 3.6.3 payload字节范围

UFCS payload从协议头开始，至实际收到的wire CRC结束。保留头、正文、校验字节及协议内其他字节字段；不包含训练序列、模拟波形和非字节定界符。未来DPDM协议遵守同一完整字节转发原则，其具体起止字段须在对应协议定义中列明。

边界可信才输出记录，超容量只保留连续前缀并置TRUNCATED。错误校验字节原样保留，设备不得计算后替换。正常路径必须取得校验字节，不能用补算值代替。未知边界不生成伪造包。

## 4. HELLO（TYPE=02）

### 4.1 发送条件与布局

仅未配置时发送HELLO，固件自行安排重发周期。BODY固定33 B。

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 1 | pcl_ver | 固定1 |
| 1 | 2 | schema_id | 固定314F |
| 3 | 1 | fw_major | 固件主版本 |
| 4 | 1 | fw_minor | 固件次版本 |
| 5 | 1 | transport | 支持的传输方式位域 |
| 6 | 4 | feature_caps | 功能能力位图 |
| 10 | 4 | pd_caps | PD和链接观测能力位图 |
| 14 | 4 | dpdm_caps | DPDM协议能力位图 |
| 18 | 4 | timebase_hz | 采集计时频率，默认建议1000 Hz |
| 22 | 2 | max_frame | BODY上限128～512 |
| 24 | 2 | max_pd_bytes | PD完整解码字节上限0～268，含头和CRC |
| 26 | 2 | ring_bytes | PD/DPDM记录队列实际字节容量 |
| 28 | 2 | max_dpdm_payload | 无DPDM为0，否则1～128，含校验字节 |
| 30 | 1 | cfg_err | 最近未配置态CFG拒绝码 |
| 31 | 2 | min_sample_period_ms | 无V/I为0，否则1～1000 |

### 4.2 transport

| 位 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | UART | 支持UART字节流承载 |
| 1 | USB_HID | 支持USB HID承载 |
| 2 | USB_BULK | 支持USB Bulk承载，Windows可通过WinUSB访问 |
| 3 | USB_CDC | 支持USB CDC-ACM承载 |
| 4～7 | 保留 | 必须0 |

至少一位为1，可声明多种已实现方式。该字段不选择当前接口，不表示多接口并行采集；当前链路由上位机打开的接口确定，且对应位必须为1。每次只允许一个控制连接，能力和上限须适用于发送此HELLO的接口。

HID报告在Report ID（如有）之后放 `valid_len:u8`，随后valid_len个PCL字节，余下填0。valid_len不超过报告剩余容量或255，0为空报告。双方只向PCL解析器提交有效字节；Report ID和报告大小由接口定义固定。

UART、Bulk和CDC提交有序字节流。一个PCL帧可跨多个报告/传输，一次传输也可含多帧。

### 4.3 能力位图

能力位为1表示对应板型已经实现并验证。

| feature_caps位 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | PD | 支持含原始wire CRC的PD解码包，max_pd_bytes至少34 |
| 1 | VOLTAGE | 支持VBUS电压采样 |
| 2 | CURRENT | 支持IBUS电流采样 |
| 3 | DPDM | 支持至少一种dpdm_caps协议 |
| 4 | GOODCRC_FILTER | 可过滤CRC确认通过的普通SOP GoodCRC |
| 5 | EXT_PAYLOAD | 可完整保存268 B扩展PD解码包 |
| 6 | CC_SELECT | 可固定选择CC1/CC2 |
| 7 | DUAL_CC_RX | 可同时接收CC1和CC2 |
| 8～31 | 保留 | 必须0 |

| pd_caps位 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | SOP | 可捕获普通SOP |
| 1 | SOP_PRIME | 可捕获SOP′ |
| 2 | SOP_DOUBLE_PRIME | 可捕获SOP″ |
| 3 | HARD_RESET | 可检测并记录Hard Reset |
| 4 | CABLE_RESET | 可检测并记录Cable Reset |
| 5 | BAD_PACKET_BYTES | 可暴露部分或完整坏包字节 |
| 6 | CRC_CHECK | 可取得明确PD CRC校验结果 |
| 7 | CC_STATE | 可可靠观测CC连接/极性 |
| 8 | VBUS_STATE | 可可靠观测VBUS存在状态 |
| 9～31 | 保留 | 必须0 |

支持的SOP范围以pd_caps为准，其他已识别序列不产生DATA。

| dpdm_caps位 | 协议 | proto | 定义 |
| ---: | --- | ---: | --- |
| 0 | UFCS | 1 | 支持含原始校验字节的UFCS解码包 |
| 1～31 | 保留 | — | 必须0 |

### 4.4 上限与拒绝码

- PD=0时max_pd_bytes=0；PD=1时max_pd_bytes≥34。
- EXT_PAYLOAD要求PD=1、max_pd_bytes=268、max_frame≥283。
- `5+10+max_pd_bytes <= max_frame`。
- DPDM存在时，`5+10+max_dpdm_payload <= max_frame`，且DPDM、dpdm_caps、max_dpdm_payload一致。
- ring_bytes至少容纳一条已声明最大记录；不包含接收、EVT和发送缓冲。
- min_sample_period_ms覆盖所有已支持V/I组合，且 `min_sample_period_ms*timebase_hz >= 1000`。
- cfg_err使用§7.5错误码；接受有效CFG后清0。

### 4.5 timebase_hz与u32时间

timebase_hz为每秒采集计时刻度数。默认建议1000 Hz（1 ms），允许1000～1000000 Hz内实际支持的整数频率。该值在采集中不变。elapsed_ticks为u32，从本次采集起按模2^32累计，START.elapsed_ticks=0。

| 内容 | elapsed_ticks取值时机 |
| --- | --- |
| PD | 驱动首次处理包完成/错误结束通知时 |
| Reset | 驱动处理检测通知时 |
| DPDM | decoder确认包完成时 |
| START | 采集入口开启，固定0 |
| STOP | 关闭采集入口时 |
| ERROR | 判定错误时 |
| MEASUREMENT | ADC组触发时 |
| LINK_STATE | 状态确认或周期快照取得时 |

上位机将elapsed_ticks扩展为本次采集的64位刻度extended_ticks，按 `相对秒=extended_ticks/timebase_hz` 换算。同刻度按接收顺序稳定排列。默认1 ms不保证亚毫秒间隔分辨率；计时频率不限制PHY/DMA服务速度。批装和发送等待不得重写已保存时间。

| timebase_hz | 分辨率 | u32回绕周期 |
| ---: | --- | --- |
| 1000 | 1 ms | 约49.71天 |
| 10000 | 100 µs | 约4.97天 |
| 100000 | 10 µs | 约11.93小时 |
| 1000000 | 1 µs | 约71.58分钟 |

elapsed_ticks从0xFFFFFFFF自然回绕至0，采集继续，不产生ERROR、STOP或额外事件，不重置id、队列或测量基线。END后的归零仍遵守§6.3。

上位机按以下规则扩展时间：

1. START建立64位时间锚点H=0；重复缓存STOP不参与时间扩展。
2. 对收到的u32时间t，计算 `delta = signed32((t - low32(H)) mod 2^32)`，其中signed32将值解释为有符号32位整数。
3. 本条记录的 `extended_ticks = H + delta`，随后更新 `H = max(H, extended_ticks)`。
4. 允许小幅负delta，以处理DATA/EVT独立排队造成的时间先后交错；不能把每次时间减小都当成回绕。
5. END完成或连接恢复时清除H，已归档的扩展时间不改；下一次START重新从0开始。

例如锚点为0xFFFFFFFE，随后收到t=2，则delta=4，扩展时间为0x100000002；此后若收到延迟的t=0xFFFFFFFF，该记录位于扩展时间0xFFFFFFFF，锚点不回退。

该算法要求相对锚点的真实时间前进量和乱序滞后量均小于2^31刻度。默认1 kHz下约24.85天，1 MHz下约35.79分钟。超过这一无歧义范围的静默、数据缺失或积压，单凭u32无法恢复回绕次数；主机应标记时间连续性不确定，保留原始时间和接收顺序，不伪造连续时长。主机可用本地等待时间辅助发现长静默，但不以其替代设备时间。该情况不要求设备停采，也不增加下位机高位计数或心跳。

16位计数器应软件扩展到u32并处理并发及待处理溢出。可复用1 ms系统时基或使用硬件计数器，不要求每刻度产生中断；共享系统时钟可用起始锚点生成本次elapsed_ticks。

## 5. CFG（TYPE=03）

### 5.1 布局

| 偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 1 | mode | 数据源位域 |
| 1 | 1 | opts | 采集选项位域 |
| 2 | 2 | host_timeout_ms | USB为100～60000，建议2000；无反压UART为0 |
| 4 | 2 | sample_period_ms | 未请求V/I为0；否则设备下限～1000 |
| 6 | 1 | channel_select | 数据通道选择 |
| 7 | 1 | rsvd | 必须0 |

### 5.2 mode

| 位 | 数据源 | 要求 |
| ---: | --- | --- |
| 0 | PD | feature_caps.PD=1 |
| 1 | DPDM | feature_caps.DPDM=1，当前须有UFCS能力 |
| 2～7 | 保留 | 必须0 |

合法值为0x01、0x02、0x03，0x00非法。未选源不启用。双源并行须通过资源校验，不支持时整体拒绝BUSY。

### 5.3 channel_select

| 值 | 仅PD（mode=01） | 仅DPDM（mode=02） | PD+DPDM（mode=03） |
| ---: | --- | --- | --- |
| 0 | 自动选择 | 自动选择 | 两种源各自自动选择 |
| 1 | CC1 | D+ | 非法 |
| 2 | CC2 | D− | 非法 |
| 3 | CC1+CC2 | D+与D− | 全开：CC1+CC2、D+与D− |
| 4～255 | 非法 | 非法 | 非法 |

双源仅允许0或3。固定PD通道要求CC_SELECT，PD双路要求DUAL_CC_RX。全开要求所有所选通道实际同时可用，不允许用轮询替代。DPDM线路由decoder和板级接线校验。不支持的通道或组合拒绝CHANNEL_SELECTION。

AUTO策略及切换盲区由板型说明。DATA的cc/link表示实际来源，按记录定义编码。

### 5.4 opts

| 位 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | GOODCRC_FILTER | 要求mode.bit0=1及对应能力 |
| 1 | USE_VOLTAGE | 要求VOLTAGE能力 |
| 2 | USE_CURRENT | 要求CURRENT能力 |
| 3～7 | 保留 | 必须0 |

未启PD时GOODCRC_FILTER必须0。未请求ADC通道不启动；双V/I通道共用sample_period_ms。配置整体检查，不静默忽略参数或降级。

### 5.5 接受与确认

仅未配置态接受CFG。资源准备成功后清本次队列和计数，打开采集入口，发送独立START确认全部参数。START为本次第一帧，id和elapsed_ticks均0。

运行或收尾态CFG拒绝STATE，不重新开始、不重发START。等待START超时，上位机用END停止可能已开始的采集，等待HELLO后重新CFG。

## 6. END（TYPE=04）

### 6.1 布局

BODY为1 B opts。

| 位 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | FLUSH | 1排空普通队列；0丢弃尚未组帧普通项 |
| 1～7 | 保留 | 必须0 |

方向、长度、CRC及保留位全部合法时才停止。

### 6.2 收尾流程

1. 关闭采集入口、保存STOP时间，10 ms内停止新增PD/DPDM/ADC采集并保持高阻。
2. FLUSH=1排空截止前完整普通项；FLUSH=0丢弃未组帧普通项并置FLUSH_DISCARD。
3. 已封定或在途帧完整处理，已有ERROR pending先发送，不向半帧拼STOP。
4. 发送独立STOP，作为最后新生成的采集帧。
5. 返回未配置，重发HELLO。

收尾绝对期限：USB用本次host_timeout_ms，UART为2000 ms。重复END不改选项、不延长期限。超时可终止发送并清TX状态，旧半帧由对端解析恢复；未收到STOP不得声称正常排空。

未配置时END不开始采集；有缓存STOP可原样重发，否则只发HELLO。故障不能由END隐式清除。

### 6.3 两端计数归零

| 阶段 | 设备 | 上位机 |
| --- | --- | --- |
| 运行 | id递增，elapsed_ticks从0累计 | 维护预期id和当前时间 |
| 接受END | 保存截止时间，旧记录保持原值 | 进入收尾，不提前清计数 |
| STOP生成 | 沿用本次id及已保存停止时间 | 继续处理剩余帧 |
| STOP完整发送/接收后 | 缓存STOP，id和elapsed_ticks运行状态归0 | 归档本次数据，运行id和时间状态归0 |
| 断链/收尾失败 | 丢弃旧队列和在途状态，归0 | 标结束未确认，重建解析上下文并归0 |
| 下一次CFG/START | 从id=0、elapsed_ticks=0开始 | 接受新START建立新一轮计数 |

归档数据的编号和时间不修改。缓存STOP重发不改变已清零计数；主机幂等忽略重复STOP。未收到STOP但收到HELLO时，旧采集按结束未确认关闭并归零。下一次CFG前废弃旧STOP缓存。

## 7. EVT（TYPE=05）

### 7.1 方向与布局

**EVT只由设备发送给上位机。上位机不发送EVT，也不回传START/STOP作为确认。**反向EVT整帧拒绝，不改变状态。

| BODY偏移 | 字节 | 字段 | 定义 |
| --- | ---: | --- | --- |
| 0 | 4 | id | 与DATA共用，规则见§3.2 |
| 4 | 1 | loss_flags | 定义见§3.3 |
| 5 | 12×N | items | N≥1个完整事件项 |

`LEN>=17`且 `(LEN-5)%12=0`。max_frame=512时最多42项。事件不跨帧，整帧验证后应用。

| 项内偏移 | 字节 | 字段 |
| --- | ---: | --- |
| 0 | 1 | code |
| 1 | 4 | elapsed_ticks |
| 5 | 1 | flags |
| 6 | 2 | a |
| 8 | 2 | b |
| 10 | 2 | c |

### 7.2 事件码

| code | 名称 | flags | a | b | c |
| ---: | --- | --- | --- | --- | --- |
| 01 | START | mode | opts低8位、channel_select高8位 | sample_period_ms | host_timeout_ms |
| 02 | STOP | 0 END、1失联、2故障 | 错误码 | 0 | 0 |
| 03 | ERROR | 触发TYPE，非命令为0 | 错误码 | 0 | 0 |
| 04 | MEASUREMENT | 测量标志 | Voltage_cv | Current_ma | 0 |
| 05 | LINK_STATE | 状态有效位 | 状态位 | 0 | 0 |

START、STOP、ERROR各自独立成帧，BODY=17 B，全帧24 B，不等待普通事件批装。Reset只生成PD记录。

### 7.3 START与STOP发送条件

| 场景 | 设备行为 |
| --- | --- |
| 上电/复位/未配置 | 发送HELLO，不发无采集上下文的START/STOP |
| 有效CFG且资源准备成功 | START先于全部DATA/普通EVT，无流量也发 |
| CFG非法或开始前资源失败 | 不开始，使用HELLO.cfg_err，不发START/STOP |
| 运行中再次CFG | 拒绝，可发ERROR，不再发START |
| 收尾中再次CFG | 拒绝，不新增ERROR pending |
| 合法END | 按§6收尾，STOP(reason=0)最后发送 |
| 致命故障 | 停采，链路可用时ERROR后STOP(reason=2) |
| 断链/reset/suspend/发送超时 | 停采，原链路可用才尝试STOP(reason=1)，不续发旧帧到新连接 |
| 重复END | 收尾中不重建流程；未配置时可原样重发缓存STOP |

START.elapsed_ticks=0。STOP.elapsed_ticks是实际停止新增记录的时刻，不是发送完成时刻。START/STOP各有独立pending槽；START后立即故障，链路可用则依次START、ERROR、STOP。

STOP确认停止，不保证此前无损；无法送达时主机标记结束未确认。正常顺序为 `CFG → START → DATA/EVT → END → 剩余完整帧 → STOP → HELLO`。

### 7.4 测量和链接状态

#### 7.4.1 MEASUREMENT

| flags位 | 定义 |
| ---: | --- |
| 0 | 电压变化 |
| 1 | 电流变化 |
| 2 | 首次基线 |
| 3 | 周期快照 |
| 4～7 | 保留0 |

至少一位非零。每个采样组最多生成一项，带同组V/I快照，elapsed_ticks取组触发时刻，两通道不保证同时转换。

| 字段 | 单位 | 有效值 | 无效值 |
| --- | --- | --- | --- |
| Voltage_cv | 0.01 V | 0～65534 | FFFF |
| Current_ma | 1 mA/计数 | 0～65534 | FFFF |

Current_ma为u16，数值1表示1 mA，1000表示1000 mA（1 A），0表示实测0 mA。FFFF表示未请求、无效或越界。负向电流当前报FFFF。未请求通道不启ADC，字段固定FFFF。

首组尝试发布基线；变化与上次成功入队值比较，失败不更新基线。每1000 ms在下一完整组尝试重发最新快照，失败继续尝试。周期快照不恢复期间瞬态，不保证拥塞时准时。滤波、量化和校准由板型说明。

#### 7.4.2 LINK_STATE

| flags位 | 有效性 |
| ---: | --- |
| 0 | attached已知 |
| 1 | polarity已知 |
| 2 | VBUS存在已知 |
| 3～7 | 保留0 |

| a字段位 | 定义 |
| --- | --- |
| 0 | attached：0未连接、1连接 |
| 1～2 | polarity：0未知、1 CC1、2 CC2，3非法 |
| 3 | VBUS：0不存在、1存在 |
| 4～15 | 保留0 |

无效字段对应值为0。开始、可靠状态变化及每1000 ms尝试发布当前状态，无能力不发送。不从PD是否出现推断连接；未请求电压ADC时须有独立检测路径才能报告VBUS状态。重复快照不是新插拔边沿。队列不足时可用最新pending覆盖旧状态并置EVENT_LOST，不覆盖封定帧。

### 7.5 错误码

HELLO.cfg_err、ERROR.a和STOP.a共用以下定义。

| 值 | 名称 | 定义 |
| ---: | --- | --- |
| 0 | OK | 无错误 |
| 1 | MODE | 数据源非法/不支持 |
| 2 | OPTS | 选项非法/能力不符 |
| 3 | LEN | 命令长度不符 |
| 4 | FRAME_CRC | 本地CRC诊断 |
| 5 | STATE | 当前状态不允许命令 |
| 6 | BUSY | 资源不可用 |
| 7 | RANGE | 参数越界 |
| 8 | TRANSPORT | 传输失败/超时 |
| 9 | PHY_DECODER | 接收外设/decoder故障 |
| 10 | CHANNEL_SELECTION | 数据通道或组合不支持 |

未验证CRC的候选不按其内容生成命令相关ERROR。ERROR只保留最近pending，收尾期间不新增；故障须驱动恢复后才回未配置。

## 8. 传输与解析要求

### 8.1 字节流恢复

读取边界不是PCL帧边界。先检查LEN再验CRC。长度非法或CRC错误，从候选SYNC下一字节重扫。候选连续1000 ms无新增字节仍不完整时放弃并恢复搜索；解析超时本身不停止采集。

CRC正确但方向、已知帧长度、字段或记录非法时消费整帧并原子拒绝，不执行BODY内嵌命令。未知TYPE在CRC正确后整帧跳过。

小MCU可流式验CRC并仅缓存8 B合法命令；对超长候选使用有界丢弃恢复，不强制分配最大上行帧大小的命令缓存。实现须说明精简恢复边界。

### 8.2 连接和停止恢复

连接后等待HELLO。UART或应用重开后设备可能仍采集，此时发送END(FLUSH=0)，丢弃恢复期间旧普通帧，等待HELLO后CFG。

主机停止END重试、处理本地待发命令后才能发新CFG。匹配配置的START之后才接纳普通帧，新CFG后不得继续旧END重试。本地清缓存不保证桥接器清空，恢复依赖有序链路和HELLO/START边界。

运行时意外HELLO，旧采集按中断或结束未确认关闭并归零，不自动续采。UART没有可靠断连语义时不能检测应用退出，主机正常退出须发送END。

### 8.3 发送进展与调度

HID/Bulk不要求DTR；CDC仅在接口明确约定时使用DTR下降作为断开。USB reset、断开、suspend终止旧采集。watchdog只对已有完整待发帧且无真实发送进展计时；无待发帧不计时，IN NAK本身不证明主机失联。

DATA满即提交，否则最老完整记录等待目标2 ms；普通EVT目标20 ms。这是组帧等待目标，不是到达时间保证。收尾不等批装期限。

START优先、STOP最后，中间进行有界公平轮转。可采用最多连续4个DATA后服务就绪普通EVT，ERROR同样不得饿死普通数据。

## 9. 硬件与固件注意事项

### 9.1 被动前端

被测CC、D+/D−全程高阻，不输出Rp/Rd或测试电平。验证上电、复位、退出、掉电及保护路径；检查dead-battery Rd和默认引脚配置。上行USB与被测DPDM网络电气分离。VBUS使用适当采样前端。

### 9.2 接收完整性

每种PHY须验证消息头、正文、原始CRC字节及结束边界的可见性，不能用CRC状态寄存器代替实际CRC字节。验证坏包、Reset、最大包和并发接收，不将正常协商成功视为抓包能力证明。

### 9.3 实现约束

接收与DMA重装优先。中断先保存状态、计时和字节，CRC16、批装、发送在低优先级完成。计时不使用浮点；未对齐字段显式读写。

接收缓冲、记录环、EVT和在途帧所有权明确；生命周期不重叠可复用，不覆盖待发送内容。整记录、整事件提交，空间不足丢最新并置标记。单/双接收缓冲按最短包间隔和实测重装时间选择。

mode=0x03须同时验证PD、DPDM、ADC、DMA和引脚资源，channel_select=3须验证全部通道同时工作。能力声明必须与实际可用配置一致。

### 9.4 上位机数据处理

验证HELLO、完整CFG确认、id和elapsed_ticks零起点。记录type须对应mode启用位。归档保留原始payload、CRC状态、截短及损失诊断。

CRC_UNKNOWN不等于通过，TRUNCATED/RX_ERROR不能按完整包尾部解析CRC。未报告损失不等于无损。测量FFFF显示未知，不补零或线性插值。暂停画面仍持续读取，停止采集使用END。

## 10. 验证要求

1. 验证各声明SOP/Reset、原始CRC保留、坏CRC不修正、最大包及截短前缀。
2. 验证HID有效长度、任意读取切分、BODY含SYNC、非法长度、CRC错误和半帧恢复。
3. 验证mode和channel_select组合，双源仅允许自动或全开。
4. 验证START先发、END截止、STOP最后、重复STOP原样重发及两端归零。
5. 验证u32 id及elapsed_ticks自然回绕、跨回绕的DATA/EVT乱序扩展、长静默时的不确定标记、END后归零、窄计数器扩展及计时读取并发；时间回绕不得停止采集。
6. 验证队列溢出、周期快照恢复、主机停读、传输失败和deadline。
7. 每板型记录接口、时钟、前端、接收通知延迟、DMA重装、链接map与栈水位；未验证能力不得声明。

## 附录A 长度汇总

| 项目 | 长度 |
| --- | --- |
| 外壳 | 7 B |
| DATA/EVT公共头 | 5 B |
| HELLO BODY/整帧 | 33 / 40 B |
| CFG BODY/整帧 | 8 / 15 B |
| END BODY/整帧 | 1 / 8 B |
| PD/DPDM记录固定头 | 10 B |
| 最大完整PD包 | 268 B，含原始CRC |
| 最大PD记录/DATA BODY/整帧 | 278 / 283 / 290 B |
| 无正文PD控制包/记录/整帧 | 6 / 16 / 28 B |
| Reset记录/DATA BODY/整帧 | 10 / 15 / 22 B |
| 最短DPDM记录/DATA BODY/整帧 | 11 / 16 / 23 B |
| EVT事件项 | 12 B |
| 单项EVT BODY/整帧 | 17 / 24 B |
| 512 B上限下42项EVT BODY/整帧 | 509 / 516 B |

## 附录B 参考资料

- [仓库原方案及PD尺寸依据](capture-protocol.md)。
- [WCH CH32X035](https://www.wch.cn/products/CH32X035.html)。
- [WCH CH641官方仓库](https://github.com/openwch/ch641)。
- [STM32G071数据手册](https://www.st.com/resource/en/datasheet/stm32g071cb.pdf)。
- [STM32G0x1参考手册RM0444](https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)。
