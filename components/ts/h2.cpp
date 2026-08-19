#include <cstdint>
#include "h2.hpp"

#include <cstdio>
#include <cstring>

namespace ts {
namespace {

void put_u24(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 16);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v);
}

void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t get_u24(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

uint32_t get_u32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

size_t write_frame_header(uint8_t* out, uint32_t payload_len, H2Type type, uint8_t flags,
                          uint32_t stream_id)
{
    put_u24(out, payload_len);
    out[3] = static_cast<uint8_t>(type);
    out[4] = flags;
    put_u32(out + 5, stream_id & 0x7FFFFFFF);
    return kH2FrameHeader;
}

// HPACK: 整数（プレフィックス長 prefix_bits、上位ビットは既に out[0] に入っている前提）
size_t hpack_int(uint8_t* out, size_t cap, uint8_t prefix, uint8_t prefix_bits, uint32_t value)
{
    const uint32_t max_prefix = (1u << prefix_bits) - 1;
    if (cap < 1) return 0;
    if (value < max_prefix) {
        out[0] = static_cast<uint8_t>(prefix | value);
        return 1;
    }
    out[0]      = static_cast<uint8_t>(prefix | max_prefix);
    size_t n    = 1;
    uint32_t v  = value - max_prefix;
    while (v >= 128) {
        if (n >= cap) return 0;
        out[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    if (n >= cap) return 0;
    out[n++] = static_cast<uint8_t>(v);
    return n;
}

// HPACK: 文字列（Huffman なし）
size_t hpack_string(uint8_t* out, size_t cap, const char* s)
{
    const size_t len = std::strlen(s);
    size_t       n   = hpack_int(out, cap, 0x00, 7, static_cast<uint32_t>(len));
    if (n == 0 || n + len > cap) return 0;
    std::memcpy(out + n, s, len);
    return n + len;
}

// HPACK: 静的テーブルのインデックス付きヘッダ（値だけリテラル）
size_t hpack_indexed_name(uint8_t* out, size_t cap, uint8_t index, const char* value)
{
    // 0x00 = literal without indexing, indexed name（動的テーブルを使わない）
    size_t n = hpack_int(out, cap, 0x00, 4, index);
    if (n == 0) return 0;
    const size_t m = hpack_string(out + n, cap - n, value);
    if (m == 0) return 0;
    return n + m;
}

// HPACK: 完全に既定のヘッダ（静的テーブルのインデックスのみ）
size_t hpack_indexed(uint8_t* out, size_t cap, uint8_t index)
{
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(0x80 | index);
    return 1;
}

// 静的テーブルのインデックス（RFC 7541 Appendix A）
constexpr uint8_t kIdxAuthority     = 1;
constexpr uint8_t kIdxMethodPost    = 3;
constexpr uint8_t kIdxPathRoot      = 4;
constexpr uint8_t kIdxSchemeHttp    = 6;
constexpr uint8_t kIdxContentLength = 28;
constexpr uint8_t kIdxContentType   = 31;

}  // namespace

size_t h2_build_preface(uint8_t* out, size_t cap)
{
    // SETTINGS: INITIAL_WINDOW_SIZE と MAX_FRAME_SIZE を大きめに。
    // long-poll で MapResponse が流れ続けるので、小さいと WINDOW_UPDATE の往復が増える。
    constexpr size_t kSettingsCount = 2;
    const size_t     need = kH2PrefaceLen + kH2FrameHeader + kSettingsCount * 6;
    if (cap < need) return 0;

    size_t n = 0;
    std::memcpy(out, kH2Preface, kH2PrefaceLen);
    n += kH2PrefaceLen;

    n += write_frame_header(out + n, kSettingsCount * 6, H2Type::kSettings, 0, 0);
    // SETTINGS_INITIAL_WINDOW_SIZE = 1MB
    out[n] = 0x00;
    out[n + 1] = 0x04;
    put_u32(out + n + 2, 1u << 20);
    n += 6;
    // SETTINGS_MAX_FRAME_SIZE = 16384（既定値だが明示しておく）
    out[n] = 0x00;
    out[n + 1] = 0x05;
    put_u32(out + n + 2, 16384);
    n += 6;
    return n;
}

size_t h2_build_settings_ack(uint8_t* out, size_t cap)
{
    if (cap < kH2FrameHeader) return 0;
    return write_frame_header(out, 0, H2Type::kSettings, kFlagAck, 0);
}

size_t h2_build_ping_ack(uint8_t* out, size_t cap, const uint8_t opaque[8])
{
    if (cap < kH2FrameHeader + 8) return 0;
    size_t n = write_frame_header(out, 8, H2Type::kPing, kFlagAck, 0);
    std::memcpy(out + n, opaque, 8);
    return n + 8;
}

size_t h2_build_window_update(uint8_t* out, size_t cap, uint32_t stream_id, uint32_t increment)
{
    if (cap < kH2FrameHeader + 4) return 0;
    size_t n = write_frame_header(out, 4, H2Type::kWindowUpdate, 0, stream_id);
    put_u32(out + n, increment & 0x7FFFFFFF);
    return n + 4;
}

size_t h2_build_post(uint8_t* out, size_t cap, uint32_t stream_id, const char* authority,
                     const char* path, const uint8_t* body, size_t body_len)
{
    // まずヘッダブロックを組み立てる。
    uint8_t hdr[512];
    size_t  h = 0;
    auto    add = [&](size_t written) {
        if (written == 0) {
            h = 0;
            return false;
        }
        h += written;
        return true;
    };
    if (!add(hpack_indexed(hdr + h, sizeof(hdr) - h, kIdxMethodPost))) return 0;
    if (!add(hpack_indexed(hdr + h, sizeof(hdr) - h, kIdxSchemeHttp))) return 0;
    if (!add(hpack_indexed_name(hdr + h, sizeof(hdr) - h, kIdxPathRoot, path))) return 0;
    if (!add(hpack_indexed_name(hdr + h, sizeof(hdr) - h, kIdxAuthority, authority))) return 0;
    if (!add(hpack_indexed_name(hdr + h, sizeof(hdr) - h, kIdxContentType, "application/json"))) {
        return 0;
    }
    if (body_len > 0) {
        char len_str[16];
        std::snprintf(len_str, sizeof(len_str), "%u", static_cast<unsigned>(body_len));
        if (!add(hpack_indexed_name(hdr + h, sizeof(hdr) - h, kIdxContentLength, len_str))) {
            return 0;
        }
    }

    const bool   end_with_headers = (body_len == 0);
    const size_t need = kH2FrameHeader + h + (body_len ? kH2FrameHeader + body_len : 0);
    if (cap < need) return 0;

    size_t n = write_frame_header(out, static_cast<uint32_t>(h), H2Type::kHeaders,
                                 static_cast<uint8_t>(kFlagEndHeaders |
                                                      (end_with_headers ? kFlagEndStream : 0)),
                                 stream_id);
    std::memcpy(out + n, hdr, h);
    n += h;

    if (body_len > 0) {
        n += write_frame_header(out + n, static_cast<uint32_t>(body_len), H2Type::kData,
                                kFlagEndStream, stream_id);
        std::memcpy(out + n, body, body_len);
        n += body_len;
    }
    return n;
}

bool h2_parse_frame(const uint8_t* in, size_t len, H2Frame* out, size_t* consumed)
{
    if (consumed) *consumed = 0;
    if (len < kH2FrameHeader) return false;
    const uint32_t payload_len = get_u24(in);
    if (len < kH2FrameHeader + payload_len) return false;

    if (out) {
        out->type        = static_cast<H2Type>(in[3]);
        out->flags       = in[4];
        out->stream_id   = get_u32(in + 5) & 0x7FFFFFFF;
        out->payload     = in + kH2FrameHeader;
        out->payload_len = payload_len;
    }
    if (consumed) *consumed = kH2FrameHeader + payload_len;
    return true;
}

bool h2_headers_is_status_200(const uint8_t* payload, size_t len)
{
    // 静的テーブル index 8 = ":status 200"。indexed field は 0x80 | 8 = 0x88。
    // サーバは普通これを最初に置く。違う形で来たら判定しない（false）。
    return len >= 1 && payload[0] == 0x88;
}

}  // namespace ts
