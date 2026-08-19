#pragma once
// WireGuard のデータパケット (MessageTransport, type 4)。
//
//   u8 type, u8 reserved[3], u32 receiver, u64 counter, u8 encrypted_packet[]
//
// 送信はカウンタを単調増加させ、受信はスライディングウィンドウでリプレイを弾く。
#include <cstddef>
#include <cstdint>

#include "noise.hpp"

namespace wg {

constexpr size_t kTransportHeader = 16;  // type(1) + reserved(3) + receiver(4) + counter(8)
// WireGuard 仕様のリプレイウィンドウ。
constexpr uint64_t kReplayWindow = 2048;

class Transport {
public:
    Transport(const Crypto& crypto) : c_(crypto) {}

    void set_keypair(const Keypair& kp);
    bool ready() const { return ready_; }

    // 平文パケットを暗号化する。out は len + kTransportHeader + kTagLen 必要。
    // 返り値は出力バイト数。0 なら失敗。
    size_t encrypt(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len);

    // 受信パケットを復号する。out は len 分あれば足りる。返り値は平文バイト数（0 = 失敗/keepalive）。
    // keepalive（平文長 0）は成功扱いで 0 を返すので、is_valid で区別する。
    size_t decrypt(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len, bool* is_valid);

    uint64_t send_counter() const { return send_counter_; }
    uint64_t recv_max() const { return recv_max_; }
    // リプレイと判定して捨てた数（統計）。
    uint32_t replay_drops() const { return replay_drops_; }

private:
    bool check_replay(uint64_t counter);

    const Crypto& c_;
    Keypair       kp_{};
    bool          ready_        = false;
    uint64_t      send_counter_ = 0;
    uint64_t      recv_max_     = 0;
    // recv_max_ を最上位ビットとするビットマップ。受信済みカウンタを覚えてリプレイを弾く。
    uint64_t      window_[kReplayWindow / 64] = {};
    uint32_t      replay_drops_ = 0;
};

}  // namespace wg
