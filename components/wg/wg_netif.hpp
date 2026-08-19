#pragma once
// WireGuard のトンネルを lwIP のネットワークインターフェースとして生やす。
//
// 送信: lwIP が IP パケットを渡してくる → 暗号化して UDP でピアへ送る
// 受信: UDP を受け取るタスクが復号して lwIP に注入する
//
// ルーティングはサブネットマスクで済ませる。Tailscale のアドレスは 100.64.0.0/10 に
// 収まっているので、netif のマスクを /10 にすれば tailnet 宛だけがこの netif に向く
// （lwIP にポリシールーティングは無い）。
#include <cstdint>
#include <string>
#include <vector>

#include <esp_err.h>
#include <lwip/ip_addr.h>

#include "noise.hpp"
#include "transport.hpp"

namespace wg {

struct PeerConfig {
    uint8_t     public_key[kKeyLen] = {};
    std::string endpoint;      // "192.168.0.5:41641"
    uint32_t    keepalive_sec = 25;  // 0 なら送らない
};

struct NetifStats {
    uint32_t tx_packets = 0;
    uint32_t rx_packets = 0;
    uint32_t tx_bytes   = 0;
    uint32_t rx_bytes   = 0;
    uint32_t tx_dropped = 0;
    uint32_t rx_dropped = 0;  // 復号失敗・リプレイ・宛先不一致
    uint32_t handshakes = 0;
};

class Netif {
public:
    // 自分の静的鍵。トンネルのアドレスとマスク（例: 100.64.0.3 / 255.192.0.0）。
    esp_err_t up(const uint8_t static_priv[kKeyLen], const ip4_addr_t& addr,
                 const ip4_addr_t& netmask, uint16_t listen_port = 41641);
    void      down();
    bool      is_up() const { return netif_up_; }

    // ピアを設定してハンドシェイクを開始する（今は 1 ピアのみ）。
    esp_err_t set_peer(const PeerConfig& peer);
    bool      handshake_done() const;

    const NetifStats& stats() const { return stats_; }
    const char*       last_error() const { return last_error_.c_str(); }

private:
    bool netif_up_ = false;
    NetifStats  stats_;
    std::string last_error_;
};

// 実装が持つシングルトンを返す（lwIP のコールバックから触るため）。
Netif& netif_instance();

}  // namespace wg
