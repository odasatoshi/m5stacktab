#include <cstdint>
#include "disco_responder.hpp"

#include <cstring>

#include "disco.hpp"
#include "noise.hpp"
#include "salsa20.hpp"

namespace ts {

bool DiscoResponder::set_key(const uint8_t disco_priv[32])
{
    std::memcpy(disco_priv_, disco_priv, 32);
    have_key_ = wg::default_crypto().dh_pubkey(disco_pub_, disco_priv_);
    return have_key_;
}

bool DiscoResponder::add_peer(const uint8_t disco_pub[32])
{
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
    if (!have_key_) return 0;
    uint8_t sender[32];
    if (!disco_is_packet(pkt, len, sender)) return 0;

    // 送信者の disco 公開鍵で共有鍵を引く。知らないピアからは受け付けない
    // （netmap に載っていない相手と鍵交換する理由がない）。
    const DiscoPeer* peer = nullptr;
    for (const auto& p : peers_) {
        if (std::memcmp(p.disco_pub, sender, 32) == 0) {
            peer = &p;
            break;
        }
    }
    if (!peer) {
        ++unknown_;
        return 0;
    }

    DiscoType type{};
    DiscoPing ping{};
    if (!disco_open(pkt, len, peer->shared_key, &type, &ping)) return 0;
    if (type != DiscoType::kPing) return 0;  // Pong や CallMeMaybe は今は使わない
    ++pings_;

    // 送信元アドレスをそのまま返す。相手はこれを見て自分の外側アドレスを知る。
    uint8_t src[16];
    disco_v4_mapped(src, src_ip);
    uint8_t nonce[kDiscoNonceLen];
    if (!wg::default_crypto().random_bytes(nonce, sizeof(nonce))) return 0;

    const size_t n = disco_build_pong(out, out_cap, disco_pub_, peer->shared_key, ping.tx_id, src,
                                      src_port, nonce);
    if (n > 0) ++pongs_;
    return n;
}

}  // namespace ts
