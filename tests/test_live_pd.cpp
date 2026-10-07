/**
 * @file test_live_pd.cpp
 * @brief 实时 PD C ABI 的原始字节、CRC、状态隔离和生命周期验证。
 */
#include "pd/crc.h"
#include "test.h"
#include "util.h"
#include <pdscope/pdscope.h>

using namespace pdscope;

namespace {
/**
 * @brief 构造含原始线上 CRC 的完整 PD 解码包。
 * @param header PD 消息头。
 * @param words 数据对象。
 * @return 连续的头、正文和 CRC 字节。
 */
Bytes wire(uint16_t header, std::vector<uint32_t> words = {}) {
  Bytes bytes{static_cast<uint8_t>(header), static_cast<uint8_t>(header >> 8)};
  for (auto word : words)
    for (int byte = 0; byte < 4; byte++)
      bytes.push_back(static_cast<uint8_t>(word >> (byte * 8)));
  const auto crc = pd::crc32(bytes.data(), bytes.size());
  for (int byte = 0; byte < 4; byte++)
    bytes.push_back(static_cast<uint8_t>(crc >> (byte * 8)));
  return bytes;
}

/**
 * @brief 经公开 C ABI 解码并释放库分配的 JSON。
 * @param handle 待验证句柄。
 * @param bytes 原始字节。
 * @param flags PCL 接收标志。
 * @param ticks 扩展后的设备刻度。
 * @param sop SOP 或 Reset 枚举。
 * @param cc CC 通道。
 * @return row/detail JSON。
 */
json decode(pdscope_live_pd *handle, const Bytes &bytes, uint8_t flags = 0,
            uint64_t ticks = 1234, uint8_t sop = 0, uint8_t cc = 1) {
  pdscope_buf output{};
  CHECK_EQ(pdscope_live_pd_decode(handle, bytes.data(), bytes.size(), sop, cc,
                                  flags, ticks, 1000, 7, &output),
           PDSCOPE_OK);
  const auto result = json::parse(output.data, output.data + output.len);
  pdscope_buf_free(&output);
  CHECK(output.data == nullptr);
  return result;
}
} // namespace

TEST(live_pd_original_crc_and_unknown_are_distinct) {
  auto *handle = pdscope_live_pd_create();
  CHECK(handle != nullptr);
  const auto bytes = wire(0x0183);
  auto good = decode(handle, bytes);
  CHECK_EQ(good["row"]["msgType"].get<std::string>(), "Accept");
  CHECK_EQ(good["row"]["crc"].get<std::string>(), "ok");
  CHECK(good["detail"]["rawPayload"] == bytes);
  CHECK(good["row"]["durationUs"].is_null());
  CHECK(good["row"]["ibus"].is_null());
  CHECK_EQ(good["row"]["timeMs"].get<double>(), 1234.0);
  auto unknown = decode(handle, bytes, 2);
  CHECK_EQ(unknown["row"]["crc"].get<std::string>(), "none");
  auto damaged = bytes;
  damaged.back() ^= 0x80;
  auto bad = decode(handle, damaged, 1);
  CHECK_EQ(bad["row"]["crc"].get<std::string>(), "bad");
  CHECK(bad["detail"]["rawPayload"] == damaged);
  CHECK_NE(bad["detail"]["crcValue"], good["detail"]["crcValue"]);
  const auto sentinel = wire(0x0bad);
  const auto preserved = decode(handle, sentinel);
  CHECK_EQ(preserved["row"]["msgType"].get<std::string>(), "无法解码");
  CHECK(preserved["detail"]["rawPayload"] == sentinel);
  pdscope_live_pd_close(handle);
}

TEST(live_pd_truncated_error_reset_and_u64_time) {
  auto *handle = pdscope_live_pd_create();
  const Bytes prefix{0x81, 0x11, 0xa5, 0x5a, 0x12, 0x34};
  auto partial = decode(handle, prefix, 6);
  CHECK(partial["detail"]["crcValue"].is_null());
  CHECK(partial["detail"]["rawPayload"] == prefix);
  CHECK_EQ(partial["row"]["crc"].get<std::string>(), "none");
  auto rx = decode(handle, wire(0x0183), 10);
  CHECK(rx["detail"]["crcValue"].is_null());
  auto reset = decode(handle, {}, 2, 42, 3);
  CHECK_EQ(reset["row"]["msgType"].get<std::string>(), "Hard Reset");
  CHECK(reset["detail"]["rawPayload"].empty());
  auto late = decode(handle, wire(0x0183), 0, 0x100000002ULL);
  CHECK_EQ(late["row"]["timeMs"].get<double>(), 4294967298.0);
  CHECK_EQ(pdscope_live_pd_reset(handle), PDSCOPE_OK);
  auto fresh = decode(handle, wire(0x0183), 0, 0);
  CHECK_EQ(fresh["row"]["timeMs"].get<double>(), 0.0);
  pdscope_live_pd_close(handle);
}

TEST(live_pd_argument_boundary_and_state_reset) {
  CHECK_EQ(pdscope_live_pd_reset(nullptr), PDSCOPE_ERR_ARGUMENT);
  pdscope_live_pd_close(nullptr);
  auto *handle = pdscope_live_pd_create();
  const auto bytes = wire(0x0183);
  pdscope_buf output{};
  CHECK_EQ(
      pdscope_live_pd_decode(handle, nullptr, 6, 0, 1, 0, 0, 1000, 0, &output),
      PDSCOPE_ERR_ARGUMENT);
  CHECK_EQ(pdscope_live_pd_decode(handle, bytes.data(), bytes.size(), 0, 3, 0,
                                  0, 1000, 0, &output),
           PDSCOPE_ERR_ARGUMENT);
  CHECK_EQ(pdscope_live_pd_decode(handle, bytes.data(), bytes.size(), 0, 1, 3,
                                  0, 1000, 0, &output),
           PDSCOPE_ERR_ARGUMENT);
  CHECK_EQ(pdscope_live_pd_decode(handle, bytes.data(), bytes.size(), 0, 1, 0,
                                  0, 999, 0, &output),
           PDSCOPE_ERR_ARGUMENT);
  CHECK(output.data == nullptr);
  const auto capabilities = wire(0x1181, {(100u << 10) | 300u});
  const auto request = wire(0x1082, {(1u << 28) | (300u << 10) | 300u});
  decode(handle, capabilities);
  const auto associated = decode(handle, request);
  CHECK_EQ(pdscope_live_pd_reset(handle), PDSCOPE_OK);
  const auto isolated = decode(handle, request);
  CHECK(associated["detail"]["details"] != isolated["detail"]["details"]);
  decode(handle, capabilities, 1);
  const auto afterBad = decode(handle, request);
  CHECK(afterBad["detail"]["details"] == isolated["detail"]["details"]);
  pdscope_live_pd_close(handle);
}
