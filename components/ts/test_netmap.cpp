// netmap パーサのホストテスト。実際の Headscale の応答から作った形で検証する。
#include <cstdint>

#include "netmap.hpp"

#include <cstdio>
#include <cstdlib>
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

// 実際の Headscale の MapResponse から必要なところだけ抜き出した形。
const char* kFullMap = R"({
  "Node": {
    "ID": 3,
    "Name": "tab5.tab5.test.",
    "Key": "nodekey:cdbd15f78a346f9d325455aa58d35dc5695eb78407e35cf84af8506b8e676277",
    "DiscoKey": "discokey:cdbd15f78a346f9d325455aa58d35dc5695eb78407e35cf84af8506b8e676277",
    "Addresses": ["100.64.0.3/32", "fd7a:115c:a1e0::3/128"],
    "AllowedIPs": ["100.64.0.3/32", "fd7a:115c:a1e0::3/128"],
    "Endpoints": ["192.168.0.29:41641"],
    "Online": true
  },
  "Peers": [
    {
      "ID": 1,
      "StableID": "1",
      "Name": "mac.tab5.test.",
      "Key": "nodekey:c8f6cae5e229c2b79361d9911d48ffe53af0a0dbdf02dcf2d6b1215b7cce3aaa",
      "DiscoKey": "discokey:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "Addresses": ["100.64.0.1/32"],
      "AllowedIPs": ["100.64.0.1/32", "10.0.0.0/24"],
      "Endpoints": ["192.168.0.57:41641", "203.0.113.9:12345"],
      "HomeDERP": 12,
      "Online": true,
      "MachineAuthorized": true
    },
    {
      "ID": 2,
      "Name": "phone.tab5.test.",
      "Key": "nodekey:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "Addresses": ["100.64.0.2/32"],
      "Online": false
    }
  ],
  "DNSConfig": {"Resolvers": []},
  "Domain": "tab5.test",
  "PacketFilters": {"base": []},
  "ControlTime": "2026-08-20T00:00:00Z"
})";

void test_full_map()
{
    ts::NetMap m;
    CHECK(ts::parse_netmap(kFullMap, &m));
    CHECK(!m.keepalive);
    EXPECT(m.domain, "tab5.test");
    CHECK(m.node_key.rfind("nodekey:", 0) == 0);
    CHECK(m.addresses.size() == 2);
    EXPECT(m.addresses[0], "100.64.0.3/32");

    CHECK(m.has_peers);
    CHECK(m.peers.size() == 2);

    const auto& p0 = m.peers[0];
    CHECK(p0.id == 1);
    EXPECT(p0.name, "mac.tab5.test.");
    CHECK(p0.online);
    CHECK(p0.home_derp == 12);
    // AllowedIPs は Addresses と別（サブネットルートが入る）
    CHECK(p0.allowed_ips.size() == 2);
    EXPECT(p0.allowed_ips[1], "10.0.0.0/24");
    CHECK(p0.endpoints.size() == 2);
    EXPECT(p0.endpoints[0], "192.168.0.57:41641");

    const auto& p1 = m.peers[1];
    CHECK(!p1.online);
    // AllowedIPs が無いピアは Addresses を使う（capver 112 以降の省略に対応）
    CHECK(p1.allowed_ips.size() == 1);
    EXPECT(p1.allowed_ips[0], "100.64.0.2/32");
    // エンドポイントが無い（オフライン）
    CHECK(p1.endpoints.empty());
    // DiscoKey が無いピアは空文字
    CHECK(p1.disco_key.empty());
}

void test_keepalive()
{
    // KeepAlive のときは他のフィールドを見てはいけない
    ts::NetMap m;
    CHECK(ts::parse_netmap(R"({"KeepAlive":true,"Domain":"should-be-ignored"})", &m));
    CHECK(m.keepalive);
    CHECK(m.domain.empty());
    CHECK(m.peers.empty());
}

void test_incremental()
{
    ts::NetMap m;
    const char* json = R"({
      "PeersChanged": [
        {"ID": 5, "Key": "nodekey:cc", "Addresses": ["100.64.0.5/32"],
         "Endpoints": ["192.168.0.5:41641"], "Online": true}
      ],
      "PeersRemoved": [2, 3]
    })";
    CHECK(ts::parse_netmap(json, &m));
    // Peers キーが無い = 「変更なし」なので、空リストで上書きしてはいけない
    CHECK(!m.has_peers);
    CHECK(m.peers.empty());
    CHECK(m.peers_changed.size() == 1);
    CHECK(m.peers_changed[0].id == 5);
    CHECK(m.peers_changed[0].online);
    CHECK(m.peers_removed.size() == 2);
    CHECK(m.peers_removed[0] == 2);
    CHECK(m.peers_removed[1] == 3);
}

void test_bad_input()
{
    ts::NetMap m;
    CHECK(!ts::parse_netmap("", &m));
    CHECK(!ts::parse_netmap("not json", &m));
    CHECK(!ts::parse_netmap("[1,2,3]", &m));       // オブジェクトでない
    CHECK(!ts::parse_netmap("{\"a\":", &m));       // 途中で切れている
    CHECK(!ts::parse_netmap(kFullMap, nullptr));   // 出力先が null

    // 空のオブジェクトは有効（全フィールド「変更なし」）
    CHECK(ts::parse_netmap("{}", &m));
    CHECK(!m.keepalive);
    CHECK(!m.has_peers);

    // 型が違うフィールドは既定値になる（落ちない）
    CHECK(ts::parse_netmap(R"({"Domain":123,"Peers":"nope","Node":42})", &m));
    CHECK(m.domain.empty());
    CHECK(m.peers.empty());
}

}  // namespace

// エンドポイントの選択。**先頭を無条件に取ってはいけない**（ピアは全
// インターフェースを申告する）ことと、IPv4 として解析できないものを返さないこと。
void test_pick_endpoint()
{
    // 192.168.0.29/24 にいるとする（ネットワークバイトオーダ）。
    auto v4 = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return a | (b << 8) | (c << 16) | (d << 24);
    };
    const uint32_t me   = v4(192, 168, 0, 29);
    const uint32_t mask = v4(255, 255, 255, 0);

    // 実機で踏んだ並び。先頭は届かないので 192.168.0.57 を選ぶ。
    const std::vector<std::string> real = {"111.102.218.1:41642", "192.168.0.57:41642",
                                           "192.168.0.101:41642", "192.168.64.1:41642"};
    CHECK(ts::pick_endpoint(real, me, mask) == "192.168.0.57:41642");

    // 同じサブネットが無ければ、最初に解析できた IPv4。
    const std::vector<std::string> far = {"10.0.0.1:41641", "172.16.0.1:41641"};
    CHECK(ts::pick_endpoint(far, me, mask) == "10.0.0.1:41641");

    // **IPv6 は返さない。** 返すと set_peer が落ちて、後ろの IPv4 を試さない。
    const std::vector<std::string> v6first = {"[fd7a:115c:a1e0::1]:41641", "192.168.0.57:41641"};
    CHECK(ts::pick_endpoint(v6first, me, mask) == "192.168.0.57:41641");
    const std::vector<std::string> v6only = {"[fd7a:115c:a1e0::1]:41641", "fd7a::1:41641"};
    CHECK(ts::pick_endpoint(v6only, me, mask).empty());

    // サブネットが分からないとき（マスク 0）は最初の IPv4。
    CHECK(ts::pick_endpoint(real, 0, 0) == "111.102.218.1:41642");
    CHECK(ts::pick_endpoint(real, me, 0) == "111.102.218.1:41642");

    // 壊れた入力
    CHECK(ts::pick_endpoint({}, me, mask).empty());
    CHECK(ts::pick_endpoint({"nonsense"}, me, mask).empty());
    CHECK(ts::pick_endpoint({":41641"}, me, mask).empty());
    CHECK(ts::pick_endpoint({"192.168.0.57"}, me, mask).empty());     // ポート無し
    CHECK(ts::pick_endpoint({"192.168.0.999:1"}, me, mask).empty());  // octet が範囲外
    CHECK(ts::pick_endpoint({"192.168.0:1"}, me, mask).empty());      // octet が足りない
    CHECK(ts::pick_endpoint({"192.168.0.1.2:1"}, me, mask).empty());  // 多い
    // /8 のような広いマスクでも動く
    CHECK(ts::pick_endpoint(real, v4(111, 0, 0, 5), v4(255, 0, 0, 0)) == "111.102.218.1:41642");
}

int main()
{
    test_pick_endpoint();
    test_full_map();
    test_keepalive();
    test_incremental();
    test_bad_input();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
