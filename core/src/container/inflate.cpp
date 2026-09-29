#include "inflate.h"

#include <zlib.h>

namespace pdscope {
namespace {

// zlib 的 inflate 需要一次给够输出缓冲；用「分块增长 + 触顶失败」保证不无限膨胀。
constexpr size_t kStep = 1u << 20;  // 1 MiB

}  // namespace

Bytes inflateRaw(const uint8_t* data, size_t len, uint64_t expectedSize,
                 const std::string& what) {
    if (expectedSize > kMaxInflateBytes) {
        failFormat("条目 " + what + " 声明的解压大小为 " + std::to_string(expectedSize)
                   + " 字节，超过上限 " + std::to_string(kMaxInflateBytes) + "（拒绝展开）");
    }

    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // windowBits 取负 = raw deflate（无 zlib/gzip 头）
    if (inflateInit2(&zs, -15) != Z_OK) {
        fail(PDSCOPE_STATUS_INTERNAL, "inflateInit2 失败");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(len);

    Bytes out;
    if (expectedSize > 0) out.reserve(static_cast<size_t>(expectedSize));

    Bytes chunk(kStep);
    int rc = Z_OK;
    for (;;) {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());

        rc = inflate(&zs, Z_NO_FLUSH);

        size_t produced = chunk.size() - zs.avail_out;
        if (produced) {
            if (out.size() + produced > kMaxInflateBytes) {
                inflateEnd(&zs);
                failFormat("条目 " + what + " 解压超过上限 " + std::to_string(kMaxInflateBytes)
                           + " 字节，已中止");
            }
            out.insert(out.end(), chunk.data(), chunk.data() + produced);
        }

        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR) {
            // 输入耗尽但仍未结束 —— 数据被截断
            inflateEnd(&zs);
            failFormat("条目 " + what + " 的 deflate 数据不完整（输入在流结束前耗尽）");
        }
        if (rc != Z_OK) {
            const std::string msg = zs.msg ? zs.msg : "未知错误";
            inflateEnd(&zs);
            failFormat("条目 " + what + " 解压失败：" + msg + "（zlib " + std::to_string(rc) + "）");
        }
    }
    inflateEnd(&zs);

    if (expectedSize > 0 && out.size() != expectedSize) {
        failFormat("条目 " + what + " 解压后为 " + std::to_string(out.size())
                   + " 字节，与目录里声明的 " + std::to_string(expectedSize) + " 不符");
    }
    return out;
}

}  // namespace pdscope
