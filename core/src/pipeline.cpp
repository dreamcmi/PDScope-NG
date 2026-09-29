#include "pipeline.h"

#include "pd/decoder.h"

#include <algorithm>
#include <cmath>

namespace pdscope {
namespace {

/** 把实现细节收在一处：`fmtHz` 按量级换单位，并去掉多余的尾零。 */
std::string fmtHz(double hz) {
    std::string s = toFixedStr(hz / 1e6, 3);
    // 去掉小数点后多余的 0，再去掉孤立的小数点
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) last = dot - 1;
        s = s.substr(0, last + 1);
    }
    return s + " MHz";
}

}  // namespace

/* ────────────────────────── 采样率 ────────────────────────── */

std::optional<SampleRateEstimate> probeSampleRate(const AtkccCapture& cap, int channel,
                                                 int maxChunks, size_t maxRuns,
                                                 const CancelFn& shouldStop) {
    auto it = cap.meta().channelMap.find(channel);
    if (it == cap.meta().channelMap.end()) return std::nullopt;

    std::vector<uint32_t> runs;
    Bytes data;
    const int n = std::min<int>(maxChunks, static_cast<int>(it->second.chunks.size()));
    for (int i = 0; i < n; ++i) {
        if (shouldStop && shouldStop()) break;
        if (!cap.readChunk(channel, static_cast<size_t>(i), data)) continue;
        const size_t remain = (maxRuns > runs.size()) ? (maxRuns - runs.size()) : 0;
        if (remain == 0) break;
        std::vector<uint32_t> part = collectRunStats(data.data(), data.size(), 64, remain);
        runs.insert(runs.end(), part.begin(), part.end());
        if (runs.size() >= maxRuns) break;
    }
    return estimateSampleRate(runs);
}

RateResolution resolveSampleRate(const AtkccCapture& cap, int channel,
                                 const CancelFn& shouldStop, double sampleRateOverride) {
    const double declared = cap.meta().sampleRate;
    const std::string declaredSource = cap.meta().sampleRateSource.empty()
                                           ? std::string("default")
                                           : cap.meta().sampleRateSource;

    RateResolution out;
    out.declared = declared;
    out.declaredSource = declaredSource;
    out.declaredText = cap.meta().sampleRateRaw;
    out.hasDeclaredText = !cap.meta().sampleRateRaw.empty();

    if (sampleRateOverride > 0) {
        out.rate = sampleRateOverride;
        out.source = "override";
        out.note = "采样率由调用方指定为 " + fmtHz(sampleRateOverride)
                 + "（文件声明 " + fmtHz(declared) + "）";
        out.hasNote = true;
        return out;
    }

    auto measured = probeSampleRate(cap, channel, 2, 40000, shouldStop);
    if (measured) {
        out.measured = measured->sampleRate;
        out.hasMeasured = true;
        out.measuredUi = measured->uiSamples;
        out.confidence = measured->confidence;
    }

    if (!measured) {
        // 波形认不出来（通道是空的、或数据不是 PD）→ 保持声明值 / 兜底值
        out.rate = declared;
        out.source = declaredSource;
        return out;
    }

    const double ratio = measured->sampleRate / declared;
    if (declaredSource == "declared" && ratio >= 1.0 - kRateTolerance && ratio <= 1.0 + kRateTolerance) {
        // 一致 → 信文件声明（用它算出的时标才与官方上位机一致）
        out.rate = declared;
        out.source = "declared";
    } else if (declaredSource == "default") {
        out.rate = measured->sampleRate;
        out.source = "measured";
        out.note = "文件未声明采样率，按波形节拍取 " + fmtHz(measured->sampleRate);
        out.hasNote = true;
    } else {
        out.rate = measured->sampleRate;
        out.source = "measured";
        out.note = "文件声明 " + fmtHz(declared) + " 与波形节拍（" + fmtHz(measured->sampleRate)
                 + "）相差过大，已按实测值解码";
        out.hasNote = true;
    }
    return out;
}

/* ────────────────────────── GoodCRC 配对 ────────────────────────── */

void linkGoodCrc(std::vector<Packet>& packets) {
    auto pick = [&](size_t p, bool needId) -> const Packet* {
        const int64_t start = static_cast<int64_t>(p) - 1;
        const int64_t stop = static_cast<int64_t>(p) - kAckWindow;
        for (int64_t j = start; j >= 0 && j > stop; --j) {
            const Packet& q = packets[static_cast<size_t>(j)];
            if (q.isGoodCrc() || q.role == packets[p].role) continue;
            if (needId && q.crcOk != CrcState::Bad && q.msgId != packets[p].msgId) continue;
            return &q;
        }
        return nullptr;
    };

    for (size_t p = 0; p < packets.size(); ++p) {
        Packet& pk = packets[p];
        if (!pk.isGoodCrc() || pk.crcOk == CrcState::Bad) continue;
        const Packet* ref = pick(p, false);
        if (!ref) continue;
        // 邻近报文 CRC 完好但 MessageID 对不上 → 改按 MessageID 精确匹配
        if (ref->crcOk != CrcState::Bad && ref->msgId != pk.msgId) {
            const Packet* alt = pick(p, true);
            if (alt) ref = alt;
        }
        pk.hasAck = true;
        pk.ackOf = ref->index;
        pk.ackType = ref->msgType;
    }
}

/* ────────────────────────── 模拟量轨迹 ────────────────────────── */

BusInput busAt(const std::vector<BusInput>& bus, uint64_t sample) {
    if (bus.empty()) return BusInput{};
    size_t lo = 0, hi = bus.size() - 1, ans = 0;
    while (lo <= hi) {
        const size_t mid = (lo + hi) >> 1;
        if (bus[mid].sample <= sample) { ans = mid; lo = mid + 1; }
        else { if (mid == 0) break; hi = mid - 1; }
    }
    return bus[ans];
}

BusSeries buildBusSeries(const std::vector<BusInput>& bus, uint64_t totalSamples,
                         double sampleRate, uint32_t targetPoints) {
    BusSeries s;
    s.sampleRate = sampleRate;
    if (bus.empty()) return s;

    bool hasAux = false;
    for (const BusInput& r : bus) {
        if (r.aux) { hasAux = true; break; }
    }
    s.hasAux = hasAux;

    s.step = std::max<uint64_t>(1, static_cast<uint64_t>(std::floor(
        static_cast<double>(totalSamples) / static_cast<double>(targetPoints ? targetPoints : 1))));
    s.n = static_cast<uint64_t>(std::floor(static_cast<double>(totalSamples) /
                                          static_cast<double>(s.step))) + 1;

    s.vbus.resize(static_cast<size_t>(s.n));
    s.ibus.resize(static_cast<size_t>(s.n));
    if (hasAux) {
        s.ca.resize(static_cast<size_t>(s.n));
        s.cb.resize(static_cast<size_t>(s.n));
    }

    size_t j = 0;
    for (uint64_t i = 0; i < s.n; ++i) {
        const uint64_t sample = i * s.step;
        while (j + 1 < bus.size() && bus[j + 1].sample <= sample) ++j;
        s.vbus[static_cast<size_t>(i)] = bus[j].vbus;
        s.ibus[static_cast<size_t>(i)] = bus[j].ibus;
        if (s.vbus[static_cast<size_t>(i)] > s.vmax) s.vmax = s.vbus[static_cast<size_t>(i)];
        if (s.ibus[static_cast<size_t>(i)] > s.imax) s.imax = s.ibus[static_cast<size_t>(i)];
        if (hasAux) {
            s.ca[static_cast<size_t>(i)] = bus[j].a;
            s.cb[static_cast<size_t>(i)] = bus[j].b;
            if (s.ca[static_cast<size_t>(i)] > s.camax) s.camax = s.ca[static_cast<size_t>(i)];
            if (s.cb[static_cast<size_t>(i)] > s.cbmax) s.cbmax = s.cb[static_cast<size_t>(i)];
        }
    }
    return s;
}

/* ────────────────────────── 波形包络 ────────────────────────── */

WaveformRange readWaveform(const AtkccCapture& cap, int channel,
                           uint64_t startSample, uint64_t endSample, uint32_t maxPoints) {
    WaveformRange out;
    out.startSample = startSample;
    out.endSample = endSample;
    out.sampleRate = cap.meta().sampleRate;

    auto it = cap.meta().channelMap.find(channel);
    if (it == cap.meta().channelMap.end()) return out;
    if (maxPoints == 0) maxPoints = 1;

    const uint64_t startByte = startSample / 8;
    const uint64_t endByte = std::min<uint64_t>((endSample + 7) / 8, it->second.totalBytes);
    if (endByte <= startByte) return out;

    const uint64_t firstChunk = startByte / kChunkSize;
    const uint64_t lastChunk = (endByte - 1) / kChunkSize;

    const uint64_t span = std::max<uint64_t>(1, endSample - startSample);
    out.bucket = std::max<uint64_t>(1, (span + maxPoints - 1) / maxPoints);

    auto consumeRun = [&](int level, uint64_t sample) {
        uint64_t bucketIdx = (sample >= startSample) ? ((sample - startSample) / out.bucket) : 0;
        if (bucketIdx >= maxPoints) bucketIdx = maxPoints - 1;
        while (out.points.size() <= bucketIdx) {
            WavePoint p;
            p.s = startSample + out.points.size() * out.bucket;
            out.points.push_back(p);
        }
        WavePoint& p = out.points[static_cast<size_t>(bucketIdx)];
        if (level) p.hi = 1;
        else p.lo = 0;
    };

    Bytes data;
    for (uint64_t ci = firstChunk; ci <= lastChunk; ++ci) {
        if (!cap.readChunk(channel, static_cast<size_t>(ci), data)) continue;
        const uint64_t chunkBase = ci * kChunkSize * 8;
        for (size_t b = 0; b < data.size(); ++b) {
            const uint64_t bitBase = chunkBase + static_cast<uint64_t>(b) * 8;
            if (bitBase + 8 <= startSample || bitBase >= endSample) continue;
            const uint8_t byte = data[b];
            if (byte == 0xFF) { consumeRun(1, bitBase); continue; }
            if (byte == 0x00) { consumeRun(0, bitBase); continue; }
            for (int k = 0; k < 8; ++k) {          // LSB 优先：bit0 时间最早
                const int bit = (byte >> k) & 1;
                const uint64_t s = bitBase + static_cast<uint64_t>(k);
                if (s < startSample || s >= endSample) continue;
                consumeRun(bit, s);
            }
        }
    }
    return out;
}

/* ────────────────────────── 通道自动挑选 ────────────────────────── */

ChannelPickResult pickChannel(const AtkccCapture& cap, const CancelFn& shouldStop) {
    ChannelPickResult r;
    const std::vector<int>& order = cap.meta().channelOrder;
    if (order.empty()) return r;

    r.picked = order[0];
    if (order.size() == 1) return r;

    struct Scored { int channel; uint64_t activity; double edgeLike; };
    std::vector<Scored> scored;
    for (int ch : order) {
        if (shouldStop && shouldStop()) break;
        ChannelActivity a = cap.scanActivity(ch, 3);
        r.activity[ch] = a;
        scored.push_back(Scored{ch, a.activity, a.edgeLike});
    }

    std::vector<Scored> clean;
    for (const Scored& s : scored) {
        if (s.edgeLike <= kNoiseEdgeLike) clean.push_back(s);
    }
    const std::vector<Scored>& pool = clean.empty() ? scored : clean;
    r.noiseRejected = static_cast<int>(scored.size() - clean.size());
    r.allNoisy = clean.empty();

    const Scored* best = &pool[0];
    for (const Scored& s : pool) {
        if (s.activity > best->activity) best = &s;
    }
    r.picked = best->channel;
    return r;
}

/* ────────────────────────── 主流程 ────────────────────────── */

DecodeOutcome decodeChannel(const AtkccCapture& cap, int channel, const ChannelDecodeOpts& opts) {
    DecodeOutcome out;
    auto chIt = cap.meta().channelMap.find(channel);
    if (chIt == cap.meta().channelMap.end()) {
        fail(PDSCOPE_STATUS_ARGUMENT, "通道 " + std::to_string(channel) + " 不存在");
    }

    const RateResolution rate = resolveSampleRate(cap, channel, opts.shouldStop,
                                                  opts.sampleRateOverride);
    const double sampleRate = rate.rate;

    BmcDecoder bmc(sampleRate);
    pd::PdDecoder pdDec(sampleRate);
    EdgeExtractor ex(opts.lsbFirst);

    std::vector<Packet> packets;
    const uint64_t limit = cap.effectiveSampleLimit(channel);
    uint64_t sampleBase = 0;
    uint64_t trimmed = 0;
    uint64_t edges = 0;

    auto emit = [&](BmcRawPacket&& raw) {
        edges += raw.edges.size();
        if (auto pkt = pdDec.decode(raw, channel)) packets.push_back(std::move(*pkt));
    };

    Bytes raw;
    const size_t chunkCount = chIt->second.chunks.size();
    bool cancelled = false;

    for (size_t i = 0; i < chunkCount; ++i) {
        if (opts.shouldStop && opts.shouldStop()) { cancelled = true; break; }
        if (!cap.readChunk(channel, i, raw)) continue;

        const bool isLast = (i + 1 == chunkCount);
        size_t keep = raw.size();
        if (limit > 0) {
            const uint64_t remainSamples = (sampleBase < limit) ? (limit - sampleBase) : 0;
            const uint64_t remainBytes = (remainSamples + 7) / 8;
            if (remainBytes < keep) keep = static_cast<size_t>(remainBytes);
        }
        if (isLast) {
            const size_t t = AtkccCapture::trimTrailingZeros(raw);
            if (t < keep) { trimmed += (keep - t); keep = t; }
        }

        // 分片喂入：片内同步、片间可以让出（不改变解码结果 —— 状态都挂在实例上）
        constexpr size_t kSliceBytes = 65536;
        for (size_t off = 0; off < keep; off += kSliceBytes) {
            const size_t end = std::min(off + kSliceBytes, keep);
            ex.push(raw.data() + off, end - off, sampleBase + static_cast<uint64_t>(off) * 8,
                    [&](uint64_t edge) {
                        if (auto p = bmc.pushEdge(edge)) emit(std::move(*p));
                    });
            if (opts.shouldStop && opts.shouldStop()) { cancelled = true; break; }
        }
        if (cancelled) break;

        sampleBase += static_cast<uint64_t>(keep) * 8;
        if (opts.onProgress) opts.onProgress(i + 1, chunkCount, packets.size(), sampleBase);
    }

    // 收尾：补一个虚拟边沿，让最后一个包也能落地
    if (!cancelled && !(opts.shouldStop && opts.shouldStop())) {
        ex.flush([&](uint64_t edge) {
            if (auto p = bmc.pushEdge(edge, true)) emit(std::move(*p));
        }, bmc.maxbit() + 1);
    }

    std::stable_sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) {
        return a.startSample < b.startSample;
    });
    for (size_t i = 0; i < packets.size(); ++i) packets[i].index = i;
    linkGoodCrc(packets);

    DecodeStats st;
    st.channel = channel;
    st.source = "atkcc";
    st.protocol = "USB PD";
    st.totalSamples = sampleBase;
    st.durationSec = sampleRate > 0 ? static_cast<double>(sampleBase) / sampleRate : 0.0;
    st.sampleRate = sampleRate;
    st.sampleRateSource = rate.source;
    st.sampleRateDeclared = rate.declared;
    st.sampleRateMeasured = rate.measured;
    st.hasSampleRateMeasured = rate.hasMeasured;
    st.sampleRateNote = rate.note;
    st.hasSampleRateNote = rate.hasNote;
    st.edges = edges;
    st.trimmedBytes = trimmed;
    st.packetCount = packets.size();
    for (const Packet& p : packets) {
        if (p.crcOk == CrcState::Bad) st.badCrc++;
        st.warnings += p.warnings.size();
    }

    out.packets = std::move(packets);
    out.stats = st;
    out.cancelled = cancelled;
    return out;
}

}  // namespace pdscope
