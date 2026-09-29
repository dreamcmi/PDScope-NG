// decode_stats.h — 解码统计口径（三条来源共用）
//
// 界面顶栏 chip、CSV 导出末尾的「该说的话」、自检断言都读这里。
// 这里是**对外口径**：字段含义一旦定下就是 ABI 的一部分，改名要同步 doc/abi.md。
#pragma once

#include "util.h"

#include <functional>

namespace pdscope {

/** 取消回调：返回 true 表示调用方要求停止。 */
using CancelFn = std::function<bool()>;

/**
 * 进度回调。
 * @param phase   0=空闲 1=读容器 2=解码
 * @param done    已完成量
 * @param total   总量（0 表示未知）
 * @param packets 已解出的报文数
 */
using ProgressFn = std::function<void(int phase, uint64_t done, uint64_t total, uint64_t packets)>;

struct EventRecord {
    std::string kind;    // connect | disconnect | ufcs-event | msg | event:0x..
    long long tsMs = 0;
    int code = 0;
};

/** 多通道 .atkcc 自动挑通道的结果（挑错了整份 CSV 都会是空的，必须说清挑的是哪条）。 */
struct ChannelPick {
    int picked = 0;
    int noiseRejected = 0;
    bool allNoisy = false;
};

struct DecodeStats {
    int channel = 0;
    std::string source;        // atkcc | powerz
    std::string kind;          // pd | ufcs（仅 powerz）
    std::string protocol;      // "USB PD" | "UFCS"
    std::string unsupported;

    uint64_t totalSamples = 0;
    double durationSec = 0;
    double sampleRate = 0;
    std::string sampleRateSource;    // declared | measured | default | override | powerz
    std::string sampleRateNote;
    bool hasSampleRateNote = false;
    double sampleRateDeclared = 0;
    double sampleRateMeasured = 0;
    bool hasSampleRateMeasured = false;

    uint64_t edges = 0;
    uint64_t trimmedBytes = 0;
    uint64_t packetCount = 0;
    uint64_t badCrc = 0;
    /** CRC 未被记录的报文数。POWER-Z 的 PD 报文全属此类 —— 不是「零错误」。 */
    uint64_t crcUnknown = 0;
    uint64_t warnings = 0;
    uint64_t badWire = 0;
    uint64_t truncatedRows = 0;

    uint64_t connectCount = 0;
    uint64_t disconnectCount = 0;

    uint64_t ufcsEvents = 0;
    std::vector<std::pair<int, uint64_t>> ufcsEventCodes;
    uint64_t unsupportedMsgs = 0;
    uint64_t ufcsFrames = 0;
    uint64_t ufcsUnlocatedRows = 0;
    uint64_t ufcsDirFromLine = 0;
    uint64_t ufcsDirInferred = 0;

    std::vector<EventRecord> events;

    uint64_t tableRows = 0;
    uint64_t chartRows = 0;

    bool hasChannelPick = false;
    ChannelPick channelPick;
};

}  // namespace pdscope
