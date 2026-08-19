#pragma once
// DISCO のレスポンダ。ピアからの Ping に Pong を返すだけの役。
//
// Tailscale のピアは Pong を受け取って初めて送信元アドレスを直接パスとして固定する。
// 自分から Ping を打つ必要はない（相手のエンドポイントは netmap の値を信じて送る）。
#include <cstdint>
#include <mutex>
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
    // 鍵が設定されているか（未設定のまま全ゼロの公開鍵を鍵として見せないため）。
    bool has_key() const { return have_key_; }
    const uint8_t* public_key() const { return disco_pub_; }

    // netmap から得たピアの disco 公開鍵を登録する（共有鍵をここで計算しておく）。
    bool add_peer(const uint8_t disco_pub[32]);
    void   clear_peers();
    size_t peer_count() const;

    // 受信パケットを処理する。DISCO で Ping だったら Pong を out に書いて長さを返す。
    // それ以外は 0（送信不要）。
    //
    // 受信タスクから呼ばれ、add_peer / set_key は別タスクから来るのでロックする。
    // ピアは値コピーで取り出す（ポインタで持つと push_back の再確保で dangling になる）。
    size_t handle(const uint8_t* pkt, size_t len, uint32_t src_ip, uint16_t src_port, uint8_t* out,
                  size_t out_cap);

    // 実際に送信できたかを呼び出し側から教える（送れていないのに送信済みと数えないため）。
    void note_send_result(bool sent);

    uint32_t pings_received() const { return pings_; }
    uint32_t pongs_sent() const { return pongs_; }
    uint32_t pongs_failed() const { return pong_fail_; }
    uint32_t unknown_peers() const { return unknown_; }
    uint32_t too_large() const { return too_large_; }

private:
    uint8_t                disco_priv_[32] = {};
    uint8_t                disco_pub_[32]  = {};
    bool                   have_key_       = false;
    std::vector<DiscoPeer> peers_;
    mutable std::mutex     mu_;
    uint32_t               pings_     = 0;
    uint32_t               pongs_     = 0;
    uint32_t               pong_fail_ = 0;
    uint32_t               unknown_   = 0;
    uint32_t               too_large_ = 0;
};

}  // namespace ts
