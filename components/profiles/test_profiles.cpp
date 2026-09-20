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
    {"name":"bastion","type":"ssh","host":"10.0.0.5","port":2222,"user":"user",
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
    CHECK(s->user == "user");
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

// port を書かない設定は 0（未指定）で通り、飛ばされないこと (#68)。
// スキームから決めるので、ここで 80 を埋めてはいけない。
void test_tailscale_port_unset_is_kept()
{
    auto c = prof::parse(R"({"version":1,"profiles":[
        {"name":"ts","type":"tailscale","control":"https://controlplane.tailscale.com",
         "authkey":"ts.key"}]})");
    CHECK(c.profiles.size() == 1);
    // **check() は abort せず続ける**ので、添字の前に必ず数を見る（見ないと
    // 失敗時に空 vector を触って SIGSEGV になり、FAIL の行も出ないまま落ちる）。
    if (c.profiles.size() == 1) {
        CHECK(c.profiles[0].port == 0);
        CHECK(c.profiles[0].control == "https://controlplane.tailscale.com");
    }
}

void test_tailscale_port_range()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"ts","type":"tailscale","control":"h","authkey":"k","port":100000}]})");
    CHECK(c.error.empty());
    CHECK(c.profiles.empty());
    CHECK(has_warning(c, "port"));
}

// **authkey が無い tailscale は対話ログイン (#67)。** 空を弾くとメニューに
// 出せず、この機能は使えない。control があれば authkey 無しで通す。
void test_tailscale_without_authkey()
{
    const prof::Config c = prof::parse(R"({"version":1,"profiles":[
        {"name":"login","type":"tailscale","control":"https://controlplane.tailscale.com"}]})");
    CHECK(c.error.empty());
    CHECK(c.warnings.empty());
    CHECK(c.profiles.size() == 1);
    if (c.profiles.size() == 1) CHECK(c.profiles[0].authkey.empty());

    // authkey にパスが混ざっているものは相変わらず弾く
    const prof::Config d = prof::parse(R"({"version":1,"profiles":[
        {"name":"bad","type":"tailscale","control":"h","authkey":"../out.key"}]})");
    CHECK(d.profiles.empty());
    CHECK(has_warning(d, "authkey"));
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

// **上限は種類ごと。超えた項目だけを飛ばす（ファイル全体は落とさない）。**
void test_limits()
{
    // ssh を上限 +2 件、vpn を 1 件書く。ssh は上限まで残り、vpn は無事。
    std::string j = R"({"version":1,"profiles":[)";
    for (size_t i = 0; i < prof::kMaxSshProfiles + 2; ++i) {
        if (i) j += ",";
        j += "{\"name\":\"s" + std::to_string(i) +
             "\",\"type\":\"ssh\",\"host\":\"h\",\"user\":\"u\"}";
    }
    j += R"(,{"name":"v0","type":"tailscale","control":"h","authkey":"k"}]})";
    const prof::Config c = prof::parse(j);
    CHECK(c.error.empty());  // 全体は失敗させない
    CHECK(c.profiles.size() == prof::kMaxSshProfiles + 1);
    CHECK(prof::find(c, "s0") != nullptr);
    CHECK(prof::find(c, "s4") != nullptr);
    CHECK(prof::find(c, "s5") == nullptr);  // 6 件目以降は飛ばす
    CHECK(prof::find(c, "s6") == nullptr);
    CHECK(prof::find(c, "v0") != nullptr);  // **ssh が溢れても vpn は入る**
    CHECK(c.warnings.size() == 2);
    CHECK(has_warning(c, "上限"));

    // vpn も別枠で数える
    std::string k = R"({"version":1,"profiles":[)";
    for (size_t i = 0; i < prof::kMaxVpnProfiles + 1; ++i) {
        if (i) k += ",";
        k += "{\"name\":\"v" + std::to_string(i) +
             "\",\"type\":\"tailscale\",\"control\":\"h\",\"authkey\":\"k\"}";
    }
    k += R"(,{"name":"s0","type":"ssh","host":"h","user":"u"}]})";
    const prof::Config c2 = prof::parse(k);
    CHECK(c2.error.empty());
    CHECK(c2.profiles.size() == prof::kMaxVpnProfiles + 1);
    CHECK(prof::find(c2, "v5") == nullptr);
    CHECK(prof::find(c2, "s0") != nullptr);

    // **wireguard と tailscale は同じ「vpn」枠。**
    std::string m = R"({"version":1,"profiles":[)";
    for (size_t i = 0; i < prof::kMaxVpnProfiles; ++i) {
        if (i) m += ",";
        m += "{\"name\":\"t" + std::to_string(i) +
             "\",\"type\":\"tailscale\",\"control\":\"h\",\"authkey\":\"k\"}";
    }
    m += R"(,{"name":"wg","type":"wireguard","address":"10.0.0.1/32","private_key":"k",)"
         R"("peer":{"pubkey":"p","endpoint":"192.168.0.5:1"}}]})";
    const prof::Config c3 = prof::parse(m);
    CHECK(c3.error.empty());
    CHECK(prof::find(c3, "wg") == nullptr);
    CHECK(c3.profiles.size() == prof::kMaxVpnProfiles);

    // ファイルサイズの上限は今までどおり全体を失敗させる（中身を見る前の話）
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

void test_remove_profile()
{
    std::string out;
    CHECK(prof::remove_profile(kGood, "bastion", &out));
    const prof::Config c = prof::parse(out);
    CHECK(c.error.empty());
    CHECK(prof::find(c, "bastion") == nullptr);
    // 他の項目は残っている（消したのは 1 件だけ）
    CHECK(c.profiles.size() == prof::parse(kGood).profiles.size() - 1);

    // **飛ばされた項目より後ろを消しても、ずれない。** ここが index で消せない理由:
    // parse は "broken" を飛ばすので profiles[1] は "b" だが、JSON の配列では 2 番目。
    const char* skewed = R"({"version":1,"profiles":[
        {"name":"a","type":"ssh","host":"h","user":"u"},
        {"name":"broken","type":"l2tp","host":"h"},
        {"name":"b","type":"ssh","host":"h2","user":"u"}]})";
    const prof::Config s = prof::parse(skewed);
    CHECK(s.profiles.size() == 2 && s.profiles[1].name == "b");
    CHECK(prof::remove_profile(skewed, "b", &out));
    const prof::Config t = prof::parse(out);
    CHECK(t.profiles.size() == 1 && t.profiles[0].name == "a");

    // **飛ばされた項目と名前がぶつかったら消さない。** parse の重複検査は
    // 受け入れた項目どうししか見ないので、この JSON は error にならない。
    const char* dup = R"({"version":1,"profiles":[
        {"name":"work","type":"ssh","host":"h1"},
        {"name":"work","type":"ssh","host":"h2","user":"u"}]})";
    const prof::Config d = prof::parse(dup);
    CHECK(d.error.empty() && d.profiles.size() == 1);  // 1 件目は user 欠けで飛ぶ
    out = "untouched";
    CHECK(!prof::remove_profile(dup, "work", &out));
    CHECK(out == "untouched");

    // 無い名前・空の名前・壊れた JSON では out に触らず false
    CHECK(!prof::remove_profile(kGood, "nosuch", &out));
    CHECK(!prof::remove_profile(kGood, "", &out));
    CHECK(!prof::remove_profile("{ broken", "a", &out));
    CHECK(out == "untouched");
}

void test_add_profile()
{
    const char* entry = R"({"name":"new","type":"ssh","host":"h","user":"u","key":"k.pem"})";
    std::string out;
    CHECK(prof::add_profile(kGood, entry, &out));
    const prof::Config c = prof::parse(out);
    CHECK(c.error.empty());
    CHECK(prof::find(c, "new") != nullptr);
    CHECK(c.profiles.size() == prof::parse(kGood).profiles.size() + 1);
    CHECK(prof::find(c, "bastion") != nullptr);  // 元からあるものは残る

    // **空から作れる。** 1 件も取り込んでいない端末にメニューから足す経路。
    out.clear();
    CHECK(prof::add_profile("", entry, &out));
    const prof::Config e = prof::parse(out);
    CHECK(e.error.empty() && e.profiles.size() == 1);

    // **同じ名前は断る。** 上書きすると繋ぐ先が変わったことに気づけない。
    out = "untouched";
    CHECK(!prof::add_profile(kGood, R"({"name":"bastion","type":"ssh","host":"x","user":"u"})",
                             &out));
    CHECK(out == "untouched");

    // **壊れた本文には足さない。** 器で置き換えると今までの接続先が消える。
    out = "untouched";
    CHECK(!prof::add_profile(R"({"profiles":[{"name":"a",)", entry, &out));
    CHECK(out == "untouched");

    // 壊れた入力・name 無し・配列は断る
    CHECK(!prof::add_profile(kGood, "{ broken", &out));
    CHECK(!prof::add_profile(kGood, R"({"type":"ssh","host":"h","user":"u"})", &out));
    CHECK(!prof::add_profile(kGood, "[]", &out));
    CHECK(out == "untouched");
}

// **画面で作った設定が読み戻せることを 1 本で見る (#82)。** to_json と parse が
// 食い違うと「保存はできたのに一覧に出ない」になり、実機でしか気づけない。
void test_to_json_roundtrip()
{
    prof::Profile ssh;
    ssh.type = prof::Type::kSsh;
    ssh.name = "jump";
    ssh.host = "10.0.0.5";
    ssh.user = "user";
    ssh.port = 2222;
    ssh.key  = "id.pem";
    std::string out;
    CHECK(prof::add_profile("", prof::to_json(ssh), &out));
    prof::Config c = prof::parse(out);
    CHECK(c.error.empty());
    const prof::Profile* got = prof::find(c, "jump");
    CHECK(got && got->host == "10.0.0.5" && got->user == "user" && got->port == 2222);
    CHECK(got && got->key == "id.pem" && !got->ask_password);

    // **鍵が無ければパスワード認証になる。** auth を書かないと鍵で繋ごうとする。
    prof::Profile pw = ssh;
    pw.key.clear();
    out.clear();
    CHECK(prof::add_profile("", prof::to_json(pw), &out));
    c   = prof::parse(out);
    got = prof::find(c, "jump");
    CHECK(got && got->ask_password);

    prof::Profile wg;
    wg.type           = prof::Type::kWireGuard;
    wg.name           = "office";
    wg.address        = "10.9.0.2/32";
    wg.private_key    = "wg.key";
    wg.peer.pubkey    = "abc=";
    wg.peer.endpoint  = "192.168.0.5:51820";
    wg.peer.allowed_ips = {"10.9.0.0/24"};
    out.clear();
    CHECK(prof::add_profile("", prof::to_json(wg), &out));
    c   = prof::parse(out);
    got = prof::find(c, "office");
    CHECK(got && got->peer.endpoint == "192.168.0.5:51820");
    CHECK(got && got->peer.allowed_ips.size() == 1 && got->address == "10.9.0.2/32");

    // tailscale は authkey 無し = 対話ログイン。**port 0 は書かない**
    // （書くと parse が 0 番ポートとして扱う）。
    prof::Profile ts;
    ts.type    = prof::Type::kTailscale;
    ts.name    = "ts";
    ts.control = "https://hs.example.com";
    out.clear();
    CHECK(prof::add_profile("", prof::to_json(ts), &out));
    c   = prof::parse(out);
    got = prof::find(c, "ts");
    CHECK(got && got->authkey.empty() && got->port == 0);  // 既定は「未指定」
    CHECK(prof::to_json(ts).find("port") == std::string::npos);

    // **壊れた入力は parse が弾く。** 画面側で書式を検査せず、保存の前に
    // parse に通して「一覧に出るか」で見る、という作りの土台。
    prof::Profile bad = wg;
    bad.name          = "bad";
    bad.address       = "not-an-address";
    out.clear();
    CHECK(prof::add_profile("", prof::to_json(bad), &out));
    const prof::Config badcfg = prof::parse(out);
    CHECK(prof::find(badcfg, "bad") == nullptr);
}

void test_split_list()
{
    // **本数が要**。1 本に潰れると allowed_ips が「経路 1 本の壊れた設定」になり、
    // parse は通ってしまう（書式としては CIDR 1 本に見える）。
    const auto a = prof::split_list("10.9.0.0/24,10.8.0.0/24");
    CHECK(a.size() == 2 && a[0] == "10.9.0.0/24" && a[1] == "10.8.0.0/24");
    const auto b = prof::split_list(" 10.9.0.0/24 , 10.8.0.0/24 ,");
    CHECK(b.size() == 2 && b[0] == "10.9.0.0/24" && b[1] == "10.8.0.0/24");
    CHECK(prof::split_list("").empty());
    CHECK(prof::split_list(" , , ").empty());
    const auto c = prof::split_list("10.9.0.0/24");
    CHECK(c.size() == 1 && c[0] == "10.9.0.0/24");

    // 画面から来た 1 行がそのまま WireGuard の設定になるところまで見る。
    prof::Profile wg;
    wg.type             = prof::Type::kWireGuard;
    wg.name             = "wg2";
    wg.address          = "10.9.0.3/32";
    wg.private_key      = "wg_hq.key";
    wg.peer.pubkey      = "abcd=";
    wg.peer.endpoint    = "192.168.0.101:51820";
    wg.peer.allowed_ips = prof::split_list("10.9.0.0/24,10.8.0.0/24");
    std::string out;
    CHECK(prof::add_profile("", prof::to_json(wg), &out));
    // **Config は名前を付けて持つ。** `find(parse(out), ...)` は一時オブジェクトを
    // 指すポインタを返すので、式の終わりで消える（実際にここで segfault した）。
    const prof::Config   cfg = prof::parse(out);
    const prof::Profile* got = prof::find(cfg, "wg2");
    CHECK(got && got->peer.allowed_ips.size() == 2);
    CHECK(got && got->peer.allowed_ips[1] == "10.8.0.0/24");
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
    test_tailscale_port_unset_is_kept();
    test_tailscale_port_range();
    test_tailscale_without_authkey();
    test_via_is_reported_but_not_fatal();
    test_limits();
    test_referenced_keys();
    test_remove_profile();
    test_add_profile();
    test_to_json_roundtrip();
    test_split_list();
    test_cidr();

    std::printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
