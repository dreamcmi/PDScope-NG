#include "atkcc.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace pdscope {
namespace {

inline bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/** 键名合法：`[A-Za-z_][\w .\-]*` */
bool isKeyChar(char c) {
    return isWordChar(c) || c == ' ' || c == '.' || c == '-';
}

/** 从 s 的 pos 起跳过空白，返回新位置。 */
size_t skipWs(const std::string& s, size_t p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    return p;
}

/** 解析 `[0-9]+(\.[0-9]+)?`，成功返回 true 并推进 p。 */
bool parseNumber(const std::string& s, size_t& p, double& out) {
    size_t b = p;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p == b) return false;
    if (p < s.size() && s[p] == '.') {
        size_t dot = p;
        ++p;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        if (p == dot + 1) return false;   // 有小数点但没数字
    }
    out = std::strtod(s.substr(b, p - b).c_str(), nullptr);
    return true;
}

/** 无符号整数（用于 sample= 之后的序号） */
bool parseUInt(const std::string& s, size_t& p, uint64_t& out) {
    size_t b = p;
    uint64_t v = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
        v = v * 10 + static_cast<uint64_t>(s[p] - '0');
        ++p;
    }
    if (p == b) return false;
    out = v;
    return true;
}

/** 带符号小数：`-?[\d.]+` */
bool parseSignedNumber(const std::string& s, size_t& p, double& out) {
    size_t b = p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
    bool any = false;
    while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) {
        ++p;
        any = true;
    }
    if (!any) { p = b; return false; }
    out = std::strtod(s.substr(b, p - b).c_str(), nullptr);
    return true;
}

/** 在 s 里按大小写不敏感匹配字面量 word（不在词首做边界要求）。 */
bool matchWordCI(const std::string& s, size_t& p, const char* word) {
    size_t n = std::strlen(word);
    if (p + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[p + i])) != word[i]) return false;
    }
    p += n;
    return true;
}

}  // namespace

SampleRateInfo parseSampleRate(const std::string& text) {
    SampleRateInfo fb;
    if (text.empty()) return fb;

    for (const std::string& rawLine : splitLines(text)) {
        // 找行内第一个 '=' 或 ':' —— 键名不含这两者，所以它就是键的终点
        size_t sep = rawLine.find_first_of("=:");
        if (sep == std::string::npos) continue;

        std::string key = trim(rawLine.substr(0, sep));
        if (key.empty()) continue;
        // ^[A-Za-z_][\w .\-]*$
        if (!(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) continue;
        bool okKey = true;
        for (char c : key) {
            if (!isKeyChar(c)) { okKey = false; break; }
        }
        if (!okKey) continue;

        // 归一化：小写并去掉 [\s._\-]
        std::string norm;
        for (char c : key) {
            char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (l == ' ' || l == '.' || l == '_' || l == '-') continue;
            norm.push_back(l);
        }
        // 只认像采样率的键
        if (norm.find("freq") == std::string::npos &&
            norm.find("rate") == std::string::npos &&
            norm.find("hz") == std::string::npos) {
            continue;
        }

        // 分隔符之后：\s*NUM\s*UNIT\s*$
        size_t p = skipWs(rawLine, sep + 1);
        double value = 0;
        if (!parseNumber(rawLine, p, value)) continue;
        if (!(value > 0)) continue;
        p = skipWs(rawLine, p);

        size_t ub = p;
        while (p < rawLine.size() &&
               (std::isalpha(static_cast<unsigned char>(rawLine[p])) || rawLine[p] == '/')) {
            ++p;
        }
        std::string unit = lower(rawLine.substr(ub, p - ub));
        p = skipWs(rawLine, p);
        if (p != rawLine.size()) continue;   // 行尾还有别的东西 → 不匹配

        if (unit.empty()) {
            if (norm.find("khz") != std::string::npos) unit = "khz";
            else if (norm.find("mhz") != std::string::npos) unit = "mhz";
        }

        double hz;
        if (unit == "khz" || unit == "k") hz = value * 1e3;
        else if (unit == "mhz" || unit == "m") hz = value * 1e6;
        else if (unit == "hz") hz = value;
        else hz = (value >= 100000) ? value : value * 1e3;

        if (!(hz >= 10000 && hz <= 1e9)) continue;   // 数量级明显不对的不当采样率

        SampleRateInfo out;
        out.hz = hz;
        out.source = "declared";
        out.key = key;
        out.value = value;
        out.unit = !unit.empty() ? unit : (hz == value ? "hz" : "khz");
        out.raw = trim(rawLine);
        return out;
    }
    return fb;
}

AtkccCapture AtkccCapture::open(const Bytes& bytes) {
    AtkccCapture cap;
    // 文件级只拷贝这一次：ZipReader 与被移动的容器对象共享同一份字节，
    // 于是「容器构造时读 ini 正常、一解码读块就报越界」这类悬垂引用不会发生。
    cap.zip_ = std::make_unique<ZipReader>(std::make_shared<const Bytes>(bytes));

    // ── channel.ini：采样率 ──
    std::string rootIni, subIni;
    cap.zip_->readText("channel.ini", rootIni);
    SampleRateInfo rate = parseSampleRate(rootIni);
    cap.zip_->readText("0/channel.ini", subIni);
    if (rate.source != "declared") {
        SampleRateInfo alt = parseSampleRate(subIni);
        if (alt.source == "declared") rate = alt;
    }

    // ── bus.ini ──
    std::string busIni;
    cap.zip_->readText("bus.ini", busIni);
    std::vector<BusPoint> bus;
    if (!busIni.empty()) {
        for (const std::string& line : splitLines(busIni)) {
            std::string t = trim(line);
            if (t.empty()) continue;
            // sample\s*=\s*(\d+)\s*,\s*vbus\s*=\s*(-?[\d.]+)\s*,\s*ibus\s*=\s*(-?[\d.]+)
            size_t p = 0;
            bool hit = false;
            while (p < t.size()) {
                size_t save = p;
                if (!matchWordCI(t, p, "sample")) { p = save + 1; continue; }
                size_t q = skipWs(t, p);
                if (q >= t.size() || t[q] != '=') { p = save + 1; continue; }
                q = skipWs(t, q + 1);
                uint64_t sv = 0;
                if (!parseUInt(t, q, sv)) { p = save + 1; continue; }
                q = skipWs(t, q);
                if (q >= t.size() || t[q] != ',') { p = save + 1; continue; }
                q = skipWs(t, q + 1);
                if (!matchWordCI(t, q, "vbus")) { p = save + 1; continue; }
                q = skipWs(t, q);
                if (q >= t.size() || t[q] != '=') { p = save + 1; continue; }
                q = skipWs(t, q + 1);
                double vv = 0;
                if (!parseSignedNumber(t, q, vv)) { p = save + 1; continue; }
                q = skipWs(t, q);
                if (q >= t.size() || t[q] != ',') { p = save + 1; continue; }
                q = skipWs(t, q + 1);
                if (!matchWordCI(t, q, "ibus")) { p = save + 1; continue; }
                q = skipWs(t, q);
                if (q >= t.size() || t[q] != '=') { p = save + 1; continue; }
                q = skipWs(t, q + 1);
                double iv = 0;
                if (!parseSignedNumber(t, q, iv)) { p = save + 1; continue; }
                bus.push_back(BusPoint{sv, vv, iv});
                hit = true;
                break;
            }
            (void)hit;
        }
    }

    // ── 各通道分块清单：^(\d+)/(\d+)-(\d+)\.bin$ ──
    std::map<int, AtkccChannel> channelMap;
    for (const ZipEntry& e : cap.zip_->entries()) {
        const std::string& nm = e.name;
        size_t slash = nm.find('/');
        if (slash == std::string::npos || slash == 0) continue;
        size_t dash = nm.find('-', slash + 1);
        if (dash == std::string::npos) continue;
        if (nm.size() < 4) continue;
        if (lower(nm.substr(nm.size() - 4)) != ".bin") continue;

        const std::string folder = nm.substr(0, slash);
        const std::string chStr = nm.substr(slash + 1, dash - slash - 1);
        const std::string idxStr = nm.substr(dash + 1, nm.size() - 4 - (dash + 1));
        if (folder.empty() || chStr.empty() || idxStr.empty()) continue;
        bool allDigits = true;
        for (char c : folder) if (!std::isdigit(static_cast<unsigned char>(c))) allDigits = false;
        for (char c : chStr) if (!std::isdigit(static_cast<unsigned char>(c))) allDigits = false;
        for (char c : idxStr) if (!std::isdigit(static_cast<unsigned char>(c))) allDigits = false;
        if (!allDigits) continue;

        const int ch = std::atoi(chStr.c_str());
        const int idx = std::atoi(idxStr.c_str());

        AtkccChannel& c = channelMap[ch];
        if (c.chunks.empty() && c.folder.empty()) c.folder = folder;
        c.channel = ch;
        c.chunks.push_back(AtkccChunk{idx, nm, e.uncompressedSize});
    }

    for (auto& kv : channelMap) {
        AtkccChannel& c = kv.second;
        std::sort(c.chunks.begin(), c.chunks.end(),
                  [](const AtkccChunk& a, const AtkccChunk& b) { return a.idx < b.idx; });
        c.totalBytes = 0;
        for (const AtkccChunk& k : c.chunks) c.totalBytes += k.size;
        c.totalSamples = c.totalBytes * 8;
    }

    // ── 总采样数 ──
    uint64_t totalSamples = 0;
    if (!subIni.empty()) {
        std::vector<std::string> lines;
        for (const std::string& l : splitLines(subIni)) {
            std::string t = trim(l);
            if (!t.empty()) lines.push_back(t);
        }
        if (lines.size() >= 2) {
            // Number(lines[1]) || 0
            char* endp = nullptr;
            double v = std::strtod(lines[1].c_str(), &endp);
            totalSamples = (endp != lines[1].c_str() && std::isfinite(v) && v > 0)
                               ? static_cast<uint64_t>(v) : 0;
        }
    }

    std::vector<int> order;
    order.reserve(channelMap.size());
    for (const auto& kv : channelMap) order.push_back(kv.first);
    std::sort(order.begin(), order.end());
    if (totalSamples == 0) {
        totalSamples = order.empty() ? 0 : channelMap[order[0]].totalSamples;
    }

    cap.meta_.sampleRate = rate.hz;
    cap.meta_.sampleRateSource = rate.source;
    cap.meta_.sampleRateKey = rate.key;
    cap.meta_.sampleRateRaw = rate.raw;
    cap.meta_.samplingFrequencyRaw = rate.value;
    cap.meta_.rawChannelIni = subIni;
    cap.meta_.totalSamples = totalSamples;
    cap.meta_.channelOrder = order;
    cap.meta_.channelMap = std::move(channelMap);
    cap.meta_.bus = std::move(bus);
    cap.meta_.entryCount = cap.zip_->entries().size();
    cap.warning_ = cap.zip_->lastWarning();
    return cap;
}

bool AtkccCapture::readChunk(int channel, size_t chunkIdx, Bytes& out) const {
    auto it = meta_.channelMap.find(channel);
    if (it == meta_.channelMap.end()) return false;
    if (chunkIdx >= it->second.chunks.size()) return false;
    return zip_->read(it->second.chunks[chunkIdx].name, out);
}

uint64_t AtkccCapture::effectiveSampleLimit(int channel) const {
    auto it = meta_.channelMap.find(channel);
    if (it == meta_.channelMap.end()) return 0;
    if (meta_.channelOrder.size() == 1) {
        return std::min(meta_.totalSamples, it->second.totalSamples);
    }
    return it->second.totalSamples;   // 多通道：交给分块级裁剪
}

size_t AtkccCapture::trimTrailingZeros(const Bytes& data) {
    size_t end = data.size();
    while (end > 0 && data[end - 1] == 0) --end;
    return end;
}

ChannelActivity AtkccCapture::scanActivity(int channel, int maxChunks) const {
    ChannelActivity r;
    r.channel = channel;
    auto it = meta_.channelMap.find(channel);
    if (it == meta_.channelMap.end()) return r;

    uint64_t idle = 0, live = 0, edge = 0;
    const int n = std::min<int>(maxChunks, static_cast<int>(it->second.chunks.size()));
    Bytes d;
    for (int i = 0; i < n; ++i) {
        if (!readChunk(channel, static_cast<size_t>(i), d)) continue;
        for (uint8_t b : d) {
            if (b != 0xFF) r.activity++;
            if (b == 0xFF) idle++;
            else if (b == 0x00) live++;
            else edge++;
        }
        r.bytes += d.size();
    }
    r.scannedChunks = n;
    r.idleRatio = r.bytes ? static_cast<double>(idle) / static_cast<double>(r.bytes) : 0.0;
    r.liveRatio = r.bytes ? static_cast<double>(live) / static_cast<double>(r.bytes) : 0.0;
    r.edgeLike = r.bytes ? static_cast<double>(edge) / static_cast<double>(r.bytes) : 0.0;
    return r;
}

}  // namespace pdscope
