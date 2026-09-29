// tables.h — USB PD 协议常量表
//
// 对应 JS 源：src/js/pd/tables.js
// 表里只放「规范原文怎么写」，不做任何推导；推导逻辑在各自的解析模块里。

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace pdscope { namespace pd {

/* ── 表查询辅助：未命中返回默认文案 ── */
inline std::string tbl(const std::map<int, std::string>& m, int k,
                       const std::string& d = "Reserved") {
  auto it = m.find(k);
  return it == m.end() ? d : it->second;
}

/* ══════════════════ 消息类型 ════════════════ */

extern const std::map<int, std::string> CTRL_TYPES;
extern const std::map<int, std::string> DATA_TYPES;
extern const std::map<int, std::string> EXT_TYPES;

/** 控制消息里已废弃、只能被 Not_Supported 回应的类型。 */
extern const std::map<std::string, std::string> CTRL_DEPRECATED;

/** 消息类型 → 该类型所在最低 PD 版本。 */
extern const std::map<int, double> CTRL_MIN_REV;
extern const std::map<int, double> DATA_MIN_REV;
extern const std::map<int, double> EXT_MIN_REV;

/** 消息类型 → 界面分类。 */
extern const std::map<std::string, std::string> MSG_CATEGORY;

/* ══════════════════ 规格版本 ════════════════ */

extern const std::map<int, std::string> SPEC_REV;

/** 修订号文本 → 可比较的数值（3.x 记作 3.0）。 */
double revTextNum(const std::string& text);

/** 扩展消息长度参数。 */
struct ExtMsgLimits { int maxLen; int chunkLen; int legacyLen; };
extern const ExtMsgLimits EXT_MSG_LIMITS;

/* ══════════════════ 数据对象 ════════════════ */

extern const std::map<int, std::string> BIST_MODES_V3;
extern const std::map<int, std::string> BIST_MODES_V2;
extern const std::map<int, std::string> CHARGE_STATE;

extern const std::map<int, std::string> USB_MODE;
extern const std::string USB_MODE_UNKNOWN;
extern const std::map<int, std::string> USB_SPEED;
extern const std::string USB_SPEED_UNKNOWN;
extern const std::map<int, std::string> CABLE_TYPE;
extern const std::map<int, std::string> CABLE_CURRENT_EUDO;

extern const std::map<int, std::string> EXT_ALERT_EVENT;

extern const std::map<int, std::string> EPR_MODE_ACTION;
extern const std::map<int, std::string> EPR_MODE_DATA;

extern const std::map<int, std::string> EXT_CONTROL_MSG_TYPES;

struct PeakInfo {
  std::string code;
  std::string summary;
  std::vector<std::string> steps;
};
extern const std::map<int, PeakInfo> PEAK_CURRENT_DETAILS;

extern const std::map<int, std::string> FRS_CURRENT;

extern const std::map<int, std::string> POWER_STATE;
extern const std::map<int, std::string> STATE_INDICATOR;
extern const std::map<int, std::string> TEMP_STATUS;

/** 电池槽/电池位文本。 */
std::string batteryRefText(int n);

extern const std::map<int, std::string> LOAD_STEP;
extern const std::map<int, std::string> TOUCH_TEMP_SOURCE;
extern const std::map<int, std::string> TOUCH_TEMP_SINK;

/* ══════════════════ VDM ════════════════ */

extern const std::map<int, std::string> VDM_CMDS;
constexpr int VDM_CMD_SVID_MIN = 16;
extern const char* VDM_ACK[4];
extern const std::map<int, std::string> VDM_VER_MAJOR;
extern const std::map<int, std::string> VDM_VER_MINOR;

extern const std::map<int, std::string> PRODUCT_TYPE_UFP;
extern const std::map<int, std::string> PRODUCT_TYPE_CABLE;
extern const std::map<int, std::string> PRODUCT_TYPE_DFP;
extern const std::map<int, std::string> CONNECTOR_TYPE;

/** PRODUCT_TYPE_VDO_KIND：返回产品或 null（返回 nullptr 表示 null）。 */
const std::string* productTypeVdoKind(const std::string& link, int pt);

extern const std::map<int, std::string> UFP_VDO_VERSION;
extern const std::map<int, std::string> DFP_VDO_VERSION;
extern const std::map<int, std::string> UFP_USB2;
extern const std::map<int, std::string> USB_HIGHEST_SPEED;
extern const std::map<int, std::string> USB_HIGHEST_SPEED_V30;
extern const std::map<int, std::string> VCONN_POWER;

extern const std::map<int, std::string> CABLE_CONNECTOR;
extern const std::map<int, std::string> CABLE_TERMINATION_PASSIVE;
extern const std::map<int, std::string> CABLE_TERMINATION_ACTIVE;
extern const std::map<int, std::string> CABLE_LATENCY_PASSIVE;
extern const std::map<int, std::string> CABLE_LATENCY_ACTIVE;
extern const std::map<int, std::string> CABLE_VBUS_VOLTAGE_V30;
extern const std::map<int, std::string> CABLE_VBUS_VOLTAGE_V31;
extern const std::map<int, std::string> CABLE_VBUS_CURRENT;
extern const std::map<int, std::string> CABLE_VBUS_CURRENT_ACTIVE;
extern const std::map<int, std::string> U3_CLD_POWER;
extern const std::map<int, std::string> VPD_VBUS_VOLTAGE;
extern const std::map<int, std::string> AMA_SUPERSPEED_V30;

/* ══════════════════ 逐位表 ════════════════ */

struct BitName { int bit; std::string name; };
extern const std::vector<BitName> ALERT_BITS;
extern const std::vector<BitName> STATUS_EVENT_BITS;
extern const std::vector<BitName> POWER_STATUS_BITS;

}}  // namespace pdscope::pd
