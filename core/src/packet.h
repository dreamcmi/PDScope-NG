// packet.h — 统一报文模型 + BMC 原始包
//
// 这是「三条来源解出来的报文同形」这一约定的落地点：
//   .atkcc（BMC→4B5B→PD）／POWER-Z pd_table（逻辑字节→PD）／POWER-Z ufcs_table（UFCS）
// 解出的都是本文件的 `Packet`，于是界面、筛选、详情、时间轴、导出只有
// 「读文件时分不分流」与「协议相关那几处」分叉。
#pragma once

#include "util.h"

namespace pdscope {

/** BMC 状态机产出的一条「原始包」（比特序列 + 边沿位置）。 */
struct BmcRawPacket {
    std::vector<uint8_t> bits;    // 每元素 0 / 1
    std::vector<uint64_t> edges;
    uint64_t startSample = 0;
    uint64_t endSample = 0;
    std::vector<std::pair<uint64_t, uint64_t>> bad;   // 非法序列的 [起点,终点]
    uint64_t bitrate = 0;
    bool synthetic = false;       // true = 由 decodeWire 从逻辑字节铺出来的，不是从波形解出的
    std::vector<uint8_t> wireBytes;
};

/** 详情面板的一行。`key == "Object"` 是分组标题哨兵。 */
struct DetailItem {
    std::string key;
    std::string value;
};

struct PacketWarning {
    std::string longMsg;    // 长文本（详情面板）
    std::string shortMsg;   // 短标记（表格角标），如 TRUNC / CRC / EOP
};

/**
 * CRC 口径是**三态**，不是布尔：
 *   Unrecorded —— 容器里根本没存 CRC（POWER-Z 的 PD 报文就是这种）。
 *                 绝不能因为「重算对上了」就报通过 —— 那是替分析仪的数据背书。
 *   Ok / Bad   —— 确实记录并校验过。
 */
enum class CrcState { Unrecorded = 0, Ok = 1, Bad = 2 };

/** 一条解码完成的报文。 */
struct Packet {
    // ── 身份 ──
    uint64_t seq = 0;            // 解析顺序（时间并列时用它保持稳定）
    uint64_t index = 0;          // 视图序号（排序后分配）
    int channel = 0;

    // ── 协议 ──
    std::string sop;             // SOP / SOP' / SOP'' / Hard Reset / Cable Reset
    std::string msgType;         // GoodCRC / Source_Capabilities / …
    int msgTypeRaw = -1;
    bool hasMsgTypeRaw = false;
    std::string msgKind;         // control | data | ext | special
    std::string role;            // PD: SRC / SNK / Plug      UFCS: 见 lib-ufcs
    bool roleInferred = false;   // 方向是推断出来的（没有硬依据）

    // ── 报文头 ──
    int header = -1;
    bool hasHeader = false;
    int extHeader = -1;
    bool hasExtHeader = false;
    int msgId = -1;
    bool hasMsgId = false;
    int rev = -1;
    bool hasRev = false;
    std::string revText;
    int powerRole = -1;
    int dataRole = -1;
    std::string link;            // port | cable
    int nObjects = 0;            // PD：数据对象个数
    int dataLen = 0;             // UFCS：数据字节数（包头里本来就是长度域）

    // ── CRC ──
    uint32_t crc = 0;
    bool hasCrc = false;
    uint32_t crcCalc = 0;
    CrcState crcOk = CrcState::Unrecorded;
    bool crcRecorded = false;
    bool eop = false;

    std::string category;        // control | data | ext（供筛选用）
    std::string summary;

    // ── 时间 ──
    uint64_t startSample = 0;
    uint64_t endSample = 0;
    double timeMs = 0;
    double endTimeMs = 0;
    double durationUs = 0;
    uint64_t bitrate = 0;
    bool bitrateNominal = false;   // 码率取自协议标称值而非实测

    // ── 该时刻的模拟量（由调用方按「最近邻」挂上；没有就保持 0）──
    // ⚠ 一律来自 ADC 采样序列（.atkcc 的 bus.ini / POWER-Z 的 *_chart），
    //   绝不用 pd_table 那一行的 Vbus/Ibus 去顶替 —— 那是另一个口径。
    //   .pdStream 没有 ADC 波形，于是这两项恒为 0（CSV 里也留 0，不能凭空编）。
    double vbus = 0;
    double ibus = 0;

    // ── 载荷 ──
    std::vector<uint32_t> dataWords;
    std::vector<uint8_t> dataBytes;
    std::string dataHex;

    // ── 展示 ──
    std::vector<DetailItem> details;
    std::vector<PacketWarning> warnings;
    std::string text;

    // ── 来源标记 ──
    bool synthetic = false;      // 报文不是从采样波形解出来的
    int sopByte = -1;
    bool powerz = false;

    // ── GoodCRC 配对（PD）／ACK 配对（UFCS）──
    bool hasAck = false;
    uint64_t ackOf = 0;
    std::string ackType;

    /* 便捷判定 */
    bool isGoodCrc() const { return msgType == "GoodCRC"; }
    bool isBadCrc() const { return crcOk == CrcState::Bad; }
};

/** 详情发射器：object() / detail() / note() 三个语义。 */
struct DetailEmitter {
    std::vector<DetailItem>* details = nullptr;
    std::vector<std::string>* summaryParts = nullptr;

    void object(const std::string& title) {
        if (details) details->push_back({"Object", title});
    }
    void detail(const std::string& key, const std::string& value) {
        if (details) details->push_back({key, value});
    }
    void note(const std::string& text) {
        if (summaryParts && !text.empty()) summaryParts->push_back(text);
    }
};

}  // namespace pdscope
