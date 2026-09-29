// tables.cpp — UFCS 协议常量表（实现见 tables.h）
//
// 全部数据照抄自 PDScope/src/js/ufcs/tables.js（实测归纳 + 规范表 13/14/15/16/22/24/26）。
// 中文字面值原样保留（UTF-8）。

#include "tables.h"

namespace pdscope { namespace ufcs {

/* ══════════════ 消息头（规范 8.2.2 表 13）══════════════ */

const std::map<int, std::string> kDevAddr = {
  {0b001, "供电设备（Source）"},
  {0b010, "充电设备（Sink）"},
  {0b011, "线缆电子标签（Cable）"},
};

const std::map<int, std::string> kAddrRole = {
  {0b001, "SRC"}, {0b010, "SNK"}, {0b011, "Plug"},
};

const std::map<std::string, int> kRoleAddr = {
  {"SRC", 0b001}, {"SNK", 0b010}, {"Plug", 0b011},
};

const std::map<int, std::string> kMsgType = {
  {0b000, "控制消息"},
  {0b001, "数据消息"},
  {0b010, "自定义消息"},
};

const std::map<int, std::string> kVersion = {
  {0b000001, "1.0.0"},
  {0b010001, "1.0.1"},
  {0b001001, "1.2.0"},
};

/* ══════════════ 控制命令（规范 8.2.3 表 14，17 条）══════════════ */

const std::map<int, CtrlCmd> kCtrlCmd = {
  {0x00, {"Ping",     "SRC/SNK → 任意",       "必选", "探测目标设备是否存在，或测试传输是否正常"}},
  {0x01, {"ACK",      "任意 → 任意",          "必选", "消息已被正确接收（CRC 校验通过）"}},
  {0x02, {"NCK",      "任意 → 任意",          "必选", "消息已被接收，但 CRC 校验失败"}},
  {0x03, {"Accept",   "SRC/SNK → SRC/SNK",    "必选", "同意对方的请求，随后按请求调整输出"}},
  {0x04, {"Soft_Reset", "SRC/SNK → 任意",     "必选", "软复位：不退出 UFCS，收发状态机与缓存清零"}},
  {0x05, {"Power_Ready", "SRC → SNK",         "必选", "输出已调整到请求的电压/电流值"}},
  {0x06, {"Get_Output_Capabilities", "SNK → SRC", "必选", "请求供电设备的电压/电流输出能力"}},
  {0x07, {"Get_Source_Info", "SNK → SRC",     "必选", "请求供电设备当前工作状态（输出、温度等）"}},
  {0x08, {"Get_Sink_Info", "SRC → SNK",       "必选", "请求充电设备当前工作状态（电池、温度等）"}},
  {0x09, {"Get_Cable_Info", "SRC/SNK → Cable", "必选", "请求线缆电子标签信息（阻抗、承载能力）"}},
  {0x0A, {"Get_Device_Info", "SRC/SNK → SRC/SNK", "必选", "请求对端的硬件与软件信息"}},
  {0x0B, {"Get_Error_Info", "SRC/SNK → SRC/SNK", "必选", "请求对端的异常状态信息"}},
  {0x0C, {"Detect_Cable_Info", "SRC/SNK → SRC/SNK", "可选", "命令对端去读线缆信息并把结果回报过来"}},
  {0x0D, {"Start_Cable_Detect", "SRC/SNK → SRC/SNK", "可选", "请对方停止发送并释放 TX 总线，以便与线缆通信"}},
  {0x0E, {"End_Cable_Detect", "SRC/SNK → SRC/SNK", "可选", "通知对方可重新使用 D+/D- 总线通信"}},
  {0x0F, {"Exit_UFCS_Mode", "SRC/SNK → SRC/SNK", "必选", "退出 UFCS 快充模式，回到初始状态"}},
  {0x10, {"Get_Sink_Info_Extended", "SRC → SNK", "可选", "请求充电设备更多状态（最大充电功率、电池电量）"}},
};

/* ══════════════ 数据命令（规范 8.2.4 表 15，14 条）══════════════ */

const std::map<int, DataCmd> kDataCmd = {
  {0x01, {"Output_Capabilities", "SRC → SNK", "必选", "供电设备的能力清单（每种输出模式 8 字节，最多 7 种）", true, 8, 1, 7}},
  {0x02, {"Request", "SNK → SRC", "必选", "充电设备请求某个输出模式下的具体电压与电流", false, 8}},
  {0x03, {"Source_Information", "SRC → SNK", "必选", "供电设备当前状态：输出电压/电流、内部与接口温度", false, 8}},
  {0x04, {"Sink_Information", "SNK → SRC", "必选", "充电设备当前状态：充电电压/电流、电池与接口温度", false, 8}},
  {0x05, {"Cable_Information", "Cable/SRC/SNK → SRC/SNK", "必选", "线缆信息：厂家识别码、阻抗、最大承载电压与电流", false, 10}},
  {0x06, {"Device_Information", "SRC/SNK → SRC/SNK", "必选", "设备信息：厂家识别码、硬/软件版本号", false, 8}},
  {0x07, {"Error_Information", "SRC/SNK → SRC/SNK", "必选", "异常状态：D+ / D- / CC 过压标志", false, 4}},
  {0x08, {"Config_Watchdog", "SNK → SRC", "必选", "配置供电设备的看门狗溢出时间（0 = 关闭看门狗）", false, 2}},
  {0x09, {"Refuse", "任意 → SRC/SNK", "必选", "拒绝某条消息，并给出被拒消息的编号/类型/命令与原因", false, 4}},
  {0x0A, {"Verify_Request", "SRC/SNK → 任意", "可选", "索要鉴权：指定密钥编号并给出 16 字节随机数", false, 17}},
  {0x0B, {"Verify_Response", "任意 → SRC/SNK", "可选", "鉴权应答：32 字节加密数据 + 回送 16 字节随机数", false, 48}},
  {0x0C, {"Power_Change", "SRC → SNK", "可选", "供电设备主动通知最大输出电流能力发生了变化", true, 3, 1, 7}},
  {0x0D, {"Sink_Information_Extended", "SNK → SRC", "可选", "充电设备扩展状态：电池电量、最大充电功率（每项 3 字节）", true, 3, 1, 15}},
  {0xFF, {"Test_Request", "测试设备 → 任意", "必选", "测试用：命令被测设备按指定设备地址/消息类型/命令发一条消息", false, 2}},
};

/* ══════════════ 输出模式（规范 8.2.4.1 表 16 / 8.2.4.12 表 25）══════════════ */

const std::map<int, std::string> kCurrentStep = {
  {0, "10 mA"}, {1, "20 mA"}, {2, "30 mA"}, {3, "40 mA"}, {4, "50 mA"},
};

const std::map<int, std::string> kVoltageStep = {
  {0, "10 mV"}, {1, "20 mV"},
};

/* ══════════════ Refuse 拒绝原因（规范 8.2.4.9 表 24）══════════════ */

const std::map<int, std::string> kRefuseReason = {
  {0x01, "无法识别的命令或数据"},
  {0x02, "不支持的命令或数据"},
  {0x03, "设备忙，暂无法响应"},
  {0x04, "请求的输出电压、电流或功率超出范围"},
  {0x05, "其它原因"},
};

/* ══════════════ Sink_Information_Extended 状态类型（规范 8.2.4.13 表 26）══════════════ */

const std::map<int, std::string> kExtStatusType = {
  {0b0001, "电池电量"},
  {0b0010, "最大充电功率"},
};

/* ══════════════ 异常信息位（规范 8.2.4.7 表 22）══════════════ */

const std::vector<ErrorBit> kErrorBits = {
  {8, "D+ OVP", "数据线 D+ 过压"},
  {7, "D- OVP", "数据线 D- 过压"},
  {6, "CC OVP", "配置通道 CC 过压"},
};

/* ══════════════ 物理链路（规范 7.4 / 7.2）══════════════ */

const std::map<std::string, std::string> kLine = {
  {"SRC", "D+"}, {"SNK", "D-"}, {"Plug", "D±"},
};

const int kBaudRates[3] = {115200, 57600, 38400};

const int kBitsPerByte = 10;

/* ══════════════ 单向命令表（方向还原的硬依据）══════════════ */

const std::map<int, std::string> kFixedDir = {
  {(0 << 8) | 0x05, "SRC"},   // Power_Ready               供电设备 → 充电设备
  {(0 << 8) | 0x06, "SNK"},   // Get_Output_Capabilities   充电设备 → 供电设备
  {(0 << 8) | 0x07, "SNK"},   // Get_Source_Info           充电设备 → 供电设备
  {(0 << 8) | 0x08, "SRC"},   // Get_Sink_Info             供电设备 → 充电设备
  {(0 << 8) | 0x10, "SRC"},   // Get_Sink_Info_Extended    供电设备 → 充电设备
  {(1 << 8) | 0x01, "SRC"},   // Output_Capabilities       供电设备 → 充电设备
  {(1 << 8) | 0x02, "SNK"},   // Request                   充电设备 → 供电设备
  {(1 << 8) | 0x03, "SRC"},   // Source_Information        供电设备 → 充电设备
  {(1 << 8) | 0x04, "SNK"},   // Sink_Information          充电设备 → 供电设备
  {(1 << 8) | 0x08, "SNK"},   // Config_Watchdog           充电设备 → 供电设备
  {(1 << 8) | 0x0C, "SRC"},   // Power_Change              供电设备 → 充电设备
  {(1 << 8) | 0x0D, "SNK"},   // Sink_Information_Extended 充电设备 → 供电设备
};

/* ══════════════ 辅助函数 ═════════════ */

std::string ufcsVersionText(int code) {
  int major = code & 0b11;
  int minor = (code >> 2) & 0b11;
  int patch = (code >> 4) & 0b11;
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::string ufcsFixedSender(int mtype, int cmd) {
  auto it = kFixedDir.find((mtype << 8) | (cmd & 0xFF));
  return it == kFixedDir.end() ? std::string() : it->second;
}

std::string ufcsPeerOf(const std::string& role) {
  if (role == "SRC") return "SNK";
  if (role == "SNK") return "SRC";
  return std::string();
}

std::string ufcsOpposite(const std::string& role) {
  if (role == "SRC") return "SNK";
  if (role == "SNK") return "SRC";
  return "SRC";
}

std::string ufcsRoleOf(int addr) {
  auto it = kAddrRole.find(addr);
  return it == kAddrRole.end() ? std::string() : it->second;
}

std::string ufcsRoleText(const std::string& role) {
  if (role == "SRC") return "供电设备";
  if (role == "SNK") return "充电设备";
  if (role == "Plug") return "线缆电子标签";
  return "未知";
}

}}  // namespace pdscope::ufcs
