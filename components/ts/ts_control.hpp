#pragma once
// ts2021 の制御プレーンのうち、ソケット I/O を含まない部分。
// HTTP upgrade の組み立てと検証、フレーミング、JSON の生成。ホストでテストできる。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

// Tailscale の鍵表現。"nodekey:<64hex>" のような形。
std::string key_to_string(const char* prefix, const uint8_t key[32]);
// "nodekey:<64hex>" から 32 バイトを取り出す。prefix が合わなければ false。
bool key_from_string(const std::string& s, const char* prefix, uint8_t out[32]);

// --- 接続先の URL ---

// 制御プレーンの接続先。`ts-login https://host:8443` のような文字列から作る。
struct ControlEndpoint {
    std::string host;         // "[" "]" は外した形（そのまま getaddrinfo に渡せる）
    uint16_t    port = 0;
    bool        tls  = false;
};

// スキーム付き / 無しの接続先文字列を分解する (#68)。
//
//   "https://h"      -> tls,  port 443
//   "http://h:8080"  -> 平文, port 8080
//   "h"              -> 平文, port 80   ← **スキーム無しは現状維持**（既存の設定が動く）
//   "[::1]:8080"     -> 平文, port 8080, host "::1"
//
// ポートの優先順位は **URL の :port > arg_port > スキームの既定**。
// `arg_port` は 0 で「指定なし」を表す（引数やプロファイルの port をそのまま渡す）。
//
// **曖昧な入力は黙って誤読せず false を返す。** ブラケット無しで ':' が残る
// （"::1" のような裸の IPv6）、ポートが数字でない / 範囲外、ホストが空、など。
bool parse_control_url(const std::string& in, uint16_t arg_port, ControlEndpoint* out);

// HTTP の Host ヘッダに入れる authority を作る (#68)。
// **スキームの既定ポート以外は `:port` を付ける**（リバースプロキシの vhost 振り分けや
// Headscale の server_url 検査が、ポート無しだと別のホストとして扱う）。
// IPv6 リテラルはブラケットを戻す（`Host: ::1` は不正なヘッダ）。
std::string http_authority(const std::string& host, uint16_t port, bool tls);

// --- HTTP upgrade ---

// POST /ts2021 のリクエストを組み立てる。ハンドシェイクはヘッダに base64 で載せる。
size_t build_upgrade_request(char* out, size_t cap, const char* host, const uint8_t* msg1,
                             size_t msg1_len);

struct UpgradeResult {
    enum class Status {
        kIncomplete,  // ヘッダがまだ揃っていない
        kOk,          // 101 で Upgrade ヘッダも正しい
        kBadStatus,   // 101 でない
        kBadUpgrade,  // Upgrade ヘッダが期待と違う
    };
    Status status      = Status::kIncomplete;
    size_t header_len  = 0;  // ヘッダ終端までのバイト数（この後ろは Noise のバイト列）
    int    http_status = 0;
};

// 101 応答をパースする。**ヘッダの後ろに Noise の msg2 が続いていることが多い**ので、
// header_len を返して呼び出し側が残りをバッファに回せるようにする。
UpgradeResult parse_upgrade_response(const char* in, size_t len);

// --- MapResponse のフレーミング ---
// 4 バイトのリトルエンディアン長 + JSON の繰り返し。
// 1 件取り出せたら payload / payload_len を埋めて consumed を返す。
// 足りなければ consumed = 0。長さが上限を超えていたら consumed = SIZE_MAX。
size_t take_framed_message(const uint8_t* in, size_t len, const uint8_t** payload,
                           size_t* payload_len);

// --- JSON の組み立て ---

struct RegisterParams {
    uint16_t    capability_version = 131;
    std::string node_key;   // "nodekey:<hex>"
    std::string auth_key;   // "tskey-auth-..."（空なら対話ログインになるので実質必須）
    std::string hostname;
    std::string os = "linux";  // 制御プレーンが知らない OS 名だと弾かれることがある
};
std::string build_register_request(const RegisterParams& p);

struct MapParams {
    uint16_t                 capability_version = 131;
    std::string              node_key;
    std::string              disco_key;
    std::string              hostname;
    std::vector<std::string> endpoints;  // "192.168.0.29:41641" など
    bool                     stream    = true;
};
std::string build_map_request(const MapParams& p);

// --- 応答から必要な値だけ取り出す ---
// トップレベルの単純なフィールドだけを見る軽量な抽出（ネストした構造は扱わない）。
bool json_find_string(const std::string& json, const char* key, std::string* out);
bool json_find_bool(const std::string& json, const char* key, bool* out);

// ネストした配列から最初の文字列を取り出す（"Addresses":["100.64.0.1/32", ...] 用）。
// cJSON を入れる前の最小実装。オブジェクトの入れ子は追わない。
bool json_find_first_in_array(const std::string& json, const char* key, std::string* out);

struct RegisterResult {
    bool        machine_authorized = false;
    bool        node_key_expired   = false;
    std::string auth_url;
    std::string error;
};
RegisterResult parse_register_response(const std::string& json);

}  // namespace ts
