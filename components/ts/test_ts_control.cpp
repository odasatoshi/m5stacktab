// 制御プレーンのうちソケットを使わない部分のホストテスト。
#include <cstdint>

#include "ts_control.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
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
    // NetInfo は常に入れる。無いと Headscale がエンドポイントをピアに配らない。
    // **Hostinfo の直下にあること**を 1 本で固める。位置関係だけを見ると、
    // Hostinfo の閉じ括弧の外に出ても通ってしまう。
    CHECK(s.find("\"OS\":\"linux\",\"NetInfo\":{\"PreferredDERP\":0,\"WorkingUDP\":true,"
                 "\"LinkType\":\"wifi\"}}") != std::string::npos);
    // Endpoints は Hostinfo の外（兄弟）に出す
    CHECK(s.find("\"LinkType\":\"wifi\"}},\"Endpoints\":") != std::string::npos);

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

// レビュー指摘の回帰テスト。
void test_review_regressions()
{
    // ステータス行に数字が無い応答で、バッファ外を読まないこと。
    // 以前は atoi(in + 9) が len を越えて数字を探しに行き、ガードページで落ちた。
    {
        const char kNoDigits[] = "HTTP/1.1\r\n\r\n";
        // 意図的に NUL 終端に頼らない長さを渡す
        auto r = ts::parse_upgrade_response(kNoDigits, sizeof(kNoDigits) - 1);
        CHECK(r.status == ts::UpgradeResult::Status::kBadStatus);
    }
    // 数字が途中で切れていても範囲内で止まる
    {
        const char kPartial[] = "HTTP/1.1 10";
        auto r = ts::parse_upgrade_response(kPartial, sizeof(kPartial) - 1);
        // ヘッダ終端が無いので kIncomplete
        CHECK(r.status == ts::UpgradeResult::Status::kIncomplete);
    }
    // 3 桁を超える値でも溢れない
    {
        const std::string weird = "HTTP/1.1 1010101010101010 X\r\n\r\n";
        auto r = ts::parse_upgrade_response(weird.c_str(), weird.size());
        CHECK(r.status == ts::UpgradeResult::Status::kBadStatus);
    }
    // 余分な空白があっても読める
    {
        const std::string sp = "HTTP/1.1   101 Switching\r\nUpgrade: tailscale-control-protocol\r\n\r\n";
        auto r = ts::parse_upgrade_response(sp.c_str(), sp.size());
        CHECK(r.http_status == 101);
        CHECK(r.status == ts::UpgradeResult::Status::kOk);
    }
}


// 接続先 URL のパース (#68)。**TLS にするかをここだけで決める**ので、
// 誤読は「暗号化されているつもりで平文」に直結する。
void test_control_url()
{
    auto parse = [](const char* in, uint16_t arg_port) {
        ts::ControlEndpoint e;
        const bool          ok = ts::parse_control_url(in, arg_port, &e);
        return std::make_pair(ok, e);
    };

    // スキーム無し = 現状維持（平文 80）。既存のプロファイルが無変更で動くこと。
    {
        auto [ok, e] = parse("headscale.example.com", 0);
        CHECK(ok);
        EXPECT(e.host, "headscale.example.com");
        CHECK(e.port == 80);
        CHECK(!e.tls);
    }
    // https は 443、http は 80
    {
        auto [ok, e] = parse("https://controlplane.tailscale.com", 0);
        CHECK(ok);
        EXPECT(e.host, "controlplane.tailscale.com");
        CHECK(e.port == 443);
        CHECK(e.tls);
    }
    {
        auto [ok, e] = parse("http://192.168.0.101", 0);
        CHECK(ok);
        CHECK(e.port == 80);
        CHECK(!e.tls);
    }
    // **443 以外の TLS が書けること。** これが案 A を落とした理由そのもの。
    {
        auto [ok, e] = parse("https://headscale.example.com:8443", 0);
        CHECK(ok);
        EXPECT(e.host, "headscale.example.com");
        CHECK(e.port == 8443);
        CHECK(e.tls);
    }
    // ポートの優先順位: URL > 引数 > スキームの既定
    {
        auto [ok, e] = parse("https://h:8443", 9999);
        CHECK(ok);
        CHECK(e.port == 8443);  // URL が勝つ
    }
    {
        auto [ok, e] = parse("https://h", 9999);
        CHECK(ok);
        CHECK(e.port == 9999);  // URL に無ければ引数
    }
    {
        auto [ok, e] = parse("https://h", 0);
        CHECK(ok);
        CHECK(e.port == 443);  // どちらも無ければスキームの既定
    }
    // スキーム無し + 引数 port = 今の `ts <host> <authkey> <port>` の形
    {
        auto [ok, e] = parse("192.168.0.101", 8080);
        CHECK(ok);
        CHECK(e.port == 8080);
        CHECK(!e.tls);
    }
    // パス以降は捨てる（ブラウザからのコピペ）
    {
        auto [ok, e] = parse("https://h:8443/some/path", 0);
        CHECK(ok);
        EXPECT(e.host, "h");
        CHECK(e.port == 8443);
    }
    {
        auto [ok, e] = parse("https://h/", 0);
        CHECK(ok);
        EXPECT(e.host, "h");
        CHECK(e.port == 443);
    }
    // IPv6 リテラル。**']' より後ろの ':' だけがポート区切り**
    {
        auto [ok, e] = parse("http://[::1]:8080", 0);
        CHECK(ok);
        EXPECT(e.host, "::1");
        CHECK(e.port == 8080);
    }
    {
        auto [ok, e] = parse("[fd7a:115c:a1e0::1]", 0);
        CHECK(ok);
        EXPECT(e.host, "fd7a:115c:a1e0::1");
        CHECK(e.port == 80);
    }
    // **黙って誤読しない**もの
    CHECK(!parse("::1", 0).first);              // 裸の IPv6 は曖昧
    CHECK(!parse("h:", 0).first);               // ポートが空
    CHECK(!parse("h:0", 0).first);              // 0 番
    CHECK(!parse("h:70000", 0).first);          // 範囲外
    CHECK(!parse("h:80x", 0).first);            // 数字でない
    CHECK(!parse("", 0).first);                 // 空
    CHECK(!parse("https://", 0).first);         // ホストが無い
    CHECK(!parse("http:///path", 0).first);     // ホストが無い
}

// Host ヘッダの authority (#68)。ポート無しだと vhost 振り分けが別ホスト扱いになり、
// ブラケット無しの IPv6 は不正なヘッダになる。
void test_http_authority()
{
    EXPECT(ts::http_authority("h", 80, false), "h");           // http の既定は省く
    EXPECT(ts::http_authority("h", 443, true), "h");           // https の既定も省く
    EXPECT(ts::http_authority("h", 8443, true), "h:8443");     // 既定以外は付ける
    EXPECT(ts::http_authority("h", 8080, false), "h:8080");
    EXPECT(ts::http_authority("h", 443, false), "h:443");      // 平文の 443 は既定ではない
    EXPECT(ts::http_authority("h", 80, true), "h:80");         // TLS の 80 も既定ではない
    EXPECT(ts::http_authority("::1", 8080, false), "[::1]:8080");
    EXPECT(ts::http_authority("fd7a:115c:a1e0::1", 80, false), "[fd7a:115c:a1e0::1]");
    EXPECT(ts::http_authority("h", 0, true), "h");             // 0 = 未解決なら付けない
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
    test_review_regressions();
    test_control_url();
    test_http_authority();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
