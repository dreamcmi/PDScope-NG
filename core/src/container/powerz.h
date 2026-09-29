// powerz.h — POWER-Z（ChargerLAB KM 系列）SQLite / .pdStream 抓包解析
//
// 两种导出：
//   pd_chart(Time, VBUS, IBUS, CC1, CC2) / pd_table(Time, Vbus, Ibus, Raw) / pd_table_key
//   ufcs_chart(Time, VBUS, IBUS, DP, DM) / ufcs_table(...) / ufcs_table_key
// 识别方式就是「有没有 pd_table / ufcs_table」。
//
// 报文已经是逻辑字节：PD 交给 `pd::PdDecoder::decodeWire()`，UFCS 交给
// `ufcs::UfcsDecoder::decode()` —— 与 .atkcc 路径共用语义实现，不抄第二份。
//
// 时间基准：POWER-Z 只有毫秒时间戳，统一按 **1 采样点 = 1 ms** 映射，
// 于是 `totalSamples / sampleRate` 仍是秒、`startSample` 仍是时间轴坐标。
#pragma once

#include "decode_stats.h"
#include "packet.h"
#include "rowsource.h"

#include <functional>
#include <memory>

namespace pdscope {

constexpr double kPowerzRate = 1000.0;   // 1 采样点 = 1 ms

enum class PowerzKind { Pd, Ufcs };

struct PowerzKindInfo {
    const char* table;
    const char* chart;
    const char* protocol;
    const char* labelA;    // 第 3 路模拟量（PD=CC1 / UFCS=DP）
    const char* labelB;    // 第 4 路模拟量（PD=CC2 / UFCS=DM）
    const char* title;
};

const PowerzKindInfo& powerzKindInfo(PowerzKind k);

/** 只看文件头与 sqlite_master 判断是不是 POWER-Z 导出；不做全表扫描。 */
std::optional<PowerzKind> sniffPowerz(const Bytes& bytes);

/* ── PD 的 Raw blob → 事件 ───────────────────────────────────────── */

struct PowerzEvent {
    std::string kind;      // connect | disconnect | msg | event:0x..
    long long tsMs = 0;
    int sopByte = 0;
    std::vector<uint8_t> wire;
    int code = 0;
};

struct PowerzBlob {
    std::vector<PowerzEvent> events;
    bool truncated = false;
};

/** 把一个 Raw blob 拆成事件序列；拼不通就停下并如实标记，不硬猜长度。 */
PowerzBlob parsePowerzBlob(const uint8_t* blob, size_t len);

/** `45 │ ts(3B 小端) │ 00 │ code`（code 0x11=连接 0x12=断开）。 */
bool isPowerzConnectBlob(const uint8_t* blob, size_t len);
std::optional<PowerzEvent> parsePowerzConnectBlob(const uint8_t* blob, size_t len);

/* ── UFCS 的 Raw blob → 报文 ─────────────────────────────────────── */

struct UfcsBlobFrame {
    std::vector<uint8_t> bytes;   // 消息头 + 消息主体（不含 CRC）
    uint32_t crc = 0;
    uint32_t calc = 0;
    CrcState crcOk = CrcState::Unrecorded;
    bool withCrc = false;
};

struct UfcsBlob {
    std::vector<UfcsBlobFrame> frames;
    bool truncated = false;
    size_t prefixBytes = 0;
    bool withCrc = false;
    int dirByte = -1;             // 容器给出的物理链路字节（-1 = 没有）
    std::string lineHint;         // "D+" / "D-"
    bool hasLineHint = false;
    std::string layout;           // record | scan
    bool hasCounter = false;
    long long counterX0 = 0, counterX1 = 0;
    bool hasLenField = false;
    long long lenField = 0;
    bool hasTs = false;
    double tsMs = 0;
};

/**
 * 拆一个 UFCS Raw blob：先试已知的分析仪布局（能拿到物理链路），
 * 认不出才落回穷举定位（拿不到链路 ⇒ 方向只能靠推断）。
 */
UfcsBlob parseUfcsBlob(const uint8_t* blob, size_t len);

/* ── 抓包对象 ───────────────────────────────────────────────────── */

struct PowerzBusPoint {
    uint64_t sample = 0;
    double t = 0;
    double vbus = 0;
    double ibus = 0;
    double a = 0;   // CC1 / DP
    double b = 0;   // CC2 / DM
};

struct PowerzMeta {
    std::string source = "powerz";
    std::string kind;                 // pd | ufcs
    std::string title;
    std::string protocol;
    std::string unsupported;

    double sampleRate = kPowerzRate;
    std::string sampleRateSource = "powerz";
    std::string sampleRateNote;

    uint64_t totalSamples = 0;
    double durationSec = 0;

    std::vector<PowerzBusPoint> bus;
    std::string busLabelA, busLabelB;

    uint64_t tableRows = 0;
    uint64_t chartRows = 0;
    uint64_t fileBytes = 0;

    bool isSqlite = true;
    int pageSize = 0;
    int pageCount = 0;
    int textEncoding = 0;
    int writeVersion = 0;
};

struct PowerzResult {
    std::vector<Packet> packets;
    DecodeStats stats;
};

/**
 * POWER-Z 抓包。`source` 提供事件行 —— `.sqlite` 下是 SqliteReader，
 * `.pdStream` 下是一个把二进制记录流包装成只读虚拟表的实现。
 */
class PowerzCapture {
public:
    PowerzCapture(std::shared_ptr<RowSource> source, PowerzKind kind, uint64_t fileBytes);

    const PowerzMeta& meta() const { return meta_; }
    /** 仅供 `.pdStream` 包装器修正元数据（没有 pd_chart ⇒ 时间轴总长改由末条记录决定）。 */
    PowerzMeta& metaMutable() { return meta_; }
    PowerzKind kind() const { return kind_; }

    PowerzResult decode(const CancelFn& shouldCancel, const ProgressFn& onProgress) const;

private:
    void buildMeta();

    std::shared_ptr<RowSource> source_;
    PowerzKind kind_;
    PowerzMeta meta_;
};

}  // namespace pdscope
