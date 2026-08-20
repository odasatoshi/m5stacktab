#include <cstdint>
#include "disco_responder.hpp"

#include <cstring>

#include "disco.hpp"
#include "noise.hpp"
#include "salsa20.hpp"

namespace ts {

bool DiscoResponder::set_key(const uint8_t disco_priv[32])
{
    std::lock_guard<std::mutex> guard(mu_);
    std::memcpy(disco_priv_, disco_priv, 32);
    have_key_ = wg::default_crypto().dh_pubkey(disco_pub_, disco_priv_);
    // **登録済みのピアを捨てる。** 各ピアの shared_key は自分の秘密鍵から
    // 導出したものなので、鍵が変われば全部作り直しになる。残すと
    // 「登録されているのに復号できない」状態になり、Ping が黙って失敗する。
    peers_.clear();
    return have_key_;
}

void DiscoResponder::clear_peers()
{
    std::lock_guard<std::mutex> guard(mu_);
    peers_.clear();
}

size_t DiscoResponder::peer_count() const
{
    std::lock_guard<std::mutex> guard(mu_);
    return peers_.size();
}

void DiscoResponder::note_send_result(bool sent)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (sent) {
        ++pongs_;
    } else {
        ++pong_fail_;
    }
}

bool DiscoResponder::add_peer(const uint8_t disco_pub[32])
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!have_key_) return false;
    // 既に知っているピアなら何もしない。
    for (const auto& p : peers_) {
        if (std::memcmp(p.disco_pub, disco_pub, 32) == 0) return true;
    }
    DiscoPeer peer;
    std::memcpy(peer.disco_pub, disco_pub, 32);
    uint8_t dh[32];
    if (!wg::default_crypto().dh(dh, disco_priv_, disco_pub)) return false;
    wg::box_beforenm(peer.shared_key, dh);
    peer.valid = true;
    peers_.push_back(peer);
    return true;
}

size_t DiscoResponder::handle(const uint8_t* pkt, size_t len, uint32_t src_ip, uint16_t src_port,
                              uint8_t* out, size_t out_cap)
{
    uint8_t sender[32];
    if (!disco_is_packet(pkt, len, sender)) return 0;

    // 鍵と共有鍵はロックの中で**値コピー**して取り出す。ポインタで持つと
    // add_peer の再確保や clear_peers で解放済みメモリを触る。
    uint8_t shared[32];
    uint8_t my_pub[32];
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (!have_key_) return 0;
        const DiscoPeer* found = nullptr;
        for (const auto& p : peers_) {
            if (std::memcmp(p.disco_pub, sender, 32) == 0) {
                found = &p;
                break;
            }
        }
        if (!found) {
            ++unknown_;
            return 0;
        }
        std::memcpy(shared, found->shared_key, 32);
        std::memcpy(my_pub, disco_pub_, 32);
    }

    DiscoType type{};
    DiscoPing ping{};
    if (!disco_open(pkt, len, shared, &type, &ping)) return 0;
    if (type != DiscoType::kPing) return 0;  // Pong や CallMeMaybe は今は使わない
    {
        std::lock_guard<std::mutex> guard(mu_);
        ++pings_;
    }

    // 送信元アドレスをそのまま返す。相手はこれを見て自分の外側アドレスを知る。
    uint8_t src[16];
    disco_v4_mapped(src, src_ip);
    uint8_t nonce[kDiscoNonceLen];
    if (!wg::default_crypto().random_bytes(nonce, sizeof(nonce))) return 0;

    // 実際に送れたかは呼び出し側が note_send_result で教える。
    return disco_build_pong(out, out_cap, my_pub, shared, ping.tx_id, src, src_port, nonce);
}

}  // namespace ts
