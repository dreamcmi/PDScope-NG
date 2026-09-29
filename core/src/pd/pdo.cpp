// pdo.cpp — 见 pdo.h

#include "pdo.h"
#include "decoder.h"
#include "format.h"
#include "tables.h"

namespace pdscope { namespace pd {

namespace {

const double V_STEP = 0.05;       // 电压 50 mV
const double V_STEP_AVS = 0.1;    // AVS 电压 100 mV
const double I_STEP = 0.01;       // 电流 10 mA
const double I_STEP_PPS = 0.05;   // PPS/AVS 请求电流 50 mA
const double I_STEP_AVS_MAX = 0.01;
const double P_STEP_BATT = 0.25;  // 电池 PDO 功率 250 mW

inline std::string hxv(uint32_t v) { return pdHexVar(v); }   // 不补零大写十六进制

const std::map<std::string, std::string> PDO_KIND_TITLE = {
  {"Fixed", "Fixed 固定电源"},
  {"EPR_Fixed", "EPR_Fixed 扩展固定电源"},
  {"Battery", "Battery 电池"},
  {"EPR_Battery", "EPR_Battery 扩展电池"},
  {"Variable", "Variable 可变电源"},
  {"EPR_Variable", "EPR_Variable 扩展可变电源"},
  {"PPS", "PPS 可编程电源"},
  {"EPR_AVS", "EPR_AVS 扩展可调电压"},
  {"SPR_AVS", "SPR_AVS 标准可调电压"},
  {"Reserved", "Reserved 保留"},
  {"Reserved_APDO", "Reserved_APDO 保留"},
};

void emitPeakCurrent(pdscope::DetailEmitter& em, const std::string& field,
                     int mode, const std::string& range) {
  auto it = PEAK_CURRENT_DETAILS.find(mode);
  const PeakInfo& info = (it != PEAK_CURRENT_DETAILS.end()) ? it->second : PEAK_CURRENT_DETAILS.at(0);
  em.detail(field + " [" + range + "]", info.code + " " + info.summary);
  for (size_t i = 0; i < info.steps.size(); i++)
    em.detail(field + " 档位 " + std::to_string(i + 1), info.steps[i]);
}

std::string pdoKindName(uint32_t pdo, bool isEpr) {
  const std::string pre = isEpr ? "EPR_" : "";
  const int t1 = static_cast<int>(pdField(pdo, 31, 30));
  if (t1 == 0) return pre + "Fixed";
  if (t1 == 1) return pre + "Battery";
  if (t1 == 2) return pre + "Variable";
  const int t2 = static_cast<int>(pdField(pdo, 29, 28));
  if (t2 == 0) return "PPS";
  if (t2 == 1) return "EPR_AVS";
  if (t2 == 2) return "SPR_AVS";
  return "Reserved_APDO";
}

std::string summaryShort(const std::string& s) {
  std::string r = s;
  if (!r.empty() && r[0] == '[') {
    size_t pos = r.find(']');
    if (pos != std::string::npos) {
      r = r.substr(pos + 1);
      size_t w = r.find_first_not_of(" \t");
      if (w != std::string::npos) r = r.substr(w);
    }
  }
  if (!r.empty() && r.back() == ')') {
    size_t pos = r.find('(');
    if (pos != std::string::npos) r = r.substr(0, pos);
    size_t w = r.find_last_not_of(" \t");
    if (w != std::string::npos) r = r.substr(0, w + 1);
    else r = "";
  }
  return r;
}

}  // namespace

std::string pdoParse(PdState& st, pdscope::DetailEmitter& em, uint32_t pdo,
                     const std::string& role, int position, bool isEpr,
                     const std::string& revText) {
  const std::string roleName = (role == "source") ? "Source" : "Sink";
  const int t1 = static_cast<int>(pdField(pdo, 31, 30));

  const std::string name = pdoKindName(pdo, isEpr);
  em.object("PDO #" + std::to_string(position) + " · "
            + (PDO_KIND_TITLE.count(name) ? PDO_KIND_TITLE.at(name) : name) + "（" + roleName + "）");
  em.detail("原始值", "0x" + pdHex(pdo));
  em.detail("功率范围", isEpr ? "EPR（扩展功率范围）" : "SPR（标准功率范围）");
  em.detail("角色", roleName);

  PdoMeta meta;
  meta.type = t1;
  meta.role = role;
  meta.isEpr = isEpr;
  meta.kind = "unknown";
  meta.position = position;
  std::string summary = "[" + name + "] [raw: 0x" + pdHex(pdo) + "]";

  if (t1 == 0) {
    const double mv = pdField(pdo, 19, 10) * V_STEP;
    const double ma = pdField(pdo, 9, 0) * I_STEP;
    em.detail("Supply Type [B31-30]", "00b Fixed Supply PDO");
    em.detail("电压 [B19-10]", pdNum(mv) + " V");
    em.detail(role == "source" ? "最大电流 [B9-0]" : "工作电流 [B9-0]", pdNum(ma) + " A");
    summary = "[" + name + "] " + pdNum(mv) + "V " + pdNum(ma) + "A (" + pdNum(mv * ma) + "W)";

    const bool carry5vFlags = (position == 1) && !isEpr;
    if (carry5vFlags) {
      em.detail("Dual-Role Power [B29]", pdFlag(pdBit(pdo, 29), "可通过 PR_Swap 换电源角色", "不可换"));
      em.detail(role == "source" ? "USB Suspend Supported [B28]" : "Higher Capability [B28]",
        role == "source"
          ? pdFlag(pdBit(pdo, 28), "Sink 需按 USB 规范挂起/唤醒", "无需遵循挂起规则")
          : pdFlag(pdBit(pdo, 28), "Sink 需要比 5V PDO 更多的电量", "否"));
      em.detail("Unconstrained Power [B27]", pdFlag(pdBit(pdo, 27),
        role == "source" ? "外部电源充足，可全力供电" : "外部电源不足，受自身功耗限制"));
      em.detail("USB Communications Capable [B26]", pdFlag(pdBit(pdo, 26)));
      em.detail("Dual-Role Data [B25]", pdFlag(pdBit(pdo, 25), "可通过 DR_Swap 换数据角色", "不可换"));
      if (role == "source") {
        em.detail("Unchunked Extended Messages [B24]", pdFlag(pdBit(pdo, 24), "支持分块与不分块", "仅支持分块"));
        em.detail("EPR Capable [B23]", pdFlag(pdBit(pdo, 23), "可进入 EPR 模式", "仅 SPR"));
        em.detail("Reserved [B22]", std::to_string(pdBit(pdo, 22)));
      } else {
        em.detail("FRS Current [B24-23]", FRS_CURRENT.at(pdField(pdo, 24, 23)));
        em.detail("Reserved [B22-20]", "0x" + hxv(pdField(pdo, 22, 20)));
      }
    } else if (isEpr) {
      em.detail("Reserved [B29-23]", "0x" + hxv(pdField(pdo, 29, 23)));
    } else {
      em.detail("Device Flags [B29-22]", "0x" + hxv(pdField(pdo, 29, 22)) + "（非首位 PDO 时为 Reserved）");
    }
    if (role == "source" || carry5vFlags) {
      emitPeakCurrent(em, "Peak Current", static_cast<int>(pdField(pdo, 21, 20)), "B21-20");
    } else {
      em.detail("Peak Current [B21-20]", "Reserved（Sink 侧不使用）");
    }
    em.detail("最大功率", pdNum(mv * ma) + " W");
    meta.kind = "fixed";
  } else if (t1 == 1) {
    const double minv = pdField(pdo, 19, 10) * V_STEP;
    const double maxv = pdField(pdo, 29, 20) * V_STEP;
    const double mw = pdField(pdo, 9, 0) * P_STEP_BATT;
    em.detail("Supply Type [B31-30]", "01b Battery PDO");
    em.detail("最高电压 [B29-20]", pdNum(maxv) + " V");
    em.detail("最低电压 [B19-10]", pdNum(minv) + " V");
    em.detail(role == "source" ? "最大可用功率 [B9-0]" : "工作功率 [B9-0]", pdNum(mw) + " W");
    summary = "[" + name + "] " + pdNum(minv) + "/" + pdNum(maxv) + "V " + pdNum(mw) + "W";
    meta.kind = "battery";
  } else if (t1 == 2) {
    const double minv = pdField(pdo, 19, 10) * V_STEP;
    const double maxv = pdField(pdo, 29, 20) * V_STEP;
    const double ma = pdField(pdo, 9, 0) * I_STEP;
    em.detail("Supply Type [B31-30]", "10b Variable Supply PDO");
    em.detail("最高电压 [B29-20]", pdNum(maxv) + " V");
    em.detail("最低电压 [B19-10]", pdNum(minv) + " V");
    em.detail(role == "source" ? "最大电流 [B9-0]" : "工作电流 [B9-0]", pdNum(ma) + " A");
    summary = "[" + name + "] " + pdNum(minv) + "/" + pdNum(maxv) + "V " + pdNum(ma) + "A";
    meta.kind = "variable";
  } else {
    const int t2 = static_cast<int>(pdField(pdo, 29, 28));
    meta.apdoType = t2;
    em.detail("Supply Type [B31-30]", "11b Augmented PDO");
    if (t2 == 0) {
      const double minv = pdField(pdo, 15, 8) * V_STEP_AVS;
      const double maxv = pdField(pdo, 24, 17) * V_STEP_AVS;
      const double ma = pdField(pdo, 6, 0) * I_STEP_PPS;
      const int limited = static_cast<int>(pdBit(pdo, 27));
      em.detail("APDO Type [B29-28]", "00b SPR PPS");
      em.detail("PPS Power Limited [B27]", pdFlag(limited, "电流上限由 PPS Power Limited 决定", "不受限"));
      em.detail("Reserved [B26-25]", "0x" + hxv(pdField(pdo, 26, 25)));
      em.detail("最高电压 [B24-17]", pdNum(maxv) + " V");
      em.detail("Reserved [B16]", std::to_string(pdBit(pdo, 16)));
      em.detail("最低电压 [B15-8]", pdNum(minv) + " V");
      em.detail("Reserved [B7]", std::to_string(pdBit(pdo, 7)));
      em.detail(role == "source" ? "最大电流 [B6-0]" : "所需电流 [B6-0]", pdNum(ma) + " A");
      em.detail("@Vmax 近似功率", pdNum(maxv * ma) + " W");
      summary = "[PPS] " + pdNum(minv) + "/" + pdNum(maxv) + "V " + pdNum(ma) + "A"
                + (limited ? " [limited]" : "");
      meta.kind = "pps";
    } else if (t2 == 1) {
      const double minv = pdField(pdo, 15, 8) * V_STEP_AVS;
      const double maxv = pdField(pdo, 25, 17) * V_STEP_AVS;
      const int pdp = static_cast<int>(pdField(pdo, 7, 0));
      em.detail("APDO Type [B29-28]", "01b EPR AVS（可调电压）");
      if (role == "source") {
        emitPeakCurrent(em, "Peak Current", static_cast<int>(pdField(pdo, 27, 26)), "B27-26");
      } else {
        em.detail("Reserved [B27-26]", "0x" + hxv(pdField(pdo, 27, 26)));
      }
      em.detail("最高电压 [B25-17]", pdNum(maxv) + " V");
      em.detail("Reserved [B16]", std::to_string(pdBit(pdo, 16)));
      em.detail("最低电压 [B15-8]", pdNum(minv) + " V");
      em.detail("PDP [B7-0]", std::to_string(pdp) + " W");
      em.detail("@Vmax 近似电流", pdNum(maxv > 0 ? pdp / maxv : 0) + " A");
      summary = "[EPR_AVS] " + pdNum(minv) + "~" + pdNum(maxv) + "V (" + std::to_string(pdp) + "W)";
      meta.kind = "epr_avs";
    } else if (t2 == 2) {
      const double c15 = pdField(pdo, 19, 10) * I_STEP_AVS_MAX;
      const double c20 = pdField(pdo, 9, 0) * I_STEP_AVS_MAX;
      em.detail("APDO Type [B29-28]", "10b SPR AVS（9~20V 可调）");
      if (role == "source") {
        emitPeakCurrent(em, "Peak Current", static_cast<int>(pdField(pdo, 27, 26)), "B27-26");
        em.detail("Reserved [B25-20]", "0x" + hxv(pdField(pdo, 25, 20)));
      } else {
        em.detail("Reserved [B27-20]", "0x" + hxv(pdField(pdo, 27, 20)));
      }
      em.detail("9V~15V 最大电流 [B19-10]", pdNum(c15) + " A");
      em.detail("15V~20V 最大电流 [B9-0]", c20 == 0 ? "0 A（最高只到 15V）" : pdNum(c20) + " A");
      summary = "[SPR_AVS] 9~20V  15V:" + pdNum(c15) + "A  20V:" + pdNum(c20) + "A";
      meta.kind = "spr_avs";
    } else {
      em.detail("APDO Type [B29-28]", "1" + std::to_string(t2) + "b Reserved");
      em.detail("原始值", "0x" + pdHex(pdo));
      summary = "[Reserved_APDO] [raw: 0x" + pdHex(pdo) + "]";
      meta.kind = "reserved_apdo";
    }
  }

  const std::string ref = name + " " + summaryShort(summary);
  auto& pdos = (role == "source") ? st.pdosSource : st.pdosSink;
  auto& metaMap = (role == "source") ? st.pdoMetaSource : st.pdoMetaSink;
  pdos[position] = ref;
  metaMap[position] = meta;
  return summary;
}

void rdoParse(PdState& st, pdscope::DetailEmitter& em, uint32_t rdo, bool /*isEpr*/) {
  const int pos = static_cast<int>(pdField(rdo, 31, 28));
  const bool posValid = (pos != 0) && (pos < 0x0E);
  em.object("RDO · 请求数据对象" + (posValid ? "（引用 PDO #" + std::to_string(pos) + "）" : ""));
  em.detail("原始值", "0x" + pdHex(rdo));

  if (!posValid) {
    em.detail("Object Position [" + pdRange(31, 28) + "]", std::to_string(pos) + " · 无效位置");
    const std::string s = "(RDO 位置 " + std::to_string(pos) + " 无效)";
    em.note(s);
    return;
  }

  const bool known = st.pdosSource.count(pos) > 0;
  const std::string ref = known ? st.pdosSource.at(pos) : "Unknown PDO";
  const std::string kind = known ? st.pdoMetaSource.at(pos).kind : "unknown";

  em.detail("Object Position [" + pdRange(31, 28) + "]", std::to_string(pos));
  em.detail("引用的 PDO", known ? ref : ("未捕获到 Source_Capabilities 的第 " + std::to_string(pos) + " 个 PDO"));
  em.detail("Giveback [B27]", pdFlag(pdBit(rdo, 27), "已置位（该位已废弃）", "未置位"));
  em.detail("Capability Mismatch [B26]", pdFlag(pdBit(rdo, 26), "能力不匹配", "匹配"));
  em.detail("USB Communications Cable [B25]", pdFlag(pdBit(rdo, 25), "是通信线缆", "否"));
  em.detail("No USB Suspend [B24]", pdFlag(pdBit(rdo, 24), "不遵循 USB 挂起", "遵循 USB 挂起"));
  em.detail("Unchunked Extended Messages [B23]", pdFlag(pdBit(rdo, 23), "支持分块与不分块", "仅支持分块"));
  em.detail("EPR Capable [B22]", pdFlag(pdBit(rdo, 22), "申请进入 EPR", "仅 SPR"));

  std::string s;
  if (kind == "pps") {
    const double ov = pdField(rdo, 20, 9) * 0.02;
    const double oa = pdField(rdo, 6, 0) * I_STEP_PPS;
    em.detail("Reserved [B21]", std::to_string(pdBit(rdo, 21)));
    em.detail("输出电压 [B20-9]", pdNum(ov) + " V");
    em.detail("Reserved [B8-7]", "0x" + hxv(pdField(rdo, 8, 7)));
    em.detail("工作电流 [B6-0]", pdNum(oa) + " A");
    s = "(RDO " + std::to_string(pos) + ": " + ref + ") 请求 " + pdNum(ov) + "V " + pdNum(oa) + "A";
  } else if (kind == "spr_avs" || kind == "epr_avs") {
    const int raw = static_cast<int>(pdField(rdo, 20, 9));
    const double ov = raw * 0.025;
    const double oa = pdField(rdo, 6, 0) * I_STEP_PPS;
    em.detail("Reserved [B21]", std::to_string(pdBit(rdo, 21)));
    em.detail("输出电压 [B20-9]", pdNum(ov) + " V"
      + ((raw & 0x3) ? ("（⚠ B10-9 = " + std::to_string(raw & 0x3) + "b，规范要求为 00b）") : "（B10-9 = 00b，步长等效 100mV）"));
    em.detail("Reserved [B8-7]", "0x" + hxv(pdField(rdo, 8, 7)));
    em.detail("工作电流 [B6-0]", pdNum(oa) + " A");
    s = "(RDO " + std::to_string(pos) + ": " + ref + ") 请求 " + pdNum(ov) + "V " + pdNum(oa) + "A";
  } else if (kind == "battery") {
    const double ow = pdField(rdo, 19, 10) * P_STEP_BATT;
    const double mw = pdField(rdo, 9, 0) * P_STEP_BATT;
    em.detail("Reserved [B21-20]", "0x" + hxv(pdField(rdo, 21, 20)));
    em.detail("工作功率 [B19-10]", pdNum(ow) + " W");
    em.detail("最大工作功率 [B9-0]", pdNum(mw) + " W（已废弃，应与工作功率相同）");
    s = "(RDO " + std::to_string(pos) + ": " + ref + ") 工作 " + pdNum(ow) + "W / 最大 " + pdNum(mw) + "W";
  } else {
    const double oa = pdField(rdo, 19, 10) * I_STEP;
    const double ma = pdField(rdo, 9, 0) * I_STEP;
    em.detail("Reserved [B21-20]", "0x" + hxv(pdField(rdo, 21, 20)));
    em.detail("工作电流 [B19-10]", pdNum(oa) + " A");
    em.detail("最大工作电流 [B9-0]", pdNum(ma) + " A（已废弃，应与工作电流相同）");
    s = "(RDO " + std::to_string(pos) + ": " + ref + ") 工作 " + pdNum(oa) + "A / 最大 " + pdNum(ma) + "A";
  }

  em.note(s);
}

PdoLookupResult lookupPdo(PdState& st, const std::string& role, int pos) {
  auto& pdos = (role == "source") ? st.pdosSource : st.pdosSink;
  auto& metaMap = (role == "source") ? st.pdoMetaSource : st.pdoMetaSink;
  bool known = pdos.count(pos) > 0;
  PdoLookupResult r;
  r.known = known;
  r.ref = known ? pdos.at(pos) : "Unknown PDO";
  r.kind = known ? metaMap.at(pos).kind : "unknown";
  return r;
}

}}  // namespace pdscope::pd
