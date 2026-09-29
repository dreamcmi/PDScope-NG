// pipeline.h — 把 `.atkcc` 变成 PD 报文列表的完整流水线
//
//   解压分块 → 位流(1bit/采样) → BMC 边沿状态机 → 4B5B / PD 解析 → 报文对象
//
// 另含三条来源共用的辅助：GoodCRC 配对、模拟量轨迹、波形包络。
#pragma once

#include "bmc.h"
#include "container/atkcc.h"
#include "decode_stats.h"
#include "packet.h"

#include <functional>

namespace pdscope {

/** 声明值与波形实测的最大允许偏差（见 resolveSampleRate 的说明）。 */
constexpr double kRateTolerance = 0.25;

/** 「混合字节占比」超过它就判为浮空 / 噪声线（自动挑通道时用来排除）。 */
constexpr double kNoiseEdgeLike = 0.35;

/** GoodCRC 向前搜索窗口（条）。 */
constexpr int kAckWindow = 16;

/** 两次让出之间的时间预算（毫秒）。进度回调的粒度由它决定。 */
constexpr double kYieldBudgetMs = 20.0;

/* ── 采样率解析 ─────────────────────────────────────────────────── */

struct RateResolution {
    double rate = 0;
    std::string source;    // declared | measured | default | override
    double declared = 0;
    std::string declaredSource;
    double measured = 0;
    bool hasMeasured = false;
    double measuredUi = 0;
    double confidence = 0;
    std::string declaredText;
    bool hasDeclaredText = false;
    std::string note;
    bool hasNote = false;
};

/** 只用波形游程反推采样率（只读最前面 1~2 个块）。 */
std::optional<SampleRateEstimate> probeSampleRate(const AtkccCapture& cap, int channel,
                                                 int maxChunks, size_t maxRuns,
                                                 const CancelFn& shouldStop);

/**
 * 决定用哪个采样率把「采样点序号」换算成时间。三级策略，越靠前越优先：
 *   ① 文件声明（`channel.ini`）
 *   ② 波形自检（BMC 游程反推）—— 只在 ① 缺失或与波形差超 ±25% 时推翻 ①
 *   ③ 兜底 2.5 MHz
 * `sampleRateOverride > 0` 时直接用它（命令行 `--rate` 排查异常文件用）。
 */
RateResolution resolveSampleRate(const AtkccCapture& cap, int channel,
                                 const CancelFn& shouldStop,
                                 double sampleRateOverride = 0);

/* ── GoodCRC 配对 ───────────────────────────────────────────────── */

/**
 * 给每条 GoodCRC 找出「它所确认的那条报文」，写入 `ackOf` / `ackType`。
 * 判据：① 紧邻性（向前找最近一条「非 GoodCRC 且方向相反」的报文）
 *       ② MessageID 校验（仅当①那条 CRC 完好却 ID 对不上时才改用②）
 * 坏掉的 GoodCRC 不参与配对。
 */
void linkGoodCrc(std::vector<Packet>& packets);

/* ── 模拟量轨迹 ─────────────────────────────────────────────────── */

struct BusInput {
    uint64_t sample = 0;
    double vbus = 0;
    double ibus = 0;
    double a = 0;
    double b = 0;
    bool aux = false;    // 这一路是否有第三/第四路模拟量（POWER-Z 有；.atkcc 的 bus.ini 没有）
};

struct BusSeries {
    double t0 = 0;
    uint64_t step = 0;
    uint64_t n = 0;
    std::vector<double> vbus;
    std::vector<double> ibus;
    std::vector<double> ca;
    std::vector<double> cb;
    double vmax = 0;
    double imax = 0;
    double camax = 0;
    double cbmax = 0;
    double sampleRate = 0;
    bool hasAux = false;
};

/** 把模拟量轨迹展开成等间隔序列，用于画图。 */
BusSeries buildBusSeries(const std::vector<BusInput>& bus, uint64_t totalSamples,
                         double sampleRate, uint32_t targetPoints = 3000);

/** 取某个采样点处的 VBUS/IBUS（阶梯保持）。 */
BusInput busAt(const std::vector<BusInput>& bus, uint64_t sample);

/* ── 波形包络 ───────────────────────────────────────────────────── */

struct WavePoint {
    uint64_t s = 0;
    int hi = 0;
    int lo = 1;
};

struct WaveformRange {
    std::vector<WavePoint> points;
    uint64_t bucket = 1;
    uint64_t startSample = 0;
    uint64_t endSample = 0;
    double sampleRate = 0;
};

/** 取一段采样区间内的原始电平（压缩为 min/max 包络）。 */
WaveformRange readWaveform(const AtkccCapture& cap, int channel,
                           uint64_t startSample, uint64_t endSample,
                           uint32_t maxPoints = 1200);

/* ── 通道自动挑选 ───────────────────────────────────────────────── */

/**
 * 多通道 `.atkcc` 自动挑一个「有报文」的通道。
 *
 * ⚠ 不能只挑「活动度最高」的：浮空 / 未接的线是接近 50% 的随机电平，
 * 活动度**远高于**真正在跑 PD 的 CC 线（那条线大部分时间空闲在同一电平）。
 * 所以先用「混合字节占比」把噪声排除，再在剩下的里挑活动度最高的。
 */
struct ChannelPickResult {
    int picked = 0;
    int noiseRejected = 0;
    bool allNoisy = false;
    std::map<int, ChannelActivity> activity;
};

ChannelPickResult pickChannel(const AtkccCapture& cap, const CancelFn& shouldStop);

/* ── 主流程 ─────────────────────────────────────────────────────── */

struct DecodeOutcome {
    std::vector<Packet> packets;
    DecodeStats stats;
    bool cancelled = false;
};

struct ChannelDecodeOpts {
    double sampleRateOverride = 0;
    bool lsbFirst = true;
    CancelFn shouldStop;
    /** onProgress(chunkDone, chunkTotal, packetCount, samples) */
    std::function<void(uint64_t, uint64_t, uint64_t, uint64_t)> onProgress;
};

/** 解码 `.atkcc` 的某个通道，产出与 POWER-Z 路径同形的 `{packets, stats}`。 */
DecodeOutcome decodeChannel(const AtkccCapture& cap, int channel, const ChannelDecodeOpts& opts);

}  // namespace pdscope
