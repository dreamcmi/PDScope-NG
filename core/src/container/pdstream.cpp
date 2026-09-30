#include "pdstream.h"
#include "../ufcs/frame.h"

#include <algorithm>
#include <cmath>

namespace pdscope {
namespace {

constexpr int kMinRecords = 3;
constexpr uint64_t kSniffMaxRecords = 200000;

/** 物理量程兜底：PD 3.1 EPR 上限 48 V / 5 A，各留一倍余量。 */
constexpr double kMaxVolt = 120.0;
constexpr double kMaxAmp = 20.0;

}  // namespace

PdStreamParsed readPdStream(const Bytes& bytes) {
    auto fail = [](uint64_t off, const std::string& why) {
        failFormat("不是 .pdStream：偏移 0x" + hexU(off, 0) + " 处" + why);
    };

    if (bytes.size() < kPdStreamRecFixed * kMinRecords) {
        failFormat("不是 .pdStream：文件太短（" + std::to_string(bytes.size()) + " 字节）");
    }

    PdStreamParsed out;
    out.bytes = bytes.size();
    uint64_t off = 0;
    double lastTime = -std::numeric_limits<double>::infinity();

    while (off < bytes.size()) {
        if (off + kPdStreamRecFixed > bytes.size()) fail(off, "剩余字节装不下一条记录");
        const uint32_t len = rdU32BE(bytes.data() + off);
        if (len == 0) fail(off, "payload 长度为 0");
        if (len > kPdStreamMaxPayload) {
            fail(off, "payload 长度 " + std::to_string(len) + " 超出上限 "
                      + std::to_string(kPdStreamMaxPayload));
        }
        const uint64_t end = off + 4 + len + 24;
        if (end > bytes.size()) {
            fail(off, "payload 长度 " + std::to_string(len) + " 越过了文件末尾");
        }

        const double time = rdF64BE(bytes.data() + off + 4 + len);
        const double vbus = rdF64BE(bytes.data() + off + 12 + len);
        const double ibus = rdF64BE(bytes.data() + off + 20 + len);

        if (!std::isfinite(time) || time < 0) {
            fail(off, "Time 不是有限的非负数（" + numToStr(time) + "）");
        }
        if (time < lastTime) {
            fail(off, "Time 倒退（" + numToStr(time) + " < " + numToStr(lastTime) + "）");
        }
        if (!std::isfinite(vbus) || vbus < 0 || vbus > kMaxVolt) {
            fail(off, "Vbus 超出量程（" + numToStr(vbus) + "）");
        }
        if (!std::isfinite(ibus) || std::fabs(ibus) > kMaxAmp) {
            fail(off, "Ibus 超出量程（" + numToStr(ibus) + "）");
        }

        PdStreamRecord rec;
        rec.time = time;
        rec.vbus = vbus;
        rec.ibus = ibus;
        rec.raw.assign(bytes.data() + off + 4, bytes.data() + off + 4 + len);
        out.records.push_back(std::move(rec));
        out.payloadBytes += len;
        lastTime = time;
        off = end;
    }

    if (out.records.size() < kMinRecords) {
        fail(0, "只读出 " + std::to_string(out.records.size()) + " 条记录，不足以确认格式");
    }
    out.bytesConsumed = off;
    return out;
}

bool sniffPdStream(const Bytes& bytes) {
    if (bytes.size() < kPdStreamRecFixed * kMinRecords) return false;
    uint64_t off = 0;
    uint64_t n = 0;
    double lastTime = -std::numeric_limits<double>::infinity();
    while (off < bytes.size() && n < kSniffMaxRecords) {
        if (off + kPdStreamRecFixed > bytes.size()) return false;
        const uint32_t len = rdU32BE(bytes.data() + off);
        if (len == 0 || len > kPdStreamMaxPayload) return false;
        const uint64_t end = off + 4 + len + 24;
        if (end > bytes.size()) return false;
        const double time = rdF64BE(bytes.data() + off + 4 + len);
        const double vbus = rdF64BE(bytes.data() + off + 12 + len);
        const double ibus = rdF64BE(bytes.data() + off + 20 + len);
        if (!std::isfinite(time) || time < 0 || time < lastTime) return false;
        if (!std::isfinite(vbus) || vbus < 0 || vbus > kMaxVolt) return false;
        if (!std::isfinite(ibus) || std::fabs(ibus) > kMaxAmp) return false;
        lastTime = time;
        off = end;
        n++;
    }
    return n >= kMinRecords;
}

Bytes writePdStream(const std::vector<PdStreamRecord>& rows) {
    std::vector<PdStreamRecord> list = rows;
    std::stable_sort(list.begin(), list.end(),
                     [](const PdStreamRecord& a, const PdStreamRecord& b) { return a.time < b.time; });

    uint64_t total = 0;
    for (const auto& r : list) total += 4 + r.raw.size() + 24;

    Bytes out(static_cast<size_t>(total));
    uint64_t off = 0;
    for (const auto& r : list) {
        const uint32_t len = static_cast<uint32_t>(r.raw.size());
        out[off + 0] = static_cast<uint8_t>((len >> 24) & 0xFF);
        out[off + 1] = static_cast<uint8_t>((len >> 16) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[off + 3] = static_cast<uint8_t>(len & 0xFF);
        std::copy(r.raw.begin(), r.raw.end(), out.begin() + static_cast<ptrdiff_t>(off + 4));

        auto putF64BE = [&](uint64_t at, double v) {
            uint64_t bits;
            std::memcpy(&bits, &v, 8);
            for (int i = 0; i < 8; ++i) {
                out[at + static_cast<uint64_t>(i)] = static_cast<uint8_t>((bits >> (8 * (7 - i))) & 0xFF);
            }
        };
        putF64BE(off + 4 + len, r.time);
        putF64BE(off + 12 + len, r.vbus);
        putF64BE(off + 20 + len, r.ibus);
        off += 4 + len + 24;
    }
    return out;
}

uint64_t PdStreamTable::forEachRow(const std::string& name,
                                  const std::function<bool(const SqlRow&)>& cb) const {
    if (!hasTable(name)) return 0;
    uint64_t n = 0;
    SqlRow row(4);
    for (const PdStreamRecord& r : parsed_.records) {
        row[0].kind = SqlValue::Kind::Double; row[0].d = r.time;
        row[1].kind = SqlValue::Kind::Double; row[1].d = r.vbus;
        row[2].kind = SqlValue::Kind::Double; row[2].d = r.ibus;
        row[3].kind = SqlValue::Kind::Blob;   row[3].b = r.raw;
        n++;
        if (cb && !cb(row)) break;
    }
    return n;
}

PowerzKind sniffStreamProtocol(const PdStreamParsed& parsed) {
    size_t frames = 0, events = 0;
    const size_t n = std::min<size_t>(parsed.records.size(), 64);
    for (size_t i = 0; i < n; ++i) {
        const Bytes& raw = parsed.records[i].raw;
        if (ufcs::ufcsParseRecord(raw.data(), raw.size())) ++frames;
        else if (ufcs::ufcsParseEvent(raw.data(), raw.size())) ++events;
    }
    // A PD packet can have arbitrary bytes, so require repeated UFCS evidence.
    return (frames >= 2 || (frames >= 1 && events >= 1) || events >= 3)
        ? PowerzKind::Ufcs : PowerzKind::Pd;
}

std::unique_ptr<PowerzCapture> openPdStream(const Bytes& bytes, PowerzKind kind) {
    PdStreamParsed parsed = readPdStream(bytes);
    auto table = std::make_shared<PdStreamTable>(parsed, kind);
    auto cap = std::make_unique<PowerzCapture>(table, kind, bytes.size());

    PowerzMeta& m = cap->metaMutable();

    // 时间基准：记录里的 Time 就是秒，按「1 采样点 = 1 ms」映射（与 .sqlite 同一套口径）。
    // 没有 pd_chart 就没有采样序列，但**报文时间照样撑起时间轴** —— 末条记录的时间即总长。
    const uint64_t lastSamples =
        static_cast<uint64_t>(std::llround(parsed.records.back().time * kPowerzRate));
    const uint64_t totalSamples = std::max(m.totalSamples, lastSamples);
    m.totalSamples = totalSamples;
    m.durationSec = static_cast<double>(totalSamples) / kPowerzRate;

    m.title = kind == PowerzKind::Pd ? "POWER-Z · .pdStream" : "POWER-Z · .ufcsStream";
    m.unsupported.clear();
    m.isSqlite = false;                 // 不是 SQLite，别让界面去读页大小
    m.pageSize = m.pageCount = m.textEncoding = m.writeVersion = 0;
    m.busLabelA.clear();                // 没有第三、第四路模拟量 → 也没有「差分线」档
    m.busLabelB.clear();
    m.sampleRateNote = "没有 ADC 波形；时间轴按 1 采样点 = 1 ms";
    return cap;
}

}  // namespace pdscope
