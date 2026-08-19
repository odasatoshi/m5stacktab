// 制御プレーンのうちソケットを使わない部分のホストテスト。
#include <cstdint>

#include "ts_control.hpp"

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

void expect(const std::string& got, const std::string& want, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d\n  got  %s\n  want %s\n", __FILE__, line, got.c_str(),
                     want.c_str());
        std::abort();
    }
}
#define EXPECT(a, b) expect((a), (b), __LINE__)

void test_key_strings()
{
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    const std::string s = ts::key_to_string("nodekey:", key);
    EXPECT(s, "nodekey:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    uint8_t back[32] = {};
    CHECK(ts::key_from_string(s, "nodekey:", back));
    CHECK(std::memcmp(key, back, 32) == 0);

    // prefix が違えば失敗
    CHECK(!ts::key_from_string(s, "mkey:", back));
    // 長さが違えば失敗
    CHECK(!ts::key_from_string("nodekey:00", "nodekey:", back));
    // hex でない文字は失敗
    std::string bad = s;
    bad[10] = 'z';
    CHECK(!ts::key_from_string(bad, "nodekey:", back));
}

void test_upgrade_request()
{
    uint8_t msg1[101];
    for (size_t i = 0; i < sizeof(msg1); ++i) msg1[i] = static_cast<uint8_t>(i);

    char         buf[1024];
    const size_t n = ts::build_upgrade_request(buf, sizeof(buf), "controlplane.example", msg1,
                                               sizeof(msg1));
    CHECK(n > 0);
    const std::string req(buf, n);
    CHECK(req.compare(0, 22, "POST /ts2021 HTTP/1.1\r") == 0);
    CHECK(req.find("Host: controlplane.example\r\n") != std::string::npos);
    CHECK(req.find("Upgrade: tailscale-control-protocol\r\n") != std::string::npos);
    CHECK(req.find("Connection: upgrade\r\n") != std::string::npos);
    CHECK(req.find("X-Tailscale-Handshake: ") != std::string::npos);
    CHECK(req.find("Content-Length: 0\r\n") != std::string::npos);
    CHECK(req.compare(req.size() - 4, 4, "\r\n\r\n") == 0);
    // base64 は 101 バイト → 136 文字（パディング込み）
    const size_t hs = req.find("X-Tailscale-Handshake: ") + 23;
    const size_t eol = req.find("\r\n", hs);
    CHECK(eol - hs == 136);

    // バッファ不足では 0
    CHECK(ts::build_upgrade_request(buf, 32, "h", msg1, sizeof(msg1)) == 0);
}

void test_upgrade_response()
{
    // ヘッダの後ろに Noise の msg2 が続く（同じ TCP セグメントで来る）
    const std::string head =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: tailscale-control-protocol\r\n"
        "Connection: upgrade\r\n"
        "\r\n";
    std::vector<uint8_t> wire(head.begin(), head.end());
    for (int i = 0; i < 51; ++i) wire.push_back(static_cast<uint8_t>(0xA0 + i));

    auto r = ts::parse_upgrade_response(reinterpret_cast<const char*>(wire.data()), wire.size());
    CHECK(r.status == ts::UpgradeResult::Status::kOk);
    CHECK(r.http_status == 101);
    CHECK(r.header_len == head.size());
    // 残りバイトが msg2 として使える
    CHECK(wire.size() - r.header_len == 51);
    CHECK(wire[r.header_len] == 0xA0);

    // ヘッダが途中までしか来ていない
    r = ts::parse_upgrade_response(head.c_str(), 20);
    CHECK(r.status == ts::UpgradeResult::Status::kIncomplete);
    CHECK(r.header_len == 0);

    // 101 でない
    const std::string bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    r = ts::parse_upgrade_response(bad.c_str(), bad.size());
    CHECK(r.status == ts::UpgradeResult::Status::kBadStatus);
    CHECK(r.http_status == 400);

    // Upgrade ヘッダが違う
    const std::string wrong =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";
    r = ts::parse_upgrade_response(wrong.c_str(), wrong.size());
    CHECK(r.status == ts::UpgradeResult::Status::kBadUpgrade);

    // ヘッダ名の大文字小文字は無視する
    const std::string mixed =
        "HTTP/1.1 101 Switching Protocols\r\nUPGRADE: Tailscale-Control-Protocol\r\n\r\n";
    r = ts::parse_upgrade_response(mixed.c_str(), mixed.size());
    CHECK(r.status == ts::UpgradeResult::Status::kOk);
}

void test_framing()
{
    // 4 バイトのリトルエンディアン長 + JSON
    const std::string json1 = "{\"KeepAlive\":true}";
    const std::string json2 = "{\"Node\":{}}";
    std::vector<uint8_t> buf;
    auto append = [&](const std::string& s) {
        const uint32_t n = static_cast<uint32_t>(s.size());
        buf.push_back(static_cast<uint8_t>(n));
        buf.push_back(static_cast<uint8_t>(n >> 8));
        buf.push_back(static_cast<uint8_t>(n >> 16));
        buf.push_back(static_cast<uint8_t>(n >> 24));
        buf.insert(buf.end(), s.begin(), s.end());
    };
    append(json1);
    append(json2);

    const uint8_t* payload = nullptr;
    size_t         plen = 0;
    size_t         consumed = ts::take_framed_message(buf.data(), buf.size(), &payload, &plen);
    CHECK(consumed == 4 + json1.size());
    EXPECT(std::string(reinterpret_cast<const char*>(payload), plen), json1);

    consumed = ts::take_framed_message(buf.data() + consumed, buf.size() - consumed, &payload,
                                       &plen);
    CHECK(consumed == 4 + json2.size());
    EXPECT(std::string(reinterpret_cast<const char*>(payload), plen), json2);

    // 途中までしか来ていなければ 0
    CHECK(ts::take_framed_message(buf.data(), 3, &payload, &plen) == 0);
    CHECK(ts::take_framed_message(buf.data(), 10, &payload, &plen) == 0);

    // 長さが 123 (0x7b = '{') でも壊れない。BE と誤読したり '{' 探索に頼ると破綻する箇所。
    {
        std::string body(123, 'x');
        std::vector<uint8_t> b;
        b.push_back(123);
        b.push_back(0);
        b.push_back(0);
        b.push_back(0);
        b.insert(b.end(), body.begin(), body.end());
        CHECK(ts::take_framed_message(b.data(), b.size(), &payload, &plen) == 4 + 123);
        CHECK(plen == 123);
    }

    // 壊れた長さは弾く
    const uint8_t huge[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(ts::take_framed_message(huge, 4, &payload, &plen) == SIZE_MAX);

    // 0 バイトのメッセージも合法
    const uint8_t zero[4] = {0, 0, 0, 0};
    CHECK(ts::take_framed_message(zero, 4, &payload, &plen) == 4);
    CHECK(plen == 0);
}

void test_register_request()
{
    ts::RegisterParams p;
    p.capability_version = 131;
    p.node_key = "nodekey:aabb";
    p.auth_key = "tskey-auth-secret";
    p.hostname = "tab5";
    const std::string s = ts::build_register_request(p);

    CHECK(s.front() == '{' && s.back() == '}');
    CHECK(s.find("\"Version\":131") != std::string::npos);
    CHECK(s.find("\"NodeKey\":\"nodekey:aabb\"") != std::string::npos);
    CHECK(s.find("\"Auth\":{\"AuthKey\":\"tskey-auth-secret\"}") != std::string::npos);
    CHECK(s.find("\"Hostname\":\"tab5\"") != std::string::npos);
    CHECK(s.find("\"Expiry\":\"0001-01-01T00:00:00Z\"") != std::string::npos);

    // auth key が無ければ Auth を入れない
    p.auth_key.clear();
    CHECK(ts::build_register_request(p).find("\"Auth\"") == std::string::npos);

    // ホスト名に " が入っていてもエスケープされる
    p.hostname = "ta\"b5";
    CHECK(ts::build_register_request(p).find("ta\\\"b5") != std::string::npos);
}

void test_map_request()
{
    ts::MapParams p;
    p.node_key  = "nodekey:11";
    p.disco_key = "discokey:22";
    p.hostname  = "tab5";
    p.endpoints = {"192.168.0.29:41641", "10.0.0.5:41641"};
    p.stream    = true;
    const std::string s = ts::build_map_request(p);

    CHECK(s.find("\"Stream\":true") != std::string::npos);
    CHECK(s.find("\"KeepAlive\":true") != std::string::npos);
    // zstd を載せないので圧縮なし
    CHECK(s.find("\"Compress\":\"\"") != std::string::npos);
    CHECK(s.find("\"Endpoints\":[\"192.168.0.29:41641\",\"10.0.0.5:41641\"]") != std::string::npos);
    CHECK(s.find("\"EndpointTypes\":[1,1]") != std::string::npos);
    // PreferredDERP は 0 なら入れない
    CHECK(s.find("NetInfo") == std::string::npos);

    p.preferred_derp = 12;
    CHECK(ts::build_map_request(p).find("\"NetInfo\":{\"PreferredDERP\":12}") != std::string::npos);

    // エンドポイントが無ければキー自体を出さない
    p.endpoints.clear();
    const std::string s2 = ts::build_map_request(p);
    CHECK(s2.find("Endpoints") == std::string::npos);
}

void test_json_extract()
{
    const std::string json =
        "{\"User\":{\"ID\":1},\"MachineAuthorized\":true,\"NodeKeyExpired\":false,"
        "\"AuthURL\":\"https://login.example/a/abc\",\"Error\":\"\"}";

    const auto r = ts::parse_register_response(json);
    CHECK(r.machine_authorized);
    CHECK(!r.node_key_expired);
    EXPECT(r.auth_url, "https://login.example/a/abc");
    EXPECT(r.error, "");

    // エラーが入っているケース
    const auto e = ts::parse_register_response("{\"Error\":\"invalid key\"}");
    EXPECT(e.error, "invalid key");
    CHECK(!e.machine_authorized);

    // エスケープを含む文字列
    std::string out;
    CHECK(ts::json_find_string("{\"a\":\"x\\\"y\"}", "a", &out));
    EXPECT(out, "x\"y");

    // 無いキー
    CHECK(!ts::json_find_string(json, "Nope", &out));
    bool b = false;
    CHECK(!ts::json_find_bool(json, "Nope", &b));

    // 終端の " が無い壊れた JSON では失敗する（暴走しない）
    CHECK(!ts::json_find_string("{\"a\":\"unterminated", "a", &out));
}

}  // namespace

int main()
{
    test_key_strings();
    test_upgrade_request();
    test_upgrade_response();
    test_framing();
    test_register_request();
    test_map_request();
    test_json_extract();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
