#pragma once
// 接続先 (SSH / VPN) の設定 (#49)。SD の profiles.json から NVS へ取り込む (#60)。
//
// **JSON の中身は信用しない。** 上限を決め、壊れた JSON では落ちず理由を返す。
// パーサは ESP-IDF に依存させないのでホストでテストできる（cJSON だけ使う。
// Tailscale の netmap で既に使っているので依存は増えない）。
//
// 1 件の書き損じで全部読めなくなるのは避ける: 未知の `type`・必須項目の欠け・
// 上限超えは、その項目だけ飛ばして warnings に理由を残す。
// **名前の重複だけは全体を失敗させる**（どちらに繋がったのか分からないのが一番困る）。
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
    // **既定は 0 = 未指定。** ssh の既定 22 は parse が埋める（`to_json` も 0 なら 22 を書く）。
    // ここを 22 にすると、tailscale を手で組み立てたときに **22 番が書かれてしまう**
    // （tailscale の port は「書かれていなければ control のスキームで決める」#68）。
    uint16_t    port          = 0;
    bool        ask_password  = false;  // auth: "password" で password が無い

    // --- wireguard ---
    std::string address;      // "10.9.0.2/32"
    std::string private_key;  // keys/ 配下のファイル名
    WgPeer      peer;

    // --- tailscale ---
    std::string control;  // 制御プレーンのホスト
    std::string authkey;  // keys/ 配下のファイル名
};

// **SSH と VPN は別枠で数える。** 違うレイヤなので混ぜない。合計で数えると、
// `via`（SSH 1 件が VPN 1 件を要求する）構成ですぐ埋まる。
constexpr size_t kMaxSshProfiles = 5;
constexpr size_t kMaxVpnProfiles = 5;
constexpr size_t kMaxFileBytes   = 64 * 1024;

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

// JSON から name の 1 件を落として書き戻す (#73)。消せたら true。
//
// **index ではなく name で引く。** parse は壊れた項目を飛ばすので、
// `Config::profiles[i]` の i は JSON の配列添字と一致しない
// （飛ばした項目より後ろを消すと 1 つずれた別の接続先が消える）。
//
// **同名が 2 つ以上あるときは消さずに false を返す。** parse の重複検査は
// 受け入れた項目どうししか見ないので、飛ばされた項目とは名前がぶつかれる。
// どちらを指しているか決められないまま先頭を消すと、一覧に残っている方ではなく
// 壊れている方が消えて「押したのに何も起きない」になる。
bool remove_profile(const std::string& json, const std::string& name, std::string* out);

// JSON に 1 件足して書き戻す (#82)。`entry` は profiles.json の 1 項目の JSON。
// 足せたら true。`json` が空なら器から作る（1 件も取り込んでいない端末でも足せる）。
//
// **同じ name が既に有れば false。上書きしない。**
// 新規作成の画面で既存と同じ名前を打っただけで接続先が差し替わると、
// **繋ぐ先が変わったことに気づけない**。消してから足させる。
bool add_profile(const std::string& json, const std::string& entry, std::string* out);

// Profile を profiles.json の 1 項目 (JSON) にする (#82)。`add_profile` に渡す形。
//
// **書式の知識をここから出さない。** 画面で組み立てた接続先を main 側で文字列に
// すると、parse が読む鍵の名前と 2 か所に散る（片方だけ直すと、保存はできるのに
// 読み戻せない設定ができる）。ホストでテストできるのもこちら側。
//
// 空の項目は書かない（`key` を空で書くと「鍵がある」と読める）。
std::string to_json(const Profile& p);

}  // namespace prof
