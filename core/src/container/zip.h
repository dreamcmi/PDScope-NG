// zip.h — 最小只读 ZIP / ZIP64 读取器（`.atkcc` 的容器层）
//
// 只实现读取所需的最小子集：中央目录 → 目录项 → 本地文件头 → raw DEFLATE。
// 不解压的部分不做（不写、不删、不支持受密码保护、不支持 ZIP 加密）。
//
// 与 JS 版（src/js/core/zip.js）的差别：
//   · 每个条目解压后**核对 ZIP 里记录的 CRC-32**（计划 §4 的要求）；
//   · 所有偏移/长度先校验再访问，越界一律报「偏移 + 原因」而不是静默读脏数据。
#pragma once

#include "util.h"

#include <memory>
#include <utility>

namespace pdscope {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc32 = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint64_t localOffset = 0;
    uint16_t dosTime = 0;
    uint16_t dosDate = 0;
    bool isDirectory = false;
};

class ZipReader {
public:
    /** @param bytes 整个 .atkcc / .zip 文件内容（内部复制一份自持）。 */
    explicit ZipReader(const Bytes& bytes);
    /**
     * 与调用方**共享**同一份字节。
     *
     * ⚠ 容器对象（`AtkccCapture`）会被移动进 `shared_ptr`/`unique_ptr`，
     *   而移动会把它自己那个 `Bytes` 成员搬空 —— 若这里只存 `const Bytes&`，
     *   引用就会指向「已被搬空的旧成员」，之后每次读块都是悬垂访问
     *   （症状：容器元数据读得出来，一开始解码就报「本地文件头越界」或直接段错误）。
     *   共享 owner 是唯一能同时满足「零拷贝」与「可移动」的写法。
     */
    explicit ZipReader(std::shared_ptr<const Bytes> bytes);

    bool has(const std::string& name) const { return find(name) != nullptr; }
    const ZipEntry* find(const std::string& name) const;

    const std::vector<ZipEntry>& entries() const { return entries_; }

    /** 该目录项的原始压缩数据（不解压）。返回的是入参 bytes 里的切片。 */
    std::pair<const uint8_t*, size_t> rawData(const ZipEntry& e) const;

    /** 解压单个条目。不存在返回 false。 */
    bool read(const std::string& name, Bytes& out) const;

    /** 解压为文本（用于解析 ini）。不存在返回 false。 */
    bool readText(const std::string& name, std::string& out) const;

    const std::string& lastWarning() const { return warning_; }

private:
    // 声明顺序即初始化顺序：`bytes_` 绑定到 `*own_`，所以 own_ 必须在前。
    std::shared_ptr<const Bytes> own_;
    const Bytes& bytes_;
    std::vector<ZipEntry> entries_;
    std::map<std::string, size_t> byName_;
    std::string warning_;
};

}  // namespace pdscope
