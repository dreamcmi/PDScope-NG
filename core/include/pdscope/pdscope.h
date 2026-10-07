/*
 * pdscope.h — PDScope 核心的稳定 C ABI
 *
 * 设计约束（与 doc/architecture.md 一致）：
 *   · 只导出 C 符号：不把 C++ 类、异常、STL 容器跨 ABI 边界。
 *   · 字符串一律 UTF-8；长度、偏移、时间位置一律 64 位整数。
 *   · 返回 `pdscope_buf` 的接口，内存由库分配，调用方必须用 `pdscope_buf_free`
 * 释放。 · 会话句柄是不透明指针；`pdscope_open_*` 失败时返回 NULL
 * 并给出错误文本。 ·
 * 列表按页取、波形按二进制数组取，避免把整份抓包一次复制给调用方。
 *
 * 线程约定：同一个会话不得并发调用；不同会话之间互不影响。
 * 取消：从另一线程调用 `pdscope_cancel()` 是允许的，解码循环会尽快返回
 *       `PDSCOPE_ERR_CANCELLED`。
 */

#ifndef PDSCOPE_H
#define PDSCOPE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(PDSCOPE_BUILD_SHARED)
#define PDSCOPE_API __declspec(dllexport)
#else
#define PDSCOPE_API __declspec(dllimport)
#endif
#define PDSCOPE_CALL __cdecl
#else
#define PDSCOPE_API __attribute__((visibility("default")))
#define PDSCOPE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── 版本 ─────────────────────────────────────────────────────────── */

/** ABI 版本。接口任何不兼容改动都要 +1；Dart 侧先比对再调用。 */
#define PDSCOPE_ABI_VERSION 1u

/** 库自身的版本字符串（如 "0.1.0"），静态存储，不需要释放。 */
PDSCOPE_API const char *PDSCOPE_CALL pdscope_version(void);

/** 返回编译期写定的 ABI 版本，用于运行期比对。 */
PDSCOPE_API uint32_t PDSCOPE_CALL pdscope_abi_version(void);

/* ── 状态码 ───────────────────────────────────────────────────────── */

typedef enum pdscope_status {
  PDSCOPE_OK = 0,
  PDSCOPE_ERR_ARGUMENT = -1,    /* 入参非法（空指针、越界索引…） */
  PDSCOPE_ERR_IO = -2,          /* 读文件失败 */
  PDSCOPE_ERR_FORMAT = -3,      /* 文件内容不符合任何已知格式 */
  PDSCOPE_ERR_UNSUPPORTED = -4, /* 认得容器，但该特性未实现 */
  PDSCOPE_ERR_CANCELLED = -5,   /* 被 pdscope_cancel 中断 */
  PDSCOPE_ERR_STATE = -6,       /* 调用顺序不对（例如未解码就查列表） */
  PDSCOPE_ERR_MEMORY = -7,      /* 分配失败 */
  PDSCOPE_ERR_INTERNAL = -8     /* 其它内部错误，看错误文本 */
} pdscope_status;

/** 状态码 → 英文短名（静态存储）。 */
PDSCOPE_API const char *PDSCOPE_CALL pdscope_status_name(int32_t status);

/* ── 内存 ─────────────────────────────────────────────────────────── */

/** 库分配的字节块。`data` 为 NULL 表示空结果（合法）。 */
typedef struct pdscope_buf {
  uint8_t *data;
  size_t len;
} pdscope_buf;

/** 释放任何由本库返回的 `pdscope_buf`。重复调用是安全的（会把指针清零）。 */
PDSCOPE_API void PDSCOPE_CALL pdscope_buf_free(pdscope_buf *buf);

/** 释放本库返回的 C 字符串（错误文本）。 */
PDSCOPE_API void PDSCOPE_CALL pdscope_str_free(char *s);

/** @brief 实时 PD 解码器；单个句柄只能串行调用，按 CC 分别保留协商状态。 */
typedef struct pdscope_live_pd pdscope_live_pd;

/**
 * @brief 创建实时 PD 解码器，不打开文件或硬件。
 * @return 成功返回句柄，内存不足时返回 NULL。
 */
PDSCOPE_API pdscope_live_pd *PDSCOPE_CALL pdscope_live_pd_create(void);

/**
 * @brief 清空全部 CC 的协商状态，用于新的 START。
 * @param decoder 有效解码器句柄。
 * @return 状态码。
 */
PDSCOPE_API int32_t PDSCOPE_CALL
pdscope_live_pd_reset(pdscope_live_pd *decoder);

/**
 * @brief 解码一条 PCL PD 记录，保留原始线上 CRC 和接收状态。
 * @param decoder 实时解码器；调用期间不得并发访问。
 * @param bytes PD 头、正文和原始 CRC；Reset 时可为 NULL。
 * @param len 字节数，最大 268；截短或接收错误记录仅含连续前缀。
 * @param sop PCL SOP 枚举 0～5。
 * @param cc PCL CC 枚举 0～2。
 * @param flags PCL PD flags，低四位有效。
 * @param extended_ticks 上位机已扩展的本次采集相对刻度。
 * @param timebase_hz HELLO 计时频率，1000～1000000。
 * @param index 本次采集从零开始的逻辑报文序号。
 * @param out_json 接收包含 row 和 detail 的 JSON；须先零初始化，再由库释放。
 * @return 状态码；非法参数不修改解码器状态。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_live_pd_decode(
    pdscope_live_pd *decoder, const uint8_t *bytes, size_t len, uint8_t sop,
    uint8_t cc, uint8_t flags, uint64_t extended_ticks, uint32_t timebase_hz,
    uint64_t index, pdscope_buf *out_json);

/**
 * @brief 释放解码器。传 NULL 安全；释放后不得再使用句柄。
 * @param decoder 待释放的句柄。
 */
PDSCOPE_API void PDSCOPE_CALL pdscope_live_pd_close(pdscope_live_pd *decoder);

/* ── 会话 ─────────────────────────────────────────────────────────── */

typedef struct pdscope_session pdscope_session;

/**
 * 从内存字节打开一份抓包。格式由**内容**判定（不看文件名后缀）：
 * ZIP 魔数 → .atkcc；SQLite 魔数 + pd_table/ufcs_table → POWER-Z；结构自证 →
 * .pdStream。
 *
 * @param data       文件字节（函数返回后调用方可立即释放 —— 内部会自行持有）
 * @param len        字节数
 * @param name_hint  文件名，仅用于展示与默认导出名（可为 NULL）
 * @param err_out    失败时接收 UTF-8 错误文本，需用 pdscope_str_free 释放（可为
 * NULL）
 * @return           会话句柄；失败返回 NULL
 */
PDSCOPE_API pdscope_session *PDSCOPE_CALL pdscope_open_bytes(
    const uint8_t *data, size_t len, const char *name_hint, char **err_out);

/** 从文件路径打开（内部读全文件）。语义同 pdscope_open_bytes。 */
PDSCOPE_API pdscope_session *PDSCOPE_CALL
pdscope_open_file(const char *path_utf8, char **err_out);

/** 关闭会话并释放其全部资源。传 NULL 是安全的。 */
PDSCOPE_API void PDSCOPE_CALL pdscope_close(pdscope_session *s);

/* ── 元数据 ───────────────────────────────────────────────────────── */

/**
 * 容器级元数据（JSON 对象）。**不触发报文解码**，因此对大文件也是毫秒级。
 * 字段见 doc/abi.md 的 `pdscope.metadata`。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_metadata(pdscope_session *s,
                                                  pdscope_buf *out_json);

/* ── 解码 ─────────────────────────────────────────────────────────── */

typedef struct pdscope_decode_opts {
  /** 只解这一个通道；<0 表示用会话自动挑选的通道（.atkcc 多通道时） */
  int32_t channel;
  /** >0 时强制使用该采样率（Hz），覆盖「文件声明 → 波形自检 → 兜底」三级策略 */
  double sample_rate_override;
  /** 非 0 时只解析元数据不建报文列表（用于快速预览） */
  int32_t metadata_only;
  /** 保留字段，传 0 */
  int32_t reserved;
} pdscope_decode_opts;

/**
 * 解码当前会话的全部报文。可重复调用（第二次起直接返回上次结果）。
 * 传入 opts 为 NULL 时使用默认选项。
 *
 * @param out_stats_json 可选，接收统计摘要（JSON 对象），需 free
 * @return PDSCOPE_OK / PDSCOPE_ERR_CANCELLED / 其它错误码
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_decode(pdscope_session *s,
                                                const pdscope_decode_opts *opts,
                                                pdscope_buf *out_stats_json);

/** 请求中断正在进行的解码（可从其它线程调用）。 */
PDSCOPE_API void PDSCOPE_CALL pdscope_cancel(pdscope_session *s);

/** 解码进度快照。 */
typedef struct pdscope_progress {
  int32_t phase; /* 0=空闲 1=读容器 2=解码 */
  int32_t channel;
  uint64_t done;
  uint64_t total;
  uint64_t packets;
} pdscope_progress;

PDSCOPE_API int32_t PDSCOPE_CALL pdscope_get_progress(pdscope_session *s,
                                                      pdscope_progress *out);

/* ── 筛选与排序（改变的是「视图」，不影响报文总数）────────────────── */

/**
 * 设置筛选/排序条件（JSON，字段见 doc/abi.md 的 `pdscope.filter`）。
 * 传 NULL 或 `{}` 表示清空筛选。调用后 `pdscope_query_page`
 * 与导出都按新视图走。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_set_filter(pdscope_session *s,
                                                    const char *filter_json,
                                                    char **err_out);

/** 当前视图下的报文条数。 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_view_count(pdscope_session *s,
                                                    uint64_t *out_count);

/** 未筛选时的报文总条数。 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_packet_count(pdscope_session *s,
                                                      uint64_t *out_count);

/* ── 查询 ─────────────────────────────────────────────────────────── */

/**
 * 取当前视图的一页报文（JSON 数组，按视图顺序）。每项是**列表用的紧凑对象**，
 * 字段见 doc/abi.md 的 `packet.list`。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_query_page(pdscope_session *s,
                                                    uint64_t offset,
                                                    uint32_t limit,
                                                    pdscope_buf *out_json);

/** 单条报文的完整详情（JSON 对象，含 `details` 分组数组）。`index`
 * 是原始报文序号。 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_packet_detail(pdscope_session *s,
                                                       uint64_t index,
                                                       pdscope_buf *out_json);

/**
 * 取一段采样区间内的原始电平包络（二叉数组，非 JSON）。
 *
 * 输出布局（小端）：
 *   u32 n
 *   u64 bucket                     每个桶覆盖的采样点数
 *   u64 start_sample
 *   f32 hi[n]                      每桶是否出现过「高」
 *   f32 lo[n]                      每桶是否出现过「低」
 * 共 4 + 8 + 8 + 8*n 字节。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_waveform_range(
    pdscope_session *s, int32_t channel, uint64_t start_sample,
    uint64_t end_sample, uint32_t max_points, pdscope_buf *out_binary);

/**
 * 取模拟量轨迹的等间隔抽稀序列（JSON 对象，字段见 doc/abi.md 的 `busSeries`）。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_bus_series(pdscope_session *s,
                                                    uint32_t target_points,
                                                    pdscope_buf *out_json);

/**
 * 报文类型 → 条数（JSON 数组，每项 `{"type": "...", "n": 12}`，按条数降序）。
 *
 * 统计的是**全部报文，不受筛选影响**：界面拿它当「具体报文类型」筛选的候选与计数，
 * 那份候选列表本身不该随勾选变化（否则选一个就把别的选项挤掉了）。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_type_counts(pdscope_session *s,
                                                     pdscope_buf *out_json);

/**
 * 时间轴用的报文标记（紧凑二进制，几万条报文走 JSON 太浪费）。
 *
 * 输出布局（小端），取的是**全部报文**（时间轴画整份抓包，不跟表格筛选走）：
 *   u32 n                 报文条数
 *   f64 ts[n]             每条的时间（秒）
 *   u8  kind[n]           类别编号：0=Control 1=Data 2=Extended 3=VDM 4=Error
 * 5=Custom u8  flags[n]          bit0=CRC 校验未通过；bit1=是某个配对的确认对象
 * 共 4 + 10n 字节。
 *
 * ⚠ `kind` 的编号是**接口契约**，加类别要在 core 与 Dart 两侧同时改。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_packet_marks(pdscope_session *s,
                                                      pdscope_buf *out_binary);

/* ── 导出 ─────────────────────────────────────────────────────────── */

typedef struct pdscope_export_opts {
  /** >0 时只导出前 N 条（按当前视图顺序） */
  uint64_t limit;
  /** 非 0 时在 CSV 前写 UTF-8 BOM（落盘用；管道输出传 0） */
  int32_t bom;
  /** 保留字段，传 0 */
  int32_t reserved;
} pdscope_export_opts;

/** 导出当前视图为 CSV（含表头、CRLF 行尾、逐字段加引号）。 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_export_csv(
    pdscope_session *s, const pdscope_export_opts *opts, pdscope_buf *out_text);

/** 导出全部报文（不受筛选影响）为 JSON。 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_export_json(
    pdscope_session *s, const pdscope_export_opts *opts, pdscope_buf *out_text);

/**
 * 默认导出文件名主干，如 `抓包-ch1.csv`。调用方负责拼目录。
 * 返回的字符串需用 pdscope_str_free 释放。
 */
PDSCOPE_API int32_t PDSCOPE_CALL pdscope_default_csv_name(pdscope_session *s,
                                                          char **out_name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDSCOPE_H */
