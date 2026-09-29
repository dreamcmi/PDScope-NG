// abi.cpp — C ABI 的唯一实现
//
// 边界纪律：
//   · 不把 C++ 类 / 异常 / STL 容器跨出去（会话以不透明指针 + 句柄整数表示）
//   · 库分配的内存由库释放（pdscope_buf_free / pdscope_str_free）
//   · 任何内部异常都在这里兜住，翻译成状态码 + 错误文本
#include "session.h"

#include <pdscope/pdscope.h>

#include <cstring>
#include <new>
#include <string>

using namespace pdscope;

/* 内部状态值与公开枚举必须一一对应（改了这里就会在编译期报出来） */
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_OK)        == PDSCOPE_OK, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_ARGUMENT)  == PDSCOPE_ERR_ARGUMENT, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_IO)        == PDSCOPE_ERR_IO, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_FORMAT)    == PDSCOPE_ERR_FORMAT, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_UNSUPPORTED) == PDSCOPE_ERR_UNSUPPORTED, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_CANCELLED) == PDSCOPE_ERR_CANCELLED, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_STATE)     == PDSCOPE_ERR_STATE, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_MEMORY)    == PDSCOPE_ERR_MEMORY, "");
static_assert(static_cast<int32_t>(PDSCOPE_STATUS_INTERNAL)  == PDSCOPE_ERR_INTERNAL, "");

namespace {

/** 内部会话壳：把 C++ 对象与「取消后不再接受解码」的语义包在一起。 */
struct SessionHandle {
    std::unique_ptr<Session> session;
};

/* ── 分配/释放 ── */

void setErr(char** err, const std::string& msg) {
    if (!err) return;
    char* p = static_cast<char*>(std::malloc(msg.size() + 1));
    if (!p) { *err = nullptr; return; }
    std::memcpy(p, msg.c_str(), msg.size() + 1);
    *err = p;
}

/** 把字节块交给调用方（out 里的旧内容先释放）。 */
int32_t giveBuffer(pdscope_buf* out, Bytes&& bytes) {
    if (!out) return PDSCOPE_ERR_ARGUMENT;
    if (out->data) { std::free(out->data); out->data = nullptr; out->len = 0; }
    if (bytes.empty()) return PDSCOPE_OK;
    uint8_t* p = static_cast<uint8_t*>(std::malloc(bytes.size()));
    if (!p) return PDSCOPE_ERR_MEMORY;
    std::memcpy(p, bytes.data(), bytes.size());
    out->data = p;
    out->len = bytes.size();
    return PDSCOPE_OK;
}

int32_t giveString(pdscope_buf* out, const std::string& s) {
    Bytes b(s.begin(), s.end());
    return giveBuffer(out, std::move(b));
}

int32_t giveJson(pdscope_buf* out, const json& j) {
    const std::string s = j.dump();
    return giveString(out, s);
}

/** 统一的异常兜底：把内部错误翻译成状态码并写错误文本。 */
template <typename Fn>
int32_t guard(char** err, Fn&& fn) {
    try {
        return fn();
    } catch (const Error& e) {
        setErr(err, e.what());
        return e.status();
    } catch (const std::bad_alloc&) {
        setErr(err, "内存不足");
        return PDSCOPE_ERR_MEMORY;
    } catch (const std::exception& e) {
        setErr(err, std::string("内部错误：") + e.what());
        return PDSCOPE_ERR_INTERNAL;
    } catch (...) {
        setErr(err, "未知的内部错误");
        return PDSCOPE_ERR_INTERNAL;
    }
}

Session* raw(pdscope_session* s) {
    return s ? reinterpret_cast<SessionHandle*>(s)->session.get() : nullptr;
}

bool readFile(const std::string& path, Bytes& out, std::string& why) {
    // 路径是 UTF-8，非 ASCII 一律要经 util 的平台转换（Windows 上走 _wfopen）。
    return readWholeFile(path, out, why);
}

std::string baseName(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

}  // namespace

/* ══════════════════════ 版本 / 状态 ══════════════════════ */

extern "C" {

const char* PDSCOPE_CALL pdscope_version(void) { return PDSCOPE_NG_VERSION; }
uint32_t PDSCOPE_CALL pdscope_abi_version(void) { return PDSCOPE_ABI_VERSION; }

const char* PDSCOPE_CALL pdscope_status_name(int32_t status) {
    switch (status) {
        case PDSCOPE_OK: return "OK";
        case PDSCOPE_ERR_ARGUMENT: return "ERR_ARGUMENT";
        case PDSCOPE_ERR_IO: return "ERR_IO";
        case PDSCOPE_ERR_FORMAT: return "ERR_FORMAT";
        case PDSCOPE_ERR_UNSUPPORTED: return "ERR_UNSUPPORTED";
        case PDSCOPE_ERR_CANCELLED: return "ERR_CANCELLED";
        case PDSCOPE_ERR_STATE: return "ERR_STATE";
        case PDSCOPE_ERR_MEMORY: return "ERR_MEMORY";
        case PDSCOPE_ERR_INTERNAL: return "ERR_INTERNAL";
        default: return "ERR_UNKNOWN";
    }
}

/* ══════════════════════ 内存 ══════════════════════ */

void PDSCOPE_CALL pdscope_buf_free(pdscope_buf* buf) {
    if (!buf) return;
    std::free(buf->data);
    buf->data = nullptr;
    buf->len = 0;
}

void PDSCOPE_CALL pdscope_str_free(char* s) { std::free(s); }

/* ══════════════════════ 会话 ══════════════════════ */

pdscope_session* PDSCOPE_CALL pdscope_open_bytes(const uint8_t* data, size_t len,
                                                 const char* name_hint, char** err_out) {
    if (err_out) *err_out = nullptr;
    if (!data || len == 0) { setErr(err_out, "入参为空：没有字节可解析"); return nullptr; }
    try {
        Bytes bytes(data, data + len);
        const std::string hint = name_hint ? name_hint : "";
        auto h = std::make_unique<SessionHandle>();
        h->session = Session::openBytes(bytes, hint);
        return reinterpret_cast<pdscope_session*>(h.release());
    } catch (const Error& e) {
        setErr(err_out, e.what());
        return nullptr;
    } catch (const std::exception& e) {
        setErr(err_out, std::string("内部错误：") + e.what());
        return nullptr;
    } catch (...) {
        setErr(err_out, "未知的内部错误");
        return nullptr;
    }
}

pdscope_session* PDSCOPE_CALL pdscope_open_file(const char* path_utf8, char** err_out) {
    if (err_out) *err_out = nullptr;
    if (!path_utf8 || !*path_utf8) { setErr(err_out, "入参为空：没有文件路径"); return nullptr; }

    Bytes bytes;
    std::string why;
    if (!readFile(path_utf8, bytes, why)) { setErr(err_out, why); return nullptr; }
    const std::string name = baseName(path_utf8);
    return pdscope_open_bytes(bytes.data(), bytes.size(), name.c_str(), err_out);
}

void PDSCOPE_CALL pdscope_close(pdscope_session* s) {
    delete reinterpret_cast<SessionHandle*>(s);
}

/* ══════════════════════ 元数据 ══════════════════════ */

int32_t PDSCOPE_CALL pdscope_metadata(pdscope_session* s, pdscope_buf* out_json) {
    if (!s || !out_json) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&] {
        return giveJson(out_json, raw(s)->metadata());
    });
}

/* ══════════════════════ 解码 ══════════════════════ */

int32_t PDSCOPE_CALL pdscope_decode(pdscope_session* s, const pdscope_decode_opts* opts,
                                    pdscope_buf* out_stats_json) {
    if (!s) return PDSCOPE_ERR_ARGUMENT;
    int32_t st = guard(nullptr, [&]() -> int32_t {
        Session* sess = raw(s);
        const int channel = opts ? opts->channel : -1;
        const double rate = opts ? opts->sample_rate_override : 0.0;
        const bool metaOnly = opts ? (opts->metadata_only != 0) : false;
        sess->decode(channel, rate, metaOnly);
        if (sess->cancelled()) return PDSCOPE_ERR_CANCELLED;
        if (out_stats_json) return giveJson(out_stats_json, sess->statsJson());
        return PDSCOPE_OK;
    });
    return st;
}

void PDSCOPE_CALL pdscope_cancel(pdscope_session* s) {
    Session* sess = raw(s);
    if (sess) sess->cancel();
}

int32_t PDSCOPE_CALL pdscope_get_progress(pdscope_session* s, pdscope_progress* out) {
    if (!s || !out) return PDSCOPE_ERR_ARGUMENT;
    int phase = 0, channel = 0;
    uint64_t done = 0, total = 0, packets = 0;
    raw(s)->snapshotProgress(&phase, &channel, &done, &total, &packets);
    out->phase = phase;
    out->channel = channel;
    out->done = done;
    out->total = total;
    out->packets = packets;
    return PDSCOPE_OK;
}

/* ══════════════════════ 筛选 / 视图 ══════════════════════ */

int32_t PDSCOPE_CALL pdscope_set_filter(pdscope_session* s, const char* filter_json,
                                        char** err_out) {
    if (!s) return PDSCOPE_ERR_ARGUMENT;
    if (err_out) *err_out = nullptr;
    return guard(err_out, [&]() -> int32_t {
        json j = json::object();
        if (filter_json && *filter_json) {
            // 允许传 `{}` 或 NULL 表示清空筛选
            try {
                j = json::parse(filter_json);
            } catch (const json::parse_error& e) {
                setErr(err_out, std::string("筛选条件不是合法 JSON：") + e.what());
                return PDSCOPE_ERR_ARGUMENT;
            }
        }
        raw(s)->setFilter(j);
        return PDSCOPE_OK;
    });
}

int32_t PDSCOPE_CALL pdscope_view_count(pdscope_session* s, uint64_t* out_count) {
    if (!s || !out_count) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        *out_count = raw(s)->viewCount();
        return PDSCOPE_OK;
    });
}

int32_t PDSCOPE_CALL pdscope_packet_count(pdscope_session* s, uint64_t* out_count) {
    if (!s || !out_count) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        *out_count = raw(s)->packetCount();
        return PDSCOPE_OK;
    });
}

/* ══════════════════════ 查询 ══════════════════════ */

int32_t PDSCOPE_CALL pdscope_query_page(pdscope_session* s, uint64_t offset, uint32_t limit,
                                        pdscope_buf* out_json) {
    if (!s || !out_json) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        return giveJson(out_json, raw(s)->pageJson(offset, limit));
    });
}

int32_t PDSCOPE_CALL pdscope_packet_detail(pdscope_session* s, uint64_t index,
                                           pdscope_buf* out_json) {
    if (!s || !out_json) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        return giveJson(out_json, raw(s)->packetDetailJson(index));
    });
}

int32_t PDSCOPE_CALL pdscope_waveform_range(pdscope_session* s, int32_t channel,
                                            uint64_t start_sample, uint64_t end_sample,
                                            uint32_t max_points, pdscope_buf* out_binary) {
    if (!s || !out_binary) return PDSCOPE_ERR_ARGUMENT;
    if (end_sample < start_sample) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        Bytes b = raw(s)->waveformBinary(channel, start_sample, end_sample, max_points);
        return giveBuffer(out_binary, std::move(b));
    });
}

int32_t PDSCOPE_CALL pdscope_bus_series(pdscope_session* s, uint32_t target_points,
                                        pdscope_buf* out_json) {
    if (!s || !out_json) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        return giveJson(out_json, raw(s)->busSeriesJson(target_points ? target_points : 2400));
    });
}

int32_t PDSCOPE_CALL pdscope_type_counts(pdscope_session* s, pdscope_buf* out_json) {
    if (!s || !out_json) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        return giveJson(out_json, raw(s)->typeCountsJson());
    });
}

int32_t PDSCOPE_CALL pdscope_packet_marks(pdscope_session* s, pdscope_buf* out_binary) {
    if (!s || !out_binary) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        return giveBuffer(out_binary, raw(s)->packetMarksBinary());
    });
}

/* ══════════════════════ 导出 ══════════════════════ */

int32_t PDSCOPE_CALL pdscope_export_csv(pdscope_session* s, const pdscope_export_opts* opts,
                                        pdscope_buf* out_text) {
    if (!s || !out_text) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        const uint64_t limit = opts ? opts->limit : 0;
        const bool bom = opts ? (opts->bom != 0) : true;
        return giveString(out_text, raw(s)->exportCsv(limit, bom));
    });
}

int32_t PDSCOPE_CALL pdscope_export_json(pdscope_session* s, const pdscope_export_opts* opts,
                                         pdscope_buf* out_text) {
    if (!s || !out_text) return PDSCOPE_ERR_ARGUMENT;
    return guard(nullptr, [&]() -> int32_t {
        const uint64_t limit = opts ? opts->limit : 0;
        return giveString(out_text, raw(s)->exportJson(limit));
    });
}

int32_t PDSCOPE_CALL pdscope_default_csv_name(pdscope_session* s, char** out_name) {
    if (!s || !out_name) return PDSCOPE_ERR_ARGUMENT;
    *out_name = nullptr;
    return guard(nullptr, [&]() -> int32_t {
        const std::string n = raw(s)->defaultCsvName();
        char* p = static_cast<char*>(std::malloc(n.size() + 1));
        if (!p) return PDSCOPE_ERR_MEMORY;
        std::memcpy(p, n.c_str(), n.size() + 1);
        *out_name = p;
        return PDSCOPE_OK;
    });
}

}  // extern "C"
