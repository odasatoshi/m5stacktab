#include <cstdint>
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

void Transport::set_keypair(const Keypair& kp, int64_t now_us)
{
    // 今の世代を 1 つ前に降格する。捨ててしまうと、相手がまだ古い鍵で送っている間の
    // パケットが全部復号できなくなる（rekey が交差すると数秒間そうなる）。
    //
    // ただし**未確認の世代で確認済みの世代を押し出してはいけない**。
    // こちらの msg2 が落ちるとピアは msg1 を再送し、そのたびに新しい未確認世代が来る。
    // 無条件に降格すると「ピアが一度も持っていない世代」だけが残り、
    // 送信が全損したままピアの送信を待つしかなくなる（ローカルには何も出ない）。
    if (cur_.valid && (cur_.confirmed || !prev_.valid)) prev_ = cur_;

    cur_              = SessionState{};
    cur_.kp           = kp;
    cur_.born_us      = now_us;
    cur_.valid        = true;
    cur_.confirmed    = kp.initiator;
    cur_.send_counter = 0;
    cur_.recv_max     = 0;
    std::memset(cur_.window, 0, sizeof(cur_.window));
}

bool Transport::expire_previous(int64_t now_us)
{
    if (!prev_.valid) return false;
    if (now_us - prev_.born_us < kRejectAfterUs) return false;
    // 相手はもうこの鍵を捨てている。送るのも復号を試すのも無駄。
    prev_ = SessionState{};
    return true;
}

SessionState* Transport::sending_session()
{
    // 現世代の確認が取れていて有効ならそれで送る。
    if (cur_.valid && cur_.confirmed) return &cur_;
    // 応答側で確認前なら、相手が知っている 1 つ前の鍵で送る。
    if (prev_.valid) return &prev_;
    if (cur_.valid) return &cur_;
    return nullptr;
}

size_t Transport::encrypt(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len)
{
    SessionState* s = sending_session();
    if (!s) return 0;
    const size_t need = kTransportHeader + len + kTagLen;
    if (out_cap < need) return 0;
    // カウンタは 2^64-1 まで。ここに達したら鍵を作り直すべきなので送らない。
    if (s->send_counter == UINT64_MAX) return 0;

    std::memset(out, 0, kTransportHeader);
    out[0] = kMsgTransport;
    store_le32(out + 4, s->kp.remote_index);
    store_le64(out + 8, s->send_counter);

    if (!c_.aead_encrypt(out + kTransportHeader, s->kp.send, s->send_counter, plain, len, nullptr,
                        0)) {
        return 0;
    }
    ++s->send_counter;
    return need;
}

bool Transport::check_replay(SessionState& s, uint64_t counter)
{
    if (counter >= s.recv_max + kReplayWindow) {
        // 遥か先のカウンタ: ウィンドウを丸ごと進める。
        std::memset(s.window, 0, sizeof(s.window));
        s.recv_max = counter;
        s.window[0] |= 1;  // bit0 = recv_max 自身
        return true;
    }
    if (counter > s.recv_max) {
        // ウィンドウを差分だけシフトする。
        const uint64_t shift = counter - s.recv_max;
        const size_t   words = static_cast<size_t>(shift / 64);
        const unsigned bits  = static_cast<unsigned>(shift % 64);
        constexpr size_t n   = kReplayWindow / 64;
        // shift < kReplayWindow は上の分岐で保証されているので words < n。
        // ウィンドウ幅を変えたときに UB にならないよう残してある。
        if (words >= n) {
            std::memset(s.window, 0, sizeof(s.window));
        } else {
            for (size_t i = n; i-- > 0;) {
                uint64_t v = (i >= words) ? s.window[i - words] : 0;
                if (bits && i >= words) {
                    const uint64_t lower = (i > words) ? s.window[i - words - 1] : 0;
                    v = (v << bits) | (lower >> (64 - bits));
                }
                s.window[i] = v;
            }
        }
        s.recv_max = counter;
        s.window[0] |= 1;
        return true;
    }
    // counter <= recv_max: ウィンドウ内なら重複チェック、外なら古すぎるので捨てる。
    const uint64_t back = s.recv_max - counter;
    if (back >= kReplayWindow) return false;
    const size_t   idx = static_cast<size_t>(back / 64);
    const uint64_t bit = 1ull << (back % 64);
    if (s.window[idx] & bit) return false;  // 既に受信済み
    s.window[idx] |= bit;
    return true;
}

size_t Transport::decrypt(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len,
                          bool* is_valid)
{
    if (is_valid) *is_valid = false;
    if (len < kTransportHeader + kTagLen) return 0;
    if (in[0] != kMsgTransport) return 0;

    // 宛先インデックスで世代を選ぶ。ハンドシェイクごとに違う値なので、
    // これで「新しい鍵で来たか、1 つ前の鍵で来たか」が分かる。
    const uint32_t receiver = load_le32(in + 4);
    SessionState*  s        = nullptr;
    bool           is_cur   = false;
    if (cur_.valid && receiver == cur_.kp.local_index) {
        s      = &cur_;
        is_cur = true;
    } else if (prev_.valid && receiver == prev_.kp.local_index) {
        s = &prev_;
    }
    if (!s) return 0;

    const uint64_t counter    = load_le64(in + 8);
    const size_t   cipher_len = len - kTransportHeader;
    const size_t   plain_len  = cipher_len - kTagLen;
    if (out_cap < plain_len) return 0;

    // 復号が通ってから初めてリプレイウィンドウを更新する（偽パケットで汚させない）。
    if (!c_.aead_decrypt(out, s->kp.recv, counter, in + kTransportHeader, cipher_len, nullptr, 0)) {
        return 0;
    }
    if (!check_replay(*s, counter)) {
        ++replay_drops_;
        return 0;
    }
    // 新しい鍵でデータが来た = 相手も新しい鍵を知った。以後はこちらも新しい鍵で送る。
    if (is_cur && !cur_.confirmed) cur_.confirmed = true;
    if (is_valid) *is_valid = true;
    return plain_len;  // 0 なら keepalive
}

}  // namespace wg
