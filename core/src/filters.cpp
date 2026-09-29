#include "filters.h"

#include <algorithm>

namespace pdscope {
namespace {

/** 大小写不敏感的「包含任一子串」。 */
bool containsAnyCI(const std::string& hay, std::initializer_list<const char*> needles) {
    const std::string h = lower(hay);
    for (const char* n : needles) {
        if (h.find(lower(n)) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

std::string toneOf(const Packet& p) {
    if (p.msgType == "VDM") return "VDM";
    if (p.msgKind == "custom") return "Custom";   // UFCS 厂家自定义消息
    if (p.msgKind == "ext") return "Extended";
    if (p.msgKind == "data") return "Data";
    if (p.msgKind == "special") return "Error";
    return "Control";
}

std::string packetKind(const Packet& p) {
    if (p.crcOk == CrcState::Bad) return "Error";
    return toneOf(p);
}

bool isAutoAck(const Packet& p, const std::string& protocol) {
    if (protocol == "UFCS") return p.msgType == "ACK" || p.msgType == "NCK";
    return p.msgType == "GoodCRC";
}

bool isPowerType(const std::string& msgType, const std::string& protocol) {
    if (protocol == "UFCS") {
        return containsAnyCI(msgType, {"Output_Capabilities", "Request", "Power_Change",
                                       "Power_Ready", "Accept", "Config_Watchdog"});
    }
    return containsAnyCI(msgType, {"Source_Cap", "Request", "EPR_Request", "EPR_Mode", "PPS",
                                   "BIST", "Source_Capabilities_Extended", "EPR_Source",
                                   "EPR_Sink", "Sink_Cap"});
}

bool isEnterType(const std::string& msgType, const std::string& protocol) {
    if (protocol == "UFCS") {
        return containsAnyCI(msgType, {"_Information", "Source_Info", "Sink_Info", "Cable_Info",
                                       "Device_Info", "Error_Info", "Get_"});
    }
    return containsAnyCI(msgType, {"PS_RDY", "VDM", "Alert", "Status", "Source_Info",
                                   "Revision", "Enter_USB", "Discover", "Sink_Cap", "Notify"});
}

std::vector<std::string> linkValues(const std::string& protocol) {
    if (protocol == "UFCS") return {"D+", "D-", "D±"};
    return {"SOP", "SOP'", "SOP''", "Hard Reset", "Cable Reset"};
}

std::vector<std::string> catValues(const std::string& protocol) {
    if (protocol == "UFCS") return {"Control", "Data", "Custom", "Error"};
    return {"Control", "Data", "Extended", "VDM", "Error"};
}

Filters newFilters(const std::string& protocol) {
    Filters f;
    f.roles = {"SRC", "SNK", "Plug"};
    const std::vector<std::string> lv = linkValues(protocol);
    f.sops.insert(lv.begin(), lv.end());
    const std::vector<std::string> cv = catValues(protocol);
    f.cats.insert(cv.begin(), cv.end());
    return f;
}

bool passesFilters(const Packet& p, const Filters& f, const std::string& protocol,
                   uint64_t totalSamples, ViewMode mode) {
    if (f.roles.find(p.role) == f.roles.end()) return false;
    if (f.sops.find(p.sop) == f.sops.end()) return false;
    if (f.cats.find(packetKind(p)) == f.cats.end()) return false;
    if (!f.types.empty() && f.types.find(p.msgType) == f.types.end()) return false;
    if (f.hideGoodCrc && isAutoAck(p, protocol)) return false;
    if (f.onlyBad && p.crcOk != CrcState::Bad) return false;
    if (f.onlyPower && !isPowerType(p.msgType, protocol)) return false;
    if (f.onlyEnter && !isEnterType(p.msgType, protocol)) return false;

    const uint64_t tb = totalSamples ? totalSamples : 1;
    const double t = static_cast<double>(p.startSample) / static_cast<double>(tb);
    if (t < f.tFrom - 1e-9 || t > f.tTo + 1e-9) return false;

    if (!f.q.empty()) {
        const std::string q = lower(f.q);
        const std::string hay = lower(p.msgType + " " + p.summary + " " + p.dataHex + " "
                                     + p.sop + " " + p.role);
        if (hay.find(q) == std::string::npos) return false;
    }

    if (mode == ViewMode::Neg &&
        !(isPowerType(p.msgType, protocol) || isEnterType(p.msgType, protocol))) {
        return false;
    }
    if (mode == ViewMode::Err && p.crcOk != CrcState::Bad) return false;
    return true;
}

namespace {

/** 排序取值：`null` 当 -1（与 JS 的 `if (x == null) x = -1` 一致）。 */
struct SortValue {
    bool isNumber = true;
    double num = -1;
    std::string str;
};

SortValue sortValueOf(const Packet& p, const std::string& k) {
    SortValue v;
    auto number = [&](double d) { v.isNumber = true; v.num = d; };
    auto text = [&](const std::string& s) { v.isNumber = false; v.str = s; };

    if (k == "index") number(static_cast<double>(p.index));
    else if (k == "seq") number(static_cast<double>(p.seq));
    else if (k == "timeMs") number(p.timeMs);
    else if (k == "startSample") number(static_cast<double>(p.startSample));
    else if (k == "endSample") number(static_cast<double>(p.endSample));
    else if (k == "durationUs") number(p.durationUs);
    else if (k == "bitrate") number(static_cast<double>(p.bitrate));
    else if (k == "nObjects") number(p.nObjects);
    else if (k == "dataLen") number(p.dataLen);
    else if (k == "sop") text(p.sop);
    else if (k == "msgType") text(p.msgType);
    else if (k == "role") text(p.role);
    else if (k == "kind") text(packetKind(p));
    else if (k == "summary") text(p.summary);
    else if (k == "msgId") { if (p.hasMsgId) number(p.msgId); else number(-1); }
    else if (k == "crcOk") {
        // JS 里 crcOk 是三态布尔；映射成 -1(null) / 0(false) / 1(true)
        if (p.crcOk == CrcState::Unrecorded) number(-1);
        else number(p.crcOk == CrcState::Ok ? 1 : 0);
    }
    else text(p.msgType);
    return v;
}

}  // namespace

std::vector<const Packet*> buildView(const std::vector<Packet>& packets, const Filters& f,
                                     const std::string& protocol, uint64_t totalSamples,
                                     ViewMode mode, const SortSpec& sort) {
    std::vector<const Packet*> out;
    out.reserve(packets.size());
    for (const Packet& p : packets) {
        if (passesFilters(p, f, protocol, totalSamples, mode)) out.push_back(&p);
    }

    const bool asc = sort.asc;
    std::stable_sort(out.begin(), out.end(), [&](const Packet* a, const Packet* b) {
        const SortValue x = sortValueOf(*a, sort.key);
        const SortValue y = sortValueOf(*b, sort.key);
        int cmp;
        if (x.isNumber && y.isNumber) cmp = (x.num < y.num) ? -1 : (x.num > y.num ? 1 : 0);
        else if (!x.isNumber && !y.isNumber) cmp = x.str.compare(y.str) < 0 ? -1 : (x.str == y.str ? 0 : 1);
        else cmp = x.isNumber ? -1 : 1;   // 类型不一致时保持稳定（实际不会发生）
        return asc ? (cmp < 0) : (cmp > 0);
    });
    return out;
}

}  // namespace pdscope
