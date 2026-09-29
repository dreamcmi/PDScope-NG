// tables.cpp — 见 tables.h

#include "tables.h"

#include <stdexcept>

namespace pdscope { namespace pd {

/* ══════════════════ 消息类型 ════════════════ */

const std::map<int, std::string> CTRL_TYPES = {
  {0, "Reserved"},
  {1, "GoodCRC"}, {2, "GotoMin"}, {3, "Accept"}, {4, "Reject"},
  {5, "Ping"}, {6, "PS_RDY"}, {7, "Get_Source_Cap"}, {8, "Get_Sink_Cap"},
  {9, "DR_Swap"}, {10, "PR_Swap"}, {11, "VCONN_Swap"}, {12, "Wait"}, {13, "Soft_Reset"},
  {14, "Data_Reset"}, {15, "Data_Reset_Complete"}, {16, "Not_Supported"},
  {17, "Get_Source_Cap_Extended"}, {18, "Get_Status"}, {19, "FR_Swap"},
  {20, "Get_PPS_Status"}, {21, "Get_Country_Codes"}, {22, "Get_Sink_Cap_Extended"},
  {23, "Get_Source_Info"}, {24, "Get_Revision"},
};

const std::map<int, std::string> DATA_TYPES = {
  {0, "Reserved"},
  {1, "Source_Cap"}, {2, "Request"}, {3, "BIST"}, {4, "Sink_Cap"},
  {5, "Battery_Status"}, {6, "Alert"}, {7, "Get_Country_Info"}, {8, "Enter_USB"},
  {9, "EPR_Request"}, {10, "EPR_Mode"}, {11, "Source_Info"}, {12, "Revision"},
  {13, "Reserved"}, {14, "Reserved"}, {15, "VDM"},
};

const std::map<int, std::string> EXT_TYPES = {
  {0, "Reserved"},
  {1, "Source_Capabilities_Extended"}, {2, "Status"}, {3, "Get_Battery_Cap"},
  {4, "Get_Battery_Status"}, {5, "Battery_Capabilities"}, {6, "Get_Manufacturer_Info"},
  {7, "Manufacturer_Info"}, {8, "Security_Request"}, {9, "Security_Response"},
  {10, "Firmware_Update_Request"}, {11, "Firmware_Update_Response"}, {12, "PPS_Status"},
  {13, "Country_Info"}, {14, "Country_Codes"}, {15, "Sink_Capabilities_Extended"},
  {16, "Extended_Control"}, {17, "EPR_Source_Capabilities"}, {18, "EPR_Sink_Capabilities"},
  {19, "Reserved"}, {20, "Reserved"}, {21, "Reserved"}, {22, "Reserved"}, {23, "Reserved"},
  {24, "Reserved"}, {25, "Reserved"}, {26, "Reserved"}, {27, "Reserved"}, {28, "Reserved"},
  {29, "Reserved"}, {30, "Vendor_Defined_Extended"}, {31, "Reserved"},
};

const std::map<std::string, std::string> CTRL_DEPRECATED = {
  {"GotoMin", "PD 3.0 起废弃，接收端应回 Not_Supported"},
  {"Ping", "PD 3.0 起废弃，接收端可回 Not_Supported 或忽略（线缆插头必须忽略）"},
};

const std::map<int, double> CTRL_MIN_REV = {
  {14, 3.0}, {15, 3.0}, {16, 3.0}, {17, 3.0}, {18, 3.0}, {19, 3.0}, {20, 3.0}, {21, 3.0}, {22, 3.0},
  {23, 3.1}, {24, 3.1},
};
const std::map<int, double> DATA_MIN_REV = {
  {5, 3.0}, {6, 3.0}, {7, 3.0}, {8, 3.0},
  {9, 3.1}, {10, 3.1}, {11, 3.1}, {12, 3.1},
};
const std::map<int, double> EXT_MIN_REV = {{16, 3.1}, {17, 3.1}, {18, 3.1}};

const std::map<std::string, std::string> MSG_CATEGORY = {
  {"GoodCRC", "handshake"},
  {"Ping", "handshake"},
  {"Source_Cap", "capability"},
  {"Sink_Cap", "capability"},
  {"Source_Capabilities_Extended", "capability"},
  {"Sink_Capabilities_Extended", "capability"},
  {"Source_Info", "capability"},
  {"EPR_Source_Capabilities", "capability"},
  {"EPR_Sink_Capabilities", "capability"},
  {"Request", "negotiate"},
  {"EPR_Request", "negotiate"},
  {"Accept", "negotiate"},
  {"Reject", "negotiate"},
  {"PS_RDY", "negotiate"},
  {"GotoMin", "negotiate"},
  {"Wait", "negotiate"},
  {"PPS_Status", "negotiate"},
  {"EPR_Mode", "negotiate"},
  {"Not_Supported", "negotiate"},
  {"Soft_Reset", "control"},
  {"Data_Reset", "control"},
  {"Data_Reset_Complete", "control"},
  {"DR_Swap", "control"},
  {"PR_Swap", "control"},
  {"VCONN_Swap", "control"},
  {"FR_Swap", "control"},
  {"Get_Source_Cap_Extended", "control"},
  {"Get_Sink_Cap_Extended", "control"},
  {"Get_Source_Info", "control"},
  {"Get_Status", "control"},
  {"Get_PPS_Status", "control"},
  {"Get_Country_Codes", "control"},
  {"Get_Country_Info", "control"},
  {"Get_Revision", "control"},
  {"Get_Battery_Cap", "control"},
  {"Get_Battery_Status", "control"},
  {"Battery_Capabilities", "control"},
  {"Get_Manufacturer_Info", "control"},
  {"Extended_Control", "control"},
  {"Revision", "control"},
  {"VDM", "vendor"},
  {"BIST", "data"},
  {"Battery_Status", "data"},
  {"Status", "data"},
  {"Country_Codes", "data"},
  {"Country_Info", "data"},
  {"Manufacturer_Info", "data"},
  {"Enter_USB", "data"},
  {"Alert", "alert"},
  {"Security_Request", "security"},
  {"Security_Response", "security"},
  {"Firmware_Update_Request", "security"},
  {"Firmware_Update_Response", "security"},
  {"Vendor_Defined_Extended", "vendor"},
};

/* ══════════════════ 规格版本 ════════════════ */

const std::map<int, std::string> SPEC_REV = {
  {0, "1.0"}, {1, "2.0"}, {2, "3.x"}, {3, "Reserved（11b）"},
};

double revTextNum(const std::string& text) {
  if (text == "3.x") return 3;
  try {
    size_t pos = 0;
    double n = std::stod(text, &pos);
    if (pos != text.size()) return 0;
    return n;
  } catch (...) {
    return 0;
  }
}

const ExtMsgLimits EXT_MSG_LIMITS = {260, 26, 26};

/* ══════════════════ 数据对象 ════════════════ */

const std::map<int, std::string> BIST_MODES_V3 = {
  {0x5, "BIST Carrier Mode"},
  {0x8, "BIST Test Data"},
  {0x9, "BIST Shared Test Mode Entry"},
  {0xA, "BIST Shared Test Mode Exit"},
};

const std::map<int, std::string> BIST_MODES_V2 = {
  {0, "BIST Receiver Mode"}, {1, "BIST Transmit Mode"}, {2, "Returned BIST Counters"},
  {3, "BIST Carrier Mode 0"}, {4, "BIST Carrier Mode 1"}, {5, "BIST Carrier Mode 2"},
  {6, "BIST Carrier Mode 3"}, {7, "BIST Eye Pattern"}, {8, "BIST Test Data"},
};

const std::map<int, std::string> CHARGE_STATE = {
  {0, "Charging"}, {1, "Discharging"}, {2, "Idle"}, {3, "Invalid"},
};

const std::map<int, std::string> USB_MODE = {
  {0, "USB 2.0"}, {1, "USB 3.2"}, {2, "USB4"},
};
const std::string USB_MODE_UNKNOWN = "USB4（其他取值按 USB4 处理）";
const std::map<int, std::string> USB_SPEED = {
  {0, "USB 2.0 only (no SuperSpeed)"}, {1, "USB 3.2 Gen1"}, {2, "USB 3.2 Gen2 / USB4 Gen2"},
  {3, "USB4 Gen3"}, {4, "USB4 Gen4"},
};
const std::string USB_SPEED_UNKNOWN = "USB4 Gen4（其他取值按 Gen4 处理）";
const std::map<int, std::string> CABLE_TYPE = {
  {0, "Passive"}, {1, "Active Re-timer"}, {2, "Active Re-driver"}, {3, "Optically Isolated"},
};
const std::map<int, std::string> CABLE_CURRENT_EUDO = {
  {0, "VBUS not supported"}, {1, "VBUS not supported（取值无效，按 00b 处理）"},
  {2, "3 A"}, {3, "5 A"},
};

const std::map<int, std::string> EXT_ALERT_EVENT = {
  {1, "Power State change (DFP)"},
  {2, "Power button press (UFP)"},
  {3, "Power button release (UFP)"},
  {4, "Controller initiated wake (UFP)"},
  {5, "Source is about to reduce Source Capabilities"},
};

const std::map<int, std::string> EPR_MODE_ACTION = {
  {1, "Enter EPR Mode"}, {2, "Enter Acknowledged"}, {3, "Enter Succeeded"},
  {4, "Enter Failed"}, {5, "Exit EPR Mode"},
};
const std::map<int, std::string> EPR_MODE_DATA = {
  {0, "Unknown cause"}, {1, "Cable not EPR capable"},
  {2, "Source failed to become VCONN source"},
  {3, "EPR Capable bit not set in RDO"},
  {4, "Source unable to enter EPR Mode"},
  {5, "EPR Capable bit not set in PDO"},
};

const std::map<int, std::string> EXT_CONTROL_MSG_TYPES = {
  {1, "EPR_Get_Source_Cap"}, {2, "EPR_Get_Sink_Cap"},
  {3, "EPR_Keep_Alive"}, {4, "EPR_Keep_Alive_Ack"},
};

const std::map<int, PeakInfo> PEAK_CURRENT_DETAILS = {
  {0, {"00b", "IoC only / see Source_Capabilities_Extended", {}}},
  {1, {"01b", "150/125/110% IoC overload profile",
       {"150% IoC for 1ms @ 5% duty", "125% IoC for 2ms @ 10% duty", "110% IoC for 10ms @ 50% duty"}}},
  {2, {"10b", "200/150/125% IoC overload profile",
       {"200% IoC for 1ms @ 5% duty", "150% IoC for 2ms @ 10% duty", "125% IoC for 10ms @ 50% duty"}}},
  {3, {"11b", "200/175/150% IoC overload profile",
       {"200% IoC for 1ms @ 5% duty", "175% IoC for 2ms @ 10% duty", "150% IoC for 10ms @ 50% duty"}}},
};

const std::map<int, std::string> FRS_CURRENT = {
  {0, "00b Fast Role Swap not supported"},
  {1, "01b Default USB Port"},
  {2, "10b 1.5 A @ 5 V"},
  {3, "11b 3.0 A @ 5 V"},
};

const std::map<int, std::string> POWER_STATE = {
  {0, "Status Not Supported"}, {1, "S0"}, {2, "Modern Standby"}, {3, "S3"},
  {4, "S4"}, {5, "S5 (Off with Battery)"}, {6, "G3 (Off, no Battery)"},
};
const std::map<int, std::string> STATE_INDICATOR = {
  {0, "Off LED"}, {1, "On LED"}, {2, "Blinking LED"}, {3, "Breathing LED"},
};
const std::map<int, std::string> TEMP_STATUS = {
  {0, "Not Supported"}, {1, "Normal"}, {2, "Warning"}, {3, "Over-temperature"},
};

std::string batteryRefText(int n) {
  if (n < 4) return "Fixed Battery " + std::to_string(n);
  if (n < 8) return "Hot Swappable Battery " + std::to_string(n - 4);
  return "无效电池引用 " + std::to_string(n);
}

const std::map<int, std::string> LOAD_STEP = {
  {0, "150 mA/µs (default)"}, {1, "500 mA/µs"}, {2, "取值无效，按默认 150 mA/µs"}, {3, "取值无效，按默认 150 mA/µs"},
};
const std::map<int, std::string> TOUCH_TEMP_SOURCE = {
  {0, "IEC 60950-1 (default)"}, {1, "IEC 62368-1 TS1"}, {2, "IEC 62368-1 TS2"},
};
const std::map<int, std::string> TOUCH_TEMP_SINK = {
  {0, "No applicable standard"}, {1, "IEC 60950-1 (default)"}, {2, "IEC 62368-1 TS1"}, {3, "IEC 62368-1 TS2"},
};

/* ══════════════════ VDM ════════════════ */

const std::map<int, std::string> VDM_CMDS = {
  {1, "Discover Identity"}, {2, "Discover SVIDs"}, {3, "Discover Modes"},
  {4, "Enter Mode"}, {5, "Exit Mode"}, {6, "Attention"},
};
const char* VDM_ACK[4] = {"REQ", "ACK", "NAK", "BUSY"};
const std::map<int, std::string> VDM_VER_MAJOR = {
  {0, "Version 1.0 (已废弃)"}, {1, "Version 2.x"},
};
const std::map<int, std::string> VDM_VER_MINOR = {
  {0, "Version 2.0"}, {1, "Version 2.1"},
};

const std::map<int, std::string> PRODUCT_TYPE_UFP = {
  {0, "Not a UFP"}, {1, "PDUSB Hub"}, {2, "PDUSB Peripheral"}, {3, "PSD (Power Source Device)"},
  {4, "无效取值（接收端应忽略）"}, {5, "AMA（已废弃，PD 3.0 的 Alternate Mode Adapter）"},
  {6, "无效取值（接收端应忽略）"}, {7, "无效取值（接收端应忽略）"},
};
const std::map<int, std::string> PRODUCT_TYPE_CABLE = {
  {0, "Not a Cable Plug/VPD"}, {1, "无效取值（接收端应忽略）"}, {2, "无效取值（接收端应忽略）"},
  {3, "Passive Cable"}, {4, "Active Cable"}, {5, "无效取值（接收端应忽略）"},
  {6, "VCONN Powered USB Device (VPD)"}, {7, "无效取值（不得使用）"},
};
const std::map<int, std::string> PRODUCT_TYPE_DFP = {
  {0, "Not a DFP"}, {1, "PDUSB Hub"}, {2, "PDUSB Host"}, {3, "Power Brick"},
  {4, "AMC（已废弃）"}, {5, "无效取值（不得使用）"}, {6, "无效取值（不得使用）"}, {7, "无效取值（不得使用）"},
};
const std::map<int, std::string> CONNECTOR_TYPE = {
  {0, "Unknown（已废弃）"}, {1, "无效取值（不得使用）"},
  {2, "USB Type-C Receptacle"}, {3, "USB Type-C Plug"},
};

const std::string* productTypeVdoKind(const std::string& link, int pt) {
  static const std::map<int, std::string> ufp = {{1, "ufp"}, {2, "ufp"}, {5, "ama"}};
  static const std::map<int, std::string> cable = {{3, "passiveCable"}, {4, "activeCable"}, {6, "vpd"}};
  static const std::map<int, std::string> dfp = {{1, "dfp"}, {2, "dfp"}, {3, "dfp"}};
  const std::map<int, std::string>* m = nullptr;
  if (link == "cable") m = &cable;
  else if (link == "ufp") m = &ufp;
  else if (link == "dfp") m = &dfp;
  if (!m) return nullptr;
  auto it = m->find(pt);
  return it == m->end() ? nullptr : &it->second;
}

const std::map<int, std::string> UFP_VDO_VERSION = {
  {0, "无效取值（不得使用）"}, {1, "Version 1.1（已废弃）"}, {2, "Version 1.2（已废弃）"}, {3, "Version 1.3"},
};
const std::map<int, std::string> DFP_VDO_VERSION = {
  {0, "无效取值（不得使用）"}, {1, "Version 1.1（已废弃）"}, {2, "Version 1.2"},
};
const std::map<int, std::string> UFP_USB2 = {
  {0, "不具备 USB 2.0 能力"}, {1, "仅支持 USB 2.0 Billboard 设备"}, {2, "支持 USB 2.0"}, {3, "无效取值（按 00b 处理）"},
};
const std::map<int, std::string> USB_HIGHEST_SPEED = {
  {0, "USB 2.0 only（无 SuperSpeed）"}, {1, "USB 3.2 Gen1"}, {2, "USB 3.2 Gen2 / USB4 Gen2"},
  {3, "USB4 Gen3"}, {4, "USB4 Gen4"},
};
const std::map<int, std::string> USB_HIGHEST_SPEED_V30 = {
  {0, "USB 2.0 only（无 SuperSpeed）"}, {1, "[USB 3.1] Gen1"}, {2, "[USB 3.1] Gen1 与 Gen2"},
};
const std::map<int, std::string> VCONN_POWER = {
  {0, "1 W"}, {1, "1.5 W"}, {2, "2 W"}, {3, "3 W"}, {4, "4 W"}, {5, "5 W"}, {6, "6 W"}, {7, "无效取值（不得使用）"},
};

const std::map<int, std::string> CABLE_CONNECTOR = {
  {0, "USB Type-A（已废弃）"}, {1, "USB Type-B（已废弃）"}, {2, "USB Type-C"}, {3, "Captive（固线）"},
};
const std::map<int, std::string> CABLE_TERMINATION_PASSIVE = {
  {0, "不需要 VCONN"}, {1, "需要 VCONN"}, {2, "无效取值（不得使用）"}, {3, "无效取值（不得使用）"},
};
const std::map<int, std::string> CABLE_TERMINATION_ACTIVE = {
  {0, "无效取值（不得使用）"}, {1, "无效取值（不得使用）"},
  {2, "一端 Active、一端 Passive，需要 VCONN"}, {3, "两端都 Active，需要 VCONN"},
};
const std::map<int, std::string> CABLE_LATENCY_PASSIVE = {
  {0, "无效取值（不得使用）"}, {1, "<10ns (~1m)"}, {2, "10ns ~ 20ns (~2m)"}, {3, "20ns ~ 30ns (~3m)"},
  {4, "30ns ~ 40ns (~4m)"}, {5, "40ns ~ 50ns (~5m)"}, {6, "50ns ~ 60ns (~6m)"}, {7, "60ns ~ 70ns (~7m)"},
  {8, ">70ns (>~7m)"}, {9, "无效取值（不得使用）"}, {10, "无效取值（不得使用）"}, {11, "无效取值（不得使用）"},
  {12, "无效取值（不得使用）"}, {13, "无效取值（不得使用）"}, {14, "无效取值（不得使用）"}, {15, "无效取值（不得使用）"},
};
const std::map<int, std::string> CABLE_LATENCY_ACTIVE = {
  {0, "无效取值（不得使用）"}, {1, "<10ns (~1m)"}, {2, "10ns ~ 20ns (~2m)"}, {3, "20ns ~ 30ns (~3m)"},
  {4, "30ns ~ 40ns (~4m)"}, {5, "40ns ~ 50ns (~5m)"}, {6, "50ns ~ 60ns (~6m)"}, {7, "60ns ~ 70ns (~7m)"},
  {8, "1000ns (~100m)"}, {9, "2000ns (~200m)"}, {10, "3000ns (~300m)"}, {11, "无效取值（不得使用）"},
  {12, "无效取值（不得使用）"}, {13, "无效取值（不得使用）"}, {14, "无效取值（不得使用）"}, {15, "无效取值（不得使用）"},
};
const std::map<int, std::string> CABLE_VBUS_VOLTAGE_V30 = {
  {0, "20 V"}, {1, "30 V"}, {2, "40 V"}, {3, "50 V"},
};
const std::map<int, std::string> CABLE_VBUS_VOLTAGE_V31 = {
  {0, "20 V"}, {1, "20 V（Deprecated 码，接收端按 20V 处理）"}, {2, "20 V（Deprecated 码，接收端按 20V 处理）"}, {3, "50 V"},
};
const std::map<int, std::string> CABLE_VBUS_CURRENT = {
  {0, "无效取值（按 3A 处理）"}, {1, "3 A"}, {2, "5 A"}, {3, "无效取值（按 3A 处理）"},
};
const std::map<int, std::string> CABLE_VBUS_CURRENT_ACTIVE = {
  {0, "无效取值（不得使用）"}, {1, "3 A"}, {2, "5 A"}, {3, "无效取值（不得使用）"},
};
const std::map<int, std::string> U3_CLD_POWER = {
  {0, ">10 mW"}, {1, "5 ~ 10 mW"}, {2, "1 ~ 5 mW"}, {3, "0.5 ~ 1 mW"},
  {4, "0.2 ~ 0.5 mW"}, {5, "50 ~ 200 µW"}, {6, "<50 µW"}, {7, "无效取值（按 000b 处理）"},
};
const std::map<int, std::string> VPD_VBUS_VOLTAGE = {
  {0, "20 V"}, {1, "20 V（Deprecated 码，接收端按 20V 处理）"},
  {2, "20 V（Deprecated 码，接收端按 20V 处理）"}, {3, "20 V（Deprecated 码，接收端按 20V 处理）"},
};
const std::map<int, std::string> AMA_SUPERSPEED_V30 = {
  {0, "USB 2.0 only"}, {1, "USB 3.1 Gen1 + USB 2.0"}, {2, "USB 3.1 Gen1/Gen2 + USB 2.0"},
  {3, "USB 2.0 Billboard only"}, {4, "保留值（不得使用）"}, {5, "保留值（不得使用）"},
  {6, "保留值（不得使用）"}, {7, "保留值（不得使用）"},
};

/* ══════════════════ 逐位表 ════════════════ */

const std::vector<BitName> ALERT_BITS = {
  {24, "Reserved"},
  {25, "Battery Status Change Event"},
  {26, "OCP Event"},
  {27, "OTP Event"},
  {28, "Operating Condition Change Event"},
  {29, "Source Input Change Event"},
  {30, "OVP Event"},
  {31, "Extended Alert Event"},
};

const std::vector<BitName> STATUS_EVENT_BITS = {
  {1, "Overcurrent Event (OCP)"},
  {2, "Overtemperature Event (OTP)"},
  {3, "Overvoltage Event (OVP)"},
  {4, "Current Limit (CL) Mode（仅 PPS）"},
};

const std::vector<BitName> POWER_STATUS_BITS = {
  {1, "受线缆载流能力限制"},
  {2, "受其他端口供电不足限制"},
  {3, "受外部供电不足限制"},
  {4, "受 Event Flags 限制"},
  {5, "受温度限制"},
};

}}  // namespace pdscope::pd
