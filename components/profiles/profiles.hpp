#pragma once
// SD カードに置く接続先 (SSH / VPN) の設定 (#49)。
//
// **SD の中身は信用しない。** 上限を決め、壊れた JSON では落ちず理由を返す。
// パーサは ESP-IDF に依存させないのでホストでテストできる（cJSON だけ使う。
// Tailscale の netmap で既に使っているので依存は増えない）。
//
// 1 件の書き損じで全部読めなくなるのは避ける: 未知の `type` や必須項目の欠けは
// その項目だけ飛ばして warnings に理由を残す。**名前の重複だけは全体を失敗させる**
// （どちらに繋がったのか分からないのが一番困る）。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace prof {

enum class Type { kSsh, kWireGuard, kTailscale };

struct WgPeer {
    std::string              pubkey;    // base64 (WireGuard の標準表記)
    std::string              endpoint;  // "vpn.example.com:51820"
    std::vector<std::string> allowed_ips;
};

struct Profile {
    Type        type = Type::kSsh;
    std::string name;  // 一覧に出る名前で、`via` の参照先でもある

    // --- ssh ---
    std::string host;
    std::string user;
    std::string key;       // keys/ 配下のファイル名（鍵そのものは JSON に埋めない）
    std::string via;       // 先に張る VPN プロファイルの name
    std::string password;  // 書けるが既定にしない。空 + auth=password なら入力させる
    uint16_t    port          = 22;
    bool        ask_password  = false;  // auth: "password" で password が無い

    // --- wireguard ---
    std::string address;      // "10.9.0.2/32"
    std::string private_key;  // keys/ 配下のファイル名
    WgPeer      peer;

    // --- tailscale ---
    std::string control;  // 制御プレーンのホスト
    std::string authkey;  // keys/ 配下のファイル名
};

// SD は抜けば誰でも読めるし、壊れたファイルも入り得る。上限で殴っておく。
constexpr size_t kMaxProfiles  = 32;
constexpr size_t kMaxFileBytes = 64 * 1024;

struct Config {
    std::vector<Profile>     profiles;
    std::vector<std::string> warnings;  // 飛ばした項目とその理由
    std::string              error;     // 空でなければ読み込み失敗（profiles は空）
};

Config parse(const std::string& json);

// name で引く。見つからなければ nullptr。`via` の解決に使う。
const Profile* find(const Config& cfg, const std::string& name);

// "10.9.0.2/32" を分ける。prefix が無ければ def_prefix。書式が違えば false。
bool split_cidr(const std::string& cidr, std::string* addr, int* prefix, int def_prefix = 32);

// "192.168.0.5:51820" を確かめる。**ホスト名は受けない** — wg::Netif::set_peer が
// リテラルの IPv4 しか受け取らないので、名前を書けるように見せると
// 接続時に初めて失敗する（設定を書いた人には理由が分からない）。
bool valid_endpoint(const std::string& endpoint);

// プレフィックス長 -> ドット表記のネットマスク ("255.255.255.0")。0-32 の外は空。
std::string prefix_to_mask(int prefix);

const char* type_name(Type t);

// 設定が参照している鍵の名前を、重複を畳んで並べる。
// **成功も失敗も同じ 1 か所で畳む**ために切り出してある（片方だけ畳むと本数が食い違う）。
std::vector<std::string> referenced_keys(const Config& cfg);

}  // namespace prof
