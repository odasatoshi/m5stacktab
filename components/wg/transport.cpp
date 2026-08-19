#include "transport.hpp"

#include <cstring>

namespace wg {
namespace {

void store_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void store_le64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t load_le64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

uint32_t load_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void Transport::set_keypair(const Keypair& kp)
{
    kp_           = kp;
    ready_        = true;
    send_counter_ = 0;
    recv_max_     = 0;
    replay_drops_ = 0;
    std::memset(window_, 0, sizeof(window_));
}

size_t Transport::encrypt(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len)
{
    if (!ready_) return 0;
    const size_t need = kTransportHeader + len + kTagLen;
    if (out_cap < need) return 0;
    // カウンタは 2^64-1 まで。ここに達したら鍵を作り直すべきなので送らない。
    if (send_counter_ == UINT64_MAX) return 0;

    std::memset(out, 0, kTransportHeader);
    out[0] = kMsgTransport;
    store_le32(out + 4, kp_.remote_index);
    store_le64(out + 8, send_counter_);

    if (!c_.aead_encrypt(out + kTransportHeader, kp_.send, send_counter_, plain, len, nullptr, 0)) {
        return 0;
    }
    ++send_counter_;
    return need;
}

bool Transport::check_replay(uint64_t counter)
{
    if (counter >= recv_max_ + kReplayWindow) {
        // 遥か先のカウンタ: ウィンドウを丸ごと進める。
        std::memset(window_, 0, sizeof(window_));
        recv_max_ = counter;
        window_[0] |= 1;  // bit0 = recv_max_ 自身
        return true;
    }
    if (counter > recv_max_) {
        // ウィンドウを差分だけシフトする。
        const uint64_t shift = counter - recv_max_;
        const size_t   words = static_cast<size_t>(shift / 64);
        const unsigned bits  = static_cast<unsigned>(shift % 64);
        constexpr size_t n   = kReplayWindow / 64;
        if (words >= n) {
            std::memset(window_, 0, sizeof(window_));
        } else {
            for (size_t i = n; i-- > 0;) {
                uint64_t v = (i >= words) ? window_[i - words] : 0;
                if (bits && i >= words) {
                    const uint64_t lower = (i > words) ? window_[i - words - 1] : 0;
                    v = (v << bits) | (bits ? (lower >> (64 - bits)) : 0);
                }
                window_[i] = v;
            }
        }
        recv_max_ = counter;
        window_[0] |= 1;
        return true;
    }
    // counter <= recv_max_: ウィンドウ内なら重複チェック、外なら古すぎるので捨てる。
    const uint64_t back = recv_max_ - counter;
    if (back >= kReplayWindow) return false;
    const size_t   idx = static_cast<size_t>(back / 64);
    const uint64_t bit = 1ull << (back % 64);
    if (window_[idx] & bit) return false;  // 既に受信済み
    window_[idx] |= bit;
    return true;
}

size_t Transport::decrypt(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len,
                          bool* is_valid)
{
    if (is_valid) *is_valid = false;
    if (!ready_ || len < kTransportHeader + kTagLen) return 0;
    if (in[0] != kMsgTransport) return 0;
    if (load_le32(in + 4) != kp_.local_index) return 0;  // 自分宛でない

    const uint64_t counter   = load_le64(in + 8);
    const size_t   cipher_len = len - kTransportHeader;
    const size_t   plain_len  = cipher_len - kTagLen;
    if (out_cap < plain_len) return 0;

    // 復号が通ってから初めてリプレイウィンドウを更新する（偽パケットでウィンドウを汚させない）。
    if (!c_.aead_decrypt(out, kp_.recv, counter, in + kTransportHeader, cipher_len, nullptr, 0)) {
        return 0;
    }
    if (!check_replay(counter)) {
        ++replay_drops_;
        return 0;
    }
    if (is_valid) *is_valid = true;
    return plain_len;  // 0 なら keepalive
}

}  // namespace wg
