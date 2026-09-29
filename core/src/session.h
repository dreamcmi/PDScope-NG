// session.h — 统一数据层：一份抓包 = 一个会话
//
// 职责：
//   · 按**内容**分流三种来源（.atkcc / POWER-Z .sqlite / .pdStream），不看扩展名
//   · 容器级元数据立刻可取（毫秒级）；**报文解码 lazy**，由调用方显式触发
//   · 筛选 / 排序 / 分页 / 详情 / 波形抽稀 / CSV·JSON 导出
//   · 原始偏移、时间戳与采样序号保留整数精度
#pragma once

#include "container/atkcc.h"
#include "container/pdstream.h"
#include "container/powerz.h"
#include "container/sqlite_reader.h"
#include "decode_stats.h"
#include "filters.h"
#include "packet.h"
#include "pipeline.h"

#include <atomic>
#include <memory>

namespace pdscope {

enum class SourceKind { Atkcc, PowerzSqlite, PdStream };

/** 打开失败时抛 Error，消息带文件类型与原因。 */
class Session {
public:
    static std::unique_ptr<Session> openBytes(const Bytes& bytes, const std::string& nameHint);

    /* ── 标识 ── */
    const std::string& name() const { return name_; }
    SourceKind sourceKind() const { return sourceKind_; }
    const char* sourceName() const;      // atkcc | powerz
    const char* containerName() const;   // atkcc | sqlite | pdstream
    const std::string& protocol() const { return protocol_; }
    uint64_t fileBytes() const { return fileSize_; }

    /* ── 元数据（不触发解码）── */
    json metadata() const;
    /** 已打开的通道列表（.atkcc 多通道 / POWER-Z 单通道）。 */
    std::vector<int> channelOrder() const;

    /* ── 解码 ── */
    /** 幂等：第二次起直接返回已有结果。`channel < 0` 表示自动挑选。 */
    void decode(int channel, double sampleRateOverride, bool metadataOnly);
    bool decoded() const { return decoded_; }
    bool cancelled() const { return cancelled_; }
    const DecodeStats& stats() const { return stats_; }
    const std::vector<Packet>& packets() const { return packets_; }
    json statsJson() const;

    /** 请求中断（可从其它线程调用）。 */
    void cancel() { cancelFlag_.store(true); }
    /** 解码前把标志归零。 */
    void clearCancel() { cancelFlag_.store(false); }

    /* ── 视图 ── */
    void setFilter(const json& j);
    const Filters& filters() const { return filters_; }
    const SortSpec& sort() const { return sort_; }
    ViewMode viewMode() const { return viewMode_; }
    /** 当前视图（条数 = 通过筛选的报文数）。⚠ 同上：读之前视图可能还是脏的。 */
    const std::vector<const Packet*>& view() const { return view_; }
    /**
     * 当前视图下的报文条数。
     *
     * ⚠ 这里**必须**顺带把脏视图重建出来，不能直接 `return view_.size()`。
     *   视图是懒重建的（`rebuildView` 只在 `pageJson` / `exportCsv` 里被触发），
     *   所以刚解码完或刚改过筛选时 `view_` 还是空的：`pageJson()` 会顺手重建、
     *   照常返回一行行数据，而直接读长度只能得到 0。两边不一致的后果是界面
     *   「列表里明明有行，右上角却写 0 条」。
     *
     *   这个 bug 之所以能活很久，是因为**视图恰好为空时它看不出任何异常**
     *   （夹具里三条 GoodCRC 全被默认筛选挡掉，正确答案本来就是 0）——
     *   所以回归测试必须挑一份「筛完还剩东西」的夹具，并断言它与分页结果一致。
     */
    uint64_t viewCount();
    uint64_t packetCount() const { return packets_.size(); }

    /* ── 查询 ── */
    json pageJson(uint64_t offset, uint32_t limit) const;
    json packetDetailJson(uint64_t index) const;
    Bytes waveformBinary(int channel, uint64_t startSample, uint64_t endSample,
                         uint32_t maxPoints) const;
    json busSeriesJson(uint32_t targetPoints) const;

    /**
     * 报文类型 → 条数。**统计的是全部报文，不受筛选影响** —— 界面拿它当
     * 「具体报文类型」多选列表的候选与计数，列表本身不该随勾选而变化。
     * 按条数降序、同数按类型名升序。
     */
    json typeCountsJson() const;

    /**
     * 时间轴用的报文标记（紧凑二进制，避免几万条报文的 JSON 开销）。
     * 布局见 `pdscope.h` 的 `pdscope_packet_marks`：
     *   u32 n │ f64 ts[n] │ u8 kind[n] │ u8 flags[n]
     * 取的是**全部报文**（时间轴画的是整份抓包，不跟着表格筛选走）。
     */
    Bytes packetMarksBinary() const;

    /* ── 导出 ── */
    /**
     * @param filtered true（默认）= 导出**当前视图**（受筛选/排序影响），界面「另存为」走这条；
     *                 false = 导出**全部报文**，不受筛选影响 —— 命令行导出走这条，
     *                 直接喂全部 packets。
     */
    std::string exportCsv(uint64_t limit, bool bom, bool filtered = true) const;
    std::string exportJson(uint64_t limit) const;
    std::string defaultCsvName() const;

    /* ── 进度 ── */
    void setProgressCallback(ProgressFn cb) { onProgress_ = std::move(cb); }
    void snapshotProgress(int* phase, int* channel, uint64_t* done, uint64_t* total,
                          uint64_t* packets) const;

private:
    Session() = default;
    void dispatch(const Bytes& bytes, const std::string& nameHint);
    /** 把 ADC 采样序列（最近邻）挂到每条报文上 —— 与界面/CSV 的口径一致。 */
    void attachBusValues();
    /** 与 `cap.meta.bus` 同形的输入序列。 */
    std::vector<BusInput> busInput() const;
    std::string activeProtocolName() const;   // 用于筛选默认值

    // 会话只需要知道文件多大（界面显示用），**不需要留着整份字节** ——
    // 早先存了一份 `Bytes bytes_`，白白复制整份抓包，还顺带埋了「容器引用它」的悬垂隐患。
    // 字节本身由各自容器的 reader 以 shared_ptr 持有。
    uint64_t fileSize_ = 0;
    std::string name_;
    SourceKind sourceKind_ = SourceKind::Atkcc;
    std::string protocol_ = "USB PD";

    std::shared_ptr<AtkccCapture> atkcc_;
    std::shared_ptr<PowerzCapture> powerz_;
    std::shared_ptr<SqliteReader> sqlite_;

    // 容器级元数据快照（打开时算好，避免重复解析）
    json metaCache_;

    bool decoded_ = false;
    bool cancelled_ = false;
    int channel_ = 0;
    std::vector<Packet> packets_;
    DecodeStats stats_;

    Filters filters_;
    SortSpec sort_;
    ViewMode viewMode_ = ViewMode::All;
    std::vector<const Packet*> view_;
    bool viewDirty_ = true;

    std::atomic<bool> cancelFlag_{false};
    mutable std::atomic<int> progressPhase_{0};
    mutable std::atomic<int> progressChannel_{0};
    mutable std::atomic<uint64_t> progressDone_{0};
    mutable std::atomic<uint64_t> progressTotal_{0};
    mutable std::atomic<uint64_t> progressPackets_{0};
    ProgressFn onProgress_;

    void rebuildView();
};

/* ── JSON 序列化（ABI 与会话共用）── */

json packetListItemJson(const Packet& p, const std::string& protocol);
json packetDetailJsonOf(const Packet& p, const std::string& protocol);
json packetFullJson(const Packet& p);
json decodeStatsJson(const DecodeStats& st);

}  // namespace pdscope
