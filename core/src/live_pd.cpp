/**
 * @file live_pd.cpp
 * @brief 实时 PCL PD 记录的独立 C ABI，复用核心解码并保留原始接收证据。
 */
#include "pd/crc.h"
#include "pd/decoder.h"
#include "session.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <pdscope/pdscope.h>

using namespace pdscope;

/** @brief 各 CC 独立持有跨报文协商状态，句柄由工作线程独占。 */
struct pdscope_live_pd {
  std::array<pd::PdDecoder, 3> channels{
      pd::PdDecoder(1000), pd::PdDecoder(1000), pd::PdDecoder(1000)};
};

namespace {
/**
 * @brief 将 C++ 异常约束在 ABI 内部。
 * @param ignored 保持与公共 ABI 的内部保护器调用形式一致，无错误文本输出。
 * @param fn 需要串行执行的操作。
 * @return 稳定 C 状态码。
 */
template <typename Fn> int32_t guard(char **ignored, Fn &&fn) noexcept {
  static_cast<void>(ignored);
  try {
    return fn();
  } catch (const Error &error) {
    return error.status();
  } catch (const std::bad_alloc &) {
    return PDSCOPE_ERR_MEMORY;
  } catch (...) {
    return PDSCOPE_ERR_INTERNAL;
  }
}

/**
 * @brief 返回独立持有的 UTF-8 JSON，分配成功后才替换旧输出。
 * @param output 由调用方零初始化的库输出缓冲。
 * @param value 已组装完成的 JSON。
 * @return 状态码；调用方使用 pdscope_buf_free 释放结果。
 */
int32_t giveJson(pdscope_buf *output, const json &value) {
  const auto text = value.dump();
  auto *bytes = static_cast<uint8_t *>(std::malloc(text.size()));
  if (!bytes)
    return PDSCOPE_ERR_MEMORY;
  std::memcpy(bytes, text.data(), text.size());
  pdscope_buf_free(output);
  output->data = bytes;
  output->len = text.size();
  return PDSCOPE_OK;
}
} // namespace

extern "C" {
/**
 * @brief 创建由工作线程独占的实时 PD 解码器。
 * @return 成功时返回句柄，分配失败时返回空指针。
 */
pdscope_live_pd *PDSCOPE_CALL pdscope_live_pd_create(void) {
  try {
    return new pdscope_live_pd();
  } catch (...) {
    return nullptr;
  }
}

/**
 * @brief 为新采集清空各 CC 的跨报文状态。
 * @param decoder 有效解码器句柄。
 * @return 稳定 C 状态码。
 */
int32_t PDSCOPE_CALL pdscope_live_pd_reset(pdscope_live_pd *decoder) {
  if (!decoder)
    return PDSCOPE_ERR_ARGUMENT;
  return guard(nullptr, [&]() -> int32_t {
    for (auto &channel : decoder->channels)
      channel.reset();
    return PDSCOPE_OK;
  });
}

/**
 * @brief 释放句柄，允许传入空指针。
 * @param decoder 待释放的解码器。
 */
void PDSCOPE_CALL pdscope_live_pd_close(pdscope_live_pd *decoder) {
  delete decoder;
}

/**
 * @brief 解码单个 PCL PD 记录，不更改原始接收证据。
 * @param decoder 由当前工作线程独占的解码器。
 * @param bytes 原始线上解码字节或截短前缀。
 * @param len 实际字节数。
 * @param sop PCL SOP/Reset 类型。
 * @param cc 实际接收 CC 通道。
 * @param flags 设备报告的接收状态。
 * @param extended_ticks 已扩展的采集相对刻度。
 * @param timebase_hz HELLO 声明的计时频率。
 * @param index 本次采集的逻辑报文序号。
 * @param out_json 接收独立持有的 row/detail JSON。
 * @return C 状态码；异常不跨越 ABI。
 */
int32_t PDSCOPE_CALL pdscope_live_pd_decode(
    pdscope_live_pd *decoder, const uint8_t *bytes, size_t len, uint8_t sop,
    uint8_t cc, uint8_t flags, uint64_t extended_ticks, uint32_t timebase_hz,
    uint64_t index, pdscope_buf *out_json) {
  const bool reset = sop == 3 || sop == 4;
  const bool partial = (flags & 12) != 0;
  if (!decoder || !out_json || sop > 5 || cc > 2 || flags > 15 ||
      (flags & 3) == 3 || timebase_hz < 1000 || timebase_hz > 1000000 ||
      len > 268 || (len && !bytes) ||
      (reset ? len != 0 || flags != 2 : len < (partial ? 2u : 6u)))
    return PDSCOPE_ERR_ARGUMENT;
  return guard(nullptr, [&]() -> int32_t {
    static const char *names[] = {"SOP",        "SOP'",        "SOP''",
                                  "Hard Reset", "Cable Reset", "Unknown"};
    const double timeMs =
        static_cast<double>(extended_ticks) * 1000.0 / timebase_hz;
    // 在 uint64 的采样坐标范围之外不进行浮点转整数。
    if (timeMs >= static_cast<double>(INT64_MAX))
      return PDSCOPE_ERR_ARGUMENT;
    Packet packet;
    uint32_t wireCrc = 0;
    uint32_t calculated = 0;
    bool completeCrc = !reset && !partial;
    if (reset) {
      packet.sop = names[sop];
      packet.msgType = names[sop];
      packet.msgKind = "special";
      packet.category = "special";
      packet.role = "Unknown";
      packet.summary = names[sop];
      if (sop == 3)
        decoder->channels[cc].reset();
    } else {
      const size_t decodedLength = completeCrc ? len - 4 : len;
      if (completeCrc) {
        wireCrc = static_cast<uint32_t>(bytes[len - 4]) |
                  (static_cast<uint32_t>(bytes[len - 3]) << 8) |
                  (static_cast<uint32_t>(bytes[len - 2]) << 16) |
                  (static_cast<uint32_t>(bytes[len - 1]) << 24);
        calculated = pd::crc32(bytes, decodedLength);
      }
      // 异常字节可以解读，但不能污染后续正常报文的协商状态。
      auto trial = decoder->channels[cc];
      auto result =
          trial.decodeWire(bytes, decodedLength, sop == 5 ? "SOP" : names[sop],
                           timeMs, cc, false, -1, false);
      if (result) {
        packet = std::move(*result);
        if (!partial && sop != 5 && !(flags & 9) && wireCrc == calculated &&
            packet.warnings.empty())
          decoder->channels[cc] = std::move(trial);
      } else {
        // 核心不能解析的可信边界仍须保留，不能因一个坏包终止整个采集。
        packet.msgType = "无法解码";
        packet.msgKind = "special";
        packet.category = "control";
        packet.hasHeader = true;
        packet.header = bytes[0] | (static_cast<int>(bytes[1]) << 8);
        packet.role = (packet.header & 0x100) ? "SRC" : "SNK";
        packet.summary = "保留设备取得的原始解码字节";
        packet.warnings.push_back(
            {"核心未能解码此消息头，原始字节已保留", "DECODE"});
      }
      packet.sop = names[sop];
      if (sop == 5)
        packet.role = "Unknown";
    }
    packet.index = index;
    packet.seq = index;
    packet.channel = cc;
    packet.timeMs = timeMs;
    packet.endTimeMs = timeMs;
    packet.startSample = static_cast<uint64_t>(timeMs);
    packet.endSample = packet.startSample;
    packet.durationUs = 0;
    packet.bitrate = 0;
    packet.bitrateNominal = false;
    packet.synthetic = true;
    packet.eop = false;
    packet.crcRecorded = completeCrc;
    packet.hasCrc = completeCrc;
    packet.crc = wireCrc;
    packet.crcCalc = calculated;
    packet.crcOk = partial || reset || (flags & 2)        ? CrcState::Unrecorded
                   : (flags & 1) || wireCrc != calculated ? CrcState::Bad
                                                          : CrcState::Ok;
    packet.vbus = std::numeric_limits<double>::quiet_NaN();
    packet.ibus = std::numeric_limits<double>::quiet_NaN();
    // decodeWire 内部使用合成定界符；详情不保留其校验/结束符证据。
    packet.details.erase(
        std::remove_if(packet.details.begin(), packet.details.end(),
                       [](const DetailItem &item) {
                         return item.key == "CRC" || item.key == "EOP";
                       }),
        packet.details.end());
    packet.warnings.erase(std::remove_if(packet.warnings.begin(),
                                         packet.warnings.end(),
                                         [](const PacketWarning &warning) {
                                           return warning.shortMsg == "CRC" ||
                                                  warning.shortMsg == "EOP";
                                         }),
                          packet.warnings.end());
    if (flags & 1)
      packet.warnings.push_back({"设备明确报告 PD CRC 失败", "CRC"});
    if (flags & 4)
      packet.warnings.push_back(
          {"设备只保留解码包的连续前缀，包尾不能作为 CRC", "TRUNC"});
    if (flags & 8)
      packet.warnings.push_back(
          {"PD 接收外设报告接收错误，包尾不能作为 CRC", "RX"});
    if (completeCrc && wireCrc != calculated)
      packet.warnings.push_back({"原始线上 CRC 与上位机重算结果不一致", "CRC"});
    if (sop == 5)
      packet.warnings.push_back(
          {"设备未识别 SOP，头字段的方向解释不确定", "SOP"});
    auto detail = packetDetailJsonOf(packet, "USB PD");
    auto row = packetListItemJson(packet, "USB PD");
    json original = json::array();
    for (size_t i = 0; i < len; i++)
      original.push_back(bytes[i]);
    detail["rawPayload"] = std::move(original);
    detail["pdFlags"] = flags;
    detail["extendedTicks"] = extended_ticks;
    detail["timebaseHz"] = timebase_hz;
    detail["durationUs"] = nullptr;
    row["durationUs"] = nullptr;
    // 文本也采用真实证据，避免 decodeWire 的合成 CRC 和时长造成误导。
    detail["text"] = packet.sop + " " + packet.msgType + "\n" + packet.summary;
    return giveJson(
        out_json, json{{"row", std::move(row)}, {"detail", std::move(detail)}});
  });
}

} // extern "C"
