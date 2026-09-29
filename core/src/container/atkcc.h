// atkcc.h — 正点原子 ATK-C（`.atkcc`）容器
//
// `.atkcc` 就是一个 ZIP：
//   channel.ini       SamplingFrequency=2500     （单位 kHz，即数字采样率 2.5 MHz）
//   bus.ini           sample=N,vbus=1.234,ibus=0.567
//   0/channel.ini     第 1 行=通道组号，第 2 行=总采样点数
//   0/<ch>-<idx>.bin  通道 <ch> 的第 <idx> 块，每块固定 1 MiB（deflate）
//
// 位流约定：每采样点 1 bit，**LSB 优先**（bit0 时间最早）。
#pragma once

#include "util.h"
#include "zip.h"

namespace pdscope {

constexpr uint64_t kChunkSize = 1048576;              // 1 MiB
constexpr double kDefaultSampleRate = 2500000.0;      // 文件没声明、波形也认不出时的兜底

struct SampleRateInfo {
    double hz = kDefaultSampleRate;
    std::string source = "default";   // declared | default
    std::string key;
    double value = 0;
    std::string unit;
    std::string raw;
};

/** 从 ini 文本读采样率；兼容多种键名与单位（kHz / Hz / MHz）。 */
SampleRateInfo parseSampleRate(const std::string& text);

struct AtkccChunk {
    int idx = 0;
    std::string name;
    uint64_t size = 0;   // 解压后字节数（目录里声明）
};

struct AtkccChannel {
    int channel = 0;
    std::string folder;
    std::vector<AtkccChunk> chunks;   // 按 idx 升序
    uint64_t totalBytes = 0;
    uint64_t totalSamples = 0;
};

struct BusPoint {
    uint64_t sample = 0;
    double vbus = 0;
    double ibus = 0;
};

struct AtkccMeta {
    double sampleRate = kDefaultSampleRate;
    std::string sampleRateSource;   // declared | default
    std::string sampleRateKey;
    std::string sampleRateRaw;
    double samplingFrequencyRaw = 0;
    std::string rawChannelIni;
    uint64_t totalSamples = 0;
    std::vector<int> channelOrder;              // 升序
    std::map<int, AtkccChannel> channelMap;
    std::vector<BusPoint> bus;
    size_t entryCount = 0;
};

/** 通道活动度扫描结果（用于多通道自动挑通道）。 */
struct ChannelActivity {
    int channel = 0;
    uint64_t activity = 0;      // 非 0xFF 的字节数
    uint64_t bytes = 0;
    int scannedChunks = 0;
    double idleRatio = 0;       // 全高字节占比（真实 PD 通道很高）
    double liveRatio = 0;       // 全低字节占比
    double edgeLike = 0;        // 混合字节占比（噪声 ≈ 0.5）
};

class AtkccCapture {
public:
    /** 解析容器（只读目录与 ini，不解压采样数据）。 */
    static AtkccCapture open(const Bytes& bytes);

    const AtkccMeta& meta() const { return meta_; }

    /** 读取某通道某块（已解压）。越界返回 false。 */
    bool readChunk(int channel, size_t chunkIdx, Bytes& out) const;

    /**
     * 该通道真正有效的采样上限。
     *   单通道：取 min(channel.ini 的总采样数, 按块算出的总采样数)
     *   多通道：尾部 0x00 是补齐用的，交由分块级裁剪处理
     */
    uint64_t effectiveSampleLimit(int channel) const;

    bool isSingleChannel() const { return meta_.channelOrder.size() == 1; }

    double durationSec() const {
        return meta_.sampleRate > 0 ? static_cast<double>(meta_.totalSamples) / meta_.sampleRate : 0.0;
    }

    /** 裁掉尾部连续 0x00（多通道文件的补齐区）。返回有效字节数。 */
    static size_t trimTrailingZeros(const Bytes& data);

    /** 通道活动度扫描（只扫前 maxChunks 块）。 */
    ChannelActivity scanActivity(int channel, int maxChunks = 4) const;

    const std::string& warning() const { return warning_; }

private:
    // ⚠ 不在这里存 `Bytes` 成员：`AtkccCapture` 会被移动进 shared_ptr，
    //   而 ZipReader 内部需要长期持有文件字节。字节的所有权交给 ZipReader 自己的
    //   shared_ptr（见 atkcc.cpp 的 open()），这样容器怎么移动都不会出现悬垂引用。
    std::unique_ptr<ZipReader> zip_;
    AtkccMeta meta_;
    std::string warning_;
};

}  // namespace pdscope
