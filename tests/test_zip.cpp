// test_zip.cpp — ZIP 读取器：正常解压、CRC 核对、越界与非法输入

#include "test.h"

#include "container/zip.h"
#include "fixtures.h"

using namespace pdscope;

namespace {

/** 一段可压缩性一般、长度够长的测试数据（太规则的话 deflate 会缩得过小，看不出问题）。 */
Bytes sampleData(size_t n) {
    Bytes b(n);
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        b[i] = static_cast<uint8_t>((s >> 16) & 0xff);
    }
    return b;
}

}  // namespace

TEST(zip_store_and_deflate_roundtrip) {
    const Bytes a = sampleData(4096);
    const Bytes b = sampleData(777);

    const std::string ini = "SF=2500\n";
    std::vector<fx::ZipItem> items;
    items.push_back({"channel.ini", Bytes(ini.begin(), ini.end()), true});
    items.push_back({"0/0-0.bin", a, true});
    items.push_back({"stored.bin", b, false});   // method 0：不解压

    const Bytes zip = fx::makeZip(items);
    CHECK_EQ(rdU32LE(zip.data()), 0x04034b50u);

    ZipReader z(zip);
    CHECK_EQ(z.entries().size(), 3u);
    CHECK(z.has("channel.ini"));
    CHECK(z.has("0/0-0.bin"));
    CHECK(!z.has("nope.bin"));

    Bytes got;
    CHECK(z.read("0/0-0.bin", got));
    CHECK_EQ(got.size(), a.size());
    CHECK(got == a);

    Bytes got2;
    CHECK(z.read("stored.bin", got2));
    CHECK(got2 == b);

    std::string iniText;
    CHECK(z.readText("channel.ini", iniText));
    CHECK_EQ(iniText, std::string("SF=2500\n"));

    const ZipEntry* e = z.find("0/0-0.bin");
    CHECK(e != nullptr);
    if (e) {
        CHECK_EQ(e->method, static_cast<uint16_t>(8));
        CHECK_EQ(e->uncompressedSize, static_cast<uint64_t>(a.size()));
    }
}

TEST(zip_crc_mismatch_is_detected) {
    const Bytes a = sampleData(2048);
    std::vector<fx::ZipItem> items;
    items.push_back({"x.bin", a, true});
    Bytes zip = fx::makeZip(items);

    // 把中央目录里记录的 CRC 改掉一个字节 —— 目录本身仍然自洽，只有 CRC 对不上
    bool patched = false;
    for (size_t i = 0; i + 46 <= zip.size(); ++i) {
        if (rdU32LE(zip.data() + i) == 0x02014b50u) {
            zip[i + 16] ^= 0xFF;
            patched = true;
            break;
        }
    }
    CHECK(patched);

    ZipReader z2(zip);
    Bytes out;
    CHECK_THROWS(z2.read("x.bin", out));
}

TEST(zip_rejects_non_zip) {
    Bytes junk = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    CHECK_THROWS(ZipReader z(junk));
}

TEST(zip_rejects_truncated_entries) {
    const Bytes a = sampleData(1024);
    std::vector<fx::ZipItem> items;
    items.push_back({"x.bin", a, true});
    Bytes zip = fx::makeZip(items);

    // 声称的压缩长度比文件剩余空间还大 —— 必须在**构建读取器时**就报出来
    bool patched = false;
    for (size_t i = 0; i + 46 <= zip.size(); ++i) {
        if (rdU32LE(zip.data() + i) == 0x02014b50u) {
            const uint32_t big = 0x7FFFFFF0u;
            zip[i + 20] = static_cast<uint8_t>(big & 0xff);
            zip[i + 21] = static_cast<uint8_t>((big >> 8) & 0xff);
            zip[i + 22] = static_cast<uint8_t>((big >> 16) & 0xff);
            zip[i + 23] = static_cast<uint8_t>((big >> 24) & 0xff);
            patched = true;
            break;
        }
    }
    CHECK(patched);
    CHECK_THROWS(ZipReader z(zip));
}
