#include "bmc.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pdscope {
namespace {

struct Run {
    uint8_t bit;
    uint8_t len;
};

/** 预算每个字节值在两种位序下的游程（相邻项 bit 一定不同）。 */
struct ByteRuns {
    // 每字节最多 8 段
    std::array<std::vector<Run>, 256> lsb;
    std::array<std::vector<Run>, 256> msb;

    ByteRuns() {
        for (int b = 0; b < 256; ++b) {
            for (int k = 0; k < 8; ++k) {           // LSB 优先：bit0 最早
                const int bit = (b >> k) & 1;
                auto& v = lsb[b];
                if (!v.empty() && v.back().bit == bit) v.back().len++;
                else v.push_back(Run{static_cast<uint8_t>(bit), 1});
            }
            for (int k = 7; k >= 0; --k) {          // MSB 优先：bit7 最早
                const int bit = (b >> k) & 1;
                auto& v = msb[b];
                if (!v.empty() && v.back().bit == bit) v.back().len++;
                else v.push_back(Run{static_cast<uint8_t>(bit), 1});
            }
        }
    }
};

const ByteRuns& byteRuns() {
    static const ByteRuns t;
    return t;
}

}  // namespace

void EdgeExtractor::push(const uint8_t* data, size_t len, uint64_t baseSample,
                         const std::function<void(uint64_t)>& onEdge) {
    const auto& table = lsbFirst_ ? byteRuns().lsb : byteRuns().msb;
    uint64_t sample = baseSample;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b == 0) {
            if (level_ != 0) { onEdge(sample); level_ = 0; runStart_ = sample; }
            sample += 8;
            continue;
        }
        if (b == 0xFF) {
            if (level_ != 1) { onEdge(sample); level_ = 1; runStart_ = sample; }
            sample += 8;
            continue;
        }
        for (const Run& run : table[b]) {
            if (level_ != run.bit) {
                onEdge(sample);
                level_ = run.bit;
                runStart_ = sample;
            }
            sample += run.len;
        }
    }
    sample_ = sample;
}

BmcDecoder::BmcDecoder(double sampleRate, int minEdges)
    : sampleRate_(sampleRate), minEdges_(minEdges) {
    threshold_ = static_cast<uint64_t>(std::max(1.0, std::round(kThresholdUs * sampleRate / 1e6)));
    maxbit_ = static_cast<uint64_t>(std::max(1.0, std::round(kMaxbitUs * sampleRate / 1e6)));
    reset();
}

void BmcDecoder::reset() {
    hasStart_ = false;
    startsample_ = 0;
    previous_ = 0;
    bits_.clear();
    edges_.clear();
    bad_.clear();
    halfOne_ = false;
    startOne_ = 0;
}

void BmcDecoder::restart(uint64_t sample, bool flush) {
    hasStart_ = !flush;
    startsample_ = sample;
    bits_.clear();
    edges_.clear();
    bad_.clear();
    halfOne_ = false;
    startOne_ = 0;
    previous_ = sample;
}

std::optional<BmcRawPacket> BmcDecoder::emit() {
    if (static_cast<int>(edges_.size()) < minEdges_) return std::nullopt;
    const uint64_t ss = edges_.front();
    const uint64_t es = edges_.back();
    if (!(es > ss)) return std::nullopt;

    BmcRawPacket p;
    p.bitrate = static_cast<uint64_t>(
        std::llround(sampleRate_ * static_cast<double>(bits_.size()) / static_cast<double>(es - ss)));
    p.startSample = startsample_;
    p.endSample = es;
    p.bits = std::move(bits_);
    p.edges = std::move(edges_);
    p.bad = std::move(bad_);
    return p;
}

std::optional<BmcRawPacket> BmcDecoder::pushEdge(uint64_t sample, bool flush) {
    if (!hasStart_) {
        hasStart_ = true;
        startsample_ = sample;
        previous_ = sample;
        return std::nullopt;
    }

    const uint64_t diff = sample - previous_;

    // 长时间空闲 => 视为包结束
    if (diff > maxbit_ || flush) {
        if (!flush) edges_.push_back(previous_);
        auto packet = emit();
        restart(sample, flush);
        return packet;
    }

    // 位累积超限 => 不是 PD 报文，就地丢弃并重新对齐
    if (bits_.size() >= kMaxPacketBits) {
        restart(sample, false);
        return std::nullopt;
    }

    const bool isZero = diff > threshold_;
    if (isZero && !halfOne_) {
        bits_.push_back(0);
        edges_.push_back(previous_);
    } else if (!isZero && halfOne_) {
        bits_.push_back(1);
        edges_.push_back(startOne_);
        halfOne_ = false;
    } else if (!isZero && !halfOne_) {
        halfOne_ = true;
        startOne_ = previous_;
    } else {
        bad_.emplace_back(startOne_, previous_);
        bits_.push_back(0);
        edges_.push_back(previous_);
        halfOne_ = false;
    }
    previous_ = sample;
    return std::nullopt;
}

/* ────────────────────────── 采样率反推 ────────────────────────── */

std::vector<uint32_t> collectRunStats(const uint8_t* data, size_t len,
                                      uint32_t maxRun, size_t maxRuns) {
    std::vector<uint32_t> runs;
    runs.reserve(std::min<size_t>(maxRuns, 4096));

    int level = -1;
    uint32_t cur = 0;
    auto pushRun = [&](uint32_t n) {
        if (n >= 2 && n <= maxRun) runs.push_back(n);
    };

    const auto& table = byteRuns().lsb;   // 采样率反推只按 LSB（与实测一致）

    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b == 0 || b == 0xFF) {
            const int bit = (b == 0) ? 0 : 1;
            if (bit == level) cur += 8;
            else { if (level != -1) pushRun(cur); level = bit; cur = 8; }
            continue;
        }
        for (const Run& run : table[b]) {
            if (run.bit == level) cur += run.len;
            else { if (level != -1) pushRun(cur); level = run.bit; cur = run.len; }
        }
        if (runs.size() >= maxRuns) break;
    }
    if (level != -1 && runs.size() < maxRuns) pushRun(cur);
    return runs;
}

std::optional<UiEstimate> estimateUiSamples(const std::vector<uint32_t>& runs) {
    if (runs.size() < 200) return std::nullopt;

    // ① 粗搜候选 UI
    std::vector<uint32_t> sub;
    if (runs.size() > 4000) {
        sub.resize(4000);
        for (size_t i = 0; i < 4000; ++i) {
            sub[i] = runs[(i * runs.size()) / 4000];
        }
    } else {
        sub = runs;
    }
    const double subN = static_cast<double>(sub.size());

    double bestUi = 0, bestScore = 0;
    for (double ui = 1.2; ui <= 60.0; ui += 0.05) {
        const double tol = ui * 0.3;
        int hit = 0, nS = 0, nL = 0;
        for (uint32_t r : sub) {
            const double d = std::min(std::fabs(static_cast<double>(r) - ui),
                                      std::fabs(static_cast<double>(r) - 2.0 * ui));
            if (d >= tol) continue;
            hit++;
            if (static_cast<double>(r) < ui * 1.5) nS++; else nL++;
        }
        if (static_cast<double>(hit) / subN < 0.9) continue;
        if (static_cast<double>(nS) < subN * 0.1 || static_cast<double>(nL) < subN * 0.1) continue;
        const double score = static_cast<double>(hit) / subN;
        if (score > bestScore) { bestScore = score; bestUi = ui; }
    }
    if (!(bestUi > 0)) return std::nullopt;

    // ② 精修：Σ游程 = UI × (nShort + 2×nLong)
    double ui = bestUi;
    int nS = 0, nL = 0;
    for (int it = 0; it < 8; ++it) {
        const double cut = 1.5 * ui;
        nS = 0; nL = 0;
        double total = 0;
        for (uint32_t r : runs) {
            const double d = static_cast<double>(r);
            if (d < ui * 0.55 || d > ui * 3.2) continue;
            total += d;
            if (d < cut) nS++; else nL++;
        }
        if (nS + nL < 100) return std::nullopt;
        const double next = total / static_cast<double>(nS + 2 * nL);
        if (!(next > 0)) return std::nullopt;
        if (std::fabs(next - ui) < 1e-4) { ui = next; break; }
        ui = next;
    }

    // ③ 置信度
    int good = 0;
    for (uint32_t r : runs) {
        const double d = static_cast<double>(r);
        if (std::min(std::fabs(d - ui), std::fabs(d - 2 * ui)) < ui * 0.35) good++;
    }
    const double confidence = static_cast<double>(good) / static_cast<double>(runs.size());
    if (confidence < 0.85 || nS < 50 || nL < 50) return std::nullopt;

    UiEstimate e;
    e.uiSamples = ui;
    e.nShort = nS;
    e.nLong = nL;
    e.confidence = confidence;
    e.used = nS + nL;
    return e;
}

std::optional<SampleRateEstimate> estimateSampleRate(const std::vector<uint32_t>& runs) {
    auto e = estimateUiSamples(runs);
    if (!e) return std::nullopt;
    const double sampleRate = std::round(e->uiSamples * kBmcHz);
    if (!(sampleRate >= 100000 && sampleRate <= 50000000)) return std::nullopt;
    SampleRateEstimate out;
    out.sampleRate = sampleRate;
    out.uiSamples = e->uiSamples;
    out.confidence = e->confidence;
    out.nShort = e->nShort;
    out.nLong = e->nLong;
    return out;
}

}  // namespace pdscope
