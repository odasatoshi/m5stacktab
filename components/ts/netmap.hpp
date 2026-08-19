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

}  // namespace ts
