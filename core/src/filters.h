// filters.h — 报文筛选与排序（视图）
//
// 语义对齐 PDScope（JS 基线）的 app.js：筛选改变的是「视图」，不影响报文总数；
// CSV 导出按当前视图顺序取行。
#pragma once

#include "packet.h"

#include <set>

namespace pdscope {

/** 报文分类（对应 JS 的 kindOf / toneOf）。 */
std::string toneOf(const Packet& p);
std::string packetKind(const Packet& p);

/** 自动应答心跳包：PD 是 GoodCRC，UFCS 是 ACK / NCK。 */
bool isAutoAck(const Packet& p, const std::string& protocol);

/** 功率协商 / 状态切换（两种协议各一套判据）。 */
bool isPowerType(const std::string& msgType, const std::string& protocol);
bool isEnterType(const std::string& msgType, const std::string& protocol);

/** 链路一栏的候选取值（UFCS 是 D+/D-/D±，PD 是 SOP 序列）。 */
std::vector<std::string> linkValues(const std::string& protocol);
/** 报文类别一栏的候选。 */
std::vector<std::string> catValues(const std::string& protocol);

struct Filters {
    std::set<std::string> roles;
    std::set<std::string> sops;
    std::set<std::string> cats;
    std::set<std::string> types;      // 空 = 全部
    bool hideGoodCrc = true;
    bool onlyBad = false;
    bool onlyPower = false;
    bool onlyEnter = false;
    std::string q;
    double tFrom = 0;
    double tTo = 1;
};

/**
 * 一份抓包的默认筛选条件。**默认值跟着协议走**：用 PD 的默认集合去筛 UFCS
 * 会把所有报文都筛没（UFCS 的链路是 D+/D-，类别里没有 Extended/VDM）。
 *
 * @param protocol "UFCS" 或其它的协议名
 */
Filters newFilters(const std::string& protocol);

enum class ViewMode { All, Neg, Err };

struct SortSpec {
    std::string key = "index";
    bool asc = true;
};

/** 判断一条报文是否通过筛选（不含「视图模式」那两条）。 */
bool passesFilters(const Packet& p, const Filters& f, const std::string& protocol,
                   uint64_t totalSamples, ViewMode mode);

/**
 * 按当前筛选 + 排序生成视图（存的是报文指针，不复制报文）。
 * 排序语义与 JS 一致：`null` 当 -1；字符串按序比较；数值相减。
 */
std::vector<const Packet*> buildView(const std::vector<Packet>& packets, const Filters& f,
                                     const std::string& protocol, uint64_t totalSamples,
                                     ViewMode mode, const SortSpec& sort);

}  // namespace pdscope
