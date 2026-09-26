#pragma once
// DISCO のレスポンダ。ピアからの Ping に Pong を返し、自分が打った Ping の Pong を数える。
//
// **Tailscale のピアは、DISCO の Pong で確かめた経路にしか WireGuard を送らない。**
// こちらの handshake initiation は受け取って処理する（magicsock の lazyEndpoint）が、
// 応答は magicsock の endpoint.send() を通り、bestAddr（Pong で確定した経路）が
// 無ければ UDP では送らず DERP に回す。こちらは DERP を持たないので応答は消える。
// bestAddr を立てるには相手に Ping を打たせて Pong を返す必要があり、相手がこちらの
// 送信元を候補に入れるきっかけが、こちらからの Ping（handlePingLocked の
// addCandidateEndpoint）。だから相手には自分から Ping を打つ (#98)。
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "disco.hpp"

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

    // 登録済みのピアに打つ Ping を作る。node_key は自分の node 公開鍵。
    // TxID を覚えておき、handle() に来た Pong がそれと一致したときだけ数える
    // （復号できる = 相手の鍵で閉じてある、TxID 一致 = こちらが打った Ping への返事）。
    // 登録されていないピアなら 0。
    size_t build_ping(const uint8_t peer_disco_pub[32], const uint8_t node_key[32], uint8_t* out,
                      size_t out_cap);

    uint32_t pings_received() const { return pings_; }
    uint32_t pongs_sent() const { return pongs_; }
    uint32_t pongs_failed() const { return pong_fail_; }
    uint32_t unknown_peers() const { return unknown_; }
    uint32_t too_large() const { return too_large_; }
    // 自分の Ping に対して返ってきた Pong の数。0 のままなら経路が通っていない。
    uint32_t pongs_received() const { return pongs_rx_; }

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
    uint32_t               pongs_rx_  = 0;
    // 最近打った Ping の TxID。再送のたびに新しい TxID になるので、遅れて来た
    // 前の回の Pong も受けられるよう何本か持つ。
    // ponytail: 相手ごとに分けず 4 本の輪。同時に 1 ピアしか探らないので足りる。
    static constexpr int kPendingPings = 4;
    uint8_t                pending_[kPendingPings][kDiscoTxIdLen] = {};
    bool                   pending_valid_[kPendingPings] = {};
    int                    pending_next_ = 0;
};

}  // namespace ts
