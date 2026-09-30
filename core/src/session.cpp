#include "session.h"

#include "csv.h"   // exportCsv / defaultCsvName 用的是同一份 CSV 实现（三出口共用）

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace pdscope {
namespace {

std::string crcLabel(CrcState s) {
    switch (s) {
        case CrcState::Ok: return "ok";
        case CrcState::Bad: return "bad";
        default: return "none";
    }
}

/**
 * 报文类别 → 一个字节的编号，给时间轴的二进制标记用。
 * **编号即契约**：Dart 侧按同一张表还原颜色（models.dart 的 `kKindFromCode`）。
 * 加类别就在这里加，并同步那边。
 */
int kindCodeOf(const Packet& p) {
    const std::string k = packetKind(p);
    if (k == "Control") return 0;
    if (k == "Data") return 1;
    if (k == "Extended") return 2;
    if (k == "VDM") return 3;
    if (k == "Error") return 4;
    if (k == "Custom") return 5;
    return 0;
}

/* 时间轴标记的 flags 位 */
constexpr uint8_t kMarkBadCrc = 1u << 0;   // CRC 校验未通过
constexpr uint8_t kMarkPaired = 1u << 1;   // 是某个配对（GoodCRC / ACK）的确认对象

}  // namespace

/* ────────────────────────── JSON ────────────────────────── */

json packetListItemJson(const Packet& p, const std::string& protocol) {
    json j;
    j["index"] = p.index;
    j["seq"] = p.seq;
    j["channel"] = p.channel;
    j["sop"] = p.sop;
    j["msgType"] = p.msgType;
    j["role"] = p.role;
    j["kind"] = packetKind(p);
    j["msgKind"] = p.msgKind;
    if (p.hasMsgId) j["msgId"] = p.msgId; else j["msgId"] = nullptr;
    if (protocol == "UFCS") j["bytes"] = p.dataLen;
    else j["objects"] = p.nObjects;
    j["timeMs"] = p.timeMs;
    j["elapsed"] = csvClock(p.timeMs);
    j["startSample"] = p.startSample;
    j["endSample"] = p.endSample;
    j["durationUs"] = p.durationUs;
    j["vbus"] = p.vbus;
    j["ibus"] = p.ibus;
    j["dataHex"] = p.dataHex;
    j["crc"] = crcLabel(p.crcOk);
    j["summary"] = p.summary;
    j["warn"] = p.warnings.size();
    if (p.hasAck) { j["ackOf"] = p.ackOf; j["ackType"] = p.ackType; }
    return j;
}

json packetDetailJsonOf(const Packet& p, const std::string& protocol) {
    json j = packetListItemJson(p, protocol);

    j["text"] = p.text;
    if (p.hasHeader) j["header"] = p.header; else j["header"] = nullptr;
    if (p.hasExtHeader) j["extHeader"] = p.extHeader; else j["extHeader"] = nullptr;
    if (p.hasRev) { j["rev"] = p.rev; j["revText"] = p.revText; }
    else { j["rev"] = nullptr; j["revText"] = nullptr; }
    if (p.hasMsgTypeRaw) j["msgTypeRaw"] = p.msgTypeRaw; else j["msgTypeRaw"] = nullptr;
    j["link"] = p.link;
    j["category"] = p.category;
    j["eop"] = p.eop;
    j["synthetic"] = p.synthetic;
    j["roleInferred"] = p.roleInferred;
    if (p.hasCrc) j["crcValue"] = p.crc; else j["crcValue"] = nullptr;
    j["crcCalc"] = p.crcCalc;

    json det = json::array();
    for (const DetailItem& d : p.details) {
        json o;
        o["key"] = d.key;
        o["value"] = d.value;
        det.push_back(std::move(o));
    }
    j["details"] = std::move(det);

    json wr = json::array();
    for (const PacketWarning& w : p.warnings) {
        json o;
        o["long"] = w.longMsg;
        o["short"] = w.shortMsg;
        wr.push_back(std::move(o));
    }
    j["warnings"] = std::move(wr);

    json words = json::array();
    for (uint32_t w : p.dataWords) words.push_back(w);
    j["dataWords"] = std::move(words);

    json bytes = json::array();
    for (uint8_t b : p.dataBytes) bytes.push_back(b);
    j["dataBytes"] = std::move(bytes);
    return j;
}

json packetFullJson(const Packet& p) {
    return packetDetailJsonOf(p, "");
}

json decodeStatsJson(const DecodeStats& st) {
    json j;
    j["channel"] = st.channel;
    j["source"] = st.source;
    j["kind"] = st.kind;
    j["protocol"] = st.protocol;
    j["unsupported"] = st.unsupported.empty() ? json(nullptr) : json(st.unsupported);
    j["totalSamples"] = st.totalSamples;
    j["durationSec"] = st.durationSec;
    j["sampleRate"] = st.sampleRate;
    j["sampleRateSource"] = st.sampleRateSource;
    j["sampleRateNote"] = st.hasSampleRateNote ? json(st.sampleRateNote) : json(nullptr);
    j["sampleRateDeclared"] = st.sampleRateDeclared;
    j["sampleRateMeasured"] = st.hasSampleRateMeasured ? json(st.sampleRateMeasured) : json(nullptr);
    j["edges"] = st.edges;
    j["trimmedBytes"] = st.trimmedBytes;
    j["packetCount"] = st.packetCount;
    j["badCrc"] = st.badCrc;
    j["crcUnknown"] = st.crcUnknown;
    j["warnings"] = st.warnings;
    j["badWire"] = st.badWire;
    j["truncatedRows"] = st.truncatedRows;
    j["connectCount"] = st.connectCount;
    j["disconnectCount"] = st.disconnectCount;
    j["ufcsEvents"] = st.ufcsEvents;
    j["unsupportedMsgs"] = st.unsupportedMsgs;
    j["ufcsFrames"] = st.ufcsFrames;
    j["ufcsUnlocatedRows"] = st.ufcsUnlocatedRows;
    j["ufcsDirFromLine"] = st.ufcsDirFromLine;
    j["ufcsDirInferred"] = st.ufcsDirInferred;
    j["tableRows"] = st.tableRows;
    j["chartRows"] = st.chartRows;

    json codes = json::array();
    for (const auto& kv : st.ufcsEventCodes) {
        json o;
        o["code"] = kv.first;
        o["n"] = kv.second;
        codes.push_back(std::move(o));
    }
    j["ufcsEventCodes"] = std::move(codes);

    json evs = json::array();
    for (const EventRecord& e : st.events) {
        json o;
        o["kind"] = e.kind;
        o["tsMs"] = e.tsMs;
        o["code"] = e.code;
        evs.push_back(std::move(o));
    }
    j["events"] = std::move(evs);

    if (st.hasChannelPick) {
        json cp;
        cp["picked"] = st.channelPick.picked;
        cp["noiseRejected"] = st.channelPick.noiseRejected;
        cp["allNoisy"] = st.channelPick.allNoisy;
        j["channelPick"] = std::move(cp);
    } else {
        j["channelPick"] = nullptr;
    }
    return j;
}

/* ────────────────────────── Session ────────────────────────── */

const char* Session::sourceName() const {
    return sourceKind_ == SourceKind::Atkcc ? "atkcc" : "powerz";
}

const char* Session::containerName() const {
    switch (sourceKind_) {
        case SourceKind::Atkcc: return "atkcc";
        case SourceKind::PdStream: return "pdstream";
        case SourceKind::UfcsStream: return "ufcsstream";
        default: return "sqlite";
    }
}

std::unique_ptr<Session> Session::openBytes(const Bytes& bytes, const std::string& nameHint) {
    auto s = std::unique_ptr<Session>(new Session());
    s->fileSize_ = bytes.size();
    s->name_ = nameHint;
    s->dispatch(bytes, nameHint);
    s->filters_ = newFilters(s->activeProtocolName());
    return s;
}

std::string Session::activeProtocolName() const {
    // 筛选默认值跟协议走（UFCS 是 D+/D-/Custom，PD 是 SOP/Extended/VDM）
    return protocol_ == "UFCS" ? "UFCS" : "pd";
}

void Session::dispatch(const Bytes& bytes, const std::string& nameHint) {
    if (bytes.empty()) {
        failFormat("文件为空，无法判定格式");
    }

    // ① ZIP 魔数 → .atkcc
    if (bytes.size() >= 4 && rdU32LE(bytes.data()) == 0x04034b50) {
        atkcc_ = std::make_shared<AtkccCapture>(AtkccCapture::open(bytes));
        sourceKind_ = SourceKind::Atkcc;
        protocol_ = "USB PD";
        return;
    }

    // ② SQLite 魔数 + pd_table / ufcs_table → POWER-Z
    if (SqliteReader::isSqlite(bytes)) {
        if (auto kind = sniffPowerz(bytes)) {
            // 交给 SqliteReader 一个**它自己持有**的缓冲区：sqlite3_deserialize
            // 不复制数据，而这里传进来的 bytes 可能是调用方的临时对象。
            sqlite_ = std::make_shared<SqliteReader>(std::make_shared<const Bytes>(bytes));
            powerz_ = std::make_shared<PowerzCapture>(sqlite_, *kind, bytes.size());
            sourceKind_ = SourceKind::PowerzSqlite;
            protocol_ = (*kind == PowerzKind::Pd) ? "USB PD" : "UFCS";
            return;
        }
        failFormat("是 SQLite 库，但没有 pd_table / ufcs_table —— 不是 POWER-Z 导出");
    }

    // ③ 记录流结构自证，再从 Raw 内容区分 PD / UFCS。
    if (sniffPdStream(bytes)) {
        const PowerzKind kind = sniffStreamProtocol(readPdStream(bytes));
        powerz_ = openPdStream(bytes, kind);
        sourceKind_ = kind == PowerzKind::Pd ? SourceKind::PdStream : SourceKind::UfcsStream;
        protocol_ = kind == PowerzKind::Pd ? "USB PD" : "UFCS";
        return;
    }

    failFormat("无法识别的文件格式（既不是 .atkcc（ZIP）、也不是 POWER-Z 的 .sqlite / .pdStream）");
    (void)nameHint;
}

std::vector<int> Session::channelOrder() const {
    if (atkcc_) return atkcc_->meta().channelOrder;
    return {0};
}

json Session::metadata() const {
    json j;
    j["name"] = name_;
    j["source"] = sourceName();
    j["container"] = containerName();
    j["protocol"] = protocol_;
    j["fileBytes"] = fileSize_;
    j["decoded"] = decoded_;

    json chans = json::array();

    if (atkcc_) {
        const AtkccMeta& m = atkcc_->meta();
        j["kind"] = "";
        j["title"] = "ATK-C · .atkcc";
        j["sampleRate"] = m.sampleRate;
        j["sampleRateSource"] = m.sampleRateSource;
        j["sampleRateKey"] = m.sampleRateKey.empty() ? json(nullptr) : json(m.sampleRateKey);
        j["sampleRateRaw"] = m.sampleRateRaw.empty() ? json(nullptr) : json(m.sampleRateRaw);
        j["samplingFrequencyRaw"] = m.samplingFrequencyRaw;
        j["sampleRateNote"] = nullptr;
        j["totalSamples"] = m.totalSamples;
        j["durationSec"] = atkcc_->durationSec();
        j["entryCount"] = m.entryCount;
        j["multiChannel"] = m.channelOrder.size() > 1;
        j["busLabels"] = json::array();
        j["hasBus"] = !m.bus.empty();
        j["busPoints"] = m.bus.size();

        for (int ch : m.channelOrder) {
            const AtkccChannel& c = m.channelMap.at(ch);
            json o;
            o["channel"] = ch;
            o["label"] = "ch" + std::to_string(ch);
            o["folder"] = c.folder;
            o["totalSamples"] = c.totalSamples;
            o["totalBytes"] = c.totalBytes;
            o["chunks"] = c.chunks.size();
            o["effectiveSampleLimit"] = atkcc_->effectiveSampleLimit(ch);
            chans.push_back(std::move(o));
        }
        j["tableRows"] = 0;
        j["chartRows"] = 0;
        j["sqlite"] = nullptr;
        j["stream"] = nullptr;
    } else if (powerz_) {
        const PowerzMeta& m = powerz_->meta();
        j["kind"] = m.kind;
        j["title"] = m.title;
        j["unsupported"] = m.unsupported.empty() ? json(nullptr) : json(m.unsupported);
        j["sampleRate"] = m.sampleRate;
        j["sampleRateSource"] = m.sampleRateSource;
        j["sampleRateKey"] = nullptr;
        j["sampleRateRaw"] = nullptr;
        j["samplingFrequencyRaw"] = nullptr;
        j["sampleRateNote"] = m.sampleRateNote;
        j["totalSamples"] = m.totalSamples;
        j["durationSec"] = m.durationSec;
        j["entryCount"] = 0;
        j["multiChannel"] = false;

        json labels = json::array();
        if (!m.busLabelA.empty()) labels.push_back(m.busLabelA);
        if (!m.busLabelB.empty()) labels.push_back(m.busLabelB);
        j["busLabels"] = std::move(labels);
        j["hasBus"] = !m.bus.empty();
        j["busPoints"] = m.bus.size();

        json o;
        o["channel"] = 0;
        o["label"] = m.protocol;
        o["folder"] = nullptr;
        o["totalSamples"] = m.totalSamples;
        o["totalBytes"] = m.fileBytes;
        o["chunks"] = 0;
        o["effectiveSampleLimit"] = m.totalSamples;
        chans.push_back(std::move(o));

        j["tableRows"] = m.tableRows;
        j["chartRows"] = m.chartRows;
        if (m.isSqlite) {
            json sq;
            sq["pageSize"] = m.pageSize;
            sq["pageCount"] = m.pageCount;
            sq["textEncoding"] = m.textEncoding;
            sq["writeVersion"] = m.writeVersion;
            j["sqlite"] = std::move(sq);
        } else {
            j["sqlite"] = nullptr;
        }
        j["stream"] = nullptr;
    } else {
        fail(PDSCOPE_STATUS_STATE, "会话未正确初始化");
    }

    j["channels"] = std::move(chans);
    return j;
}

std::vector<BusInput> Session::busInput() const {
    std::vector<BusInput> out;
    if (atkcc_) {
        const std::vector<BusPoint>& b = atkcc_->meta().bus;
        out.reserve(b.size());
        for (const BusPoint& p : b) {
            BusInput i;
            i.sample = p.sample;
            i.vbus = p.vbus;
            i.ibus = p.ibus;
            i.aux = false;                 // .atkcc 的 bus.ini 只有两路
            out.push_back(i);
        }
    } else if (powerz_) {
        const std::vector<PowerzBusPoint>& b = powerz_->meta().bus;
        out.reserve(b.size());
        for (const PowerzBusPoint& p : b) {
            BusInput i;
            i.sample = p.sample;
            i.vbus = p.vbus;
            i.ibus = p.ibus;
            i.a = p.a;
            i.b = p.b;
            i.aux = true;                  // POWER-Z 有 CC1/CC2 或 DP/DM
            out.push_back(i);
        }
    }
    return out;
}

void Session::attachBusValues() {
    const std::vector<BusInput> bus = busInput();
    for (Packet& p : packets_) {
        if (bus.empty()) { p.vbus = 0; p.ibus = 0; continue; }
        const BusInput v = busAt(bus, p.startSample);
        p.vbus = v.vbus;
        p.ibus = v.ibus;
    }
}

void Session::decode(int channel, double sampleRateOverride, bool metadataOnly) {
    if (decoded_) return;
    clearCancel();

    if (metadataOnly) {
        decoded_ = true;
        return;
    }

    packets_.clear();
    stats_ = DecodeStats{};

    if (atkcc_) {
        const ChannelPickResult pick = pickChannel(*atkcc_, [this] { return cancelFlag_.load(); });
        channel_ = (channel >= 0) ? channel : pick.picked;

        ChannelDecodeOpts opts;
        opts.sampleRateOverride = sampleRateOverride;
        opts.shouldStop = [this] { return cancelFlag_.load(); };
        opts.onProgress = [this](uint64_t done, uint64_t total, uint64_t pkts, uint64_t samples) {
            progressPhase_.store(2);
            progressChannel_.store(channel_);
            progressDone_.store(done);
            progressTotal_.store(total);
            progressPackets_.store(pkts);
            if (onProgress_) onProgress_(2, done, total, pkts);
        };

        DecodeOutcome out = decodeChannel(*atkcc_, channel_, opts);
        packets_ = std::move(out.packets);
        stats_ = std::move(out.stats);
        cancelled_ = out.cancelled;

        const size_t nCh = atkcc_->meta().channelOrder.size();
        if (nCh > 1 || pick.noiseRejected > 0 || pick.allNoisy) {
            stats_.hasChannelPick = true;
            stats_.channelPick.picked = pick.picked;
            stats_.channelPick.noiseRejected = pick.noiseRejected;
            stats_.channelPick.allNoisy = pick.allNoisy;
        }
        stats_.protocol = "USB PD";
    } else if (powerz_) {
        channel_ = 0;
        progressPhase_.store(1);
        CancelFn stop = [this] { return cancelFlag_.load(); };
        ProgressFn prog = [this](int phase, uint64_t done, uint64_t total, uint64_t pkts) {
            progressPhase_.store(phase);
            progressDone_.store(done);
            progressTotal_.store(total);
            progressPackets_.store(pkts);
            if (onProgress_) onProgress_(phase, done, total, pkts);
        };
        PowerzResult r = powerz_->decode(stop, prog);
        packets_ = std::move(r.packets);
        stats_ = std::move(r.stats);
        cancelled_ = cancelFlag_.load();
        stats_.protocol = powerz_->meta().protocol;
    } else {
        fail(PDSCOPE_STATUS_STATE, "会话未正确初始化");
    }

    attachBusValues();
    progressPhase_.store(0);
    decoded_ = true;
    viewDirty_ = true;
}

json Session::statsJson() const { return decodeStatsJson(stats_); }

void Session::setFilter(const json& j) {
    Filters f = newFilters(activeProtocolName());

    if (j.is_object()) {
        auto loadSet = [&](const char* key, std::set<std::string>& target) {
            if (!j.contains(key)) return;
            const json& v = j[key];
            if (!v.is_array()) return;
            target.clear();
            for (const json& e : v) {
                if (e.is_string()) target.insert(e.get<std::string>());
            }
        };
        if (j.contains("roles") || j.contains("sops") || j.contains("cats") || j.contains("types")) {
            // 显式给了这几项 → 以调用方为准（空数组表示「一个都不选」）
            if (j.contains("roles")) loadSet("roles", f.roles);
            if (j.contains("sops")) loadSet("sops", f.sops);
            if (j.contains("cats")) loadSet("cats", f.cats);
            if (j.contains("types")) loadSet("types", f.types);
        }
        if (j.contains("hideGoodCrc") && j["hideGoodCrc"].is_boolean()) {
            f.hideGoodCrc = j["hideGoodCrc"].get<bool>();
        }
        if (j.contains("onlyBad") && j["onlyBad"].is_boolean()) f.onlyBad = j["onlyBad"].get<bool>();
        if (j.contains("onlyPower") && j["onlyPower"].is_boolean()) f.onlyPower = j["onlyPower"].get<bool>();
        if (j.contains("onlyEnter") && j["onlyEnter"].is_boolean()) f.onlyEnter = j["onlyEnter"].get<bool>();
        if (j.contains("q") && j["q"].is_string()) f.q = trim(j["q"].get<std::string>());
        if (j.contains("tFrom") && j["tFrom"].is_number()) f.tFrom = j["tFrom"].get<double>();
        if (j.contains("tTo") && j["tTo"].is_number()) f.tTo = j["tTo"].get<double>();

        if (j.contains("sort") && j["sort"].is_object()) {
            const json& s = j["sort"];
            if (s.contains("key") && s["key"].is_string()) sort_.key = s["key"].get<std::string>();
            if (s.contains("asc") && s["asc"].is_boolean()) sort_.asc = s["asc"].get<bool>();
        }
        if (j.contains("viewMode") && j["viewMode"].is_string()) {
            const std::string v = j["viewMode"].get<std::string>();
            if (v == "neg") viewMode_ = ViewMode::Neg;
            else if (v == "err") viewMode_ = ViewMode::Err;
            else viewMode_ = ViewMode::All;
        }
    }

    filters_ = std::move(f);
    viewDirty_ = true;
}

void Session::rebuildView() {
    view_ = buildView(packets_, filters_, activeProtocolName(),
                      stats_.totalSamples ? stats_.totalSamples : 1,
                      viewMode_, sort_);
    viewDirty_ = false;
}

uint64_t Session::viewCount() {
    // 与 pageJson / exportCsv 一样：先保证视图是最新的，再报条数。
    if (viewDirty_) rebuildView();
    return view_.size();
}

json Session::pageJson(uint64_t offset, uint32_t limit) const {
    if (viewDirty_) const_cast<Session*>(this)->rebuildView();
    json arr = json::array();
    if (offset >= view_.size()) return arr;
    const uint64_t end = std::min<uint64_t>(view_.size(),
                                            offset + static_cast<uint64_t>(limit ? limit : 0));
    for (uint64_t i = offset; i < end; ++i) {
        arr.push_back(packetListItemJson(*view_[static_cast<size_t>(i)], protocol_));
    }
    return arr;
}

json Session::packetDetailJson(uint64_t index) const {
    if (index >= packets_.size()) {
        fail(PDSCOPE_STATUS_ARGUMENT, "报文序号 " + std::to_string(index) + " 越界（共 "
                                      + std::to_string(packets_.size()) + " 条）");
    }
    return packetDetailJsonOf(packets_[static_cast<size_t>(index)], protocol_);
}

Bytes Session::waveformBinary(int channel, uint64_t startSample, uint64_t endSample,
                              uint32_t maxPoints) const {
    if (!atkcc_) {
        fail(PDSCOPE_STATUS_UNSUPPORTED,
             "这份抓包没有原始电平采样（只有 .atkcc 才有），无法给出波形");
    }
    const WaveformRange r = readWaveform(*atkcc_, channel, startSample, endSample, maxPoints);

    const uint32_t n = static_cast<uint32_t>(r.points.size());
    const size_t total = 4 + 8 + 8 + static_cast<size_t>(n) * 4 + static_cast<size_t>(n) * 4;
    Bytes out(total, 0);

    auto putU32 = [&](size_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto putU64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) out[off + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    auto putF32 = [&](size_t off, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        putU32(off, bits);
    };

    putU32(0, n);
    putU64(4, r.bucket);
    putU64(12, r.startSample);

    size_t off = 20;
    for (const WavePoint& p : r.points) { putF32(off, static_cast<float>(p.hi)); off += 4; }
    for (const WavePoint& p : r.points) { putF32(off, static_cast<float>(p.lo)); off += 4; }
    return out;
}

json Session::busSeriesJson(uint32_t targetPoints) const {
    const std::vector<BusInput> bus = busInput();
    const uint64_t total = stats_.totalSamples ? stats_.totalSamples : (powerz_ ? powerz_->meta().totalSamples : 0);
    const double rate = stats_.sampleRate > 0 ? stats_.sampleRate
                                              : (atkcc_ ? atkcc_->meta().sampleRate : kPowerzRate);
    const BusSeries s = buildBusSeries(bus, total, rate, targetPoints);

    json j;
    j["t0"] = s.t0;
    j["step"] = s.step;
    j["n"] = s.n;
    j["sampleRate"] = s.sampleRate;
    j["hasAux"] = s.hasAux;
    j["vmax"] = s.vmax;
    j["imax"] = s.imax;
    j["camax"] = s.camax;
    j["cbmax"] = s.cbmax;
    if (!atkcc_ && powerz_) {
        json labels = json::array();
        if (!powerz_->meta().busLabelA.empty()) labels.push_back(powerz_->meta().busLabelA);
        if (!powerz_->meta().busLabelB.empty()) labels.push_back(powerz_->meta().busLabelB);
        j["labels"] = std::move(labels);
    } else {
        j["labels"] = json::array();
    }
    j["vbus"] = s.vbus;
    j["ibus"] = s.ibus;
    j["ca"] = s.hasAux ? json(s.ca) : json(nullptr);
    j["cb"] = s.hasAux ? json(s.cb) : json(nullptr);
    return j;
}

json Session::typeCountsJson() const {
    std::map<std::string, uint64_t> counts;
    for (const Packet& p : packets_) counts[p.msgType]++;

    std::vector<std::pair<std::string, uint64_t>> rows(counts.begin(), counts.end());
    // 条数降序；同数按类型名升序 —— 界面上位置稳定，不会每次刷新换顺序。
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    json arr = json::array();
    for (const auto& kv : rows) {
        json o;
        o["type"] = kv.first;
        o["n"] = kv.second;
        arr.push_back(std::move(o));
    }
    return arr;
}

Bytes Session::packetMarksBinary() const {
    const uint32_t n = static_cast<uint32_t>(packets_.size());
    const size_t total = 4 + static_cast<size_t>(n) * (8 + 1 + 1);
    Bytes out(total, 0);

    auto putU32 = [&](size_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto putF64 = [&](size_t off, double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        for (int i = 0; i < 8; ++i) {
            out[off + static_cast<size_t>(i)] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
        }
    };

    putU32(0, n);

    const size_t tsOff = 4;
    const size_t kindOff = tsOff + static_cast<size_t>(n) * 8;
    const size_t flagOff = kindOff + n;

    // 先扫一遍收集「被确认的报文序号」。
    // ⚠ 别在下面的循环里为每条报文再去全表找 `ackOf` —— 那是 O(n²)，
    //   26000 条的样本要跑几个亿次比较，界面一开就卡住。
    std::vector<uint8_t> referenced(n, 0);
    for (const Packet& p : packets_) {
        if (p.hasAck && p.ackOf < n) referenced[static_cast<size_t>(p.ackOf)] = 1;
    }

    for (uint32_t i = 0; i < n; ++i) {
        const Packet& p = packets_[i];
        putF64(tsOff + static_cast<size_t>(i) * 8, p.timeMs / 1000.0);

        uint8_t flags = 0;
        if (p.crcOk == CrcState::Bad) flags |= kMarkBadCrc;
        if (referenced[i]) flags |= kMarkPaired;
        out[kindOff + i] = static_cast<uint8_t>(kindCodeOf(p));
        out[flagOff + i] = flags;
    }
    return out;
}

std::string Session::exportCsv(uint64_t limit, bool bom, bool filtered) const {    // 不筛选时直接把全部报文按 index 顺序列出（`packets_` 本身就是采集顺序）。
    std::vector<const Packet*> all;
    const std::vector<const Packet*>* rows = nullptr;
    if (filtered) {
        if (viewDirty_) const_cast<Session*>(this)->rebuildView();
        rows = &view_;
    } else {
        all.reserve(packets_.size());
        for (const Packet& p : packets_) all.push_back(&p);
        rows = &all;
    }

    ExportDoc doc;
    doc.fileName = name_;
    doc.channel = channel_;
    doc.protocol = protocol_;
    doc.source = sourceName();
    doc.rate = stats_.sampleRate;
    doc.fileSize = fileSize_;
    doc.stats = &stats_;
    doc.totalPackets = packets_.size();
    return csvExport(*rows, doc, limit, bom).csv;
}

std::string Session::exportJson(uint64_t limit) const {
    const uint64_t n = (limit > 0 && limit < packets_.size()) ? limit : packets_.size();
    json root;
    root["metadata"] = metadata();
    root["stats"] = statsJson();
    json arr = json::array();
    for (uint64_t i = 0; i < n; ++i) {
        arr.push_back(packetFullJson(packets_[static_cast<size_t>(i)]));
    }
    root["packets"] = std::move(arr);
    return root.dump(2);
}

std::string Session::defaultCsvName() const {
    return csvFileName(name_, channel_);
}

void Session::snapshotProgress(int* phase, int* channel, uint64_t* done, uint64_t* total,
                              uint64_t* packets) const {
    if (phase) *phase = progressPhase_.load();
    if (channel) *channel = progressChannel_.load();
    if (done) *done = progressDone_.load();
    if (total) *total = progressTotal_.load();
    if (packets) *packets = progressPackets_.load();
}

}  // namespace pdscope
