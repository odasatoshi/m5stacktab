// HTTP/2 最小クライアントのホストテスト。組み立てたバイト列を自分でパースして検証する。
#include <cstdint>

#include "h2.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

void check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

void test_preface()
{
    uint8_t buf[128];
    const size_t n = ts::h2_build_preface(buf, sizeof(buf));
    CHECK(n > 0);
    // プリフェイスは決まった 24 バイト
    CHECK(std::memcmp(buf, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0);

    // 続いて SETTINGS フレーム
    ts::H2Frame f;
    size_t      consumed = 0;
    CHECK(ts::h2_parse_frame(buf + 24, n - 24, &f, &consumed));
    CHECK(f.type == ts::H2Type::kSettings);
    CHECK(f.stream_id == 0);
    CHECK(f.flags == 0);
    CHECK(f.payload_len == 12);  // 2 エントリ x 6 バイト
    CHECK(consumed == ts::kH2FrameHeader + 12);
    // 1 つ目は INITIAL_WINDOW_SIZE (0x04) = 1MB
    CHECK(f.payload[0] == 0 && f.payload[1] == 4);
    const uint32_t win = (static_cast<uint32_t>(f.payload[2]) << 24) |
                         (static_cast<uint32_t>(f.payload[3]) << 16) |
                         (static_cast<uint32_t>(f.payload[4]) << 8) | f.payload[5];
    CHECK(win == (1u << 20));

    // バッファが足りなければ 0
    CHECK(ts::h2_build_preface(buf, 10) == 0);
}

void test_acks_and_window()
{
    uint8_t     buf[64];
    ts::H2Frame f;
    size_t      consumed = 0;

    const size_t a = ts::h2_build_settings_ack(buf, sizeof(buf));
    CHECK(a == ts::kH2FrameHeader);
    CHECK(ts::h2_parse_frame(buf, a, &f, &consumed));
    CHECK(f.type == ts::H2Type::kSettings);
    CHECK(f.flags == ts::kFlagAck);
    CHECK(f.payload_len == 0);

    const uint8_t opaque[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t  p = ts::h2_build_ping_ack(buf, sizeof(buf), opaque);
    CHECK(ts::h2_parse_frame(buf, p, &f, &consumed));
    CHECK(f.type == ts::H2Type::kPing);
    CHECK(f.flags == ts::kFlagAck);
    CHECK(f.payload_len == 8);
    CHECK(std::memcmp(f.payload, opaque, 8) == 0);

    const size_t w = ts::h2_build_window_update(buf, sizeof(buf), 1, 65535);
    CHECK(ts::h2_parse_frame(buf, w, &f, &consumed));
    CHECK(f.type == ts::H2Type::kWindowUpdate);
    CHECK(f.stream_id == 1);
    CHECK(f.payload_len == 4);
    const uint32_t inc = (static_cast<uint32_t>(f.payload[0]) << 24) |
                         (static_cast<uint32_t>(f.payload[1]) << 16) |
                         (static_cast<uint32_t>(f.payload[2]) << 8) | f.payload[3];
    CHECK(inc == 65535);
}

void test_post()
{
    uint8_t     buf[1024];
    const char* body = "{\"Version\":131}";
    const size_t n = ts::h2_build_post(buf, sizeof(buf), 1, "controlplane.example",
                                       "/machine/register",
                                       reinterpret_cast<const uint8_t*>(body), std::strlen(body));
    CHECK(n > 0);

    // HEADERS フレーム
    ts::H2Frame f;
    size_t      consumed = 0;
    CHECK(ts::h2_parse_frame(buf, n, &f, &consumed));
    CHECK(f.type == ts::H2Type::kHeaders);
    CHECK(f.stream_id == 1);
    CHECK((f.flags & ts::kFlagEndHeaders) != 0);
    // ボディがあるので HEADERS では END_STREAM を立てない
    CHECK((f.flags & ts::kFlagEndStream) == 0);
    // 先頭は :method POST の indexed field (0x80 | 3)
    CHECK(f.payload[0] == 0x83);
    // :scheme http (0x80 | 6)
    CHECK(f.payload[1] == 0x86);
    // パスと authority が生の文字列として入っている（Huffman なし）
    const std::string block(reinterpret_cast<const char*>(f.payload), f.payload_len);
    CHECK(block.find("/machine/register") != std::string::npos);
    CHECK(block.find("controlplane.example") != std::string::npos);
    CHECK(block.find("application/json") != std::string::npos);
    CHECK(block.find("15") != std::string::npos);  // content-length

    // 続いて DATA フレーム
    ts::H2Frame d;
    size_t      c2 = 0;
    CHECK(ts::h2_parse_frame(buf + consumed, n - consumed, &d, &c2));
    CHECK(d.type == ts::H2Type::kData);
    CHECK(d.stream_id == 1);
    CHECK((d.flags & ts::kFlagEndStream) != 0);
    CHECK(d.payload_len == std::strlen(body));
    CHECK(std::memcmp(d.payload, body, d.payload_len) == 0);
    CHECK(consumed + c2 == n);

    // ボディなしなら HEADERS だけで END_STREAM が立つ
    const size_t m = ts::h2_build_post(buf, sizeof(buf), 3, "h", "/p", nullptr, 0);
    CHECK(ts::h2_parse_frame(buf, m, &f, &consumed));
    CHECK((f.flags & ts::kFlagEndStream) != 0);
    CHECK(consumed == m);

    // バッファ不足で 0
    CHECK(ts::h2_build_post(buf, 20, 1, "h", "/p", reinterpret_cast<const uint8_t*>(body), 15) == 0);
}

void test_long_path_hpack_int()
{
    // 長い path は HPACK の可変長整数を使う（127 バイト超で 2 バイト以上になる）
    std::string long_path = "/machine/map?";
    long_path.append(200, 'x');
    uint8_t buf[1024];
    const size_t n = ts::h2_build_post(buf, sizeof(buf), 1, "h", long_path.c_str(), nullptr, 0);
    CHECK(n > 0);
    ts::H2Frame f;
    size_t      consumed = 0;
    CHECK(ts::h2_parse_frame(buf, n, &f, &consumed));
    const std::string block(reinterpret_cast<const char*>(f.payload), f.payload_len);
    CHECK(block.find(long_path) != std::string::npos);
}

void test_parse_streaming()
{
    // フレームが細切れに届いても、揃った時点で 1 つずつ取り出せること
    uint8_t     src[256];
    const char* body = "hello";
    const size_t n = ts::h2_build_post(src, sizeof(src), 1, "h", "/p",
                                       reinterpret_cast<const uint8_t*>(body), 5);
    std::vector<uint8_t> buf;
    int                  frames = 0;
    for (size_t i = 0; i < n; ++i) {
        buf.push_back(src[i]);
        for (;;) {
            ts::H2Frame f;
            size_t      consumed = 0;
            if (!ts::h2_parse_frame(buf.data(), buf.size(), &f, &consumed)) break;
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
            ++frames;
        }
    }
    CHECK(frames == 2);  // HEADERS + DATA
    CHECK(buf.empty());
}

void test_status_check()
{
    // :status 200 は indexed field 0x88
    const uint8_t ok[] = {0x88, 0x5f};
    CHECK(ts::h2_headers_is_status_200(ok, sizeof(ok)));
    const uint8_t not_ok[] = {0x8b};  // :status 404
    CHECK(!ts::h2_headers_is_status_200(not_ok, sizeof(not_ok)));
    CHECK(!ts::h2_headers_is_status_200(nullptr, 0));
}

}  // namespace

int main()
{
    test_preface();
    test_acks_and_window();
    test_post();
    test_long_path_hpack_int();
    test_parse_streaming();
    test_status_check();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
