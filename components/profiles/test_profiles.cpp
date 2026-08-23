// profiles.json パーサのホストテスト。SD の中身は信用しないので、
// 壊れた入力で落ちないこと・理由が出ることを固める。
#include <cstdio>

#include "profiles.hpp"

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const char* expr, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

bool has_warning(const prof::Config& c, const char* needle)
{
    for (const auto& w : c.warnings) {
        if (w.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* kGood = R"({
  "version": 1,
  "profiles": [
    {"name":"bastion","type":"ssh","host":"10.0.0.5","port":2222,"user":"oda",
     "auth":"key","key":"id_rsa_work","via":"hq","unknown_field":42},
    {"name":"hq","type":"wireguard","address":"10.9.0.2/32","private_key":"wg_hq.key",
     "peer":{"pubkey":"aGVsbG8=","endpoint":"203.0.113.10:51820",
             "allowed_ips":["10.0.0.0/8"]}},
    {"name":"home","type":"tailscale","control":"headscale.example.com","port":8080,
     "authkey":"ts_home.key"}
  ]
})";

void test_good()
{
    const prof::Config c = prof::parse(kGood);
    CHECK(c.error.empty());
    CHECK(c.warnings.empty());
    CHECK(c.profiles.size() == 3);

    const prof::Profile* s = prof::find(c, "bastion");
    CHECK(s != nullptr);
    CHECK(s->type == prof::Type::kSsh);
    CHECK(s->host == "10.0.0.5");
    CHECK(s->port == 2222);
    CHECK(s->user == "oda");
    CHECK(s->key == "id_rsa_work");
    CHECK(s->via == "hq");
    CHECK(!s->ask_password);

    const prof::Profile* w = prof::find(c, "hq");
    CHECK(w != nullptr);
    CHECK(w->type == prof::Type::kWireGuard);
    CHECK(w->address == "10.9.0.2/32");
    CHECK(w->private_key == "wg_hq.key");
    CHECK(w->peer.endpoint == "203.0.113.10:51820");
    CHECK(w->peer.allowed_ips.size() == 1);

    const prof::Profile* t = prof::find(c, "home");
    CHECK(t != nullptr);
    CHECK(t->type == prof::Type::kTailscale);
    CHECK(t->control == "headscale.example.com");
    CHECK(t->port == 8080);
    CHECK(t->authkey == "ts_home.key");

    CHECK(prof::find(c, "nope") == nullptr);
    CHECK(prof::find(c, "") == nullptr);
}

void test_broken_json()
{
    // 壊れていても落ちず、理由が出る。
    for (const char* s : {"", "{", "[]", "not json", "{\"version\":1}",
                          "{\"version\":1,\"profiles\":{}}", "null"}) {
        const prof::Config c = prof::parse(s);
        CHECK(!c.error.empty());
        CHECK(c.profiles.empty());
    }
}

void test_version()
{
    const prof::Config c = prof::parse(R"({"version":2,"profiles":[]})");
    CHECK(!c.error.empty());
    const prof::Config ok = prof::parse(R"({"version":1,"profiles":[]})");
    CHECK(ok.error.empty());
    CHECK(ok.profiles.empty());
}

void test_duplicate_name()
{
    // **重複は全体を失敗させる。** どちらに繋がったのか分からないのが一番困る。
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u"},
        {"name":"a","type":"ssh","host":"other","user":"u"}]})");
    CHECK(!c.error.empty());
    CHECK(c.error.find("重複") != std::string::npos);
    CHECK(c.profiles.empty());
}

void test_unknown_type_is_skipped()
{
    // 1 件の書き損じで全部読めなくなるのは避ける。
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u"},
        {"name":"b","type":"l2tp","host":"h"},
        {"name":"c","type":"ssh","host":"h2","user":"u"},
        {"name":"d"},
        {"type":"ssh","host":"h3","user":"u"},
        "junk"]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.size() == 2);
    CHECK(prof::find(c, "a") != nullptr);
    CHECK(prof::find(c, "c") != nullptr);
    CHECK(prof::find(c, "b") == nullptr);
    CHECK(has_warning(c, "l2tp"));
    CHECK(c.warnings.size() == 4);
}

void test_missing_fields_are_skipped()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"noshost","type":"ssh","user":"u"},
        {"name":"nouser","type":"ssh","host":"h"},
        {"name":"badport","type":"ssh","host":"h","user":"u","port":70000},
        {"name":"badaddr","type":"wireguard","address":"nope","private_key":"k",
         "peer":{"pubkey":"p","endpoint":"192.168.0.5:1"}},
        {"name":"nopeer","type":"wireguard","address":"10.0.0.1/24","private_key":"k"},
        {"name":"noctl","type":"tailscale","authkey":"k"}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(c.warnings.size() == 6);
}

void test_key_names_must_be_bare()
{
    // 鍵は keys/ 配下のファイル名だけ。パスを書けると SD の外まで指せる。
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u","key":"../../etc/shadow"},
        {"name":"b","type":"wireguard","address":"10.0.0.1/32","private_key":"sub/dir.key",
         "peer":{"pubkey":"p","endpoint":"192.168.0.5:1"}},
        {"name":"c","type":"tailscale","control":"h","authkey":"/abs.key"}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(c.warnings.size() == 3);
}

// **auth: "password" でも key は検査する。** `else if` にすると
// "auth":"password" + "key":"../../evil" が素通りして keys/ の外を読める。
void test_password_auth_still_checks_key()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"evil","type":"ssh","host":"h","user":"u","auth":"password",
         "password":"p","key":"../../evil"}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(has_warning(c, "ディレクトリを含まない"));
}

// **0.0.0.0/0 は受けない。** netif のマスクが 0.0.0.0 になると lwIP が
// 何にでも一致させ、ピアへの暗号化 UDP までトンネルに入る。
void test_default_route_rejected()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"all","type":"wireguard","address":"10.9.0.2/32","private_key":"k",
         "peer":{"pubkey":"p","endpoint":"192.168.0.5:51820","allowed_ips":["0.0.0.0/0"]}}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(has_warning(c, "0.0.0.0/0"));
}

// **endpoint はリテラルの IPv4 だけ。** wg::Netif::set_peer が名前を引けないので、
// 書けるように見せると接続時に初めて失敗する。
void test_endpoint_must_be_literal()
{
    CHECK(prof::valid_endpoint("192.168.0.5:51820"));
    CHECK(prof::valid_endpoint("10.0.0.1:1"));
    CHECK(!prof::valid_endpoint("vpn.example.com:51820"));
    CHECK(!prof::valid_endpoint("192.168.0.5"));
    CHECK(!prof::valid_endpoint("192.168.0.5:0"));
    CHECK(!prof::valid_endpoint("192.168.0.5:70000"));
    CHECK(!prof::valid_endpoint(":51820"));
    CHECK(!prof::valid_endpoint(""));

    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"hq","type":"wireguard","address":"10.9.0.2/32","private_key":"k",
         "peer":{"pubkey":"p","endpoint":"vpn.example.com:51820"}}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(has_warning(c, "IPv4"));
}

void test_tailscale_port_range()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"ts","type":"tailscale","control":"h","authkey":"k","port":100000}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(has_warning(c, "port"));
}

void test_password_auth()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"ask","type":"ssh","host":"h","user":"u","auth":"password"},
        {"name":"inline","type":"ssh","host":"h","user":"u","auth":"password",
         "password":"hunter2"}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.size() == 2);
    CHECK(prof::find(c, "ask")->ask_password);
    CHECK(!prof::find(c, "inline")->ask_password);
    CHECK(prof::find(c, "inline")->password == "hunter2");
}

void test_via_is_reported_but_not_fatal()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u","via":"ghost"},
        {"name":"b","type":"ssh","host":"h","user":"u","via":"a"}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.size() == 2);  // via が壊れていても本体は使える
    CHECK(has_warning(c, "ghost"));
    CHECK(has_warning(c, "VPN ではない"));
}

void test_limits()
{
    std::string j = R"({"version":1,"profiles":[)";
    for (int i = 0; i < 33; ++i) {
        if (i) j += ",";
        j += "{\"name\":\"p" + std::to_string(i) + "\",\"type\":\"ssh\",\"host\":\"h\",\"user\":\"u\"}";
    }
    j += "]}";
    const prof::Config c = prof::parse(j);
    CHECK(!c.error.empty());
    CHECK(c.profiles.empty());

    // 上限ちょうどは通る
    std::string k = R"({"version":1,"profiles":[)";
    for (int i = 0; i < 32; ++i) {
        if (i) k += ",";
        k += "{\"name\":\"p" + std::to_string(i) + "\",\"type\":\"ssh\",\"host\":\"h\",\"user\":\"u\"}";
    }
    k += "]}";
    CHECK(prof::parse(k).profiles.size() == 32);

    // ファイルサイズの上限
    const prof::Config big = prof::parse(std::string(prof::kMaxFileBytes + 1, ' '));
    CHECK(!big.error.empty());
}

// **同じ鍵を 2 つの接続先が参照していることがある**（`via` 構成など）。
// 畳まないと「取り込んだ本数」が水増しされる。
void test_referenced_keys()
{
    const prof::Config c = prof::parse(kGood);
    const auto         k = prof::referenced_keys(c);
    CHECK(k.size() == 3);  // id_rsa_work / wg_hq.key / ts_home.key
    CHECK(k[0] == "id_rsa_work");

    // 2 つの ssh が同じ鍵を指しても 1 本
    const prof::Config d = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u","key":"same"},
        {"name":"b","type":"ssh","host":"h2","user":"u","key":"same"},
        {"name":"c","type":"ssh","host":"h3","user":"u"}]})");
    CHECK(d.error.empty());
    CHECK(prof::referenced_keys(d).size() == 1);

    // 鍵を使わない設定なら空
    const prof::Config e = prof::parse(R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u","auth":"password","password":"p"}]})");
    CHECK(prof::referenced_keys(e).empty());
}

void test_cidr()
{
    std::string a;
    int         p = -1;
    CHECK(prof::split_cidr("10.9.0.2/32", &a, &p) && a == "10.9.0.2" && p == 32);
    CHECK(prof::split_cidr("10.0.0.0/8", &a, &p) && a == "10.0.0.0" && p == 8);
    CHECK(prof::split_cidr("192.168.1.1", &a, &p, 24) && a == "192.168.1.1" && p == 24);
    CHECK(!prof::split_cidr("10.9.0.2/33", &a, &p));
    CHECK(!prof::split_cidr("10.9.0.2/", &a, &p));
    CHECK(!prof::split_cidr("10.9.0", &a, &p));
    CHECK(!prof::split_cidr("10.9.0.256", &a, &p));
    CHECK(!prof::split_cidr("10.9.0.2.5", &a, &p));
    CHECK(!prof::split_cidr("fd7a::1/64", &a, &p));  // IPv6 は今のところ扱わない
    CHECK(!prof::split_cidr("", &a, &p));

    CHECK(prof::prefix_to_mask(32) == "255.255.255.255");
    CHECK(prof::prefix_to_mask(24) == "255.255.255.0");
    CHECK(prof::prefix_to_mask(10) == "255.192.0.0");
    CHECK(prof::prefix_to_mask(0) == "0.0.0.0");
    CHECK(prof::prefix_to_mask(33).empty());
}

}  // namespace

int main()
{
    test_good();
    test_broken_json();
    test_version();
    test_duplicate_name();
    test_unknown_type_is_skipped();
    test_missing_fields_are_skipped();
    test_key_names_must_be_bare();
    test_password_auth();
    test_password_auth_still_checks_key();
    test_default_route_rejected();
    test_endpoint_must_be_literal();
    test_tailscale_port_range();
    test_via_is_reported_but_not_fatal();
    test_limits();
    test_referenced_keys();
    test_cidr();

    std::printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
