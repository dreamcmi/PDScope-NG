#include "zip.h"

#include "inflate.h"

#include <zlib.h>

namespace pdscope {
namespace {

constexpr uint32_t SIG_EOCD      = 0x06054b50;
constexpr uint32_t SIG_EOCD64    = 0x06064b50;
constexpr uint32_t SIG_EOCD64LOC = 0x07064b50;
constexpr uint32_t SIG_CDIR      = 0x02014b50;
constexpr uint32_t SIG_LOCAL     = 0x04034b50;

constexpr size_t EOCD_MIN = 22;
constexpr size_t EOCD_MAX_SCAN = 0xffff + EOCD_MIN;

constexpr uint16_t METHOD_STORE = 0;
constexpr uint16_t METHOD_DEFLATE = 8;

/** 在尾部 64 KiB + 22 字节范围内反向查找 EOCD。返回偏移，找不到返回 npos。 */
size_t findEocd(const Bytes& b) {
    if (b.size() < EOCD_MIN) return std::string::npos;
    const size_t lo = (b.size() > EOCD_MAX_SCAN) ? (b.size() - EOCD_MAX_SCAN) : 0;
    for (size_t i = b.size() - EOCD_MIN + 1; i-- > lo;) {
        if (rdU32LE(b.data() + i) == SIG_EOCD) {
            // 注释长度必须与剩余字节吻合，避免误命中
            const uint16_t commentLen = rdU16LE(b.data() + i + 20);
            if (i + EOCD_MIN + commentLen == b.size()) return i;
        }
        if (i == lo) break;
    }
    return std::string::npos;
}

}  // namespace

// 复制一份再解析：给「调用方自己还留着 bytes 且不共享」的场景用（测试为主）。
ZipReader::ZipReader(const Bytes& bytes)
    : ZipReader(std::make_shared<const Bytes>(bytes)) {}

// 共享同一份字节：容器被移动也不会让 bytes_ 悬垂（见 zip.h 的说明）。
ZipReader::ZipReader(std::shared_ptr<const Bytes> bytes)
    : own_(std::move(bytes)), bytes_(*own_) {
    const size_t eocd = findEocd(bytes_);
    if (eocd == std::string::npos) {
        failFormat("不是有效的 ZIP 文件：未找到 EOCD 记录");
    }

    uint64_t entryCount = rdU16LE(bytes_.data() + eocd + 10);
    uint64_t cdSize = rdU32LE(bytes_.data() + eocd + 12);
    uint64_t cdOffset = rdU32LE(bytes_.data() + eocd + 16);

    // ZIP64 升级：三个字段任何一个取到哨兵值就走 ZIP64 EOCD
    if (entryCount == 0xffff || cdSize == 0xffffffffull || cdOffset == 0xffffffffull) {
        if (eocd >= 20) {
            const size_t loc = eocd - 20;
            if (rdU32LE(bytes_.data() + loc) == SIG_EOCD64LOC) {
                const uint64_t z64 = rdU64LE(bytes_.data() + loc + 8);
                if (z64 + 56 <= bytes_.size() && rdU32LE(bytes_.data() + z64) == SIG_EOCD64) {
                    entryCount = rdU64LE(bytes_.data() + z64 + 32);
                    cdSize = rdU64LE(bytes_.data() + z64 + 40);
                    cdOffset = rdU64LE(bytes_.data() + z64 + 48);
                } else if (z64 + 56 > bytes_.size()) {
                    failFormat("ZIP64 中央目录偏移 " + std::to_string(z64) + " 越界");
                }
            }
        }
    }

    if (cdOffset > bytes_.size()) {
        failFormat("中央目录偏移 " + std::to_string(cdOffset) + " 超出文件大小 "
                   + std::to_string(bytes_.size()));
    }
    const uint64_t end = std::min<uint64_t>(cdOffset + cdSize, bytes_.size());

    uint64_t p = cdOffset;
    entries_.reserve(static_cast<size_t>(std::min<uint64_t>(entryCount, 1u << 20)));
    while (p + 46 <= end && rdU32LE(bytes_.data() + p) == SIG_CDIR) {
        const uint8_t* e = bytes_.data() + p;
        ZipEntry ent;
        ent.flags = rdU16LE(e + 8);
        ent.method = rdU16LE(e + 10);
        ent.dosTime = rdU16LE(e + 12);
        ent.dosDate = rdU16LE(e + 14);
        ent.crc32 = rdU32LE(e + 16);
        ent.compressedSize = rdU32LE(e + 20);
        ent.uncompressedSize = rdU32LE(e + 24);
        const uint16_t nameLen = rdU16LE(e + 28);
        const uint16_t extraLen = rdU16LE(e + 30);
        const uint16_t commentLen = rdU16LE(e + 32);
        ent.localOffset = rdU32LE(e + 42);

        if (p + 46 + nameLen > bytes_.size()) {
            failFormat("中央目录项名字越界（偏移 " + std::to_string(p) + "）");
        }
        ent.name = toValidUtf8(e + 46, nameLen);

        // ZIP64 扩展字段（0x0001）
        uint64_t ep = p + 46 + nameLen;
        const uint64_t extraEnd = ep + extraLen;
        if (extraEnd > bytes_.size()) {
            failFormat("中央目录项扩展字段越界（条目 " + ent.name + "）");
        }
        while (ep + 4 <= extraEnd) {
            const uint16_t hid = rdU16LE(bytes_.data() + ep);
            const uint16_t hsz = rdU16LE(bytes_.data() + ep + 2);
            if (hid == 0x0001) {
                uint64_t q = ep + 4;
                const uint64_t limit = ep + 4 + hsz;
                if (ent.uncompressedSize == 0xffffffffull && q + 8 <= limit) {
                    ent.uncompressedSize = rdU64LE(bytes_.data() + q); q += 8;
                }
                if (ent.compressedSize == 0xffffffffull && q + 8 <= limit) {
                    ent.compressedSize = rdU64LE(bytes_.data() + q); q += 8;
                }
                if (ent.localOffset == 0xffffffffull && q + 8 <= limit) {
                    ent.localOffset = rdU64LE(bytes_.data() + q); q += 8;
                }
            }
            ep += 4 + hsz;
        }

        ent.isDirectory = !ent.name.empty() && ent.name.back() == '/';

        if (ent.localOffset > bytes_.size()) {
            failFormat("条目 " + ent.name + " 的本地文件头偏移 " + std::to_string(ent.localOffset)
                       + " 超出文件大小");
        }
        if (!ent.isDirectory && ent.compressedSize > bytes_.size() - ent.localOffset) {
            failFormat("条目 " + ent.name + " 的压缩长度 " + std::to_string(ent.compressedSize)
                       + " 超出文件剩余空间");
        }

        byName_[ent.name] = entries_.size();
        entries_.push_back(std::move(ent));

        p += 46ull + nameLen + extraLen + commentLen;
    }
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &entries_[it->second];
}

std::pair<const uint8_t*, size_t> ZipReader::rawData(const ZipEntry& e) const {
    const uint64_t lo = e.localOffset;
    if (lo + 30 > bytes_.size()) {
        failFormat("条目 " + e.name + " 的本地文件头越界（偏移 " + std::to_string(lo)
                   + "，文件大小 " + std::to_string(bytes_.size()) + "）");
    }
    if (rdU32LE(bytes_.data() + lo) != SIG_LOCAL) {
        failFormat("本地文件头损坏：" + e.name);
    }
    const uint16_t nameLen = rdU16LE(bytes_.data() + lo + 26);
    const uint16_t extraLen = rdU16LE(bytes_.data() + lo + 28);
    const uint64_t start = lo + 30 + nameLen + extraLen;
    if (start > bytes_.size() || e.compressedSize > bytes_.size() - start) {
        failFormat("条目 " + e.name + " 的压缩数据越界（偏移 " + std::to_string(start) + "）");
    }
    return {bytes_.data() + start, static_cast<size_t>(e.compressedSize)};
}

bool ZipReader::read(const std::string& name, Bytes& out) const {
    const ZipEntry* e = find(name);
    if (!e) return false;
    if (e->isDirectory) { out.clear(); return true; }

    const auto raw = rawData(*e);

    if (e->method == METHOD_STORE) {
        out.assign(raw.first, raw.first + raw.second);
    } else if (e->method == METHOD_DEFLATE) {
        if (e->compressedSize == 0 && e->uncompressedSize == 0) { out.clear(); return true; }
        out = inflateRaw(raw.first, raw.second, e->uncompressedSize, name);
    } else {
        failFormat("不支持的压缩方式 " + std::to_string(e->method) + "（" + name + "）");
    }

    // 核对 CRC-32（zlib 除解压外还要核对 ZIP CRC）
    if (e->crc32 != 0 || e->uncompressedSize > 0) {
        const uint32_t got = static_cast<uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(out.data()), static_cast<uInt>(out.size())));
        if (got != e->crc32) {
            failFormat("条目 " + name + " 的 CRC-32 不符（目录记录 "
                       + hexU(e->crc32, 8) + "，实际 " + hexU(got, 8) + "）");
        }
    }
    return true;
}

bool ZipReader::readText(const std::string& name, std::string& out) const {
    Bytes b;
    if (!read(name, b)) return false;
    out = toValidUtf8(b.data(), b.size());
    return true;
}

}  // namespace pdscope
