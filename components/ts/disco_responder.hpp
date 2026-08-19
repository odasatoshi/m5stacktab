#pragma once
// DISCO のレスポンダ。ピアからの Ping に Pong を返すだけの役。
//
// Tailscale のピアは Pong を受け取って初めて送信元アドレスを直接パスとして固定する。
// 自分から Ping を打つ必要はない（相手のエンドポイントは netmap の値を信じて送る）。
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

struct DiscoPeer {
    uint8_t disco_pub[32] = {};
    uint8_t shared_key[32] = {};  // crypto_box_beforenm の結果（nonce に依存しないので再利用可）
    bool    valid          = false;
};

class DiscoResponder {
public:
    // 自分の disco 秘密鍵。公開鍵は内部で導出する。
    bool set_key(const uint8_t disco_priv[32]);
    const uint8_t* public_key() const { return disco_pub_; }

    // netmap から得たピアの disco 公開鍵を登録する（共有鍵をここで計算しておく）。
    bool add_peer(const uint8_t disco_pub[32]);
    void clear_peers() { peers_.clear(); }
    size_t peer_count() const { return peers_.size(); }

    // 受信パケットを処理する。DISCO で Ping だったら Pong を out に書いて長さを返す。
    // それ以外は 0（送信不要）。
    size_t handle(const uint8_t* pkt, size_t len, uint32_t src_ip, uint16_t src_port, uint8_t* out,
                  size_t out_cap);

    uint32_t pings_received() const { return pings_; }
    uint32_t pongs_sent() const { return pongs_; }
    uint32_t unknown_peers() const { return unknown_; }

private:
    uint8_t                disco_priv_[32] = {};
    uint8_t                disco_pub_[32]  = {};
    bool                   have_key_       = false;
    std::vector<DiscoPeer> peers_;
    uint32_t               pings_   = 0;
    uint32_t               pongs_   = 0;
    uint32_t               unknown_ = 0;
};

}  // namespace ts
