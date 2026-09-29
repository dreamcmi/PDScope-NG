// bmc.h — USB PD 的 BMC（Biphase Mark Coding）解码
//
// 与 sigrok `usb_power_delivery` 的语义对齐：
//   UI        = 1 / 600kHz = 1.6667 µs
//   threshold = 1.5 × UI   = 2.5 µs   （区分「半位 1」与「位 0」）
//   maxbit    = 3 × UI     = 5.0 µs   （超过即视为空闲，包结束）
//
// 600 kHz 是 PD 协议规定的，与抓包设备的采样率无关；threshold / maxbit 都是
// **按采样率换算出来的采样点数**，不是写死的常数。
#pragma once

#include "packet.h"

#include <functional>

namespace pdscope {

constexpr double kBmcHz = 600000.0;
constexpr double kUiUs = 1000000.0 / kBmcHz;
constexpr double kThresholdUs = (kUiUs + 2.0 * kUiUs) / 2.0;
constexpr double kMaxbitUs = 3.0 * kUiUs;

/**
 * 单包允许累积的最大位数。超过就判「根本不是 PD 报文」并就地丢弃。
 *
 * 为什么必须有这道闸：`pushEdge` 只在「长时间空闲」或收尾时才收包，于是遇到
 * 噪声数据（电平每个采样点都跳变）时 `bits` 会一路涨到几千万位；收尾那一次
 * `decode()` 会对着这个巨型数组做同步的 SOP 扫描 —— 界面彻底卡死
 * （实测一份 18 KB 的噪声样本能让主线程连续阻塞 6.3 秒）。
 *
 * 8192 位对合法报文有 3 倍余量（PD 3.1 最长扩展报文 260 字节 → 4B5B 后 2600 位，
 * 加 SOP/EOP 约 2625 位），不会误伤。
 */
constexpr size_t kMaxPacketBits = 8192;

/**
 * 从字节流提取电平游程并转成「边沿采样点」序列。
 * 每字节 8 个采样点，默认 LSB 优先（bit0 时间最早）。
 */
class EdgeExtractor {
public:
    explicit EdgeExtractor(bool lsbFirst = true) : lsbFirst_(lsbFirst) {}

    void push(const uint8_t* data, size_t len, uint64_t baseSample,
              const std::function<void(uint64_t)>& onEdge);

    /** 收尾：补一个虚拟边沿，让最后一个包也能被刷新出来。 */
    void flush(const std::function<void(uint64_t)>& onEdge, uint64_t padSamples) const {
        onEdge(sample_ + padSamples);
    }

    uint64_t sample() const { return sample_; }

private:
    bool lsbFirst_;
    int level_ = -1;            // 当前电平，-1 表示尚未确定
    uint64_t runStart_ = 0;
    uint64_t sample_ = 0;
};

/** PD 包的 BMC 状态机：边沿进，包出。 */
class BmcDecoder {
public:
    BmcDecoder(double sampleRate, int minEdges = 50);

    void reset();

    /** 送入一个边沿；收完一包时返回该包。 */
    std::optional<BmcRawPacket> pushEdge(uint64_t sample, bool flush = false);

    uint64_t threshold() const { return threshold_; }
    uint64_t maxbit() const { return maxbit_; }

private:
    void restart(uint64_t sample, bool flush);
    std::optional<BmcRawPacket> emit();

    double sampleRate_;
    int minEdges_;
    uint64_t threshold_;
    uint64_t maxbit_;

    bool hasStart_ = false;
    uint64_t startsample_ = 0;
    uint64_t previous_ = 0;
    std::vector<uint8_t> bits_;
    std::vector<uint64_t> edges_;
    std::vector<std::pair<uint64_t, uint64_t>> bad_;
    bool halfOne_ = false;
    uint64_t startOne_ = 0;
};

/* ── 采样率反推（波形自检）── */

/** 收集采样字节流里的游程长度（相邻同电平采样点个数）。 */
std::vector<uint32_t> collectRunStats(const uint8_t* data, size_t len,
                                      uint32_t maxRun = 64, size_t maxRuns = 40000);

struct UiEstimate {
    double uiSamples = 0;
    int nShort = 0;
    int nLong = 0;
    double confidence = 0;
    int used = 0;
};

/** 由游程分布反推「1 UI = 多少采样点」。 */
std::optional<UiEstimate> estimateUiSamples(const std::vector<uint32_t>& runs);

struct SampleRateEstimate {
    double sampleRate = 0;
    double uiSamples = 0;
    double confidence = 0;
    int nShort = 0;
    int nLong = 0;
};

/** 从波形反推采样率（Hz）。 */
std::optional<SampleRateEstimate> estimateSampleRate(const std::vector<uint32_t>& runs);

}  // namespace pdscope
