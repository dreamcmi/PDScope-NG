// tables.h — UFCS 协议常量表
//
// 全部取自 T/CCSA 393—2024 / T/TAF 083—2024《移动终端融合快速充电技术要求》
// 第 7 章物理层、第 8 章协议层。每张表都在实现文件里写明出处，便于日后核对版本。
//
// 命名统一带 UFCS_/k 前缀：单文件打包器会把整个库拍平进同一个作用域，
// 不带前缀会和 pd/ 库里的同名表互相覆盖。

#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

namespace pdscope { namespace ufcs {

/* ─═══════════ 消息头（规范 8.2.2 表 13）══════════ */

/** 设备地址（Header bit15…13）—— 注意这是接收方的地址，不是发送方。 */
extern const std::map<int, std::string> kDevAddr;   // 0b001 → 供电设备（Source） …

/** 设备地址 → 短名（与 PD 侧的 SRC / SNK / Plug 口径对齐）。 */
extern const std::map<int, std::string> kAddrRole;  // 0b001 → "SRC"
extern const std::map<std::string, int> kRoleAddr;   // "SRC" → 0b001

/** 消息类型（Header bit2…0）。 */
extern const std::map<int, std::string> kMsgType;    // 0b000 → 控制消息 …

/** 协议版本编号（Header bit8…3）→ 文本。 */
extern const std::map<int, std::string> kVersion;    // 0b000001 → "1.0.0" …

/* ─═══════════ 控制命令（规范 8.2.3 表 14，17 条）══════════ */

struct CtrlCmd {
  std::string name;   // 命令名（界面表格 / msgType 直接用它）
  std::string dir;    // 规范表 14 的「发送者 → 接收者」
  std::string req;    // 必选 / 可选
  std::string sum;    // 一句话说明（详情面板）
};
extern const std::map<int, CtrlCmd> kCtrlCmd;

/* ─═══════════ 数据命令（规范 8.2.4 表 15，14 条）══════════ */

struct DataCmd {
  std::string name;
  std::string dir;
  std::string req;
  std::string sum;
  bool lenVar = false;    // false ⇒ 定长 lenUnit；true ⇒ 变长（每项 lenUnit 字节，1…lenMax 项）
  int lenUnit = 0;        // 定长时即长度；变长时为每项字节数
  int lenMin = 0;         // 变长时最小项数（恒为 1）
  int lenMax = 0;         // 变长时最大项数
};
extern const std::map<int, DataCmd> kDataCmd;

/* ─═══════════ 输出模式（规范 8.2.4.1 表 16 / 8.2.4.12 表 25）══════════ */

extern const std::map<int, std::string> kCurrentStep;  // 电流调节步进（bit59…57）
extern const std::map<int, std::string> kVoltageStep;  // 电压调节步进（bit56）

/* ─═══════════ Refuse 拒绝原因（规范 8.2.4.9 表 24）══════════ */

extern const std::map<int, std::string> kRefuseReason;

/* ─═══════════ Sink_Information_Extended 状态类型（规范 8.2.4.13 表 26）══════════ */

extern const std::map<int, std::string> kExtStatusType;

/* ─═══════════ 异常信息位（规范 8.2.4.7 表 22）══════════ */

struct ErrorBit { int bit; std::string name; std::string text; };
extern const std::vector<ErrorBit> kErrorBits;

/* ─═══════════ 物理链路（规范 7.4 / 7.2）══════════ */

/** 链路标签：角色 → 物理线名（供电设备 D+ 发送、充电设备 D- 发送）。 */
extern const std::map<std::string, std::string> kLine;   // "SRC" → "D+"

/** 波特率基准档位（规范 7.4.6）：115200 为缺省支持档位。 */
extern const int kBaudRates[3];

/** 一个数据帧 = 1 起始位 + 8 数据位 + 1 结束位（规范 7.4.1）。 */
extern const int kBitsPerByte;

/* ─═══════════ 单向命令表（方向还原的硬依据）══════════ */

/** 查单向命令的发送方；双向命令返回空串。键 = mtype<<8 | cmd。 */
extern const std::map<int, std::string> kFixedDir;

/* ─═══════════ 辅助函数 ─══════════ */

/** 协议版本编号（6 bit）→ 文本 "主.中.小"。 */
std::string ufcsVersionText(int code);

/** 查单向命令的发送方角色；双向命令返回 ""。 */
std::string ufcsFixedSender(int mtype, int cmd);

/** 某角色发送时，消息头里的接收方应当是谁（用于交叉校验）。 */
std::string ufcsPeerOf(const std::string& role);

/** 取反角色（SRC↔SNK，其它兜底 SRC）。 */
std::string ufcsOpposite(const std::string& role);

/** 设备地址（0b001…0b011）→ 角色短名；未知返回 ""。 */
std::string ufcsRoleOf(int addr);

/** 角色 → 中文名。 */
std::string ufcsRoleText(const std::string& role);

}}  // namespace pdscope::ufcs
