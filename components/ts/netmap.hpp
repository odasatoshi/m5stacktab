#pragma once
// MapResponse (netmap) の解析。cJSON を使う。
//
// 必要なのは「自分のアドレス」と「ピアの鍵・許可 IP・エンドポイント」だけ。
// DERPMap / DNSConfig / PacketFilters は今は使わないので読み飛ばす。
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

struct Peer {
    int64_t                  id = 0;
    std::string              name;         // "host.tailnet.ts.net."
    std::string              node_key;     // "nodekey:<hex>"
    std::string              disco_key;    // "discokey:<hex>"（無いこともある）
    std::vector<std::string> allowed_ips;  // WireGuard の AllowedIPs
    std::vector<std::string> endpoints;    // "192.168.0.5:41641"（オフラインだと空）
    int                      home_derp = 0;
    bool                     online    = false;
};

struct NetMap {
    bool                     keepalive = false;  // true なら他フィールドは無効
    std::string              domain;
    std::string              node_key;
    std::string              disco_key;
    std::vector<std::string> addresses;  // 自分の 100.x / fd7a:...
    std::vector<Peer>        peers;
    // 差分更新
    std::vector<Peer>    peers_changed;
    std::vector<int64_t> peers_removed;
    bool                 has_peers = false;  // Peers キーがあったか（無い = 変更なし）
};

// 1 件の MapResponse JSON を解析する。失敗したら false。
bool parse_netmap(const std::string& json, NetMap* out);

// ピアが申告した複数のエンドポイントから 1 つ選ぶ。
//
// **先頭を無条件に取ってはいけない。** ピアは自分の全インターフェースを申告する
// ので、先頭が届かないアドレスであることが普通にある（実機で踏んだ: mac-peer の
// 申告は 111.102.218.1 / 192.168.0.57 / 192.168.0.101 / 192.168.64.1 の順で、
// 先頭は Mac の別インターフェース）。IPv6 のエンドポイントも普通に混ざる。
//
// my_addr / my_mask はネットワークバイトオーダの IPv4（lwIP の ip4_addr_t と同じ）。
// 0 を渡すとサブネットの一致は見ない。
//
// 選び方: 自分と同じサブネットの IPv4 → 最初に解析できた IPv4 → 空。
// **IPv4 として解析できないものは返さない。** 返すと set_peer が
// ESP_ERR_INVALID_ARG で落ちて、後ろにある届く候補を試さないまま終わる。
//
// ponytail: 本物の Tailscale は DISCO で全部に Ping を投げて通る経路を選ぶ。
// こちらは応答専用なので探りに行かない。同一 LAN 限定という天井と同じ範囲。
std::string pick_endpoint(const std::vector<std::string>& endpoints, uint32_t my_addr,
                          uint32_t my_mask);

}  // namespace ts
