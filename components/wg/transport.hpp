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

// 1 つの鍵世代ぶんの状態（鍵・送信カウンタ・リプレイウィンドウ）。
struct SessionState {
    Keypair  kp{};
    bool     valid        = false;
    // このセッションで相手からデータを受け取ったか。
    // 応答側は、確認が取れるまで**古い鍵で送り続ける**（相手はまだ新しい鍵を知らない）。
    bool     confirmed    = false;
    uint64_t send_counter = 0;
    uint64_t recv_max     = 0;
    uint64_t window[kReplayWindow / 64] = {};
};

class Transport {
public:
    Transport(const Crypto& crypto) : c_(crypto) {}

    // 新しい鍵世代を入れる。今の世代は「1 つ前」に降格し、そのまま復号に使える。
    // confirmed = true は「自分がハンドシェイクを完了させた側」（開始側）のとき。
    // 応答側は false で入れて、相手からデータが来たら昇格させる。
    void set_keypair(const Keypair& kp, bool confirmed = true);
    bool ready() const { return cur_.valid || prev_.valid; }

    // 平文パケットを暗号化する。out は len + kTransportHeader + kTagLen 必要。
    // 返り値は出力バイト数。0 なら失敗。
    size_t encrypt(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len);

    // 受信パケットを復号する。現世代 → 1 つ前の順に試す。
    // keepalive（平文長 0）は成功扱いで 0 を返すので、is_valid で区別する。
    size_t decrypt(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len, bool* is_valid);

    // 実際に送信に使っている世代のカウンタ（rekey 確認前は 1 つ前の世代）。
    uint64_t send_counter() const
    {
        const SessionState* s = (cur_.valid && cur_.confirmed) ? &cur_
                                : prev_.valid                  ? &prev_
                                                               : &cur_;
        return s->send_counter;
    }
    uint64_t recv_max() const { return cur_.recv_max; }
    uint32_t replay_drops() const { return replay_drops_; }
    bool     current_confirmed() const { return cur_.confirmed; }
    bool     has_previous() const { return prev_.valid; }

private:
    bool check_replay(SessionState& s, uint64_t counter);
    // 送信に使う世代を選ぶ。現世代の確認が取れていなければ 1 つ前を使う。
    SessionState* sending_session();

    const Crypto& c_;
    SessionState  cur_{};
    SessionState  prev_{};
    uint32_t      replay_drops_ = 0;
};

}  // namespace wg
