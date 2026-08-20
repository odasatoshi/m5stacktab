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
    // 応答側として処理したハンドシェイク（相手から rekey された回数）。
    uint32_t responses_sent = 0;
    uint32_t stale_initiations = 0;  // タイムスタンプが巻き戻っていて拒否した数
    uint32_t tx_packets = 0;
    uint32_t rx_packets = 0;
    uint32_t tx_bytes   = 0;
    uint32_t rx_bytes   = 0;
    uint32_t tx_dropped = 0;
    uint32_t rx_dropped = 0;  // 復号失敗・リプレイ・宛先不一致
    uint32_t handshakes = 0;
    uint32_t keepalives = 0;
    uint32_t rekeys     = 0;
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
    // 鍵が確定したかを立てる。wg_netif.cpp の内部から呼ぶ（ロックの内側）。
    void      set_handshake_ok(bool v) { handshake_ok_ = v; }

    // WireGuard 以外のパケット（DISCO / STUN）を受けたときに呼ばれる。
    // wg は ts に依存しないので、DISCO の処理は呼び出し側（ts 層）に任せる。
    // src_ip はネットワークバイトオーダ。
    using ForeignPacketHandler = void (*)(const uint8_t* pkt, size_t len, uint32_t src_ip,
                                         uint16_t src_port);
    void set_foreign_handler(ForeignPacketHandler fn);

    // 同じ UDP ソケットから任意の宛先へ生のバイト列を送る（DISCO の応答用）。
    // WireGuard と同じポートを共有するので、ここを通さないと NAT のマッピングがずれる。
    bool send_raw(const uint8_t* data, size_t len, uint32_t dst_ip, uint16_t dst_port);

    // タイムスタンプの単調性を保つために、最後に送った TAI64N を保存・復元する。
    // 保存しないと再起動でカウンタが巻き戻り、ピアがリプレイとして無視する。
    using TimestampStore = bool (*)(uint64_t* seconds, bool write);
    void set_timestamp_store(TimestampStore fn) { ts_store_ = fn; }

    const NetifStats& stats() const { return stats_; }
    const char*       last_error() const { return last_error_.c_str(); }

private:
    bool           netif_up_ = false;
    // 鍵が確定したか。生の transport ポインタを別タスクから触らせないための写し。
    volatile bool  handshake_ok_ = false;
    NetifStats     stats_;
    std::string    last_error_;
    TimestampStore ts_store_ = nullptr;
};

// 実装が持つシングルトンを返す（lwIP のコールバックから触るため）。
Netif& netif_instance();

}  // namespace wg
