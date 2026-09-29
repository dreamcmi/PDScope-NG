#include "powerz.h"

#include "pipeline.h"
#include "sqlite_reader.h"

#include "../bmc.h"
#include "../pd/decoder.h"
#include "../ufcs/decoder.h"
#include "../ufcs/frame.h"

#include <algorithm>
#include <cmath>

namespace pdscope {
namespace {

PowerzKindInfo kPdInfo{"pd_table", "pd_chart", "USB PD", "CC1", "CC2", "POWER-Z · USB PD"};
PowerzKindInfo kUfcsInfo{"ufcs_table", "ufcs_chart", "UFCS", "DP", "DM", "POWER-Z · UFCS"};

inline uint8_t byteAt(const uint8_t* p, size_t len, size_t i) {
    return (i < len) ? p[i] : 0;
}

/** Uint8Array / ArrayBuffer / 其它 → 有字节就 true。 */
bool isBytes(const SqlValue& v) { return v.isBlob(); }

}  // namespace

const PowerzKindInfo& powerzKindInfo(PowerzKind k) {
    return k == PowerzKind::Pd ? kPdInfo : kUfcsInfo;
}

std::optional<PowerzKind> sniffPowerz(const Bytes& bytes) {
    if (!SqliteReader::isSqlite(bytes)) return std::nullopt;
    try {
        SqliteReader db(bytes);
        if (db.hasTable("pd_table")) return PowerzKind::Pd;
        if (db.hasTable("ufcs_table")) return PowerzKind::Ufcs;
    } catch (const Error&) {
        // 魔数对但不是我们认识的库（例如别的 App 的 sqlite）—— 当作不支持
    }
    return std::nullopt;
}

/* ────────────────────────── PD 的 Raw blob ────────────────────────── */

PowerzBlob parsePowerzBlob(const uint8_t* blob, size_t len) {
    PowerzBlob out;
    size_t off = 0;

    while (off < len) {
        const uint8_t marker = blob[off];

        if (marker == 0x45) {
            if (off + 6 > len) { out.truncated = true; break; }
            const long long ts = static_cast<long long>(blob[off + 1])
                               | (static_cast<long long>(blob[off + 2]) << 8)
                               | (static_cast<long long>(blob[off + 3]) << 16);
            const uint8_t code = blob[off + 5];
            PowerzEvent ev;
            if (code == 0x12) ev.kind = "disconnect";
            else if (code == 0x11) ev.kind = "connect";
            else ev.kind = "event:0x" + hexU(code, 2);
            ev.tsMs = ts;
            ev.code = code;
            out.events.push_back(std::move(ev));
            off += 6;
            continue;
        }

        if (marker >= 0x80 && marker <= 0xBF) {
            const int total = marker & 0x3F;
            if (total < 5) { out.truncated = true; break; }
            const size_t wireLen = static_cast<size_t>(total - 5);
            const size_t size = static_cast<size_t>(total) + 1;
            if (off + size > len) { out.truncated = true; break; }
            PowerzEvent ev;
            ev.kind = "msg";
            ev.tsMs = static_cast<long long>(rdU32LE(blob + off + 1));
            ev.sopByte = blob[off + 5];
            ev.wire.assign(blob + off + 6, blob + off + 6 + wireLen);
            out.events.push_back(std::move(ev));
            off += size;
            continue;
        }

        // 既不是连接事件也不是包裹报文 —— 不是我们认识的 blob，停下
        out.truncated = true;
        break;
    }
    return out;
}

bool isPowerzConnectBlob(const uint8_t* blob, size_t len) {
    return len == 6 && blob[0] == 0x45 && blob[3] == 0x00
        && (blob[5] == 0x11 || blob[5] == 0x12);
}

std::optional<PowerzEvent> parsePowerzConnectBlob(const uint8_t* blob, size_t len) {
    if (len < 6) return std::nullopt;
    PowerzEvent ev;
    ev.tsMs = static_cast<long long>(blob[1]) | (static_cast<long long>(blob[2]) << 8)
            | (static_cast<long long>(blob[3]) << 16);
    ev.kind = (blob[5] == 0x12) ? "disconnect" : "connect";
    ev.code = blob[5];
    return ev;
}

/* ────────────────────────── UFCS 的 Raw blob ──────────────────────── */

UfcsBlob parseUfcsBlob(const uint8_t* blob, size_t len) {
    UfcsBlob out;

    // ── ① 已知布局：链路直接来自容器，不再推断 ──
    if (auto rec = ufcs::ufcsParseRecord(blob, len)) {
        for (const auto& f : rec->frames) {
            UfcsBlobFrame fr;
            fr.bytes.assign(blob + f.off, blob + f.bodyEnd);
            fr.crc = f.crc;
            fr.calc = f.calc;
            fr.crcOk = f.crcOk ? CrcState::Ok : CrcState::Bad;
            fr.withCrc = true;
            out.frames.push_back(std::move(fr));
        }
        out.truncated = false;
        out.prefixBytes = rec->prefixBytes;
        out.withCrc = true;
        // 物理链路字节在 blob[7]：
        // ⚠ 绝不能写成 blob[prefixBytes-1] —— 那读到的是恒为 0xAA 的 Training 字节，
        //   于是链路永远读不出、方向只能靠猜还不自知（读出来从不变化的字段，位置一定错了）。
        out.dirByte = (len > 7) ? static_cast<int>(blob[7]) : -1;
        out.lineHint = rec->line;
        out.hasLineHint = rec->hasLine;
        out.layout = "record";
        out.hasCounter = rec->hasCounter;
        out.counterX0 = rec->counterX0;
        out.counterX1 = rec->counterX1;
        out.hasLenField = rec->hasLenField;
        out.lenField = rec->lenField;
        out.hasTs = rec->hasTs;
        out.tsMs = static_cast<double>(rec->tsMs);
        return out;
    }

    // ── ② 未知布局：穷举定位（拿不到链路，只能靠方向推断）──
    if (auto loc = ufcs::ufcsLocateFrames(blob, len)) {
        for (const auto& f : loc->frames) {
            UfcsBlobFrame fr;
            fr.bytes.assign(blob + f.off, blob + f.bodyEnd);
            fr.crc = loc->withCrc ? f.crc : 0;
            fr.calc = f.calc;
            fr.crcOk = loc->withCrc ? (f.crcOk ? CrcState::Ok : CrcState::Bad)
                                    : CrcState::Unrecorded;
            fr.withCrc = loc->withCrc;
            out.frames.push_back(std::move(fr));
        }
        out.truncated = false;
        out.prefixBytes = loc->prefixBytes;
        out.withCrc = loc->withCrc;
        out.dirByte = -1;
        out.hasLineHint = false;
        out.layout = "scan";
        return out;
    }

    out.truncated = true;
    out.layout = "scan";
    return out;
}

/* ────────────────────────── 抓包对象 ────────────────────────── */

PowerzCapture::PowerzCapture(std::shared_ptr<RowSource> source, PowerzKind kind, uint64_t fileBytes)
    : source_(std::move(source)), kind_(kind) {
    meta_.kind = (kind_ == PowerzKind::Pd) ? "pd" : "ufcs";
    meta_.fileBytes = fileBytes;
    buildMeta();
}

void PowerzCapture::buildMeta() {
    const PowerzKindInfo& info = powerzKindInfo(kind_);
    const bool sqlite = source_->isSqlite();

    meta_.title = info.title;
    meta_.protocol = info.protocol;
    meta_.tableRows = source_->hasTable(info.table) ? source_->count(info.table) : 0;
    meta_.chartRows = source_->hasTable(info.chart) ? source_->count(info.chart) : 0;

    // ── 模拟量序列 → 与 .atkcc 的 bus.ini 同形的 {sample, vbus, ibus, a, b} ──
    uint64_t tMax = 0;
    if (meta_.chartRows) {
        std::vector<PowerzBusPoint> bus;
        bus.reserve(static_cast<size_t>(std::min<uint64_t>(meta_.chartRows, 1u << 22)));
        source_->forEachRow(info.chart, [&](const SqlRow& r) {
            if (r.size() < 3) return true;
            const double t = r[0].asDouble();
            if (!std::isfinite(t)) return true;
            PowerzBusPoint p;
            p.t = t;
            p.sample = static_cast<uint64_t>(std::llround(t * 1000.0));
            p.vbus = r[1].asDouble();
            p.ibus = r[2].asDouble();
            p.a = (r.size() > 3) ? r[3].asDouble() : 0.0;
            p.b = (r.size() > 4) ? r[4].asDouble() : 0.0;
            if (p.sample > tMax) tMax = p.sample;
            bus.push_back(p);
            return true;
        });
        std::stable_sort(bus.begin(), bus.end(),
                         [](const PowerzBusPoint& x, const PowerzBusPoint& y) {
                             return x.sample < y.sample;
                         });
        meta_.bus = std::move(bus);
    }

    meta_.busLabelA = info.labelA;
    meta_.busLabelB = info.labelB;

    meta_.sampleRate = kPowerzRate;
    meta_.sampleRateSource = "powerz";
    meta_.sampleRateNote = "POWER-Z 导出的是毫秒时间戳，没有采样点波形；时间轴按「1 采样点 = 1 ms」映射";
    meta_.totalSamples = tMax;
    meta_.durationSec = static_cast<double>(tMax) / kPowerzRate;

    if (sqlite) {
        meta_.isSqlite = true;
        meta_.pageSize = source_->pageSize();
        meta_.pageCount = source_->pageCount();
        meta_.textEncoding = source_->textEncoding();
        meta_.writeVersion = source_->writeVersion();
    } else {
        meta_.isSqlite = false;
    }
}

PowerzResult PowerzCapture::decode(const CancelFn& shouldCancel,
                                   const ProgressFn& onProgress) const {
    const PowerzKindInfo& info = powerzKindInfo(kind_);
    PowerzResult result;

    // ── 1. 取全部行并按时间排序（导出文件一般已有序，这里不依赖它）──
    struct Row { double t; double vbus; double ibus; std::vector<uint8_t> raw; bool hasRaw; };
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(std::min<uint64_t>(meta_.tableRows + 64, 1u << 22)));

    const uint64_t tableRows = meta_.tableRows;
    uint64_t i = 0;
    source_->forEachRow(info.table, [&](const SqlRow& r) {
        Row row;
        row.t = r.size() > 0 ? r[0].asDouble() : 0.0;
        row.vbus = r.size() > 1 ? r[1].asDouble() : 0.0;
        row.ibus = r.size() > 2 ? r[2].asDouble() : 0.0;
        row.hasRaw = (r.size() > 3) && isBytes(r[3]);
        if (row.hasRaw) row.raw = r[3].b;
        rows.push_back(std::move(row));

        ++i;
        if ((i & 2047) == 2047) {
            if (onProgress) onProgress(1, i, tableRows, 0);
            if (shouldCancel && shouldCancel()) return false;
        }
        return true;
    });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.t < b.t; });

    // ── 2. 拆事件 / 拆报文 ──
    std::vector<EventRecord> events;
    std::vector<std::pair<std::string, long long>> eventDetail;   // (kind, tsMs) 供 stats.events
    uint64_t truncated = 0;
    uint64_t frameCount = 0;
    uint64_t unlocated = 0;

    struct RawEvent {
        std::string kind;
        long long tsMs = 0;
        int sopByte = 0;
        std::vector<uint8_t> wire;
        int code = 0;
        double rowT = 0;
        double vbus = 0;
        double ibus = 0;
    };
    std::vector<RawEvent> msgEvents;

    struct UfcsPending { UfcsBlob blob; double t; };
    std::vector<UfcsPending> ufcsBlobs;

    if (kind_ == PowerzKind::Pd) {
        for (const Row& row : rows) {
            if (!row.hasRaw) continue;
            PowerzBlob r = parsePowerzBlob(row.raw.data(), row.raw.size());
            for (const PowerzEvent& ev : r.events) {
                RawEvent e;
                e.kind = ev.kind;
                e.tsMs = ev.tsMs;
                e.sopByte = ev.sopByte;
                e.wire = ev.wire;
                e.code = ev.code;
                e.rowT = row.t;
                e.vbus = row.vbus;
                e.ibus = row.ibus;
                if (ev.kind == "msg") msgEvents.push_back(std::move(e));
                else {
                    events.push_back(EventRecord{ev.kind, ev.tsMs, ev.code});
                    eventDetail.emplace_back(ev.kind, ev.tsMs);
                }
            }
            if (r.truncated) truncated++;
        }
    } else {
        for (const Row& row : rows) {
            if (!row.hasRaw) continue;
            UfcsBlob r = parseUfcsBlob(row.raw.data(), row.raw.size());
            if (r.frames.empty()) {
                // 先看它是不是「状态事件」行（UFCS 自己的 8 字节格式），
                // 也兼容 PD 那套 `45 … 00 code`
                if (auto ev = ufcs::ufcsParseEvent(row.raw.data(), row.raw.size())) {
                    events.push_back(EventRecord{"ufcs-event", ev->tsMs, ev->code});
                    eventDetail.emplace_back("ufcs-event", ev->tsMs);
                } else if (isPowerzConnectBlob(row.raw.data(), row.raw.size())) {
                    if (auto c = parsePowerzConnectBlob(row.raw.data(), row.raw.size())) {
                        events.push_back(EventRecord{c->kind, c->tsMs, c->code});
                        eventDetail.emplace_back(c->kind, c->tsMs);
                    }
                } else {
                    unlocated++;
                }
                continue;
            }
            frameCount += r.frames.size();
            ufcsBlobs.push_back(UfcsPending{std::move(r), row.t});
        }
    }

    uint64_t connects = 0, disconnects = 0, ufcsEvents = 0;
    std::map<int, uint64_t> ufcsEventCodes;
    for (const EventRecord& e : events) {
        if (e.kind == "connect") connects++;
        else if (e.kind == "disconnect") disconnects++;
        else if (e.kind == "ufcs-event") { ufcsEvents++; ufcsEventCodes[e.code]++; }
    }

    // ── 3. 逐条解报文 ──
    std::vector<Packet> packets;
    uint64_t badWire = 0;

    if (kind_ == PowerzKind::Pd) {
        pd::PdDecoder dec(kPowerzRate);
        const uint64_t total = msgEvents.size();
        for (uint64_t k = 0; k < total; ++k) {
            const RawEvent& ev = msgEvents[static_cast<size_t>(k)];
            const std::vector<uint8_t>& wire = ev.wire;

            if (wire.size() >= 2) {
                const int hdr = wire[0] | (wire[1] << 8);
                if (2 + 4 * ((hdr >> 12) & 7) != static_cast<int>(wire.size())) badWire++;
            } else {
                badWire++;
            }

            const char* sopName = "SOP";
            if (ev.sopByte == 1) sopName = "SOP'";
            else if (ev.sopByte == 2) sopName = "SOP''";

            if (auto pkt = dec.decodeWire(wire.data(), wire.size(), sopName,
                                          static_cast<double>(ev.tsMs), 0,
                                          /*crcRecorded=*/false, ev.sopByte, /*powerz=*/true)) {
                // 注意：**不**从 pd_table 那行的 Vbus/Ibus 取值。
                // 界面与 CSV 的 VBUS/IBUS 两列一律来自 ADC 采样序列（busAt 按时间取最近邻，
                // 见 session.cpp 的 attachBusValues），所以 .pdStream（没有 pd_chart）
                // 导出的这两列是空的 —— 那是容器里确实没有波形，不是解析漏了。
                packets.push_back(std::move(*pkt));
            }

            if ((k & 255) == 255) {
                if (onProgress) onProgress(2, k + 1, total, packets.size());
                if (shouldCancel && shouldCancel()) break;
            }
        }
    } else {
        ufcs::UfcsDecoder dec(kPowerzRate);
        const uint64_t totalFrames = frameCount ? frameCount : 1;
        uint64_t k = 0;
        bool cancelled = false;
        for (const UfcsPending& b : ufcsBlobs) {
            for (const UfcsBlobFrame& f : b.blob.frames) {
                ufcs::DecodeOpts o;
                o.crc = f.crc;
                o.hasCrc = f.withCrc;
                o.crcCalc = f.calc;
                o.withCrc = f.withCrc;
                o.timeMs = std::round(b.t * 1000.0);
                o.line = b.blob.hasLineHint ? b.blob.lineHint : std::string();
                o.dirByte = b.blob.dirByte;
                o.prefixBytes = b.blob.prefixBytes;
                o.counterX0 = b.blob.counterX0;
                o.counterX1 = b.blob.counterX1;
                o.hasCounter = b.blob.hasCounter;
                o.lenField = b.blob.lenField;
                o.hasLenField = b.blob.hasLenField;
                o.training = (b.blob.prefixBytes > 0 && b.blob.layout == "record");
                o.layout = b.blob.layout;
                o.channel = 0;

                if (auto pkt = dec.decode(f.bytes.data(), f.bytes.size(), o)) {
                    packets.push_back(std::move(*pkt));
                }
                ++k;
                if ((k & 255) == 0) {
                    if (onProgress) onProgress(2, k, totalFrames, packets.size());
                    if (shouldCancel && shouldCancel()) { cancelled = true; break; }
                }
            }
            if (cancelled) break;
        }
    }

    // ── 序号 + 时间排序（时间戳可能并列，保持解析顺序）──
    std::stable_sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) {
        if (a.startSample != b.startSample) return a.startSample < b.startSample;
        return a.seq < b.seq;
    });
    for (size_t idx = 0; idx < packets.size(); ++idx) packets[idx].index = idx;

    if (kind_ == PowerzKind::Ufcs) ufcs::ufcsLinkAck(packets);
    else linkGoodCrc(packets);

    uint64_t totalSamples = meta_.totalSamples;
    if (!packets.empty()) {
        totalSamples = std::max(totalSamples, packets.back().endSample);
    }

    DecodeStats st;
    st.channel = 0;
    st.source = "powerz";
    st.kind = meta_.kind;
    st.protocol = info.protocol;
    st.totalSamples = totalSamples;
    st.durationSec = static_cast<double>(totalSamples) / kPowerzRate;
    st.sampleRate = kPowerzRate;
    st.sampleRateSource = "powerz";
    st.packetCount = packets.size();
    for (const Packet& p : packets) {
        if (p.crcOk == CrcState::Bad) st.badCrc++;
        if (p.crcOk == CrcState::Unrecorded) st.crcUnknown++;
        st.warnings += p.warnings.size();
    }
    // PD 路径分析仪不存 CRC，全部报文都无从判定 —— 不是「零错误」
    if (kind_ == PowerzKind::Pd) st.crcUnknown = packets.size();
    st.badWire = badWire;
    st.truncatedRows = truncated;
    st.connectCount = connects;
    st.disconnectCount = disconnects;
    st.ufcsEvents = (kind_ == PowerzKind::Ufcs) ? ufcsEvents : 0;
    if (kind_ == PowerzKind::Ufcs) {
        for (const auto& kv : ufcsEventCodes) st.ufcsEventCodes.emplace_back(kv.first, kv.second);
        st.ufcsFrames = frameCount;
        st.ufcsUnlocatedRows = unlocated;
        for (const Packet& p : packets) {
            if (p.roleInferred) st.ufcsDirInferred++;
            else st.ufcsDirFromLine++;
        }
    }
    for (const auto& e : eventDetail) st.events.push_back(EventRecord{e.first, e.second, 0});
    st.tableRows = rows.size();
    st.chartRows = meta_.chartRows;

    result.packets = std::move(packets);
    result.stats = st;
    return result;
}

}  // namespace pdscope
